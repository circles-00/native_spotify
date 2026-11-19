package app.nativespotify

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import org.json.JSONArray
import org.json.JSONObject

/** JNI entry points into the shared C core (android/app/src/main/cpp/bridge.c). */
internal object NativeCore {
    init { System.loadLibrary("native_spotify_android") }

    external fun start(directory: String): Boolean
    external fun connect()
    external fun disconnect()
    external fun refresh()
    external fun search(query: String, filter: Int): Boolean
    external fun browse(kind: Int): Boolean
    external fun open(item: String): Boolean
    external fun toggleLike(item: String): Boolean
    external fun artistLikes(item: String): Boolean
    external fun songRadio(item: String): Boolean
    external fun historyAdd(item: String)
    external fun historyRemove(id: String)
    external fun toggleSaved(): Boolean
    external fun discography(group: String): Boolean
    external fun sort(sort: Int, descending: Boolean): Boolean
    external fun filter(query: String): Boolean
    external fun loadMore(): Boolean
    external fun viewport(first: Int)
    external fun libraryViewport(first: Int)
    external fun setSearchRadio(enabled: Boolean): Boolean
    external fun play(item: String): Boolean
    external fun playHistory(item: String): Boolean
    external fun playUpcoming(item: String): Boolean
    external fun startRadio(item: String): Boolean
    external fun enqueue(item: String): Boolean
    external fun dequeue(id: String): Boolean
    external fun playQueued(id: String): Boolean
    external fun clearQueue(): Boolean
    external fun queueViewport(first: Int)
    external fun queueVisible(visible: Boolean)
    external fun lyricsVisible(visible: Boolean)
    external fun playCollection(): Boolean
    external fun playList(): Boolean
    external fun playPopular(index: Int): Boolean
    external fun action(action: Int)
    external fun seek(fraction: Int): Boolean
    external fun seekLyric(uri: String, position: Int): Boolean
    external fun tick(visible: Boolean): Int
    external fun enginePlaybackChanged()
    external fun devicesVisible(visible: Boolean)
    external fun transferTo(id: String): Boolean
    external fun transferHere(): Boolean
    external fun volume(percent: Int): Boolean
    external fun engineStop()
    external fun engineExited(id: Int, code: Int)
    external fun snapshot(): ByteArray?
}

/** A catalog item from the core. `json` is passed back verbatim when acting on it. */
internal data class Item(
    val kind: Int, val id: String, val title: String, val detail: String, val guests: String, val artwork: String,
    val playable: Boolean, val durationMs: Int, val explicit: Boolean, val liked: Boolean, val trackNumber: Int,
    val discNumber: Int, val artistId: String, val albumId: String, val albumTitle: String,
    val releaseDate: String, val albumType: String, val ownerId: String, val json: String,
) {
    val uri: String get() = "spotify:${kindName(kind)}:$id"

}

internal fun kindName(kind: Int) = when (kind) {
    BuildConfig.ITEM_TRACK -> "track"
    BuildConfig.ITEM_ALBUM -> "album"
    BuildConfig.ITEM_PLAYLIST -> "playlist"
    BuildConfig.ITEM_EPISODE -> "episode"
    else -> "artist"
}

internal data class ListState(
    val kind: Int = -1, val searchFilter: Int = 0, val query: String = "", val id: String = "", val title: String = "",
    val artwork: String = "", val first: Int = 0, val count: Int = 0, val total: Int = 0, val durationMs: Long = 0,
    val sort: Int = 0, val descending: Boolean = false, val filter: String = "", val loaded: Boolean = false,
    val more: Boolean = false, val loading: Boolean = false, val revision: Int = 0, val arrangeable: Boolean = false,
    val rows: List<Item> = emptyList(),
) {
    /** The row at an absolute index, if it is inside the snapshot's window. */
    fun row(index: Int): Item? = rows.getOrNull(index - first)
}

internal data class Page(val total: Int = 0, val loaded: Boolean = false, val items: List<Item> = emptyList())

