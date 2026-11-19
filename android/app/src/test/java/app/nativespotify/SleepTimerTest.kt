package app.nativespotify

import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.yield
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SleepTimerTest {
    @Test
    fun validatesHoursAndMinutes() {
        assertEquals(5_400_000L, sleepTimerDuration(1, 30))
        assertNull(sleepTimerDuration(0, 0))
        assertNull(sleepTimerDuration(0, 60))
        assertNull(sleepTimerDuration(-1, 30))
    }

    @Test
    fun replacementStartsANewCountdownAndPausesOnce() = runBlocking {
        withTimeout(2_000) {
            var now = 100L
            val waits = Channel<Long>()
            val advance = Channel<Unit>()
            var pauses = 0
            val timer = SleepTimer(this, now = { now }, wait = {
                waits.send(it)
                advance.receive()
            }) { ++pauses }

            assertTrue(timer.start(0, 1))
            assertEquals(60_000L, waits.receive())
            now += 30_000
            assertTrue(timer.start(0, 2))
            assertFalse(timer.start(0, 0))
            assertEquals(120_000L, waits.receive())
            assertEquals(150_100L, timer.deadline.value)
            now += 120_000
            advance.send(Unit)
            yield()
            assertEquals(1, pauses)
            assertNull(timer.deadline.value)

            assertTrue(timer.start(0, 1))
            assertEquals(60_000L, waits.receive())
            timer.cancel()
            assertNull(timer.deadline.value)
            assertEquals(1, pauses)
        }
    }
}
