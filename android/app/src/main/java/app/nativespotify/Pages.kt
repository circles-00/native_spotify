package app.nativespotify

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.Sort
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.FavoriteBorder
import androidx.compose.material.icons.filled.FilterList
import androidx.compose.material.icons.filled.Link
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.distinctUntilChanged

/** What a page does with a song: play it, or open its actions sheet. */
internal class SongActions(val play: (Item) -> Unit, val more: (Item) -> Unit)

/**
 * Keeps the core's row window following the scroll position and asks for the next page
 * near the end, the same contract the desktop list uses.
 */
@Composable
internal fun FollowWindow(state: LazyListState, list: ListState, header: Int, library: Boolean = false) {
    val current by rememberUpdatedState(list)

    LaunchedEffect(state, header, library) {
        snapshotFlow { (state.firstVisibleItemIndex - header).coerceAtLeast(0) }.distinctUntilChanged().collect { first ->
            if (library) Core.libraryViewport(first) else Core.viewport(first)
        }
    }

    LaunchedEffect(state, header, library) {
        snapshotFlow { state.layoutInfo.visibleItemsInfo.lastOrNull()?.index ?: 0 }.distinctUntilChanged().collect { last ->
            val nearEnd = last - header >= current.count - 12

            if (!library && nearEnd && current.more && !current.loading) {
                Core.loadMore()
            }
        }
    }
}

internal fun LazyListScope.rows(list: ListState, numbered: Boolean, actions: SongActions, open: (Item) -> Unit) {
    items(list.count, key = { index -> list.row(index)?.let { "${it.id}:$index" } ?: "slot:$index" }) { index ->
        val item = list.row(index)
        val song = item != null && (item.kind == BuildConfig.ITEM_TRACK || item.kind == BuildConfig.ITEM_EPISODE)

        ItemRow(item, index = if (numbered && song) (item?.trackNumber?.minus(1) ?: index) else -1,
            actions = if (song) actions.more else null) { selected ->
            if (song) actions.play(selected) else open(selected)
        }
    }
}

@Composable
internal fun LoadingLine(visible: Boolean) {
    Box(Modifier.fillMaxWidth().height(2.dp)) {
        if (visible) {
            LinearProgressIndicator(Modifier.fillMaxWidth(), color = Palette.accent, trackColor = Palette.tile)
        }
    }
}

@Composable
internal fun Chips(options: List<Pair<String, Int>>, selected: Int, select: (Int) -> Unit) {
    LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp), contentPadding = PaddingValues(vertical = 8.dp)) {
        items(options) { (label, value) ->
            FilterChip(selected = selected == value, onClick = { select(value) }, label = { Text(label) },
                colors = FilterChipDefaults.filterChipColors(containerColor = Palette.tile, labelColor = Palette.text,
                    selectedContainerColor = Palette.accent, selectedLabelColor = Color.Black),
                border = null)
        }
    }
}

internal val PLACEHOLDER = Item(0, "", "", "", "", "", false, 0, false, false, 0, 0, "", "", "", "", "", "", "{}")
internal val PLACEHOLDER_ARTIST = PLACEHOLDER.copy(kind = BuildConfig.ITEM_ARTIST)
internal val PLACEHOLDER_ALBUM = PLACEHOLDER.copy(kind = BuildConfig.ITEM_ALBUM)
internal val PLACEHOLDER_PLAYLIST = PLACEHOLDER.copy(kind = BuildConfig.ITEM_PLAYLIST)

@Composable
internal fun SectionTitle(text: String) {
    Text(text, Modifier.padding(top = 16.dp, bottom = 8.dp).semantics { heading() }, style = MaterialTheme.typography.titleLarge)
}

@Composable
internal fun EmptyLine(text: String) {
    Text(text, Modifier.padding(vertical = 16.dp), style = MaterialTheme.typography.bodyMedium, color = Palette.muted)
}

internal const val LIBRARY_PLAYLISTS = "playlists"
internal const val LIBRARY_ALBUMS = "albums"
internal const val LIBRARY_ARTISTS = "artists"

