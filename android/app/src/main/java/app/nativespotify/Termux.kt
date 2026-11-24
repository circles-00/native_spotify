package app.nativespotify

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.flow.MutableStateFlow

internal object Termux {
    const val permission = "com.termux.permission.RUN_COMMAND"
    private const val directory = "\$HOME/.local/share/native-spotify-android"
    private val sequence = AtomicInteger()
    /** The latest engine message for the UI; empty until something happens. */
    val status = MutableStateFlow("")

    /** Writes the bundled scripts; a rename keeps any running session on its own copy. */
    private fun installScripts(context: Context): String {
        val files = listOf("audio.sh", "player.sh").joinToString("\n") { name ->
            val body = context.assets.open(name).bufferedReader().use { it.readText() }
            val target = "$directory/$name"
            "tmp=\$(mktemp \"$target.XXXXXX\")\ncat > \"\$tmp\" <<'NATIVE_SPOTIFY_SCRIPT'\n$body\nNATIVE_SPOTIFY_SCRIPT\nmv -f \"\$tmp\" \"$target\""
        }
        return "umask 077\nmkdir -p \"$directory\"\n$files"
    }

    fun setupScript(context: Context): String =
        installScripts(context) + "\nbash \"$directory/player.sh\" setup"

    /** Returns why this app cannot send commands to Termux, or null when it can. */
    fun problem(context: Context): String? {
        val packages = context.packageManager
        if (runCatching { packages.getPackageInfo("com.termux", 0) }.isFailure) {
            return "Termux is not installed. Install it from F-Droid or GitHub, not Google Play."
        }

        // Android only defines the permission if Termux was installed before this app.
        if (runCatching { packages.getPermissionInfo(permission, 0) }.isFailure) {
            return "Android doesn't know Termux's command permission. Uninstall and reinstall Native Spotify, " +
                "then grant the permission. Your Termux setup is kept."
        }

        if (context.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
            return "Grant Run commands in Termux first."
        }

        return null
    }

    fun run(context: Context, action: String, key: String = "", engineId: Int = 0, port: Int = 0): Boolean {
        require(action in setOf("check", "setup", "status", "pair", "start", "stop", "keepalive"))
        val quiet = action == "keepalive" || action == "stop"
        problem(context)?.let {
            if (!quiet) status.value = it
            return false
        }

        val command = when (action) {
            "check" -> "printf '%s\\n' 'Native Spotify can run commands in Termux.'"
            "setup" -> "set -e\n" + setupScript(context)
            "status" -> "set -e\n" + installScripts(context) + "\nexec bash \"$directory/player.sh\" status"
            "start" -> "set -e\n" + installScripts(context) + "\nexec bash \"$directory/player.sh\" start $port"
            else -> "exec bash \"$directory/player.sh\" $action"
        }

        // Setup and the check run in a visible Termux session so progress and errors can be read there.
        val visible = action == "setup" || action == "check"
        val intent = Intent("com.termux.RUN_COMMAND")
            .setClassName("com.termux", "com.termux.app.RunCommandService")
            .putExtra("com.termux.RUN_COMMAND_PATH", "\$PREFIX/bin/bash")
            .putExtra("com.termux.RUN_COMMAND_ARGUMENTS", arrayOf("-c", command))
            .putExtra("com.termux.RUN_COMMAND_BACKGROUND", !visible)
            .putExtra("com.termux.RUN_COMMAND_SESSION_ACTION", "0")
            .putExtra("com.termux.RUN_COMMAND_COMMAND_LABEL", "Native Spotify $action")
        if (key.isNotEmpty()) intent.putExtra("com.termux.RUN_COMMAND_STDIN", "$key\n")
        if (!quiet) {
            val callback = Intent(context, TermuxResult::class.java).putExtra("operation", action)
                .putExtra("engineId", engineId)
            intent.putExtra("com.termux.RUN_COMMAND_PENDING_INTENT", PendingIntent.getBroadcast(
                context, sequence.incrementAndGet(), callback,
                PendingIntent.FLAG_ONE_SHOT or PendingIntent.FLAG_MUTABLE
            ))
        }

        val request = runCatching { context.startService(intent) }
        if (request.getOrNull() != null) return true
        if (!quiet) {
            status.value = when (request.exceptionOrNull()) {
                is SecurityException -> "Android denied Termux command access. Grant Run commands in Termux in Settings."
                is IllegalStateException -> "Android blocked the Termux launch. Open Termux, then retry while this app is visible."
                else -> "Termux command service unavailable. Install and open the F-Droid or GitHub version of Termux."
            }
        }
        return false
    }
}

