package app.nativespotify

import android.content.Context
import android.os.Handler
import android.os.Looper
import kotlinx.coroutines.flow.MutableStateFlow

/** Where first-time playback setup stands. Mirrors the desktop engine states, plus Termux's own steps. */
internal enum class SetupStep { Checking, Termux, Install, Installing, Key, Pairing, Ready, Failed }

internal data class EngineSetup(val step: SetupStep = SetupStep.Checking, val message: String = "",
                                val error: Boolean = false)

/** The fixed lines `player.sh status` prints. */
internal data class EngineProbe(val install: String, val session: String, val active: String)

internal enum class SetupDecision { Install, Key, AwaitPairing, StartPairing, Ready }

internal fun parseEngineProbe(output: String): EngineProbe? {
    val values = output.lineSequence()
        .filter { it.startsWith("NATIVE_SPOTIFY_") && '=' in it }
        .associate { it.substringBefore('=') to it.substringAfter('=').trim() }

    val install = values["NATIVE_SPOTIFY_INSTALL"] ?: return null
    val session = values["NATIVE_SPOTIFY_SESSION"] ?: return null
    val active = values["NATIVE_SPOTIFY_ACTIVE"] ?: return null
    return EngineProbe(install, session, active)
}

/**
 * Decides the next setup step. A running pairing is never started twice, and a stored session
 * only counts until the engine has reported that it could not use it.
 */
internal fun nextSetupDecision(probe: EngineProbe, hasKey: Boolean, sessionRejected: Boolean): SetupDecision {
    if (probe.install != "ready") {
        return SetupDecision.Install
    }

    if (!hasKey) {
        return SetupDecision.Key
    }

    if (probe.active == "pair") {
        return SetupDecision.AwaitPairing
    }

    // A playback session is already running with a session it accepted.
    if (probe.active != "none") {
        return SetupDecision.Ready
    }

    if (probe.session == "stored" && !sessionRejected) {
        return SetupDecision.Ready
    }

    return SetupDecision.StartPairing
}

internal fun installMessage(install: String): String = when (install) {
    "packages" -> "Termux needs a few packages for the playback engine. Install the engine to add them."
    "audio" -> "Termux's audio package is missing or too old. Install the engine to update it."
    else -> "Install Spotify Soloist, Spotify's official background player, inside Termux. It downloads from Spotify."
}

internal const val PAIRING_MESSAGE =
    "Open Spotify on this phone, tap the devices icon, and choose “Native Spotify Phone”. You only do this once."

internal object Engine {
    suspend fun signInForLocalPlayback(context: Context): PlaybackGrant =
        PlaybackService.localSession().signIn(context)

    val state = MutableStateFlow(EngineSetup())
    private val main = Handler(Looper.getMainLooper())
    private var generation = 0

    private fun prefs(context: Context) =
        context.getSharedPreferences("engine_setup", Context.MODE_PRIVATE)

    fun hasKey(context: Context) = !Secrets(context).get("soloist").isNullOrBlank()

    /** Checks Termux access locally, then asks Termux what is installed and running. */
    fun refresh(context: Context) {
        val app = context.applicationContext
        val problem = Termux.problem(app)

        if (problem != null) {
            state.value = EngineSetup(SetupStep.Termux, problem)
            return
        }

        state.value = EngineSetup(SetupStep.Checking, "Checking the playback engine…")

        if (!Termux.run(app, "status")) {
            state.value = EngineSetup(SetupStep.Termux, Termux.status.value, error = true)
            return
        }

        // Termux answers through a broadcast; if it never does, it is usually not allowed to.
        val expected = ++generation
        main.postDelayed({
            if (expected == generation && state.value.step == SetupStep.Checking) {
                state.value = EngineSetup(SetupStep.Termux,
                    "Termux didn't answer. Open Termux once, check allow-external-apps, then try again.", error = true)
            }
        }, 20_000)
    }

