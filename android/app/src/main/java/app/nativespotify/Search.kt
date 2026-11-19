package app.nativespotify

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyGridState
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.rememberLazyGridState
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.FavoriteBorder
import androidx.compose.material.icons.filled.Link
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.distinctUntilChanged

private val filters = listOf("All" to BuildConfig.SEARCH_ALL, "Songs" to BuildConfig.SEARCH_SONGS,
    "Artists" to BuildConfig.SEARCH_ARTISTS, "Albums" to BuildConfig.SEARCH_ALBUMS,
    "Playlists" to BuildConfig.SEARCH_PLAYLISTS)

private fun duration(ms: Int) = "${ms / 60_000}:${(ms / 1_000 % 60).toString().padStart(2, '0')}"

/** The desktop's card and top-result subtitle rules (search_scene.c). */
internal fun searchSubtitle(item: Item, top: Boolean): String {
    fun with(label: String) = if (item.detail.isNotEmpty()) "$label · ${item.detail}" else label

    return when {
        item.kind == BuildConfig.ITEM_ARTIST -> "Artist"
        item.kind == BuildConfig.ITEM_PLAYLIST -> with("Playlist")
        top && item.kind == BuildConfig.ITEM_ALBUM -> with("Album")
        top && item.kind == BuildConfig.ITEM_TRACK -> with("Song")
        item.kind == BuildConfig.ITEM_ALBUM -> item.detail.ifEmpty { "Album" }
        else -> item.detail
    }
}

/** Desktop filter pills: white with black text when active, otherwise a tile. */
@Composable
private fun FilterPills(selected: Int, select: (Int) -> Unit) {
    LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp), contentPadding = PaddingValues(vertical = 12.dp)) {
        items(filters) { (label, value) ->
            val active = selected == value

            Box(Modifier.height(32.dp).clip(CircleShape).background(if (active) Palette.text else Palette.tile)
                .pressable { select(value) }.padding(horizontal = 16.dp)
                .semantics { this.selected = active; role = Role.Tab }, contentAlignment = Alignment.Center) {
                Text(label, color = if (active) Color.Black else Palette.text, fontSize = 13.sp, fontWeight = FontWeight.Bold)
            }
        }
    }
}

@Composable
private fun Placeholder(modifier: Modifier, round: Boolean = false) {
    Box(modifier.clip(if (round) CircleShape else RoundedCornerShape(6.dp)).background(Palette.tile))
}

