package app.nativespotify

import android.content.Context
import android.os.Handler
import androidx.media3.common.AudioAttributes
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch

internal class LocalPlaybackController(
    context: Context,
    val generation: Long,
    val session: LocalPlaybackSession,
    private val source: AuthorizedPlaybackSource,
    private val scope: CoroutineScope,
) : AutoCloseable {
    private val player = ExoPlayer.Builder(context).build()
    private val handler = Handler(player.applicationLooper)
    private var pending: Job? = null
    private var current = 0L
    private var closed = false
    private val listener = object : Player.Listener {
        override fun onEvents(player: Player, events: Player.Events) {
            if (closed || player.currentMediaItem?.mediaId != current.toString()) {
                return
            }

            val state = when {
                player.playerError != null -> LocalPlaybackState.ERROR
                player.playbackState == Player.STATE_ENDED -> LocalPlaybackState.ENDED
                player.playbackState == Player.STATE_BUFFERING -> LocalPlaybackState.BUFFERING
                player.isPlaying -> LocalPlaybackState.PLAYING
                player.playbackState == Player.STATE_READY -> LocalPlaybackState.PAUSED
                else -> return
            }
            report(state)
        }

        override fun onPositionDiscontinuity(oldPosition: Player.PositionInfo, newPosition: Player.PositionInfo,
            reason: Int) {
            if (!closed && reason == Player.DISCONTINUITY_REASON_SEEK &&
                newPosition.mediaItem?.mediaId == current.toString()) {
                report(LocalPlaybackState.SEEKED)
            }
        }
    }

    init {
        player.setAudioAttributes(AudioAttributes.DEFAULT, true)
        player.setHandleAudioBecomingNoisy(true)
        player.addListener(listener)
    }

    fun post(command: LocalPlaybackCommand): Boolean = command.generation == generation && handler.post {
        if (!closed) {
            handle(command)
        }
    }

    private fun handle(command: LocalPlaybackCommand) {
        if (command.action == LocalPlaybackAction.PLAY) {
            if (command.requestId <= current) {
                return
            }

            val uri = command.contentUri ?: return
            pending?.cancel()
            current = command.requestId
            player.stop()
            player.clearMediaItems()
            report(LocalPlaybackState.BUFFERING)

            pending = scope.launch {
                try {
                    val grant = session.refresh()
                    val resolved = try {
                        source.resolve(uri, grant)
                    } catch (_: PlaybackGrantRejected) {
                        source.resolve(uri, session.refresh(rejected = grant))
                    }
                    ensureActive()

                    if (current != command.requestId || closed) {
                        return@launch
                    }

                    val item = MediaItem.Builder().setMediaId(current.toString()).setUri(resolved.mediaUri)
                    resolved.drm?.let {
                        item.setDrmConfiguration(MediaItem.DrmConfiguration.Builder(it.scheme)
                            .setLicenseUri(it.licenseUri).build())
                    }
                    player.setMediaItem(item.build(), command.positionMs)
                    player.prepare()
                    player.play()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (_: Exception) {
                    if (!closed && current == command.requestId) {
                        report(LocalPlaybackState.ERROR)
                    }
                }
            }
            return
        }

        if (command.requestId != current) {
            return
        }

        when (command.action) {
            LocalPlaybackAction.PAUSE -> {
                pending?.cancel()
                player.pause()
                if (player.currentMediaItem == null) {
                    report(LocalPlaybackState.PAUSED)
                }
            }
            LocalPlaybackAction.RESUME -> {
                if (player.currentMediaItem != null) {
                    player.play()
                }
            }
            LocalPlaybackAction.SEEK -> {
                if (player.isCurrentMediaItemSeekable && player.duration >= 0) {
                    player.seekTo(command.positionMs.coerceAtMost(player.duration))
                }
            }
            LocalPlaybackAction.STOP -> {
                pending?.cancel()
                player.stop()
                player.clearMediaItems()
                report(LocalPlaybackState.PAUSED)
            }
            LocalPlaybackAction.PLAY -> Unit
        }
    }

    private fun report(state: LocalPlaybackState) {
        CoreHost.reportLocalPlaybackEvent(LocalPlaybackEvent(generation, current, state,
            player.currentPosition.coerceAtLeast(0), player.duration.coerceAtLeast(-1)))
    }

    override fun close() {
        closed = true
        pending?.cancel()
        player.removeListener(listener)
        player.release()
    }
}