internal data class Details(
    val item: Item? = null, val artist: Item? = null, val popular: List<Item> = emptyList(), val release: String = "",
    val duration: String = "", val label: String = "", val copyrights: String = "", val totalTracks: Int = 0,
    val followers: Long = 0, val followersKnown: Boolean = false, val popularLoaded: Boolean = false,
    val loaded: Boolean = false, val saved: Boolean = false, val savedKnown: Boolean = false, val saving: Boolean = false,
    val permissionRequired: Boolean = false,
)

/** What is playing, as the core last confirmed it; `receivedAt` dates the position. */
internal data class PlaybackInfo(
    val playing: Boolean = false, val shuffle: Boolean = false, val repeat: Int = 0, val uri: String = "",
    val contextUri: String = "", val title: String = "", val artist: String = "", val artistId: String = "",
    val album: String = "", val albumId: String = "", val artwork: String = "", val device: String = "",
    val positionMs: Int = 0, val durationMs: Int = 0, val hasDevice: Boolean = false, val canSeek: Boolean = false,
    val canPrevious: Boolean = true, val canNext: Boolean = true, val liked: Boolean = false, val revision: Int = 0,
    val receivedAt: Long = 0, val deviceId: String = "", val volume: Int = -1, val canVolume: Boolean = false,
    val changingTrack: Boolean = false,
) {
    /** The position now, moving on from the snapshot while playing. */
    fun position(now: Long = clock()): Int {
        val moved = if (playing && !changingTrack) (now - receivedAt).coerceAtLeast(0) else 0
        return (positionMs + moved).coerceIn(0, durationMs.toLong().coerceAtLeast(0)).toInt()
    }

    /** The playing song as an item, for liking it or opening its actions. */
    fun item(): Item? {
        val id = uri.removePrefix("spotify:track:").takeIf { uri.startsWith("spotify:track:") } ?: return null
        return itemFromJson(JSONObject().put("kind", BuildConfig.ITEM_TRACK).put("id", id).put("title", title)
            .put("detail", artist).put("artistId", artistId).put("albumId", albumId).put("albumTitle", album)
            .put("artwork", artwork).put("playable", true).put("liked", liked).put("durationMs", durationMs).toString())
    }
}

internal fun clock(): Long = System.nanoTime() / 1_000_000

/** A Spotify Connect device; `here` marks this phone's own Soloist. */
internal data class DeviceInfo(val id: String, val name: String, val type: String, val active: Boolean,
                               val restricted: Boolean, val canTransfer: Boolean, val here: Boolean)

internal data class DevicesInfo(val engineId: String = "", val remoteSelected: Boolean = false,
                                val items: List<DeviceInfo> = emptyList())

internal data class QueueInfo(val first: Int = 0, val total: Int = 0, val queued: Int = 0, val current: Item? = null,
                              val items: List<Item> = emptyList())

internal data class LyricLine(val timeMs: Int, val text: String)
internal data class LyricsInfo(val state: Int = 0, val uri: String = "", val active: Int = -1, val text: String = "",
                               val lines: List<LyricLine> = emptyList())

internal data class CoreProfile(
    val id: String = "", val name: String = "", val artwork: String = "", val followers: Long = 0,
    val followingTotal: Int = 0, val following: List<Item> = emptyList(), val loaded: Boolean = false,
    val permissionRequired: Boolean = false,
)

internal data class CoreState(
    val started: Boolean = false, val authenticated: Boolean = false, val connecting: Boolean = false,
    val loadingIntro: Boolean = true, val status: String = "", val session: Int = 0, val searchRadio: Boolean = false,
    val profile: CoreProfile = CoreProfile(), val savedTracks: Page = Page(), val savedAlbums: Page = Page(),
    val playlistsPage: Page = Page(), val recent: List<Item> = emptyList(), val recentLoaded: Boolean = false,
    val recentPermissionRequired: Boolean = false, val list: ListState = ListState(),
    val playlists: ListState = ListState(), val details: Details = Details(), val searchTop: Item? = null,
    val searchArtists: List<Item> = emptyList(), val searchAlbums: List<Item> = emptyList(),
    val searchPlaylists: List<Item> = emptyList(), val history: List<Item> = emptyList(),
    val artistLikedCount: Int = 0, val artistLikesReady: Boolean = false, val artistLikesFailed: Boolean = false,
    val playback: PlaybackInfo = PlaybackInfo(), val queue: QueueInfo = QueueInfo(), val lyrics: LyricsInfo = LyricsInfo(),
    val engine: Int = 0, val engineMessage: String = "", val setupRevision: Int = 0,
    val devices: DevicesInfo = DevicesInfo(),
)