/** A search song row: artwork, title, explicit mark, first artist and duration, as on desktop. */
@Composable
internal fun SongRow(item: Item?, actions: SongActions, select: (Item) -> Unit) {
    if (item == null) {
        Row(Modifier.fillMaxWidth().height(58.dp), verticalAlignment = Alignment.CenterVertically) {
            Placeholder(Modifier.size(44.dp))
            Column(Modifier.padding(start = 12.dp)) {
                Placeholder(Modifier.width(160.dp).height(12.dp))
                Spacer(Modifier.height(8.dp))
                Placeholder(Modifier.width(96.dp).height(10.dp))
            }
        }
        return
    }

    Row(Modifier.fillMaxWidth().heightIn(min = 58.dp).clip(RoundedCornerShape(6.dp))
        .pressable(onLongClick = { actions.more(item) }) { select(item) }.padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Cover(item.artwork, fallbackIcon(item), Modifier.size(44.dp), radius = 4.dp)
        Column(Modifier.weight(1f).padding(horizontal = 12.dp)) {
            Text(item.title, fontSize = 15.sp, maxLines = 1, overflow = TextOverflow.Ellipsis,
                color = if (item.playable) Palette.text else Palette.muted)
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (item.explicit) {
                    Box(Modifier.padding(end = 6.dp).size(14.dp).clip(RoundedCornerShape(2.dp)).background(Palette.muted),
                        contentAlignment = Alignment.Center) {
                        Text("E", fontSize = 9.sp, fontWeight = FontWeight.Bold, color = Palette.body)
                    }
                }
                Text(item.detail, fontSize = 13.sp, color = Palette.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
        IconButton(onClick = { Core.toggleLike(item) }) {
            Icon(if (item.liked) Icons.Filled.Favorite else Icons.Filled.FavoriteBorder,
                contentDescription = if (item.liked) "Remove ${item.title} from Liked Songs" else "Save ${item.title} to Liked Songs",
                tint = if (item.liked) Palette.accent else Palette.muted, modifier = Modifier.size(20.dp))
        }
        if (item.durationMs > 0) {
            Text(duration(item.durationMs), fontSize = 13.sp, color = Palette.muted)
        }
    }
}

/** A desktop result card: inset artwork (round for artists), bold title, muted subtitle. */
@Composable
private fun ResultCard(item: Item?, modifier: Modifier, artist: Boolean, select: (Item) -> Unit) {
    Column(modifier.clip(RoundedCornerShape(8.dp)).background(Palette.tile)
        .then(if (item != null) Modifier.pressable { select(item) } else Modifier).padding(12.dp)) {
        if (item == null) {
            Placeholder(Modifier.fillMaxWidth().aspectRatio(1f), round = artist)
            Spacer(Modifier.height(10.dp))
            Placeholder(Modifier.fillMaxWidth(0.7f).height(12.dp))
            Spacer(Modifier.height(8.dp))
            Placeholder(Modifier.fillMaxWidth(0.4f).height(10.dp))
            return@Column
        }

        val round = item.kind == BuildConfig.ITEM_ARTIST
        Cover(item.artwork, fallbackIcon(item), Modifier.fillMaxWidth().aspectRatio(1f), radius = if (round) 100.dp else 6.dp)
        Spacer(Modifier.height(10.dp))
        Text(item.title, fontSize = 15.sp, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Text(searchSubtitle(item, top = false), fontSize = 13.sp, color = Palette.muted, maxLines = 2,
            overflow = TextOverflow.Ellipsis, modifier = Modifier.heightIn(min = 34.dp))
    }
}

@Composable
private fun TopResult(item: Item?, select: (Item) -> Unit) {
    Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp)).background(Palette.tile)
        .then(if (item != null) Modifier.pressable { select(item) } else Modifier).padding(20.dp)) {
        val round = item?.kind == BuildConfig.ITEM_ARTIST

        if (item == null) {
            Placeholder(Modifier.size(92.dp))
            Spacer(Modifier.height(20.dp))
            Placeholder(Modifier.width(180.dp).height(22.dp))
            Spacer(Modifier.height(10.dp))
            Placeholder(Modifier.width(110.dp).height(12.dp))
            return@Column
        }

        Cover(item.artwork, fallbackIcon(item), Modifier.size(92.dp), radius = if (round) 46.dp else 6.dp)
        Spacer(Modifier.height(20.dp))
        Text(item.title, fontSize = 28.sp, fontWeight = FontWeight.Bold, maxLines = 2, overflow = TextOverflow.Ellipsis,
            lineHeight = 32.sp)
        Spacer(Modifier.height(6.dp))
        Text(searchSubtitle(item, top = true), fontSize = 13.sp, color = Palette.muted, maxLines = 1,
            overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun Heading(text: String) {
    Text(text, Modifier.padding(top = 24.dp, bottom = 12.dp).semantics { heading() }, fontSize = 22.sp,
        fontWeight = FontWeight.Bold)
}

/** One row of result cards, scrolled sideways on a phone where the desktop shows a full row. */
@Composable
private fun CardRow(items: List<Item>, loading: Boolean, artist: Boolean, open: (Item) -> Unit) {
    LazyRow(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        if (loading) {
            items(4) { ResultCard(null, Modifier.width(150.dp), artist, open) }
        } else {
            items(items, key = { it.uri }) { item -> ResultCard(item, Modifier.width(150.dp), artist, open) }
        }
    }
}

/** Keeps the core's row window on the grid's scroll position and loads the next page near the end. */
@Composable
private fun FollowGrid(state: LazyGridState, list: ListState) {
    val current by rememberUpdatedState(list)

    LaunchedEffect(state) {
        snapshotFlow { state.firstVisibleItemIndex }.distinctUntilChanged().collect { Core.viewport(it) }
    }

    LaunchedEffect(state) {
        snapshotFlow { state.layoutInfo.visibleItemsInfo.lastOrNull()?.index ?: 0 }.distinctUntilChanged().collect { last ->
            if (last >= current.count - 12 && current.more && !current.loading) {
                Core.loadMore()
            }
        }
    }
}

/**
 * Desktop search on a phone: the pill field, filter pills, then the results. All shows the top
 * result, four songs and rows of artists, albums and playlists; the other filters list or grid
 * every result, paging through the core.
 */
@Composable
internal fun SearchScreen(core: CoreState, query: String, setQuery: (String) -> Unit, filter: Int, setFilter: (Int) -> Unit,
                          actions: SongActions, open: (Item) -> Unit, playLink: (String) -> Unit) {
    val list = core.list
    val trimmed = query.trim()
    val link = spotifyUri(query)
    val focus = LocalFocusManager.current
    val current = list.kind == BuildConfig.BROWSE_SEARCH && list.query == trimmed && list.searchFilter == filter
    val loading = trimmed.isNotEmpty() && link == null && (!current || (list.loading && !list.loaded))

    // Desktop waits for a pause in typing before searching; changing the filter searches at once.
    LaunchedEffect(trimmed, filter) {
        if (trimmed.isEmpty() || link != null || current) {
            return@LaunchedEffect
        }

        delay(300)
        Core.search(trimmed, filter)
    }

    // A song picked from search goes to recent searches and plays, exactly as on desktop.
    val playSong: (Item) -> Unit = { item ->
        Core.historyAdd(item)
        actions.play(item)
    }
    val select: (Item) -> Unit = { item ->
        if (item.kind == BuildConfig.ITEM_TRACK || item.kind == BuildConfig.ITEM_EPISODE) playSong(item) else open(item)
    }

    Column {
        OutlinedTextField(query, { setQuery(it.take(200)) }, Modifier.fillMaxWidth(), singleLine = true,
            placeholder = { Text("What do you want to play?") },
            leadingIcon = { Icon(Icons.Filled.Search, contentDescription = null) },
            trailingIcon = {
                if (query.isNotEmpty()) {
                    IconButton(onClick = { setQuery("") }) { Icon(Icons.Filled.Close, contentDescription = "Clear search") }
                }
            },
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
            keyboardActions = KeyboardActions(onSearch = { focus.clearFocus() }),
            shape = CircleShape,
            colors = OutlinedTextFieldDefaults.colors(focusedContainerColor = Palette.tile, unfocusedContainerColor = Palette.tile,
                focusedBorderColor = Palette.text, unfocusedBorderColor = Palette.tile))

        when {
            link != null -> LinkResult(link, playLink)
            trimmed.isEmpty() -> History(core.history, actions)
            filter == BuildConfig.SEARCH_ALL -> {
                FilterPills(filter, setFilter)
                AllResults(core, current, loading, playSong, open, actions)
            }
            filter == BuildConfig.SEARCH_SONGS -> {
                FilterPills(filter, setFilter)
                SongResults(list, current, loading, playSong, actions)
            }
            else -> {
                FilterPills(filter, setFilter)
                GridResults(list, current, loading, filter == BuildConfig.SEARCH_ARTISTS, select)
            }
        }
    }
}

@Composable
private fun LinkResult(link: String, play: (String) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(top = 12.dp).clip(RoundedCornerShape(8.dp)).background(Palette.tile)
        .pressable { play(link) }.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.CenterVertically) {
        Icon(Icons.Filled.Link, contentDescription = null, tint = Palette.accent)
        Text("Play this ${link.split(':')[1]} link", style = MaterialTheme.typography.bodyLarge)
    }
}

/** The desktop's recent searches panel: songs you picked from search, to play, like or remove. */
@Composable
private fun History(history: List<Item>, actions: SongActions) {
    LazyColumn(contentPadding = PaddingValues(bottom = 24.dp)) {
        item { Heading("Recent searches") }

        if (history.isEmpty()) {
            item { Text("No recent songs", fontSize = 13.sp, color = Palette.muted) }
        }

        items(history, key = { it.id }) { item ->
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.weight(1f)) { SongRow(item, actions) { Core.playHistory(it) } }
                IconButton(onClick = { Core.historyRemove(item) }) {
                    Icon(Icons.Filled.Close, contentDescription = "Remove ${item.title} from recent searches", tint = Palette.muted)
                }
            }
        }
    }
}

