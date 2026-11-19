package app.nativespotify

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class CoreSnapshotTest {
    private val song = """{"kind":${BuildConfig.ITEM_TRACK},"id":"4uLU6hMCjMI75M1A2tKUQC","title":"Let Down",""" +
        """"detail":"Radiohead","guests":"","artwork":"","playable":true,"durationMs":299000,"liked":true,""" +
        """"albumId":"6dVIqQ8qmQ5GBnJ9shOYGE","albumTitle":"OK Computer"}"""

    @Test
    fun listRowsAreAddressedByAbsoluteIndex() {
        val state = parseSnapshot("""{"authenticated":true,"list":{"kind":${BuildConfig.BROWSE_PLAYLIST_TRACKS},""" +
            """"first":64,"count":200,"rows":[$song]}}""")

        assertTrue(state.authenticated)
        assertEquals(200, state.list.count)
        assertEquals("Let Down", state.list.row(64)?.title)
        assertNull(state.list.row(0))
        assertNull(state.list.row(65))
    }

    @Test
    fun itemsKeepTheirJsonForCommandsAndBuildPlayableTracks() {
        val state = parseSnapshot("""{"history":[$song]}""")
        val item = state.history.single()

        assertEquals("spotify:track:4uLU6hMCjMI75M1A2tKUQC", item.uri)
        assertEquals(item, itemFromJson(item.json))
        assertEquals("Radiohead", subtitle(item))
    }

    @Test
    fun playbackPositionMovesOnOnlyWhilePlaying() {
        val playing = parseSnapshot("""{"playback":{"playing":true,"uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC",""" +
            """"positionMs":1000,"durationMs":5000}}""", now = 10_000).playback
        val paused = playing.copy(playing = false)

        assertEquals(3_000, playing.position(now = 12_000))
        assertEquals(5_000, playing.position(now = 99_000))
        assertEquals(1_000, paused.position(now = 12_000))
        assertEquals("4uLU6hMCjMI75M1A2tKUQC", playing.item()?.id)
    }

    @Test
    fun trackChangeFreezesProgressEvenIfPlaybackIsStillPlaying() {
        val playback = parseSnapshot(
            """{"playback":{"playing":true,"changingTrack":true,"positionMs":0,"durationMs":240000}}""",
            now = 10_000,
        ).playback

        assertTrue(playback.changingTrack)
        assertEquals(0, playback.position(now = 12_000))
        assertEquals(2_000, playback.copy(changingTrack = false).position(now = 12_000))
    }

    @Test
    fun searchSubtitlesFollowTheDesktopRules() {
        val song = itemFromJson(song)!!
        val album = song.copy(kind = BuildConfig.ITEM_ALBUM)
        val playlist = song.copy(kind = BuildConfig.ITEM_PLAYLIST, detail = "")

        assertEquals("Song · Radiohead", searchSubtitle(song, top = true))
        assertEquals("Album · Radiohead", searchSubtitle(album, top = true))
        assertEquals("Radiohead", searchSubtitle(album, top = false))
        assertEquals("Playlist", searchSubtitle(playlist, top = false))
        assertEquals("Artist", searchSubtitle(song.copy(kind = BuildConfig.ITEM_ARTIST), top = false))
    }

    @Test
    fun missingSectionsFallBackToEmptyState() {
        val state = parseSnapshot("{}")

        assertFalse(state.authenticated)
        assertTrue(state.list.rows.isEmpty())
        assertNull(state.details.item)
    }

    @Test
    fun navigationStacksSurviveSaving() {
        val stacks = mapOf("Home" to listOf(Destination("Collection", "open", song)), "Search" to emptyList())
        assertEquals(stacks, decodeStacks(encodeStacks(stacks)))
    }
}