private fun JSONArray?.objects(): List<JSONObject> = (0 until (this?.length() ?: 0)).mapNotNull { this?.optJSONObject(it) }

/** Rebuilds an item from the JSON a snapshot gave, e.g. one saved in the navigation stack. */
internal fun itemFromJson(json: String): Item? = runCatching { JSONObject(json).item() }.getOrNull()

/** An album or artist reference built from a song's fields, e.g. for "Go to artist". */
internal fun itemStub(kind: Int, id: String, title: String, artwork: String): Item =
    itemFromJson(JSONObject().put("kind", kind).put("id", id).put("title", title).put("artwork", artwork).toString())!!

private fun JSONObject.item() = Item(
    optInt("kind"), optString("id"), optString("title"), optString("detail"), optString("guests"),
    optString("artwork"), optBoolean("playable"), optInt("durationMs"), optBoolean("explicit"), optBoolean("liked"),
    optInt("trackNumber"), optInt("discNumber"), optString("artistId"), optString("albumId"), optString("albumTitle"),
    optString("releaseDate"), optString("albumType"), optString("ownerId"), toString(),
)

private fun JSONObject.items(key: String) = optJSONArray(key).objects().map { it.item() }

private fun JSONObject.list(): ListState = ListState(
    optInt("kind", -1), optInt("searchFilter"), optString("query"), optString("id"), optString("title"),
    optString("artwork"), optInt("first"), optInt("count"), optInt("total"), optLong("durationMs"), optInt("sort"),
    optBoolean("descending"), optString("filter"), optBoolean("loaded"), optBoolean("more"), optBoolean("loading"),
    optInt("revision"), optBoolean("arrangeable"), items("rows"),
)

private fun JSONObject.page() = Page(optInt("total"), optBoolean("loaded"), items("items"))

private fun JSONObject.playback(now: Long) = PlaybackInfo(
    optBoolean("playing"), optBoolean("shuffle"), optInt("repeat"), optString("uri"), optString("contextUri"),
    optString("title"), optString("artist"), optString("artistId"), optString("album"), optString("albumId"),
    optString("artwork"), optString("device"), optInt("positionMs"), optInt("durationMs"), optBoolean("hasDevice"),
    optBoolean("canSeek"), optBoolean("canPrevious", true), optBoolean("canNext", true), optBoolean("liked"),
    optInt("revision"), now, optString("deviceId"), optInt("volume", -1), optBoolean("canVolume"),
    optBoolean("changingTrack"),
)

private fun JSONObject.devices(): DevicesInfo {
    val engine = optString("engineId")
    return DevicesInfo(engine, optBoolean("remoteSelected"), optJSONArray("items").objects().map {
        DeviceInfo(it.optString("id"), it.optString("name"), it.optString("type"), it.optBoolean("active"),
            it.optBoolean("restricted"), it.optBoolean("canTransfer"), engine.isNotEmpty() && it.optString("id") == engine)
    })
}

