package app.nativespotify

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.QueueMusic
import androidx.compose.material.icons.filled.Album
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.LibraryMusic
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.outlined.Home
import androidx.compose.material.icons.outlined.LibraryMusic
import androidx.compose.material.icons.outlined.Search
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.ripple
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import coil.compose.AsyncImage
import java.util.Calendar

/**
 * A tappable surface that dips slightly while pressed, with the platform ripple and an
 * optional long press that gives the standard haptic tick.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
internal fun Modifier.pressable(label: String? = null, onLongClick: (() -> Unit)? = null, onClick: () -> Unit): Modifier {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val scale by animateFloatAsState(if (pressed) 0.97f else 1f,
        spring(dampingRatio = Spring.DampingRatioMediumBouncy, stiffness = Spring.StiffnessMedium), label = "press")
    val haptics = LocalHapticFeedback.current

    return this
        .graphicsLayer {
            scaleX = scale
            scaleY = scale
        }
        .combinedClickable(interactionSource = interaction, indication = ripple(), role = Role.Button,
            onClickLabel = label, onLongClick = onLongClick?.let { longClick ->
                {
                    haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                    longClick()
                }
            }, onClick = onClick)
}

/** Artwork with a fallback: the Liked Songs gradient, or an icon on a tile. Images fade in. */
@Composable
internal fun Cover(url: String, fallback: ImageVector, modifier: Modifier, radius: Dp = 6.dp, liked: Boolean = false) {
    val shape = RoundedCornerShape(radius)
    val background = if (liked) {
        Modifier.background(Brush.linearGradient(listOf(Color(BuildConfig.DARK_LIKED_FROM), Color(BuildConfig.DARK_LIKED_TO))), shape)
    } else {
        Modifier.background(Palette.hover, shape)
    }

    Box(modifier.clip(shape).then(background), contentAlignment = Alignment.Center) {
        Icon(fallback, contentDescription = null, tint = Palette.text, modifier = Modifier.size(24.dp))

        if (url.isNotBlank()) {
            AsyncImage(url, contentDescription = null, modifier = Modifier.fillMaxSize(), contentScale = ContentScale.Crop)
        }
    }
}

internal fun greeting(hour: Int = Calendar.getInstance().get(Calendar.HOUR_OF_DAY)) = when {
    hour < 12 -> "Good morning"
    hour < 18 -> "Good afternoon"
    else -> "Good evening"
}

/** Mirrors the desktop shelf rule: Spotify's own mixes, if present, become "Made for you". */
internal fun homePlaylistShelf(playlists: List<Item>): Pair<String, List<Item>> {
    val mixes = playlists.filter { it.ownerId == "spotify" && it.title in setOf("Discover Weekly", "Release Radar") }
    return if (mixes.isNotEmpty()) "Made for you" to mixes else "Your playlists" to playlists
}

internal sealed interface Shortcut {
    data object LikedSongs : Shortcut
    data class Open(val item: Item) : Shortcut
    data class Tab(val title: String, val icon: ImageVector, val tab: String) : Shortcut
}

/** Liked Songs plus up to seven playlists, or the desktop's fixed shortcuts when there are none. */
internal fun homeShortcuts(playlists: List<Item>): List<Shortcut> {
    if (playlists.isEmpty()) {
        return listOf(Shortcut.LikedSongs, Shortcut.Tab("Albums", Icons.Filled.Album, "Library"),
            Shortcut.Tab("Playlists", Icons.AutoMirrored.Filled.QueueMusic, "Library"),
            Shortcut.Tab("Search", Icons.Filled.Search, "Search"))
    }

    return listOf(Shortcut.LikedSongs) + playlists.take(7).map { Shortcut.Open(it) }
}

