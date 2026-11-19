package app.nativespotify

import androidx.compose.animation.AnimatedContentTransitionScope
import androidx.compose.animation.ContentTransform
import androidx.compose.animation.core.CubicBezierEasing
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.animation.slideOutVertically
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.PlaylistAdd
import androidx.compose.material.icons.filled.Album
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.FavoriteBorder
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Person
import androidx.compose.material.icons.filled.Radio
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Timer
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject

/** Where the app is: a bottom tab, the page on top of it, and how deep that page is. */
internal data class Nav(val tab: String, val screen: String, val depth: Int = 0)

/**
 * A page opened on top of a tab. `request` says how to ask the core for the page's data
 * again when you come back to it: open an item, browse a library kind, and so on.
 */
internal data class Destination(val screen: String, val request: String = "", val item: String = "") {
    /** Asks the core for this page's list again; the core serves it from its cache when it can. */
    fun replay() {
        val target = itemFromJson(item)

        when {
            request == "open" && target != null -> Core.open(target)
            request == "likes" && target != null -> Core.artistLikes(target)
            request == "radio" && target != null -> Core.songRadio(target)
            request.startsWith("browse:") -> request.removePrefix("browse:").toIntOrNull()?.let(Core::browse)
        }
    }
}

internal fun encodeStacks(stacks: Map<String, List<Destination>>): String = JSONObject(stacks.mapValues { (_, stack) ->
    JSONArray(stack.map { JSONObject().put("screen", it.screen).put("request", it.request).put("item", it.item) })
}).toString()

internal fun decodeStacks(text: String): Map<String, List<Destination>> = runCatching {
    val root = JSONObject(text)
    root.keys().asSequence().associateWith { tab ->
        val array = root.getJSONArray(tab)
        (0 until array.length()).map { index ->
            val entry = array.getJSONObject(index)
            Destination(entry.getString("screen"), entry.optString("request"), entry.optString("item"))
        }
    }
}.getOrDefault(emptyMap())

/** Material 3's emphasized easing, used by the platform for navigation. */
private val emphasized = CubicBezierEasing(0.2f, 0f, 0f, 1f)

private const val PLAYER = "Now Playing"

/**
 * Android-style motion between destinations: tabs fade through, pages slide in from the
 * trailing edge and back out, and Now Playing rises from the mini-player.
 */
internal fun AnimatedContentTransitionScope<Nav>.navTransition(from: Nav, to: Nav, back: Boolean): ContentTransform {
    val slow = tween<Float>(350, easing = emphasized)

    // Switching tabs: fade through, the new tab settling in from slightly smaller.
    if (from.tab != to.tab || (from.depth == 0 && to.depth == 0)) {
        return (fadeIn(tween(220, delayMillis = 90)) + scaleIn(tween(220, delayMillis = 90), initialScale = 0.96f))
            .togetherWith(fadeOut(tween(90)))
    }

    if (to.screen == PLAYER && !back) {
        return (slideInVertically(tween(400, easing = emphasized)) { it } + fadeIn(tween(150)))
            .togetherWith(fadeOut(tween(250)))
    }

    if (from.screen == PLAYER && back) {
        return fadeIn(tween(250)).togetherWith(slideOutVertically(tween(350, easing = emphasized)) { it } + fadeOut(slow))
            .apply { targetContentZIndex = -1f }
    }

    if (back) {
        return (slideInHorizontally(tween(350, easing = emphasized)) { -it / 4 } + fadeIn(slow))
            .togetherWith(slideOutHorizontally(tween(350, easing = emphasized)) { it / 3 } + fadeOut(tween(200)))
            .apply { targetContentZIndex = -1f }
    }

    return (slideInHorizontally(tween(350, easing = emphasized)) { it / 3 } + fadeIn(slow))
        .togetherWith(slideOutHorizontally(tween(350, easing = emphasized)) { -it / 4 } + fadeOut(tween(200)))
}

@Composable
private fun SheetAction(icon: ImageVector, label: String, enabled: Boolean = true, select: () -> Unit) {
    ListItem(
        headlineContent = { Text(label) },
        leadingContent = { Icon(icon, contentDescription = null) },
        colors = ListItemDefaults.colors(containerColor = Palette.sheet,
            headlineColor = if (enabled) Palette.text else Palette.muted,
            leadingIconColor = if (enabled) Palette.text else Palette.muted),
        modifier = Modifier.pressable(onClick = { if (enabled) select() }),
    )
}

/** Song actions as a bottom sheet, the Android counterpart of the desktop context menu. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun TrackActions(track: Item, dismiss: () -> Unit, addToQueue: () -> Unit, toggleLiked: () -> Unit,
                          startMix: () -> Unit, openAlbum: (() -> Unit)?, openArtist: (() -> Unit)?,
                          sleepTimer: () -> Unit, share: () -> Unit) {
    val state = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val scope = rememberCoroutineScope()

    // Let the sheet slide away before acting, so the next screen doesn't animate under it.
    fun run(action: () -> Unit) {
        scope.launch { state.hide() }.invokeOnCompletion {
            dismiss()
            action()
        }
    }

    ModalBottomSheet(onDismissRequest = dismiss, sheetState = state, containerColor = Palette.sheet) {
        Column(Modifier.navigationBarsPadding().padding(bottom = 8.dp)) {
            Row(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                Cover(track.artwork, Icons.Filled.MusicNote, Modifier.size(48.dp), radius = 4.dp)
                Column(Modifier.weight(1f)) {
                    Text(track.title, style = MaterialTheme.typography.titleMedium, maxLines = 1,
                        overflow = TextOverflow.Ellipsis)
                    Text(subtitle(track), style = MaterialTheme.typography.bodyMedium, color = Palette.muted, maxLines = 1,
                        overflow = TextOverflow.Ellipsis)
                }
            }

            SheetAction(if (track.liked) Icons.Filled.Favorite else Icons.Filled.FavoriteBorder,
                if (track.liked) "Remove from Liked Songs" else "Save to Liked Songs") { run(toggleLiked) }
            SheetAction(Icons.AutoMirrored.Filled.PlaylistAdd, "Add to queue") { run(addToQueue) }
            SheetAction(Icons.Filled.Radio, "Go to song radio") { run(startMix) }
            openAlbum?.let { SheetAction(Icons.Filled.Album, "Go to album") { run(it) } }
            openArtist?.let { SheetAction(Icons.Filled.Person, "Go to artist") { run(it) } }

            SheetAction(Icons.Filled.Timer, "Sleep timer") { run(sleepTimer) }
            SheetAction(Icons.Filled.Share, "Share Spotify link") { run(share) }
        }
    }
}