internal fun parseSnapshot(json: String, now: Long = clock()): CoreState {
    val root = JSONObject(json)
    val queue = root.optJSONObject("queue") ?: JSONObject()
    val lyrics = root.optJSONObject("lyrics") ?: JSONObject()
    val library = root.optJSONArray("library").objects().map { it.page() }
    val details = root.optJSONObject("details") ?: JSONObject()
    val profile = root.optJSONObject("profile") ?: JSONObject()

    return CoreState(
        started = true,
        authenticated = root.optBoolean("authenticated"),
        connecting = root.optBoolean("connecting"),
        loadingIntro = root.optBoolean("loadingIntro"),
        status = root.optString("status"),
        session = root.optInt("session"),
        searchRadio = root.optBoolean("searchRadio"),
        profile = CoreProfile(profile.optString("id"), profile.optString("name"), profile.optString("artwork"),
            profile.optLong("followers"), profile.optInt("followingTotal"), profile.items("following"),
            profile.optBoolean("loaded"), profile.optBoolean("permissionRequired")),
        savedTracks = library.getOrElse(0) { Page() },
        savedAlbums = library.getOrElse(1) { Page() },
        playlistsPage = library.getOrElse(2) { Page() },
        recent = root.items("recent"),
        recentLoaded = root.optBoolean("recentLoaded"),
        recentPermissionRequired = root.optBoolean("recentPermissionRequired"),
        list = root.optJSONObject("list")?.list() ?: ListState(),
        playlists = root.optJSONObject("playlists")?.list() ?: ListState(),
        details = Details(
            details.items("item").firstOrNull()?.takeIf { it.id.isNotEmpty() },
            details.items("artist").firstOrNull()?.takeIf { it.id.isNotEmpty() },
            details.items("popular"), details.optString("release"), details.optString("duration"),
            details.optString("label"), details.optString("copyrights"), details.optInt("totalTracks"),
            details.optLong("followers"), details.optBoolean("followersKnown"), details.optBoolean("popularLoaded"),
            details.optBoolean("loaded"), details.optBoolean("saved"), details.optBoolean("savedKnown"),
            details.optBoolean("saving"), details.optBoolean("permissionRequired")),
        searchTop = root.items("searchTop").firstOrNull(),
        searchArtists = root.items("searchArtists"),
        searchAlbums = root.items("searchAlbums"),
        searchPlaylists = root.items("searchPlaylists"),
        history = root.items("history"),
        artistLikedCount = root.optInt("artistLikedCount"),
        artistLikesReady = root.optBoolean("artistLikesReady"),
        artistLikesFailed = root.optBoolean("artistLikesFailed"),
        playback = root.optJSONObject("playback")?.playback(now) ?: PlaybackInfo(),
        queue = QueueInfo(queue.optInt("first"), queue.optInt("total"), queue.optInt("queued"),
            queue.items("current").firstOrNull(), queue.items("items")),
        lyrics = LyricsInfo(lyrics.optInt("state"), lyrics.optString("uri"), lyrics.optInt("active", -1),
            lyrics.optString("text"), lyrics.optJSONArray("lines").objects().map { LyricLine(it.optInt("timeMs"), it.optString("text")) }),
        engine = root.optInt("engine"),
        engineMessage = root.optString("engineMessage"),
        setupRevision = root.optInt("setupRevision"),
        devices = root.optJSONObject("devices")?.devices() ?: DevicesInfo(),
    )
}

/**
 * The one process-wide owner of the shared core. Activity recreation reuses it; the UI
 * reads [state] and sends commands, and never calls Spotify itself.
 */
internal object Core {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    @OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
    private val commands = Dispatchers.Default.limitedParallelism(1)
    private val wakes = Channel<Unit>(Channel.CONFLATED)
    private val ticks = Channel<Unit>(Channel.CONFLATED)
    @Volatile private var visible = false
    @Volatile private var context: Context? = null
    private val mutableState = MutableStateFlow(CoreState())
    val state: StateFlow<CoreState> = mutableState
    @Volatile private var started = false

    @Synchronized
    fun start(context: Context) {
        if (started) {
            return
        }

        this.context = context.applicationContext
        CoreHost.initialize(context)
        CoreHost.upgradeSignIn(context)
        scope.launch {
            // Wakes arrive in bursts while pages load; one snapshot covers each burst.
            for (wake in wakes) {
                NativeCore.snapshot()?.let { bytes ->
                    runCatching { parseSnapshot(bytes.decodeToString()) }.onSuccess(::publish)
                }
            }
        }
        scope.launch {
            // The desktop event loop's job: run the core's timers (playback sync, lyric lines, device checks).
            while (true) {
                val wait = NativeCore.tick(visible || state.value.playback.playing || EngineHost.running)

                if (wait < 0) {
                    ticks.receive()
                } else {
                    withTimeoutOrNull(wait.toLong()) { ticks.receive() }
                }
            }
        }
        started = NativeCore.start(context.filesDir.resolve("core").path)
        requestSnapshot()
    }

