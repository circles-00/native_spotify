package app.nativespotify

import android.content.Context
import android.content.Intent
import android.net.Uri
import java.nio.ByteBuffer
import java.security.MessageDigest
import java.util.concurrent.TimeUnit
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody

/**
 * Android services for the C core's platform adapter (src/platform/android_system.c).
 * Each method is called from native threads; none of them touches the UI.
 */
internal object CoreHost {
    private const val BODY_LIMIT = 2L * 1024 * 1024
    private const val ENGINE_KEY_ACCOUNT = "native-spotify-soloist-key"

    // The only hosts the core needs: Spotify sign-in and Web API, and LRCLIB for lyrics.
    private val hosts = setOf("accounts.spotify.com", "api.spotify.com", "lrclib.net")

    private val client = OkHttpClient.Builder()
        .followRedirects(false).followSslRedirects(false)
        .connectTimeout(5, TimeUnit.SECONDS).callTimeout(20, TimeUnit.SECONDS)
        .build()
    // A lost response does not mean the device missed the skip.
    private val skipClient = client.newBuilder().retryOnConnectionFailure(false).build()

    @Volatile private var context: Context? = null

    fun initialize(context: Context) {
        this.context = context.applicationContext
    }

    private fun secrets() = Secrets(checkNotNull(context))

    // The core's Soloist key account is the key the setup screen saves.
    private fun account(name: String) = if (name == ENGINE_KEY_ACCOUNT) "soloist" else "core:$name"

    /**
     * Earlier builds copied over the Kotlin catalog's sign-in, which lacks the playback scopes
     * the core needs. Clear it once so the core asks for a login with the desktop's scopes.
     */
    fun upgradeSignIn(context: Context) {
        val prefs = context.getSharedPreferences("core_host", Context.MODE_PRIVATE)

        if (prefs.getInt("sign_in_version", 0) >= 1) {
            return
        }

        val secrets = Secrets(context)
        secrets.put(account(BuildConfig.SPOTIFY_CLIENT_ID), null)
        secrets.put("catalog", null)
        prefs.edit().putInt("sign_in_version", 1).apply()
    }

    /** Returns [status][retry-after seconds][body], or null when no response arrived. */
    @JvmStatic
    fun http(method: String, url: String, token: ByteArray?, body: ByteArray?, form: Boolean): ByteArray? = runCatching {
        val address = url.toHttpUrlOrNull() ?: return null

        if (address.scheme != "https" || address.host !in hosts) {
            return null
        }

        val type = (if (form) "application/x-www-form-urlencoded" else "application/json").toMediaType()
        val payload = body?.toRequestBody(type) ?: if (method == "GET" || method == "DELETE") null else ByteArray(0).toRequestBody(type)
        val request = Request.Builder().url(address).method(method, payload).header("User-Agent", "NativeController/0.1")

        if (token != null) {
            request.header("Authorization", "Bearer ${token.decodeToString()}")
        }

        val skip = method == "POST" && address.host == "api.spotify.com" &&
            (address.encodedPath == "/v1/me/player/next" || address.encodedPath == "/v1/me/player/previous")
        (if (skip) skipClient else client).newCall(request.build()).execute().use { response ->
            val source = response.body?.source()

            // Refuse oversized bodies instead of truncating JSON the core would then misparse.
            if (source != null && source.request(BODY_LIMIT + 1)) {
                return null
            }

            val data = source?.readByteArray() ?: ByteArray(0)

            val retry = response.header("Retry-After")?.toIntOrNull()?.coerceIn(0, 86_400) ?: 0
            ByteBuffer.allocate(8 + data.size).putInt(response.code).putInt(retry).put(data).array()
        }
    }.getOrNull()

    @JvmStatic
    fun openUrl(url: String): Boolean {
        val app = context ?: return false
        val uri = Uri.parse(url)

        if (uri.scheme != "https") {
            return false
        }

        return runCatching {
            app.startActivity(Intent(Intent.ACTION_VIEW, uri).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        }.isSuccess
    }

    @JvmStatic
    fun secretLoad(name: String): ByteArray? = secrets().get(account(name))?.toByteArray()

    @JvmStatic
    fun secretSave(name: String, secret: ByteArray): Boolean =
        runCatching { secrets().put(account(name), secret.decodeToString()) }.isSuccess

    @JvmStatic
    fun secretDelete(name: String): Boolean = runCatching { secrets().put(account(name), null) }.isSuccess

    @JvmStatic
    fun sha256(text: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(text)

    @JvmStatic
    fun wake() {
        Core.requestSnapshot()
    }

    @JvmStatic
    fun dispatchLocalPlayback(action: Int, uri: String?, generation: Long, requestId: Long, positionMs: Long): Boolean {
        val kind = LocalPlaybackAction.entries.firstOrNull { it.wireValue == action } ?: return false

        if (generation <= 0 || requestId <= 0 || positionMs < 0 || (kind == LocalPlaybackAction.PLAY && uri.isNullOrBlank())) {
            return false
        }

        return try {
            PlaybackService.dispatchLocalPlayback(
                LocalPlaybackCommand(kind, uri?.let(Uri::parse), generation, requestId, positionMs),
            )
        } catch (_: LocalPlaybackUnavailable) {
            false
        }
    }

    fun reportLocalPlaybackEvent(event: LocalPlaybackEvent): Boolean = NativeCore.localPlaybackEvent(
        event.generation, event.requestId, event.state.wireValue, event.positionMs, event.durationMs,
    )

    /** Starts the Termux session that runs Soloist; the playback service reports when it ends. */
    @JvmStatic
    fun engineStart(id: Int, key: ByteArray, port: Int): Boolean {
        val app = context ?: return false
        return EngineHost.start(app, id, key.decodeToString(), port)
    }

    @JvmStatic
    fun engineStop(id: Int) {
        context?.let { EngineHost.stop(it, id) }
    }
}