@Composable
private fun AllResults(core: CoreState, current: Boolean, loading: Boolean, playSong: (Item) -> Unit, open: (Item) -> Unit,
                       actions: SongActions) {
    val list = core.list
    val state: LazyListState = rememberLazyListState()
    val songs = if (current && list.first == 0) list.rows.take(4) else emptyList()
    // The core picks the top result; desktop falls back to the first song.
    val top = if (current) core.searchTop ?: list.row(0) else null
    val empty = current && !loading && list.count == 0 && core.searchArtists.isEmpty() && core.searchAlbums.isEmpty() &&
        core.searchPlaylists.isEmpty()

    LazyColumn(state = state, contentPadding = PaddingValues(bottom = 24.dp)) {
        if (empty) {
            item { Text("No results found", Modifier.padding(top = 32.dp), fontSize = 22.sp, fontWeight = FontWeight.Bold) }
            return@LazyColumn
        }

        item { Heading("Top result") }
        item {
            TopResult(if (loading) null else top) { item ->
                if (item.kind == BuildConfig.ITEM_TRACK) playSong(item) else open(item)
            }
        }

        item { Heading("Songs") }

        if (loading) {
            items(4) { SongRow(null, actions, playSong) }
        } else {
            items(songs, key = { "song:${it.id}" }) { SongRow(it, actions, playSong) }
        }

        if (loading || core.searchArtists.isNotEmpty()) {
            item { Heading("Artists") }
            item { CardRow(core.searchArtists, loading, artist = true, open) }
        }

        if (loading || core.searchAlbums.isNotEmpty()) {
            item { Heading("Albums") }
            item { CardRow(core.searchAlbums, loading, artist = false, open) }
        }

        if (loading || core.searchPlaylists.isNotEmpty()) {
            item { Heading("Playlists") }
            item { CardRow(core.searchPlaylists, loading, artist = false, open) }
        }
    }
}