class TermuxResult : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.getStringExtra("operation") ?: return
        if (action == "stop") return
        val engineId = intent.getIntExtra("engineId", 0)
        if (action == "start" && !EngineHost.isCurrent(engineId)) return
        val result = intent.getBundleExtra("result")
        if (result == null) {
            val message = "Termux returned no result for $action. Open Termux and check its setup."
            Termux.status.value = message
            Engine.failed(message)
            if (action == "start") EngineHost.exited(engineId, -1)
            return
        }

        val exit = result.getInt("exitCode", -1)
        val allowed = result.getInt("err", Activity.RESULT_OK) == Activity.RESULT_OK
        // Visible sessions return their combined transcript as stdout; background commands use stderr.
        val output = result.getString("stdout").orEmpty() + "\n" + result.getString("stderr").orEmpty()

        if (action == "status") {
            Engine.acceptStatus(context, exit, allowed, output)
            return
        }

        // Setup output holds no secrets; keep it where adb can read it when Termux closes the window.
        if (action == "setup" && exit != 0) {
            output.lines().takeLast(60).forEach { android.util.Log.w("NativeSetup", it) }
        }

        val message = termuxResultMessage(action, exit, allowed, output)
        Termux.status.value = message
        Engine.acceptResult(context, action, exit, allowed, message)
        // The core's engine adapter decides whether to start Soloist again.
        if (action == "start") EngineHost.exited(engineId, if (allowed) exit else -1)
    }
}

internal fun termuxResultMessage(action: String, exit: Int, allowed: Boolean, stderr: String): String {
    if (!allowed) return "Termux rejected $action. Open Termux, enable allow-external-apps, and check Run commands in Termux permission."
    if (exit == 0) return when (action) {
        "check" -> "Termux connection works. Tap Install or update engine next."
        "setup" -> "Engine installed. Pair if needed, then start playback."
        "pair" -> "Soloist paired. You can close Spotify and play here."
        else -> "Playback engine stopped."
    }

    // Only display fixed stage names, never raw command output or credentials.
    val stage = stderr.lineSequence().filter { it.startsWith("NATIVE_SPOTIFY_STAGE=") }
        .lastOrNull()?.substringAfter('=')
    val reason = when {
        exit == 127 -> "A required command is missing. Run Install or update engine."
        exit == 4 && stage in setOf("audio-output", "audio-package") ->
            "The PulseAudio package is too old. Update it to 17.0-4 or newer."
        exit == 7 && stage == "audio-output" -> "The local audio bridge port is in use. Stop the manual bridge and retry."
        exit == 7 -> "A playback or pairing session is already running."
        exit == 124 && stage == "pairing" -> "Pairing timed out. Try again, then choose Native Spotify Phone in Spotify."
        exit == 10 && stage in setOf("launch", "pairing", "binary") ->
            "The Soloist build expired. Run Install or update engine."
        stage == "repositories" -> "Termux package update or installation failed."
        stage == "audio-package" -> "PulseAudio package setup failed."
        stage == "debian" -> "Termux could not open or install the Debian guest."
        stage == "debian-packages" -> "Debian dependency installation failed."
        stage == "download" -> "The Soloist download or archive extraction failed."
        stage == "binary" -> "The Soloist binary could not run in Debian."
        stage == "native-libraries" -> "Collecting Soloist's libraries from Debian failed."
        stage == "native-engine" -> "Soloist could not run on Termux's glibc. Reinstall the engine."
        stage == "audio-output" -> "Neither Android audio output started."
        stage == "launch" -> "Soloist or its audio bridge exited during startup or playback."
        stage == "pairing" -> "Soloist pairing failed. Check your saved key and try pairing again."
        else -> "Could not complete $action. Check Termux setup and the saved Soloist key."
    }
    return "$reason (exit $exit)"
}