@Composable
private fun QuickTile(shortcut: Shortcut, modifier: Modifier, select: () -> Unit) {
    val title = when (shortcut) {
        Shortcut.LikedSongs -> "Liked Songs"
        is Shortcut.Open -> shortcut.item.title
        is Shortcut.Tab -> shortcut.title
    }

    Row(modifier.height(56.dp).clip(RoundedCornerShape(6.dp)).background(Palette.tile).pressable(onClick = select),
        verticalAlignment = Alignment.CenterVertically) {
        when (shortcut) {
            Shortcut.LikedSongs -> Cover("", Icons.Filled.Favorite, Modifier.size(56.dp), liked = true)
            is Shortcut.Open -> Cover(shortcut.item.artwork, Icons.AutoMirrored.Filled.QueueMusic, Modifier.size(56.dp))
            is Shortcut.Tab -> Cover("", shortcut.icon, Modifier.size(56.dp))
        }
        Text(title, Modifier.padding(horizontal = 10.dp), style = MaterialTheme.typography.labelLarge,
            maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun ShelfCard(item: Item, fallback: ImageVector, select: () -> Unit) {
    Column(Modifier.width(140.dp).clip(RoundedCornerShape(6.dp)).pressable(onClick = select).padding(vertical = 8.dp)) {
        Cover(item.artwork, fallback, Modifier.fillMaxWidth().aspectRatio(1f),
            radius = if (item.kind == BuildConfig.ITEM_ARTIST) 70.dp else 6.dp)
        Spacer(Modifier.height(8.dp))
        Text(item.title, style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
        Text(subtitle(item), style = MaterialTheme.typography.bodySmall,
            color = Palette.muted, maxLines = 2, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun ShelfHeading(title: String, showAll: (() -> Unit)?) {
    Row(Modifier.fillMaxWidth().padding(top = 20.dp, bottom = 4.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(title, Modifier.weight(1f).semantics { heading() }, style = MaterialTheme.typography.titleLarge)

        if (showAll != null) {
            TextButton(onClick = showAll) { Text("Show all", color = Palette.muted) }
        }
    }
}

@Composable
internal fun Shelf(title: String, items: List<Item>, empty: String, fallback: ImageVector,
                  showAll: (() -> Unit)?, open: (Item) -> Unit) {
    ShelfHeading(title, showAll.takeIf { items.isNotEmpty() })

    if (items.isEmpty()) {
        Text(empty, style = MaterialTheme.typography.bodyMedium, color = Palette.muted)
        return
    }

    // Artwork lines up with the heading; spacing sits between cards rather than inside them.
    LazyRow(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
        items(items.take(20), key = { it.uri }) { item -> ShelfCard(item, fallback) { open(item) } }
    }
}

/** Desktop Home, as a single scrolling column: shortcuts in two columns, then three shelves. */
@Composable
internal fun HomeScreen(core: CoreState, list: LazyListState, openLiked: () -> Unit, open: (Item) -> Unit,
                        openTab: (String) -> Unit) {
    val (playlistTitle, playlists) = homePlaylistShelf(core.playlistsPage.items)
    val shortcuts = homeShortcuts(core.playlistsPage.items)

    LazyColumn(state = list, contentPadding = PaddingValues(bottom = 24.dp)) {
        items(shortcuts.chunked(2)) { row ->
            Row(Modifier.fillMaxWidth().padding(bottom = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                row.forEach { shortcut ->
                    QuickTile(shortcut, Modifier.weight(1f)) {
                        when (shortcut) {
                            Shortcut.LikedSongs -> openLiked()
                            is Shortcut.Open -> open(shortcut.item)
                            is Shortcut.Tab -> openTab(shortcut.tab)
                        }
                    }
                }

                if (row.size == 1) {
                    Spacer(Modifier.weight(1f))
                }
            }
        }

        item {
            Shelf(playlistTitle, playlists, "Your playlists will appear here.", Icons.AutoMirrored.Filled.QueueMusic,
                { openTab("Library") }, open)
        }
        item {
            val empty = if (core.recentPermissionRequired) "Log in again to allow recently played."
                else if (core.recentLoaded) "Albums you listen to will appear here." else "Loading…"
            Shelf("Recently played", core.recent, empty, Icons.Filled.Album, null, open)
        }
        item {
            val empty = if (core.savedAlbums.loaded) "Save albums to find them here." else "Loading…"
            Shelf("Saved albums", core.savedAlbums.items, empty, Icons.Filled.Album, { openTab("Library") }, open)
        }
    }
}

/** The desktop transport bar, as a compact bar above the tabs. Tapping it opens Now Playing. */
@Composable
internal fun MiniPlayer(title: String, subtitle: String, image: String, playing: Boolean, progress: Float, changingTrack: Boolean,
                        deviceIcon: Int, remote: Boolean, open: () -> Unit, devices: () -> Unit, toggle: () -> Unit) {
    val played = animateFloatAsState(progress.coerceIn(0f, 1f), label = "progress").value

    Column(Modifier.fillMaxWidth().padding(horizontal = 8.dp).clip(RoundedCornerShape(8.dp))
        .background(Palette.tile).pressable(label = "Open Now Playing", onClick = open)) {
        Row(Modifier.fillMaxWidth().height(56.dp).padding(start = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Cover(image, Icons.Filled.MusicNote, Modifier.size(40.dp), radius = 4.dp)
            Column(Modifier.weight(1f).padding(horizontal = 10.dp)) {
                Text(title, style = MaterialTheme.typography.labelLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)

                // Like Spotify, playback on another device shows that device in green.
                if (subtitle.isNotBlank()) {
                    Text(subtitle, style = MaterialTheme.typography.bodySmall, color = if (remote) Palette.accent else Palette.muted,
                        maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
            Box(Modifier.size(48.dp).clip(CircleShape).clickable(role = Role.Button, onClickLabel = "Devices", onClick = devices)
                .semantics { contentDescription = "Devices" }, contentAlignment = Alignment.Center) {
                DesktopIcon(deviceIcon, 20.dp, if (remote) Palette.accent else Palette.text)
            }
            Box(Modifier.size(48.dp).clip(CircleShape).clickable(role = Role.Button, onClick = toggle)
                .semantics { contentDescription = when {
                    changingTrack -> "Switching track"
                    playing -> "Pause"
                    else -> "Play"
                } }, contentAlignment = Alignment.Center) {
                if (changingTrack) {
                    CircularProgressIndicator(Modifier.size(18.dp), color = Palette.text, strokeWidth = 2.dp)
                } else {
                    DesktopIcon(if (playing) BuildConfig.ICON_PAUSE else BuildConfig.ICON_PLAY, 20.dp, Palette.text)
                }
            }
        }

        // Played portion as a thin line, like the desktop scrubber at rest.
        Box(Modifier.fillMaxWidth().padding(horizontal = 8.dp).height(2.dp).clip(CircleShape)
            .background(Palette.control).clearAndSetSemantics { }) {
            Box(Modifier.fillMaxWidth(played).fillMaxHeight().background(Palette.text))
        }
    }
}

private data class TabSpec(val key: String, val label: String, val selected: ImageVector, val unselected: ImageVector)

private val tabs = listOf(
    TabSpec("Home", "Home", Icons.Filled.Home, Icons.Outlined.Home),
    TabSpec("Search", "Search", Icons.Filled.Search, Icons.Outlined.Search),
    TabSpec("Library", "Your Library", Icons.Filled.LibraryMusic, Icons.Outlined.LibraryMusic),
)

/**
 * Home, Search and Library. Each tab is one button: icon and label share the touch target,
 * the ripple and the selected highlight.
 */
@Composable
internal fun BottomTabs(tab: String, select: (String) -> Unit) {
    Row(Modifier.fillMaxWidth().background(Palette.body).navigationBarsPadding().padding(horizontal = 8.dp, vertical = 6.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        tabs.forEach { spec ->
            val active = tab == spec.key
            val color by animateColorAsState(if (active) Palette.text else Palette.muted, label = "tab")
            val highlight by animateColorAsState(if (active) Palette.selected else Color.Transparent, label = "highlight")

            Column(Modifier.weight(1f).heightIn(min = 56.dp).clip(RoundedCornerShape(16.dp)).background(highlight)
                .selectable(selected = active, role = Role.Tab, onClick = { select(spec.key) }).padding(vertical = 6.dp),
                horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
                Icon(if (active) spec.selected else spec.unselected, contentDescription = null, tint = color)
                Spacer(Modifier.height(2.dp))
                Text(spec.label, color = color, style = MaterialTheme.typography.labelMedium, maxLines = 1)
            }
        }
    }
}

/** The account picture, or its initial; it opens the account menu. */
@Composable
internal fun Avatar(profile: CoreProfile, size: Dp = 32.dp, onClick: () -> Unit) {
    IconButton(onClick = onClick) {
        Box(Modifier.size(size).clip(CircleShape).background(Palette.accent), contentAlignment = Alignment.Center) {
            Text(profile.name.take(1).uppercase().ifBlank { "?" }, color = Color.Black,
                style = MaterialTheme.typography.labelLarge)
            val image = profile.artwork

            if (image.isNotBlank()) {
                AsyncImage(image, contentDescription = "Account menu", modifier = Modifier.fillMaxSize(),
                    contentScale = ContentScale.Crop)
            }
        }
    }
}

/** The muted line under an item, as on desktop: artists for songs, owner for playlists. */
internal fun subtitle(item: Item): String {
    val people = listOf(item.detail, item.guests).filter(String::isNotBlank).joinToString(", ")

    return when (item.kind) {
        BuildConfig.ITEM_TRACK, BuildConfig.ITEM_EPISODE -> people
        BuildConfig.ITEM_ALBUM -> listOf(item.albumType.replaceFirstChar(Char::uppercase).ifBlank { "Album" }, people)
            .filter(String::isNotBlank).joinToString(" · ")
        BuildConfig.ITEM_PLAYLIST -> listOf("Playlist", item.detail).filter(String::isNotBlank).joinToString(" · ")
        else -> "Artist"
    }
}

internal fun fallbackIcon(item: Item): ImageVector = when (item.kind) {
    BuildConfig.ITEM_ALBUM -> Icons.Filled.Album
    BuildConfig.ITEM_PLAYLIST -> Icons.AutoMirrored.Filled.QueueMusic
    BuildConfig.ITEM_ARTIST -> Icons.Filled.Person
    else -> Icons.Filled.MusicNote
}

/** One row for any catalog item; artists get round artwork like the desktop. */
@Composable
internal fun ItemRow(item: Item?, index: Int = -1, actions: ((Item) -> Unit)? = null, select: (Item) -> Unit) {
    // Rows outside the core's current window are placeholders until the next snapshot arrives.
    if (item == null) {
        Row(Modifier.fillMaxWidth().height(64.dp).padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
            Box(Modifier.size(52.dp).clip(RoundedCornerShape(4.dp)).background(Palette.tile))
        }
        return
    }

    val dimmed = item.kind == BuildConfig.ITEM_TRACK && !item.playable
    val round = item.kind == BuildConfig.ITEM_ARTIST

    Row(Modifier.fillMaxWidth().heightIn(min = 64.dp).clip(RoundedCornerShape(6.dp))
        .pressable(onLongClick = actions?.let { { it(item) } }, onClick = { select(item) }).padding(vertical = 6.dp)
        .graphicsLayer { alpha = if (dimmed) 0.5f else 1f },
        horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
        if (index >= 0) {
            Text("${index + 1}", Modifier.width(28.dp), style = MaterialTheme.typography.bodyMedium, color = Palette.muted,
                maxLines = 1)
        } else {
            Cover(item.artwork, fallbackIcon(item), Modifier.size(52.dp), radius = if (round) 26.dp else 4.dp)
        }
        Column(Modifier.weight(1f)) {
            Text(item.title, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (item.explicit) {
                    Box(Modifier.padding(end = 6.dp).clip(RoundedCornerShape(2.dp)).background(Palette.muted)
                        .padding(horizontal = 4.dp)) {
                        Text("E", style = MaterialTheme.typography.labelSmall, color = Palette.body)
                    }
                }
                Text(subtitle(item), style = MaterialTheme.typography.bodyMedium, color = Palette.muted,
                    maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }

        if (item.liked) {
            Icon(Icons.Filled.Favorite, contentDescription = "Liked", tint = Palette.accent, modifier = Modifier.size(18.dp))
        }

        if (actions != null) {
            IconButton(onClick = { actions(item) }) {
                Icon(Icons.Filled.MoreVert, contentDescription = "More actions for ${item.title}", tint = Palette.muted)
            }
        }
    }
}
