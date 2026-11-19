package app.nativespotify

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TermuxResultTest {
    @Test
    fun reportsLastSetupStageWithoutIncludingRawOutput() {
        val message = termuxResultMessage("setup", 1, true,
            "NATIVE_SPOTIFY_STAGE=repositories\nNATIVE_SPOTIFY_STAGE=download\nprivate detail")
        assertTrue(message.contains("Soloist download"))
        assertFalse(message.contains("private detail"))
    }

    @Test
    fun distinguishesPermissionDenialFromAnAudioFailure() {
        val error = "NATIVE_SPOTIFY_STAGE=audio-output"
        assertTrue(termuxResultMessage("start", 5, false, error).contains("permission"))
        assertTrue(termuxResultMessage("start", 5, true, error).contains("audio output"))
    }
}
