package app.nativespotify

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.media.MediaMetadata
import android.media.session.MediaSession
import android.media.session.PlaybackState
import android.os.IBinder
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch

/**
 * The Android side of the core's engine: the core decides when Soloist runs, this starts and
 * stops its Termux session. Only one session exists; its id comes from the core's adapter.
 */
internal object EngineHost {
    @Volatile var running = false
        private set
    @Volatile private var current = 0
    @Volatile private var pendingKey: String? = null
    /** The loopback port the core's Soloist client connects to. */
    @Volatile var port = 0
        private set

    fun start(context: Context, id: Int, key: String, port: Int): Boolean {
        current = id
        pendingKey = key
        this.port = port
        running = true
        val intent = Intent(context, PlaybackService::class.java).setAction(PlaybackService.START).putExtra("id", id)

        if (runCatching { context.startForegroundService(intent) }.isSuccess) {
            return true
        }

        running = false
        pendingKey = null
        return false
    }

    /** Hands the key to the service once; it goes to Termux on stdin and is not kept. */
    fun takeKey(id: Int): String? {
        if (!isCurrent(id)) return null
        val key = pendingKey
        pendingKey = null
        return key
    }

    fun isCurrent(id: Int): Boolean = running && current == id

    fun stop(context: Context, id: Int) {
        if (id != current || !running) {
            return
        }

        running = false
        Termux.run(context, "stop")
        context.stopService(Intent(context, PlaybackService::class.java))
    }

    /** The Termux session ended (or never started); the core decides whether to try again. */
    fun exited(id: Int, code: Int) {
        if (!isCurrent(id)) {
            return
        }

        running = false
        NativeCore.engineExited(id, code)
    }
}

class PlaybackService : Service() {
    companion object {
        const val START = "start"
        const val QUIT = "quit"
        internal const val START_FIXTURE = "start-media3-fixture"
        internal const val FIXTURE_FILE = "media3-service-tone.wav"
        internal const val FIXTURE_URI = "fixture:service-tone"
        private val nextGeneration = java.util.concurrent.atomic.AtomicLong()
        @Volatile private var local: LocalPlaybackController? = null

        internal fun dispatchLocalPlayback(command: LocalPlaybackCommand): Boolean = local?.post(command) ?: false

        internal fun localSession(): LocalPlaybackSession = local?.session
            ?: throw LocalPlaybackUnavailable("No authorized playback provider is active")

        internal fun localGeneration(): Long = local?.generation ?: 0L
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private lateinit var media: MediaSession
    private var keepalive: Job? = null
    private var localController: LocalPlaybackController? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel("playback", "Local playback", NotificationManager.IMPORTANCE_LOW))
        media = MediaSession(this, "Native Spotify")