/** Your Library: playlists from the core's library rail, or saved albums and followed artists. */
@Composable
internal fun LibraryScreen(core: CoreState, section: String, setSection: (String) -> Unit, openLiked: () -> Unit,
                           actions: SongActions, open: (Item) -> Unit) {
    val state = rememberLazyListState()
    val options = listOf("Playlists" to 0, "Albums" to 1, "Artists" to 2)
    val selected = when (section) { LIBRARY_ALBUMS -> 1; LIBRARY_ARTISTS -> 2; else -> 0 }
    val wanted = if (section == LIBRARY_ALBUMS) BuildConfig.BROWSE_SAVED_ALBUMS else BuildConfig.BROWSE_FOLLOWED_ARTISTS
    val list = if (section == LIBRARY_PLAYLISTS) core.playlists else core.list
    val ready = section == LIBRARY_PLAYLISTS || list.kind == wanted

    // Playlists come with the session; albums and artists are browsed on demand.
    LaunchedEffect(section) {
        if (section != LIBRARY_PLAYLISTS) {
            Core.browse(wanted)
        }
    }

    if (ready) {
        FollowWindow(state, list, header = if (section == LIBRARY_PLAYLISTS) 1 else 0, library = section == LIBRARY_PLAYLISTS)
    }

    Column {
        Chips(options, selected) { setSection(listOf(LIBRARY_PLAYLISTS, LIBRARY_ALBUMS, LIBRARY_ARTISTS)[it]) }
        LoadingLine(!ready || list.loading)

        LazyColumn(state = state, contentPadding = PaddingValues(bottom = 24.dp)) {
            if (section == LIBRARY_PLAYLISTS) {
                item {
                    Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(6.dp)).pressable(onClick = openLiked).padding(vertical = 6.dp),
                        horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Cover("", Icons.Filled.Favorite, Modifier.size(52.dp), radius = 4.dp, liked = true)
                        Column {
                            Text("Liked Songs", style = MaterialTheme.typography.bodyLarge)
                            val total = core.savedTracks.total
                            Text(if (total > 0) "Playlist · $total songs" else "Playlist", style = MaterialTheme.typography.bodyMedium,
                                color = Palette.muted)
                        }
                    }
                }
            }

            if (!ready) {
                return@LazyColumn
            }

            if (list.loaded && list.count == 0) {
                item {
                    EmptyLine(when (section) {
                        LIBRARY_ALBUMS -> "Save albums to find them here."
                        LIBRARY_ARTISTS -> "Artists you follow will appear here."
                        else -> "Your playlists will appear here."
                    })
                }
            }

            rows(list, numbered = false, actions, open)
        }
    }
}

private val sortLabels = listOf("Custom order" to BuildConfig.LIST_SORT_NONE, "Title" to BuildConfig.LIST_SORT_TITLE,
    "Album" to BuildConfig.LIST_SORT_ALBUM, "Duration" to BuildConfig.LIST_SORT_DURATION)

private fun minutes(ms: Long): String {
    val total = ms / 60_000
    return if (total >= 60) "${total / 60} hr ${total % 60} min" else "$total min"
}

@Composable
private fun PlayButton(enabled: Boolean, play: () -> Unit) {
    IconButton(onClick = play, enabled = enabled, modifier = Modifier.size(56.dp).clip(CircleShape)
        .background(if (enabled) Palette.accent else Palette.control)) {
        Icon(Icons.Filled.PlayArrow, contentDescription = "Play", tint = Color.Black, modifier = Modifier.size(32.dp))
    }
}

/** Sort menu and in-list filter for lists the core can arrange; the core applies both. */
@Composable
private fun Arrange(list: ListState) {
    var menu by remember { mutableStateOf(false) }
    var filtering by rememberSaveable { mutableStateOf(list.filter.isNotEmpty()) }
    var text by rememberSaveable { mutableStateOf(list.filter) }

    LaunchedEffect(text) {
        delay(200)
        Core.filter(text)
    }

    Row(verticalAlignment = Alignment.CenterVertically) {
        IconButton(onClick = { filtering = !filtering }) {
            Icon(Icons.Filled.FilterList, contentDescription = "Filter songs", tint = if (filtering) Palette.accent else Palette.muted)
        }
        Box {
            IconButton(onClick = { menu = true }) {
                Icon(Icons.AutoMirrored.Filled.Sort, contentDescription = "Sort songs",
                    tint = if (list.sort != BuildConfig.LIST_SORT_NONE) Palette.accent else Palette.muted)
            }
            DropdownMenu(menu, { menu = false }, Modifier.background(Palette.sheet)) {
                sortLabels.forEach { (label, value) ->
                    val active = list.sort == value
                    DropdownMenuItem(text = { Text(if (active && value != BuildConfig.LIST_SORT_NONE) "$label ${if (list.descending) "↓" else "↑"}" else label,
                        color = if (active) Palette.accent else Palette.text) }, onClick = {
                        menu = false
                        // Choosing the active sort again flips its direction, like the desktop column headers.
                        Core.sort(value, active && value != BuildConfig.LIST_SORT_NONE && !list.descending)
                    })
                }
            }
        }
    }

    if (filtering) {
        OutlinedTextField(text, { text = it.take(120) }, Modifier.fillMaxWidth(), singleLine = true,
            placeholder = { Text("Find in this list") },
            leadingIcon = { Icon(Icons.Filled.Search, contentDescription = null) },
            shape = RoundedCornerShape(8.dp),
            colors = OutlinedTextFieldDefaults.colors(focusedContainerColor = Palette.tile, unfocusedContainerColor = Palette.tile,
                unfocusedBorderColor = Palette.tile))
    }
}

