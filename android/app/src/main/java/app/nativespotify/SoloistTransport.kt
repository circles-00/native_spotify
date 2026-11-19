package app.nativespotify

import java.util.concurrent.CompletableFuture
import java.util.concurrent.TimeUnit
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import org.json.JSONObject

internal class SoloistTransport(port: Int) {
    companion object {
        private val client = OkHttpClient.Builder().followRedirects(false)
            .pingInterval(15, TimeUnit.SECONDS).build()
        private val commands = mapOf("play" to "play", "pause" to "pause", "next" to "skip_next",
            "previous" to "skip_prev", "seek" to "seek", "volume" to "set_volume", "shuffle" to "set_shuffle",
            "repeat_track" to "set_repeat_track", "repeat_context" to "set_repeat_context", "enqueue" to "add_to_queue")
        private const val UNAVAILABLE = 0
        private const val ACCEPTED = 1
        private const val UNCONFIRMED = 2
        private val stateEvents = setOf("auth_state", "device_changed", "track_changed", "playback_changed",
            "context_changed", "options_changed", "volume_changed", "position_sync", "playback_state")
    }

    private val request = Request.Builder().url("ws://127.0.0.1:$port/").build()
    private var socket: WebSocket? = null
    private var loggedIn = false
    private var active = false
    private val commandLock = Any()
    private var pending: Pair<String, CompletableFuture<Int>>? = null
    private val playbackState = SoloistPlaybackState()

    private val listener = object : WebSocketListener() {
        override fun onMessage(webSocket: WebSocket, text: String) {
            val event = runCatching { JSONObject(text) }.getOrNull() ?: return
            synchronized(this@SoloistTransport) {
                if (socket !== webSocket) return
                val wasActive = active && loggedIn
                when (event.optString("type")) {
                    "auth_state" -> {
                        loggedIn = event.optBoolean("logged_in")
                        active = event.optBoolean("is_active")
                    }
                    "device_changed" -> active = event.optBoolean("is_active", active)
                    "playback_state" -> {
                        active = event.optBoolean("is_active", active)
                    }
                    "command_result" -> pending?.let { (name, result) ->
                        if (name == event.optString("command")) {
                            result.complete(if (event.optBoolean("success", true)) ACCEPTED else UNAVAILABLE)
                        }
                    }
                    "error" -> pending?.second?.complete(UNAVAILABLE)
                }

                if (active && loggedIn) {
                    playbackState.accept(event)
                    if (!wasActive && event.optString("type") == "device_changed") {
                        webSocket.send("""{"type":"command","command":"get_state"}""")
                    }
                } else {
                    playbackState.clear()
                }
            }

            if (event.optString("type") in stateEvents) NativeCore.enginePlaybackChanged()
        }

        override fun onClosing(webSocket: WebSocket, code: Int, reason: String) = disconnected(webSocket)
        override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) = disconnected(webSocket)
    }

    @Synchronized
    fun connect() {
        if (socket == null) socket = client.newWebSocket(request, listener)
    }

    @Synchronized
    fun close() {
        val old = socket
        socket = null
        active = false
        loggedIn = false
        pending?.second?.complete(UNCONFIRMED)
        pending = null
        playbackState.clear()
        old?.cancel()
    }

    private fun disconnected(webSocket: WebSocket) {
        synchronized(this) {
            if (socket !== webSocket) return
            close()
        }
        NativeCore.enginePlaybackChanged()
    }

    @Synchronized
    fun playback(device: String): ByteArray? {
        return playbackState.snapshot(device)?.toString()?.toByteArray()
    }

    fun command(action: String, value: Int): Int = synchronized(commandLock) { dispatch(action, value) }

    private fun dispatch(action: String, value: Int): Int {
        val command = action.substringBefore(':')
        val name = commands[command] ?: return UNAVAILABLE
        val uri = action.substringAfter(':', "")
        val packet = JSONObject().put("type", "command").put("command", name)
        when (command) {
            "play", "enqueue" -> if (uri.isNotEmpty()) packet.put("uri", uri)
            "seek" -> packet.put("position_ms", value)
            "volume" -> packet.put("volume", value)
            "shuffle", "repeat_track", "repeat_context" -> packet.put("enabled", value != 0)
        }
        val result = CompletableFuture<Int>()
        synchronized(this) {
            val connection = socket ?: return UNAVAILABLE
            if (!loggedIn || (!active && command != "play")) return UNAVAILABLE
            if (command == "seek" && !playbackState.matchesTrack(uri)) return UNAVAILABLE
            pending = name to result
            if (!connection.send(packet.toString())) {
                close()
                return UNAVAILABLE
            }
        }

        val outcome = runCatching { result.get(1200, TimeUnit.MILLISECONDS) }.getOrDefault(UNCONFIRMED)
        synchronized(this) {
            if (pending?.second === result) {
                pending = null
                if (outcome == UNCONFIRMED) close()
            }
        }
        return outcome
    }
}

internal class SoloistPlaybackState {
    private var state: JSONObject? = null
    private var lastAnchorMs = 0L

    fun clear() {
        state = null
        lastAnchorMs = 0L
    }