    private fun publish(next: CoreState) {
        val before = mutableState.value
        mutableState.value = next
        val app = context ?: return

        // The setup screen follows the core's view of Soloist once it runs.
        if (next.engine != before.engine) {
            when (next.engine) {
                BuildConfig.ENGINE_READY -> Engine.sessionConfirmed(app)
                BuildConfig.ENGINE_PAIRING -> Engine.sessionMissing(app, next.engineMessage)
            }
        }
    }

    fun requestSnapshot() {
        wakes.trySend(Unit)
        ticks.trySend(Unit)
    }

    /** Whether the app is on screen; the core polls playback only while it is, or while playing. */
    fun setVisible(value: Boolean) {
        visible = value
        ticks.trySend(Unit)
    }

    /** Runs a core command off the main thread; the core wakes the UI when state changes. */
    private fun send(command: () -> Unit) {
        scope.launch(commands) {
            command()
            requestSnapshot()
        }
    }

    fun connect() = send { NativeCore.connect() }
    fun disconnect() = send { NativeCore.disconnect() }
    fun refresh() = send { NativeCore.refresh() }
    fun search(query: String, filter: Int) = send { NativeCore.search(query, filter) }
    fun browse(kind: Int) = send { NativeCore.browse(kind) }
    fun open(item: Item) = send { NativeCore.open(item.json) }
    fun toggleLike(item: Item) = send { NativeCore.toggleLike(item.json) }
    fun artistLikes(item: Item) = send { NativeCore.artistLikes(item.json) }
    fun songRadio(item: Item) = send { NativeCore.songRadio(item.json) }
    fun historyAdd(item: Item) = send { NativeCore.historyAdd(item.json) }
    fun historyRemove(item: Item) = send { NativeCore.historyRemove(item.id) }
    fun toggleSaved() = send { NativeCore.toggleSaved() }
    fun discography(group: String) = send { NativeCore.discography(group) }
    fun sort(sort: Int, descending: Boolean) = send { NativeCore.sort(sort, descending) }
    fun filter(query: String) = send { NativeCore.filter(query) }
    fun loadMore() = send { NativeCore.loadMore() }
    fun viewport(first: Int) = send { NativeCore.viewport(first) }
    fun libraryViewport(first: Int) = send { NativeCore.libraryViewport(first) }
    fun setSearchRadio(enabled: Boolean) = send { NativeCore.setSearchRadio(enabled) }
    fun play(item: Item) = send { NativeCore.play(item.json) }
    fun playHistory(item: Item) = send { NativeCore.playHistory(item.json) }
    fun playUpcoming(item: Item) = send { NativeCore.playUpcoming(item.json) }
    fun startRadio(item: Item) = send { NativeCore.startRadio(item.json) }
    fun enqueue(item: Item) = send { NativeCore.enqueue(item.json) }
    fun dequeue(item: Item) = send { NativeCore.dequeue(item.id) }
    fun playQueued(item: Item) = send { NativeCore.playQueued(item.id) }
    fun clearQueue() = send { NativeCore.clearQueue() }
    fun queueViewport(first: Int) = send { NativeCore.queueViewport(first) }
    fun queueVisible(visible: Boolean) = send { NativeCore.queueVisible(visible) }
    fun lyricsVisible(visible: Boolean) = send { NativeCore.lyricsVisible(visible) }
    fun playCollection() = send { NativeCore.playCollection() }
    fun playList() = send { NativeCore.playList() }
    fun playPopular(index: Int) = send { NativeCore.playPopular(index) }
    fun action(action: Int) = send { NativeCore.action(action) }
    fun seek(fraction: Int) = send { NativeCore.seek(fraction.coerceIn(0, 1000)) }
    fun seekLyric(uri: String, position: Int) = send { NativeCore.seekLyric(uri, position) }
    fun engineStop() = send { NativeCore.engineStop() }
    fun devicesVisible(visible: Boolean) = send { NativeCore.devicesVisible(visible) }
    fun transferTo(device: DeviceInfo) = send { NativeCore.transferTo(device.id) }
    fun transferHere() = send { NativeCore.transferHere() }
    fun volume(percent: Int) = send { NativeCore.volume(percent.coerceIn(0, 100)) }
}