/**
 * Albums, playlists, Liked Songs, song radio and an artist's liked songs: a header, the play
 * and save controls, then the core's song list.
 */
@Composable
internal fun CollectionScreen(core: CoreState, actions: SongActions, open: (Item) -> Unit) {
    val state = rememberLazyListState()
    val list = core.list
    val details = core.details
    val album = list.kind == BuildConfig.BROWSE_ALBUM_TRACKS
    val playlist = list.kind == BuildConfig.BROWSE_PLAYLIST_TRACKS
    val liked = list.kind == BuildConfig.BROWSE_SAVED_TRACKS
    FollowWindow(state, list, header = 1)

    LazyColumn(state = state, contentPadding = PaddingValues(bottom = 24.dp)) {
        item {
            Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                val art = details.item?.artwork?.takeIf { album } ?: list.artwork
                Cover(art, if (liked) Icons.Filled.Favorite else fallbackIcon(PLACEHOLDER_PLAYLIST), Modifier.size(220.dp),
                    liked = liked && art.isBlank())
                Spacer(Modifier.height(16.dp))
                Text(list.title.ifBlank { if (liked) "Liked Songs" else "" }, Modifier.fillMaxWidth().semantics { heading() },
                    style = MaterialTheme.typography.headlineSmall, maxLines = 2, overflow = TextOverflow.Ellipsis)

                val meta = when {
                    album -> listOf(details.item?.detail.orEmpty(), details.release,
                        if (details.totalTracks > 0) "${details.totalTracks} songs" else "", details.duration)
                    else -> listOf(if (list.total > 0) "${list.total} songs" else "", if (list.durationMs > 0) minutes(list.durationMs) else "")
                }.filter(String::isNotBlank).joinToString(" · ")
                Text(meta, Modifier.fillMaxWidth(), style = MaterialTheme.typography.bodyMedium, color = Palette.muted)
                Spacer(Modifier.height(8.dp))

                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    if ((album || playlist) && details.savedKnown) {
                        IconButton(onClick = Core::toggleSaved, enabled = !details.saving) {
                            Icon(if (details.saved) Icons.Filled.Favorite else Icons.Filled.FavoriteBorder,
                                contentDescription = if (details.saved) "Remove from Your Library" else "Save to Your Library",
                                tint = if (details.saved) Palette.accent else Palette.muted)
                        }
                    }

                    if (list.arrangeable) {
                        Arrange(list)
                    }

                    Spacer(Modifier.weight(1f))
                    PlayButton(list.count > 0) {
                        // As on desktop: an unsorted playlist plays as a Spotify playlist, other lists as shown.
                        val arranged = list.sort != BuildConfig.LIST_SORT_NONE || list.filter.isNotEmpty()

                        when {
                            !list.arrangeable -> Core.playCollection()
                            playlist && !arranged -> Core.playCollection()
                            else -> Core.playList()
                        }
                    }
                }
                LoadingLine(list.loading)
            }
        }

        if (list.loaded && list.count == 0) {
            item { EmptyLine(if (list.filter.isNotEmpty()) "No songs match “${list.filter}”." else "No songs here yet.") }
        }

        rows(list, numbered = album, actions, open)

        if (album && details.copyrights.isNotBlank()) {
            item {
                Text(listOf(details.release, details.label, details.copyrights).filter(String::isNotBlank).joinToString("\n"),
                    Modifier.padding(vertical = 16.dp), style = MaterialTheme.typography.bodySmall, color = Palette.muted)
            }
        }
    }
}

private val discography = listOf("All" to 0, "Albums" to 1, "Singles and EPs" to 2)
private val groups = listOf("", "album", "single")

