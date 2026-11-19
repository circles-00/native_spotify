package app.nativespotify

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Test

class HomeTest {
    private fun playlist(title: String, owner: String = "me") = itemFromJson(JSONObject()
        .put("kind", BuildConfig.ITEM_PLAYLIST).put("id", title.padEnd(22, 'x').take(22)).put("title", title)
        .put("ownerId", owner).toString())!!

    @Test
    fun spotifyMixesBecomeMadeForYou() {
        val mix = playlist("Discover Weekly", owner = "spotify")
        val (title, shelf) = homePlaylistShelf(listOf(playlist("Road trip"), mix))
        assertEquals("Made for you", title)
        assertEquals(listOf(mix), shelf)
    }

    @Test
    fun aMixNameFromAnotherOwnerIsJustAPlaylist() {
        val copy = playlist("Discover Weekly")
        assertEquals("Your playlists" to listOf(copy), homePlaylistShelf(listOf(copy)))
    }

    @Test
    fun shortcutsAreLikedSongsAndUpToSevenPlaylists() {
        val shortcuts = homeShortcuts((1..10).map { playlist("List $it") })
        assertEquals(8, shortcuts.size)
        assertEquals(Shortcut.LikedSongs, shortcuts.first())
    }

    @Test
    fun withoutPlaylistsTheDesktopFixedShortcutsAppear() {
        val titles = homeShortcuts(emptyList()).map {
            when (it) {
                Shortcut.LikedSongs -> "Liked Songs"
                is Shortcut.Tab -> it.title
                is Shortcut.Open -> it.item.title
            }
        }
        assertEquals(listOf("Liked Songs", "Albums", "Playlists", "Search"), titles)
    }

    @Test
    fun greetingFollowsTheDesktopHours() {
        assertEquals("Good morning", greeting(11))
        assertEquals("Good afternoon", greeting(12))
        assertEquals("Good evening", greeting(18))
    }
}
