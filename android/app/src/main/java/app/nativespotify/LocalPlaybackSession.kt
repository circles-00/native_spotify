package app.nativespotify

import android.content.Context
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.withContext
import org.json.JSONObject

internal class PlaybackGrant(val bearer: String, val expiresAtMs: Long)
internal class PlaybackGrantRejected : Exception()

internal class LocalPlaybackSession(
    context: Context,
    private val source: AuthorizedPlaybackSource,
    private val scope: CoroutineScope,
    private val secrets: Secrets,
    private val account: String,
) {
    private val appContext = context.applicationContext
    private var inFlight: Deferred<PlaybackGrant>? = null
    private var signingIn = false

    suspend fun signIn(context: Context): PlaybackGrant = withContext(Dispatchers.Main.immediate) {
        if (signingIn) {
            throw LocalPlaybackUnavailable("Playback sign-in already in progress")
        }

        signingIn = true
        try {
            inFlight?.cancelAndJoin()
            source.signIn(context).also { save(it) }
        } finally {
            signingIn = false
        }
    }

    suspend fun refresh(rejected: PlaybackGrant? = null): PlaybackGrant = withContext(Dispatchers.Main.immediate) {
        if (signingIn) {
            throw LocalPlaybackUnavailable("Playback sign-in in progress")
        }

        val work = inFlight?.takeIf { it.isActive } ?: scope.async(start = CoroutineStart.LAZY) {
            val saved = withContext(Dispatchers.IO) {
                secrets.get(account)?.let { encoded ->
                    val json = JSONObject(encoded)
                    PlaybackGrant(json.getString("bearer"), json.getLong("expiresAtMs"))
                }
            }
            val invalid = rejected != null && saved?.bearer == rejected.bearer &&
                saved.expiresAtMs == rejected.expiresAtMs

            if (saved != null && !invalid && saved.expiresAtMs > System.currentTimeMillis() + 60_000) {
                return@async saved
            }

            if (invalid) {
                withContext(Dispatchers.IO) { secrets.put(account, null, durable = true) }
            }

            source.refresh(appContext, saved.takeUnless { invalid }).also { save(it) }
        }.also {
            inFlight = it
            it.start()
        }

        work.await()
    }

    private suspend fun save(grant: PlaybackGrant) = withContext(Dispatchers.IO) {
        val encoded = JSONObject().put("bearer", grant.bearer).put("expiresAtMs", grant.expiresAtMs)
        secrets.put(account, encoded.toString(), durable = true)
    }
}