/** Artist page: header, popular songs, the link to your liked songs by them, then the discography. */
@Composable
internal fun ArtistScreen(core: CoreState, actions: SongActions, open: (Item) -> Unit, openLikes: (Item) -> Unit) {
    val state = rememberLazyListState()
    val list = core.list
    val artist = core.details.item
    var expanded by rememberSaveable { mutableStateOf(false) }
    val group = groups.indexOf(list.query).coerceAtLeast(0)
    FollowWindow(state, list, header = 4 + (if (expanded) 10 else 5))

    LazyColumn(state = state, contentPadding = PaddingValues(bottom = 24.dp)) {
        item {
            Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                Cover(artist?.artwork ?: list.artwork, fallbackIcon(PLACEHOLDER_ARTIST), Modifier.size(200.dp), radius = 100.dp)
                Spacer(Modifier.height(16.dp))
                Text(artist?.title ?: list.title, style = MaterialTheme.typography.headlineMedium, textAlign = TextAlign.Center,
                    modifier = Modifier.semantics { heading() })

                if (core.details.followersKnown) {
                    Text("%,d followers".format(core.details.followers), style = MaterialTheme.typography.bodyMedium,
                        color = Palette.muted)
                }

                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Spacer(Modifier.weight(1f))
                    PlayButton(artist != null) { Core.playCollection() }
                }
            }
        }

        item { SectionTitle("Popular") }
        val popular = core.details.popular.take(if (expanded) 10 else 5)
        items(popular.size) { index ->
            ItemRow(popular[index], index = index, actions = actions.more) { Core.playPopular(index) }
        }
        item {
            if (core.details.popular.size > 5) {
                TextButton(onClick = { expanded = !expanded }) { Text(if (expanded) "Show less" else "See more", color = Palette.muted) }
            }
        }
        item {
            if (artist != null && core.artistLikesReady && core.artistLikedCount > 0) {
                Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(6.dp)).pressable { openLikes(artist) }.padding(vertical = 8.dp),
                    horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                    Box(Modifier.size(52.dp), contentAlignment = Alignment.Center) {
                        Cover(artist.artwork, fallbackIcon(PLACEHOLDER_ARTIST), Modifier.size(52.dp), radius = 26.dp)
                        Box(Modifier.align(Alignment.BottomEnd).size(20.dp).clip(CircleShape).background(Palette.accent),
                            contentAlignment = Alignment.Center) {
                            Icon(Icons.Filled.Favorite, contentDescription = null, tint = Color.Black, modifier = Modifier.size(12.dp))
                        }
                    }
                    Column {
                        Text("Liked songs", style = MaterialTheme.typography.bodyLarge)
                        Text("${core.artistLikedCount} songs by ${artist.title}", style = MaterialTheme.typography.bodyMedium,
                            color = Palette.muted)
                    }
                }
            }
            SectionTitle("Discography")
            Chips(discography, group) { Core.discography(groups[it]) }
        }

        rows(list, numbered = false, actions, open)
    }
}

/** Profile: who you are, artists you follow, and your public playlists. */
@Composable
internal fun ProfileScreen(core: CoreState, actions: SongActions, open: (Item) -> Unit) {
    val state = rememberLazyListState()
    val profile = core.profile
    val list = core.list
    FollowWindow(state, list, header = 3)

    LazyColumn(state = state, contentPadding = PaddingValues(bottom = 24.dp)) {
        item {
            Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                Box(Modifier.size(140.dp).clip(CircleShape).background(Palette.accent), contentAlignment = Alignment.Center) {
                    Text(profile.name.take(1).uppercase().ifBlank { "?" }, style = MaterialTheme.typography.displayMedium,
                        color = Color.Black)

                    if (profile.artwork.isNotBlank()) {
                        Cover(profile.artwork, fallbackIcon(PLACEHOLDER_ARTIST), Modifier.size(140.dp), radius = 70.dp)
                    }
                }
                Spacer(Modifier.height(12.dp))
                Text(profile.name.ifBlank { "Spotify account" }, style = MaterialTheme.typography.headlineSmall)
                Text(listOf(if (profile.followers > 0) "${profile.followers} followers" else "",
                    if (profile.followingTotal > 0) "${profile.followingTotal} following" else "")
                    .filter(String::isNotBlank).joinToString(" · "), style = MaterialTheme.typography.bodyMedium, color = Palette.muted)
            }
        }
        item {
            val empty = if (profile.permissionRequired) "Log in again to show who you follow." else "You don't follow any artists yet."
            Shelf("Following", profile.following, empty, fallbackIcon(PLACEHOLDER_ARTIST), null, open)
        }
        item { SectionTitle("Public playlists") }

        if (list.kind == BuildConfig.BROWSE_PROFILE_PLAYLISTS) {
            if (list.loaded && list.count == 0) item { EmptyLine("No public playlists.") }
            rows(list, numbered = false, actions, open)
        }
    }
}