        // System media controls send the same core actions as the app's buttons.
        media.setCallback(object : MediaSession.Callback() {
            override fun onPlay() = Core.action(BuildConfig.ACTION_PLAY)
            override fun onPause() = Core.action(BuildConfig.ACTION_PAUSE)
            override fun onSkipToNext() = Core.action(BuildConfig.ACTION_NEXT)
            override fun onSkipToPrevious() = Core.action(BuildConfig.ACTION_PREVIOUS)

            override fun onSeekTo(position: Long) {
                val playback = Core.state.value.playback

                if (playback.canSeek && playback.durationMs > 0) {
                    Core.seekLyric(playback.uri, position.coerceIn(0, playback.durationMs.toLong() - 1).toInt())
                }
            }

            override fun onStop() = Core.action(BuildConfig.ACTION_PAUSE)
        })
        media.isActive = true
        scope.launch {
            Core.state.map { it.playback }.distinctUntilChanged().collect(::publish)
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(1, notification(Core.state.value.playback))

        if (BuildConfig.MEDIA3_FIXTURE && intent?.action == START_FIXTURE) {
            if (localController == null) {
                val file = java.io.File(cacheDir, FIXTURE_FILE)

                if (!file.isFile) {
                    stopSelf()
                    return START_NOT_STICKY
                }

                val source = object : AuthorizedPlaybackSource {
                    override suspend fun signIn(context: Context) = refresh(context, null)

                    override suspend fun refresh(context: Context, previous: PlaybackGrant?): PlaybackGrant =
                        PlaybackGrant("generated-audio-fixture", System.currentTimeMillis() + 300_000)

                    override suspend fun resolve(contentUri: android.net.Uri, grant: PlaybackGrant): ResolvedPlayback {
                        if (contentUri.toString() != FIXTURE_URI) {
                            throw LocalPlaybackUnavailable("Not a fixture track")
                        }

                        return ResolvedPlayback(android.net.Uri.fromFile(file), null)
                    }
                }
                val generation = nextGeneration.incrementAndGet()
                val session = LocalPlaybackSession(this, source, scope, Secrets(this), "media3-fixture-session")
                val controller = LocalPlaybackController(this, generation, session, source, scope)
                localController = controller
                NativeCore.setLocalPlaybackRoute(generation, true)
                local = controller
            }

            return START_NOT_STICKY
        }

        if (intent?.action == QUIT) {
            Core.engineStop()
            return START_NOT_STICKY
        }

        val id = intent?.getIntExtra("id", 0) ?: 0
        val key = EngineHost.takeKey(id)

        if (intent?.action != START || key == null) {
            return START_NOT_STICKY
        }

        if (!Termux.run(this, "start", key, engineId = id, port = EngineHost.port)) {
            EngineHost.exited(id, -1)
            stopSelf()
            return START_NOT_STICKY
        }

        // player.sh stops Soloist if the lease is not renewed, so a dead app never leaves it running.
        keepalive?.cancel()
        keepalive = scope.launch {
            while (true) {
                delay(25_000)
                Termux.run(this@PlaybackService, "keepalive")
            }
        }
        return START_NOT_STICKY
    }

    private fun publish(playback: PlaybackInfo) {
        media.setMetadata(MediaMetadata.Builder()
            .putString(MediaMetadata.METADATA_KEY_TITLE, playback.title)
            .putString(MediaMetadata.METADATA_KEY_ARTIST, playback.artist)
            .putString(MediaMetadata.METADATA_KEY_ALBUM, playback.album)
            .putLong(MediaMetadata.METADATA_KEY_DURATION, playback.durationMs.toLong()).build())
        val status = when {
            playback.loading -> PlaybackState.STATE_BUFFERING
            playback.playing -> PlaybackState.STATE_PLAYING
            else -> PlaybackState.STATE_PAUSED
        }
        val actions = PlaybackState.ACTION_PLAY_PAUSE or PlaybackState.ACTION_PLAY or PlaybackState.ACTION_PAUSE or
            PlaybackState.ACTION_STOP or (if (playback.canSeek) PlaybackState.ACTION_SEEK_TO else 0L) or
            (if (playback.canPrevious) PlaybackState.ACTION_SKIP_TO_PREVIOUS else 0L) or
            (if (playback.canNext) PlaybackState.ACTION_SKIP_TO_NEXT else 0L)
        media.setPlaybackState(PlaybackState.Builder().setActions(actions)
            .setState(status, playback.position().toLong(),
                if (status == PlaybackState.STATE_PLAYING && !playback.seeking) 1f else 0f).build())

        if (EngineHost.running) {
            getSystemService(NotificationManager::class.java).notify(1, notification(playback))
        }
    }

    private fun notification(playback: PlaybackInfo): Notification = Notification.Builder(this, "playback")
        .setSmallIcon(android.R.drawable.ic_media_play)
        .setContentTitle(playback.title.ifBlank { "Native Spotify" })
        .setContentText(playback.artist.ifBlank { "Playback engine" })
        .setContentIntent(PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE))
        .addAction(Notification.Action.Builder(null, "Quit", PendingIntent.getService(this, 1,
            Intent(this, PlaybackService::class.java).setAction(QUIT), PendingIntent.FLAG_IMMUTABLE)).build())
        .setStyle(Notification.MediaStyle().setMediaSession(media.sessionToken)).setOngoing(true).build()

    override fun onDestroy() {
        localController?.let {
            local = null
            NativeCore.setLocalPlaybackRoute(it.generation, false)
            it.close()
        }
        keepalive?.cancel()
        scope.cancel()
        media.release()
        stopForeground(STOP_FOREGROUND_REMOVE)
        super.onDestroy()
    }
}