    fun matchesTrack(uri: String): Boolean = uri.isNotEmpty() && state?.optJSONObject("item")?.optString("uri") == uri

    fun accept(event: JSONObject, now: Long = System.currentTimeMillis()) {
        val type = event.optString("type")
        if (type == "playback_state") {
            val previous = state
            val anchor = event.optJSONObject("position")
            val stamp = anchor?.optLong("timestamp_ms") ?: 0L
            if (stamp != 0L && stamp < lastAnchorMs) {
                event.remove("position")
                if (matchesTrack(event.optJSONObject("item")?.optString("uri").orEmpty())) {
                    event.put("position", previous?.optJSONObject("position"))
                }
            }
            lastAnchorMs = maxOf(lastAnchorMs, stamp)
            state = event
            return
        }

        val current = state ?: return
        when (type) {
            "track_changed" -> {
                val item = event.optJSONObject("item")
                if (!matchesTrack(item?.optString("uri").orEmpty())) {
                    current.remove("position")
                    current.remove("available_actions")
                }
                current.put("item", item)
            }
            "playback_changed" -> {
                val status = event.optString("status")
                val anchor = current.optJSONObject("position")
                if (status != "playing" && current.optString("status") == "playing" && anchor != null) {
                    anchor.put("position_ms", soloistPosition(anchor, now, true)).put("speed", 0.0)
                }
                current.put("status", status)
            }
            "context_changed" -> current.put("context", event.opt("context"))
            "options_changed" -> current.put("options", event.opt("options"))
            "volume_changed" -> current.put("volume", event.opt("volume"))
            "position_sync" -> {
                val position = event.optJSONObject("position") ?: return
                val stamp = position.optLong("timestamp_ms")
                if (stamp == 0L || stamp >= lastAnchorMs) {
                    current.put("position", position)
                    lastAnchorMs = maxOf(lastAnchorMs, stamp)
                }
            }
        }
    }

    fun snapshot(device: String, now: Long = System.currentTimeMillis()): JSONObject? =
        state?.let { soloistPlaybackJson(it, device, now) }
}

private fun soloistPosition(anchor: JSONObject, now: Long, playing: Boolean): Long {
    val timestamp = anchor.optLong("timestamp_ms")
    val elapsed = if (timestamp > 0 && playing) (now - timestamp).coerceAtLeast(0) else 0
    return anchor.optLong("position_ms") + (elapsed * anchor.optDouble("speed", 1.0)).toLong()
}

/** Normalize the local protocol at the Android boundary, for the shared playback parser. */
internal fun soloistPlaybackJson(state: JSONObject, device: String, now: Long = System.currentTimeMillis()): JSONObject? {
    val status = state.optString("status")
    if (status !in setOf("playing", "paused", "buffering", "idle")) return null
    val item = state.optJSONObject("item")
    val details = item?.optJSONObject("decorations")
    val album = details?.optJSONObject("parent")?.optJSONObject("entity")
    val artist = details?.optJSONArray("creators")?.optJSONObject(0)?.optJSONObject("entity")
    val covers = details?.optJSONObject("visual_identity")?.optJSONArray("cover")
    val actions = state.optJSONObject("available_actions")
    val options = state.optJSONObject("options")
    val anchor = state.optJSONObject("position")
    val playing = status == "playing" && anchor != null && anchor.optDouble("speed", 1.0) > 0.0
    val duration = details?.optJSONObject("playback")?.optInt("duration_ms") ?: 0
    val progress = anchor?.let {
        soloistPosition(it, now, playing)
            .coerceIn(0, if (duration > 0) duration.toLong() else Int.MAX_VALUE.toLong()).toInt()
    }
    val type = item?.optString("entity_type").orEmpty()
    val media = JSONObject().put("uri", item?.optString("uri").orEmpty()).put("type", type)
        .put("name", details?.optJSONObject("identity")?.optString("name").orEmpty())
        .put("duration_ms", duration)
        .put("images", covers)
        .put("album", JSONObject().put("id", album?.optString("uri")?.substringAfterLast(':'))
            .put("name", album?.optJSONObject("decorations")?.optJSONObject("identity")?.optString("name"))
            .put("images", covers))
        .put("artists", org.json.JSONArray().put(JSONObject()
            .put("id", artist?.optString("uri")?.substringAfterLast(':'))
            .put("name", artist?.optJSONObject("decorations")?.optJSONObject("identity")?.optString("name"))))
    return JSONObject().put("is_playing", playing).put("item", media)
        .put("context", state.optJSONObject("context"))
        .put("progress_ms", progress)
        .put("shuffle_state", options?.optBoolean("shuffle") ?: false)
        .put("repeat_state", options?.optString("repeat") ?: "off")
        .put("device", JSONObject().put("id", device).put("name", "Native Spotify Phone")
            .put("is_active", true).put("is_restricted", false).put("supports_volume", state.has("volume"))
            .put("volume_percent", state.opt("volume")))
        .put("actions", JSONObject().put("disallows", JSONObject()
            .put("seeking", actions?.has("seek") == false)
            .put("skipping_next", actions?.has("skip_next") == false)
            .put("skipping_prev", actions?.has("skip_prev") == false)))
}
