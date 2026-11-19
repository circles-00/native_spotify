package app.nativespotify

import android.os.SystemClock
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

internal class SleepTimer(
    private val scope: CoroutineScope,
    private val now: () -> Long = SystemClock::elapsedRealtime,
    private val wait: suspend (Long) -> Unit = { delay(it) },
    private val pause: () -> Unit,
) {
    private val activeDeadline = MutableStateFlow<Long?>(null)
    val deadline = activeDeadline.asStateFlow()
    private var countdown: Job? = null
    private var generation = 0L

    @Synchronized
    fun start(hours: Int, minutes: Int): Boolean {
        val duration = sleepTimerDuration(hours, minutes) ?: return false
        cancel()
        val ticket = generation
        val deadlineMs = now() + duration
        activeDeadline.value = deadlineMs
        countdown = scope.launch {
            var remaining = deadlineMs - now()
            while (remaining > 0) {
                wait(remaining)
                remaining = deadlineMs - now()
            }

            synchronized(this@SleepTimer) {
                if (generation == ticket) {
                    activeDeadline.value = null
                    countdown = null
                    pause()
                }
            }
        }
        return true
    }

    @Synchronized
    fun cancel() {
        ++generation
        countdown?.cancel()
        countdown = null
        activeDeadline.value = null
    }
}

internal fun sleepTimerDuration(hours: Int, minutes: Int): Long? =
    if (hours in 0..23 && minutes in 0..59 && hours + minutes > 0) (hours * 60L + minutes) * 60_000 else null

@Composable
internal fun SleepTimerDialog(timer: SleepTimer, dismiss: () -> Unit) {
    val deadline by timer.deadline.collectAsState()
    val seconds by produceState(0L, deadline) {
        val end = deadline ?: return@produceState
        do {
            value = ((end - SystemClock.elapsedRealtime() + 999) / 1000).coerceAtLeast(0)
            if (value == 0L) break
            delay(1000)
        } while (true)
    }
    var hours by rememberSaveable { mutableStateOf("0") }
    var minutes by rememberSaveable { mutableStateOf("30") }
    val hourCount = hours.toIntOrNull() ?: 0
    val minuteCount = minutes.toIntOrNull() ?: 0

    AlertDialog(
        onDismissRequest = dismiss,
        title = { Text("Sleep timer") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                if (deadline != null) {
                    val clock = "%d:%02d:%02d".format(seconds / 3600, seconds / 60 % 60, seconds % 60)
                    Text("Playback pauses in $clock")
                } else {
                    Text("Pause playback after:")
                }
                val keyboard = KeyboardOptions(keyboardType = KeyboardType.Number)
                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                    OutlinedTextField(hours, { hours = it.filter(Char::isDigit).take(2) }, Modifier.weight(1f),
                        label = { Text("Hours") }, singleLine = true, keyboardOptions = keyboard, isError = hourCount > 23)
                    OutlinedTextField(minutes, { minutes = it.filter(Char::isDigit).take(2) }, Modifier.weight(1f),
                        label = { Text("Minutes") }, singleLine = true, keyboardOptions = keyboard, isError = minuteCount > 59)
                }
            }
        },
        confirmButton = {
            TextButton(enabled = sleepTimerDuration(hourCount, minuteCount) != null, onClick = {
                if (timer.start(hourCount, minuteCount)) dismiss()
            }) { Text(if (deadline == null) "Start timer" else "Replace timer") }
        },
        dismissButton = {
            TextButton(onClick = {
                if (deadline != null) timer.cancel()
                dismiss()
            }) { Text(if (deadline == null) "Close" else "Cancel timer") }
        },
    )
}
