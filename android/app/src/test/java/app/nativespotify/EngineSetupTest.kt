package app.nativespotify

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class EngineSetupTest {
    private fun probe(install: String = "ready", session: String = "none", active: String = "none") =
        EngineProbe(install, session, active)

    @Test
    fun parsesOnlyTheFixedStatusLines() {
        val output = "noise\nNATIVE_SPOTIFY_INSTALL=ready\nNATIVE_SPOTIFY_SESSION=stored\nNATIVE_SPOTIFY_ACTIVE=pair\n"
        assertEquals(probe(session = "stored", active = "pair"), parseEngineProbe(output))
    }

    @Test
    fun anOlderScriptWithoutStatusIsNotAProbe() {
        assertNull(parseEngineProbe("NATIVE_SPOTIFY_STAGE=repositories\n"))
    }

    @Test
    fun installComesBeforeTheKey() {
        assertEquals(SetupDecision.Install, nextSetupDecision(probe(install = "engine"), false, false))
        assertEquals(SetupDecision.Key, nextSetupDecision(probe(), false, false))
    }

    @Test
    fun startsPairingOnlyWhenNoSessionAndNothingIsRunning() {
        assertEquals(SetupDecision.StartPairing, nextSetupDecision(probe(), true, false))
        assertEquals(SetupDecision.AwaitPairing, nextSetupDecision(probe(active = "pair"), true, false))
        assertEquals(SetupDecision.Ready, nextSetupDecision(probe(active = "start"), true, false))
    }

    @Test
    fun aStoredSessionNeedsNoPairingUntilSoloistRejectsIt() {
        assertEquals(SetupDecision.Ready, nextSetupDecision(probe(session = "stored"), true, false))
        assertEquals(SetupDecision.StartPairing, nextSetupDecision(probe(session = "stored"), true, true))
    }
}