@Composable
private fun SongResults(list: ListState, current: Boolean, loading: Boolean, playSong: (Item) -> Unit, actions: SongActions) {
    val state = rememberLazyListState()

    if (current) {
        FollowWindow(state, list, header = 0)
    }

    LazyColumn(state = state, contentPadding = PaddingValues(bottom = 24.dp)) {
        if (loading) {
            items(6) { SongRow(null, actions, playSong) }
            return@LazyColumn
        }

        if (current && list.loaded && list.count == 0) {
            item { Text("No results found", Modifier.padding(top = 32.dp), fontSize = 22.sp, fontWeight = FontWeight.Bold) }
        }

        if (current) {
            items(list.count, key = { index -> list.row(index)?.let { "${it.id}:$index" } ?: "slot:$index" }) { index ->
                SongRow(list.row(index), actions, playSong)
            }
        }
    }
}

@Composable
private fun GridResults(list: ListState, current: Boolean, loading: Boolean, artist: Boolean, select: (Item) -> Unit) {
    val state = rememberLazyGridState()

    if (current) {
        FollowGrid(state, list)
    }

    LazyVerticalGrid(GridCells.Fixed(2), state = state, horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp), contentPadding = PaddingValues(bottom = 24.dp)) {
        if (loading) {
            items(6) { ResultCard(null, Modifier, artist, select) }
            return@LazyVerticalGrid
        }

        if (current && list.loaded && list.count == 0) {
            item(span = { GridItemSpan(2) }) {
                Text("No results found", Modifier.padding(top = 32.dp), fontSize = 22.sp, fontWeight = FontWeight.Bold)
            }
        }

        if (current) {
            items(list.count, key = { index -> list.row(index)?.let { "${it.id}:$index" } ?: "slot:$index" }) { index ->
                ResultCard(list.row(index), Modifier, artist, select)
            }
        }
    }
}