    fun acceptStatus(context: Context, exit: Int, allowed: Boolean, output: String) {
        ++generation

        if (!allowed) {
            state.value = EngineSetup(SetupStep.Termux,
                "Termux rejected the request. Add allow-external-apps = true to ~/.termux/termux.properties.",
                error = true)
            return
        }

        val probe = parseEngineProbe(output)

        // A failed check is not proof that nothing is installed; let the user retry or reinstall.
        if (exit != 0 || probe == null) {
            state.value = EngineSetup(SetupStep.Failed,
                "Couldn't check the playback engine in Termux (exit $exit). Try again, or reinstall the engine.",
                error = true)
            return
        }

        val decision = nextSetupDecision(probe, hasKey(context),
            prefs(context).getBoolean("session_rejected", false))

        when (decision) {
            SetupDecision.Install -> state.value = EngineSetup(SetupStep.Install, installMessage(probe.install))
            SetupDecision.Key -> state.value = EngineSetup(SetupStep.Key)
            SetupDecision.AwaitPairing -> state.value = EngineSetup(SetupStep.Pairing, PAIRING_MESSAGE)
            SetupDecision.StartPairing -> pair(context)
            SetupDecision.Ready -> state.value = EngineSetup(SetupStep.Ready)
        }
    }

    fun install(context: Context) {
        if (Termux.run(context, "setup")) {
            state.value = EngineSetup(SetupStep.Installing,
                "Installing in Termux. Follow progress there; it can take several minutes.")
            return
        }

        state.value = EngineSetup(SetupStep.Install, Termux.status.value, error = true)
    }

    fun pair(context: Context) {
        val key = Secrets(context).get("soloist")

        if (key.isNullOrBlank()) {
            state.value = EngineSetup(SetupStep.Key)
            return
        }

        if (Termux.run(context, "pair", key)) {
            state.value = EngineSetup(SetupStep.Pairing, PAIRING_MESSAGE)
            return
        }

        state.value = EngineSetup(SetupStep.Failed, Termux.status.value, error = true)
    }

    fun acceptResult(context: Context, action: String, exit: Int, allowed: Boolean, message: String) {
        val ok = allowed && exit == 0

        when (action) {
            "setup" -> {
                if (ok) {
                    refresh(context)
                    return
                }

                state.value = EngineSetup(SetupStep.Install, message, error = true)
            }
            "pair" -> {
                if (ok) {
                    sessionConfirmed(context)
                    return
                }

                // Another pairing or playback session holds the lock; it keeps the current state.
                if (allowed && exit == 7) {
                    return
                }

                state.value = EngineSetup(SetupStep.Failed, message, error = true)
            }
        }
    }

    /** Soloist reported a logged-in session, either after pairing or when playback started. */
    fun sessionConfirmed(context: Context) {
        prefs(context).edit().putBoolean("session_rejected", false).apply()

        if (state.value.step != SetupStep.Ready) {
            state.value = EngineSetup(SetupStep.Ready)
        }
    }

    /**
     * Soloist is running but has no usable session. While it runs it accepts a Spotify Connect
     * pick itself, so it stays the pairing session instead of starting another one.
     */
    fun sessionMissing(context: Context, message: String = PAIRING_MESSAGE) {
        prefs(context).edit().putBoolean("session_rejected", true).apply()
        state.value = EngineSetup(SetupStep.Pairing, message.ifBlank { PAIRING_MESSAGE })
    }

    fun failed(message: String) {
        state.value = EngineSetup(SetupStep.Failed, message, error = true)
    }

    fun saveKey(context: Context, text: String): Boolean {
        val key = text.trim()

        if (!key.matches(Regex("spak_[A-Za-z0-9_-]+"))) {
            state.value = EngineSetup(SetupStep.Key,
                "The clipboard does not contain a Soloist key. Copy the key, then press Paste key again.",
                error = true)
            return false
        }

        val saved = runCatching { Secrets(context).put("soloist", key) }.isSuccess

        if (!saved) {
            state.value = EngineSetup(SetupStep.Key, "Android could not securely save the key.", error = true)
            return false
        }

        // A new key may belong to another account; prove the session again.
        prefs(context).edit().putBoolean("session_rejected", false).apply()
        refresh(context)
        return true
    }

    fun forgetKey(context: Context) {
        runCatching { Secrets(context).put("soloist", null) }
        refresh(context)
    }
}
