package app.nativespotify

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SoloistPlaybackTest {
    @Test
    fun trackChangesDiscardPreviousClockAndSeekCanOnlyTargetCurrentTrack() {
        val state = SoloistPlaybackState()
        state.accept(JSONObject("""{"type":"playback_state","status":"playing",
            "item":{"uri":"spotify:track:old"},
            "position":{"position_ms":10000,"timestamp_ms":1000,"speed":1.0}}"""))
        assertEquals(50_000, state.snapshot("phone", 41_000)?.getInt("progress_ms"))
        state.accept(JSONObject("""{"type":"track_changed","item":{"uri":"spotify:track:new"}}"""))
        assertTrue(!state.matchesTrack("spotify:track:old"))
        assertTrue(state.matchesTrack("spotify:track:new"))
        assertTrue(state.snapshot("phone", 42_000)?.has("progress_ms") == false)
        assertEquals(false, state.snapshot("phone", 42_000)?.getBoolean("is_playing"))

        state.accept(JSONObject("""{"type":"position_sync",
            "position":{"position_ms":10000,"timestamp_ms":999,"speed":1.0}}"""))
        assertTrue(state.snapshot("phone", 42_000)?.has("progress_ms") == false)

        state.accept(JSONObject("""{"type":"position_sync",
            "position":{"position_ms":1500,"timestamp_ms":42000,"speed":1.0}}"""))
        assertEquals(2_000, state.snapshot("phone", 42_500)?.getInt("progress_ms"))
        assertEquals(true, state.snapshot("phone", 42_500)?.getBoolean("is_playing"))
    }

    @Test
    fun pauseFreezesTheCurrentClockUntilTheEngineSendsItsNewAnchor() {
        val state = SoloistPlaybackState()
        state.accept(JSONObject("""{"type":"playback_state","status":"playing",
            "item":{"uri":"spotify:track:song"},
            "position":{"position_ms":3000,"timestamp_ms":1000,"speed":1.0}}"""))
        state.accept(JSONObject("""{"type":"playback_changed","status":"paused"}"""), now = 5000)
        assertEquals(7000, state.snapshot("phone", 7500)?.getInt("progress_ms"))
        assertEquals(false, state.snapshot("phone", 7500)?.getBoolean("is_playing"))

        state.accept(JSONObject("""{"type":"position_sync",
            "position":{"position_ms":450, "timestamp_ms":8000,"speed":0.0}}"""))
        state.accept(JSONObject("""{"type":"track_changed","item":{"uri":"spotify:track:song"}}"""))
        assertEquals(450, state.snapshot("phone", 9000)?.getInt("progress_ms"))
    }

    @Test
    fun mapsNewTrackAndPositionFromSoloist() {
        val source = JSONObject("""{
            "status":"playing", "position":{"position_ms":420},
            "item":{"uri":"spotify:track:next","entity_type":"track","decorations":{
                "identity":{"name":"Next track"},"playback":{"duration_ms":180000}
            }}, "available_actions":{"seek":{},"skip_next":{}}, "volume":65
        }""")

        val playback = checkNotNull(soloistPlaybackJson(source, "phone-id"))
        assertTrue(playback.getBoolean("is_playing"))
        assertEquals("phone-id", playback.getJSONObject("device").getString("id"))
        assertEquals("spotify:track:next", playback.getJSONObject("item").getString("uri"))
        assertEquals(420, playback.getInt("progress_ms"))
    }
}
