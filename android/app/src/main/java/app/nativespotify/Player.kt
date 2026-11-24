package app.nativespotify

import android.graphics.Bitmap
import android.graphics.drawable.BitmapDrawable
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.basicMarquee
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.unit.Dp
import coil.imageLoader
import coil.request.ImageRequest
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.key
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.abs
import kotlinx.coroutines.delay

private fun clockText(ms: Int) = "${ms / 60_000}:${(ms / 1_000 % 60).toString().padStart(2, '0')}"

/** The position, moving smoothly between snapshots while something plays. */
@Composable
internal fun livePosition(playback: PlaybackInfo, step: Long = 500): Int {
    var position by remember(playback.uri, playback.deviceId) { mutableStateOf(playback.position()) }

    LaunchedEffect(playback) {
        if (playback.changingTrack) return@LaunchedEffect

        position = playback.position()

        while (playback.playing && !playback.changingTrack && !playback.seeking) {
            delay(step)
            position = playback.position()
        }
    }

    return position
}

/** Where playback comes from, as the desktop queue panel names it. */
internal fun playingFrom(core: CoreState): String {
    val uri = core.playback.contextUri

    return when {
        uri.startsWith("spotify:playlist:") -> core.playlistsPage.items.firstOrNull { it.id == uri.removePrefix("spotify:playlist:") }?.title.orEmpty()
        uri.startsWith("spotify:album:") -> core.playback.album
        else -> ""
    }
}

/** The device icon the desktop uses for each kind of Spotify Connect device; this phone shows a phone. */
internal fun deviceIcon(device: DeviceInfo?): Int = when {
    device == null -> BuildConfig.ICON_SPEAKER
    device.here -> BuildConfig.ICON_PHONE
    device.type == "Computer" -> BuildConfig.ICON_DEVICE
    device.type == "Smartphone" || device.type == "Tablet" -> BuildConfig.ICON_PHONE
    else -> BuildConfig.ICON_SPEAKER
}

/** The device playback is on, as the desktop panel finds it: the playing device, else the active one. */
internal fun currentDevice(core: CoreState): DeviceInfo? {
    val items = core.devices.items
    return items.firstOrNull { core.playback.hasDevice && it.id == core.playback.deviceId } ?: items.firstOrNull { it.active }
}

internal fun deviceName(core: CoreState, device: DeviceInfo?): String = when {
    device?.here == true -> "This phone"
    device != null -> device.name
    core.playback.deviceId.isNotEmpty() && core.playback.deviceId == core.devices.engineId -> "This phone"
    else -> core.playback.device
}

/** A darker, richer version of the artwork's average colour, for the Now Playing backdrop. */
@Composable
private fun artworkColor(url: String): Color {
    val context = LocalContext.current
    var color by remember { mutableStateOf(Palette.tile) }

    LaunchedEffect(url) {
        if (url.isBlank()) {
            color = Palette.tile
            return@LaunchedEffect
        }

        val result = context.imageLoader.execute(ImageRequest.Builder(context).data(url).allowHardware(false).size(64).build())
        val bitmap = (result.drawable as? BitmapDrawable)?.bitmap ?: return@LaunchedEffect
        val pixel = Bitmap.createScaledBitmap(bitmap, 1, 1, true).getPixel(0, 0)
        val hsv = FloatArray(3)
        android.graphics.Color.colorToHSV(pixel, hsv)
        // Grey artwork stays grey; only coloured artwork is made richer.
        if (hsv[1] > 0.15f) {
            hsv[1] = hsv[1].coerceAtLeast(0.35f)
        }

        hsv[2] = 0.42f
        color = Color(android.graphics.Color.HSVToColor(hsv))
    }

    return animateColorAsState(color, tween(600), label = "backdrop").value
}

/** A round button around a desktop icon, with a 48 dp touch target. */
@Composable
private fun IconControl(icon: Int, label: String, size: Dp, tint: Color, enabled: Boolean = true, dot: Boolean = false,
                        badge: String? = null, rotation: Float = 0f, onClick: () -> Unit) {
    Box(Modifier.size(48.dp).clip(CircleShape).clickable(enabled = enabled, role = Role.Button, onClickLabel = label, onClick = onClick)
        .semantics { contentDescription = label }, contentAlignment = Alignment.Center) {
        DesktopIcon(icon, size, if (enabled) tint else Palette.muted.copy(alpha = 0.4f), Modifier.rotate(rotation))

        // Like the desktop: a green dot under an active mode, and a "1" inside repeat-one.
        if (badge != null) {
            Text(badge, fontSize = 8.sp, fontWeight = FontWeight.Bold, color = tint)
        }

        if (dot) {
            Box(Modifier.align(Alignment.BottomCenter).padding(bottom = 6.dp).size(4.dp).clip(CircleShape).background(tint))
        }
    }
}

/** The desktop's thin slider, sized for touch: a line with a knob that turns green while dragged. */
@Composable
private fun ThinSlider(fraction: Float, enabled: Boolean, label: String, scrub: (Float?) -> Unit, seek: (Float) -> Unit) {
    var drag by remember { mutableStateOf<Float?>(null) }
    val currentSeek by rememberUpdatedState(seek)
    val currentScrub by rememberUpdatedState(scrub)
    val shown = (drag ?: fraction).coerceIn(0f, 1f)

    Box(Modifier.fillMaxWidth().height(28.dp)
        .pointerInput(enabled) {
            if (!enabled) return@pointerInput
            awaitEachGesture {
                val down = awaitFirstDown()
                try {
                    while (true) {
                        val change = awaitPointerEvent().changes.firstOrNull { it.id == down.id } ?: break
                        if (change.isConsumed) break
                        val fraction = (change.position.x / size.width).coerceIn(0f, 1f)
                        if (!change.pressed) {
                            change.consume()
                            currentSeek(fraction)
                            break
                        }

                        val delta = change.position - down.position
                        val slop = viewConfiguration.touchSlop
                        if (drag == null && abs(delta.y) > slop && abs(delta.y) > abs(delta.x)) break
                        if (drag != null || abs(delta.x) > slop) {
                            change.consume()
                            drag = fraction
                            currentScrub(fraction)
                        }
                    }
                } finally {
                    drag = null
                    currentScrub(null)
                }
            }
        }
        .semantics {
            contentDescription = label
            progressBarRangeInfo = ProgressBarRangeInfo(shown, 0f..1f)
            if (enabled) setProgress { currentSeek(it.coerceIn(0f, 1f)); true }
        },
        contentAlignment = Alignment.CenterStart) {
        Box(Modifier.fillMaxWidth().height(4.dp).clip(CircleShape).background(Color.White.copy(alpha = 0.25f)))
        Box(Modifier.fillMaxWidth(shown).height(4.dp).clip(CircleShape)
            .background(if (drag != null) Palette.accent else Palette.text))

        if (enabled) {
            BoxWithConstraints(Modifier.fillMaxWidth()) {
                val knob = if (drag != null) 16.dp else 12.dp
                Box(Modifier.offset(x = maxWidth * shown - knob / 2).size(knob).clip(CircleShape).background(Palette.text))
            }
        }
    }
}

/**
 * Now Playing, polished like the Spotify app but drawn with the desktop's icons and driven by
 * the core: backdrop from the artwork, the like heart, the seek bar, transport, devices, lyrics
 * and queue.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
internal fun NowPlayingScreen(core: CoreState, back: () -> Unit, openArtist: (Item) -> Unit, openScreen: (String) -> Unit,
                              more: (Item) -> Unit, devices: () -> Unit) {
    val playback = core.playback
    val position = livePosition(playback, step = 150)
    var scrub by remember(playback.uri, playback.deviceId, playback.changingTrack) { mutableStateOf<Float?>(null) }
    var releasedSeek by remember(playback.uri, playback.deviceId, playback.changingTrack, playback.revision) {
        mutableStateOf<Int?>(null)
    }
    LaunchedEffect(releasedSeek) {
        if (releasedSeek != null) {
            delay(500)
            releasedSeek = null
        }
    }

    val track = playback.item()
    val hasTrack = playback.uri.isNotEmpty()
    val backdrop = artworkColor(playback.artwork)
    val device = currentDevice(core)
    val remote = playback.hasDevice && device?.here != true && playback.deviceId != core.devices.engineId

    Column(Modifier.fillMaxSize().background(Brush.verticalGradient(listOf(backdrop, lerp(backdrop, Palette.body, 0.75f), Palette.body)))
        .statusBarsPadding().navigationBarsPadding().padding(horizontal = 24.dp)) {
        Row(Modifier.fillMaxWidth().height(56.dp), verticalAlignment = Alignment.CenterVertically) {
            // The desktop has no down chevron; its right chevron turned down reads the same.
            IconControl(BuildConfig.ICON_CHEVRON_RIGHT, "Close Now Playing", 22.dp, Palette.text, rotation = 90f, onClick = back)
            val source = playingFrom(core)
            Column(Modifier.weight(1f), horizontalAlignment = Alignment.CenterHorizontally) {
                Text(if (source.isNotEmpty()) "PLAYING FROM" else "NOW PLAYING", fontSize = 11.sp, letterSpacing = 1.sp,
                    color = Palette.text.copy(alpha = 0.8f))
                if (source.isNotEmpty()) {
                    Text(source, fontSize = 13.sp, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
            IconControl(BuildConfig.ICON_MORE, "More actions", 22.dp, Palette.text, enabled = track != null) {
                track?.let(more)
            }
        }

        Spacer(Modifier.weight(1f))
        Box(Modifier.fillMaxWidth().aspectRatio(1f).shadow(24.dp, RoundedCornerShape(8.dp))) {
            Cover(playback.artwork, Icons.Filled.MusicNote, Modifier.fillMaxSize(), radius = 8.dp)
        }
        Spacer(Modifier.weight(1f))

        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(playback.title.ifBlank { "Nothing playing" }, Modifier.basicMarquee(), fontSize = 22.sp,
                    fontWeight = FontWeight.Bold, maxLines = 1)
                Text(playback.artist.ifBlank { miniSubtitle(core) }, fontSize = 16.sp, color = Palette.text.copy(alpha = 0.7f),
                    maxLines = 1, overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.clickable(enabled = playback.artistId.isNotEmpty()) {
                        openArtist(itemStub(BuildConfig.ITEM_ARTIST, playback.artistId, playback.artist, ""))
                    })
            }

            if (track != null) {
                IconControl(if (playback.liked) BuildConfig.ICON_HEART else BuildConfig.ICON_HEART_OUTLINE,
                    if (playback.liked) "Remove from Liked Songs" else "Save to Liked Songs", 24.dp,
                    if (playback.liked) Palette.accent else Palette.text) { Core.toggleLike(track) }
            }
        }

        Spacer(Modifier.height(12.dp))
        val duration = playback.durationMs.coerceAtLeast(1)
        val shown = releasedSeek ?: position
        key(playback.uri, playback.deviceId, playback.changingTrack) {
            ThinSlider(shown.toFloat() / duration, playback.canSeek && !playback.changingTrack, "Song position", { scrub = it }) {
                val requested = (it * duration).toInt().coerceIn(0, duration - 1)
                releasedSeek = requested
                Core.seekLyric(playback.uri, requested)
            }
        }
        Row(Modifier.fillMaxWidth()) {
            Text(clockText(scrub?.let { (it * duration).toInt() } ?: shown), fontSize = 12.sp,
                color = Palette.text.copy(alpha = 0.7f))
            if (playback.seeking) {
                Text("Seeking…", Modifier.padding(start = 8.dp), fontSize = 12.sp, color = Palette.muted)
            }
            Spacer(Modifier.weight(1f))
            Text(clockText(playback.durationMs), fontSize = 12.sp, color = Palette.text.copy(alpha = 0.7f))
        }

        Spacer(Modifier.height(8.dp))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
            IconControl(BuildConfig.ICON_SHUFFLE, if (playback.shuffle) "Shuffle on" else "Shuffle off", 22.dp,
                if (playback.shuffle) Palette.accent else Palette.text, enabled = hasTrack, dot = playback.shuffle) {
                Core.action(BuildConfig.ACTION_SHUFFLE)
            }
            IconControl(BuildConfig.ICON_PREVIOUS, "Previous", 30.dp, Palette.text, enabled = hasTrack && playback.canPrevious) {
                Core.action(BuildConfig.ACTION_PREVIOUS)
            }
            Box(Modifier.size(68.dp).clip(CircleShape).background(Palette.text)
                .clickable(role = Role.Button) { Core.action(BuildConfig.ACTION_TOGGLE_PLAYBACK) }
                .semantics { contentDescription = when {
                    playback.loading -> "Switching track"
                    playback.playing -> "Pause"
                    else -> "Play"
                } },
                contentAlignment = Alignment.Center) {
                if (playback.loading) {
                    CircularProgressIndicator(Modifier.size(24.dp), color = Color.Black, strokeWidth = 2.dp)
                } else {
                    DesktopIcon(if (playback.playing) BuildConfig.ICON_PAUSE else BuildConfig.ICON_PLAY, 28.dp, Color.Black)
                }
            }
            IconControl(BuildConfig.ICON_NEXT, "Next", 30.dp, Palette.text, enabled = hasTrack && playback.canNext) {
                Core.action(BuildConfig.ACTION_NEXT)
            }
            val repeating = playback.repeat != BuildConfig.REPEAT_OFF
            IconControl(BuildConfig.ICON_REPEAT, when (playback.repeat) {
                BuildConfig.REPEAT_TRACK -> "Repeat song"
                BuildConfig.REPEAT_CONTEXT -> "Repeat all"
                else -> "Repeat off"
            }, 22.dp, if (repeating) Palette.accent else Palette.text, enabled = hasTrack, dot = repeating,
                badge = if (playback.repeat == BuildConfig.REPEAT_TRACK) "1" else null) {
                Core.action(BuildConfig.ACTION_REPEAT)
            }
        }

        Spacer(Modifier.height(8.dp))
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Row(Modifier.weight(1f).clip(RoundedCornerShape(8.dp)).clickable(role = Role.Button, onClick = devices)
                .padding(vertical = 12.dp, horizontal = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                DesktopIcon(deviceIcon(device), 18.dp, if (remote) Palette.accent else Palette.text)
                Spacer(Modifier.width(8.dp))
                Text(when {
                    playback.hasDevice -> deviceName(core, device)
                    core.engineMessage.isNotEmpty() -> core.engineMessage
                    else -> "Devices"
                }, fontSize = 12.sp, color = if (remote) Palette.accent else Palette.text.copy(alpha = 0.8f), maxLines = 1,
                    overflow = TextOverflow.Ellipsis)
            }
            IconControl(BuildConfig.ICON_LYRICS, "Lyrics", 20.dp, Palette.text, enabled = hasTrack) { openScreen("Lyrics") }
            IconControl(BuildConfig.ICON_QUEUE, "Queue", 20.dp, Palette.text) { openScreen("Queue") }
        }
    }
}

/**
 * The desktop devices panel as a bottom sheet: what plays where, its controls and volume, and
 * the other Spotify Connect devices to move playback to.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun DeviceSheet(core: CoreState, setupReady: Boolean, openSetup: () -> Unit, dismiss: () -> Unit) {
    val playback = core.playback
    val current = currentDevice(core)
    val state = rememberModalBottomSheetState(skipPartiallyExpanded = true)

    // The core lists devices only while someone is looking at them, as with the desktop panel.
    DisposableEffect(Unit) {
        Core.devicesVisible(true)
        onDispose { Core.devicesVisible(false) }
    }

    ModalBottomSheet(onDismissRequest = dismiss, sheetState = state, containerColor = Palette.sheet) {
        LazyColumn(Modifier.navigationBarsPadding(), contentPadding = PaddingValues(start = 16.dp, end = 16.dp, bottom = 16.dp)) {
            item {
                Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(8.dp)).background(Palette.tile).padding(16.dp)) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        DesktopIcon(if (playback.hasDevice) deviceIcon(current) else BuildConfig.ICON_SPEAKER, 28.dp,
                            if (playback.hasDevice) Palette.accent else Palette.muted)
                        Spacer(Modifier.width(14.dp))
                        Column(Modifier.weight(1f)) {
                            if (!playback.hasDevice) {
                                Text("No active device", fontSize = 15.sp, fontWeight = FontWeight.Bold)
                                Text("Choose a device below to start listening.", fontSize = 13.sp, color = Palette.muted)
                            } else {
                                Text(if (playback.playing) "Playing on" else "Current device", fontSize = 12.sp, color = Palette.muted)
                                Text(deviceName(core, current), fontSize = 16.sp, fontWeight = FontWeight.Bold, color = Palette.accent,
                                    maxLines = 1, overflow = TextOverflow.Ellipsis)
                                if (playback.title.isNotEmpty()) {
                                    Text(listOf(playback.title, playback.artist).filter(String::isNotEmpty).joinToString(" · "),
                                        fontSize = 13.sp, color = Palette.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                }
                            }
                        }
                    }

                    if (playback.hasDevice) {
                        Spacer(Modifier.height(12.dp))
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
                            IconControl(BuildConfig.ICON_PREVIOUS, "Previous", 20.dp, Palette.text, enabled = playback.canPrevious) {
                                Core.action(BuildConfig.ACTION_PREVIOUS)
                            }
                            Spacer(Modifier.width(16.dp))
                            Box(Modifier.size(48.dp).clip(CircleShape).background(Palette.text)
                                .clickable(role = Role.Button) { Core.action(BuildConfig.ACTION_TOGGLE_PLAYBACK) }
                                .semantics { contentDescription = if (playback.playing) "Pause" else "Play" },
                                contentAlignment = Alignment.Center) {
                                DesktopIcon(if (playback.playing) BuildConfig.ICON_PAUSE else BuildConfig.ICON_PLAY, 20.dp, Palette.body)
                            }
                            Spacer(Modifier.width(16.dp))
                            IconControl(BuildConfig.ICON_NEXT, "Next", 20.dp, Palette.text, enabled = playback.canNext) {
                                Core.action(BuildConfig.ACTION_NEXT)
                            }
                        }
                    }

                    if (playback.canVolume && playback.volume >= 0) {
                        var draft by remember { mutableStateOf<Float?>(null) }
                        val level = draft ?: (playback.volume / 100f)
                        Spacer(Modifier.height(8.dp))
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            DesktopIcon(if (level <= 0f) BuildConfig.ICON_MUTE else BuildConfig.ICON_VOLUME, 18.dp, Palette.muted)
                            Spacer(Modifier.width(12.dp))
                            Box(Modifier.weight(1f)) {
                                ThinSlider(playback.volume / 100f, true, "Volume", { draft = it }) { Core.volume((it * 100).toInt()) }
                            }
                        }
                    }
                }
            }

            item {
                Text("Select another device", Modifier.padding(top = 20.dp, bottom = 8.dp).semantics { heading() },
                    fontSize = 15.sp, fontWeight = FontWeight.Bold, color = Palette.text)
            }

            // This phone appears here whenever it is not the device already playing.
            val others = core.devices.items.filter { it.id != current?.id || !playback.hasDevice }
            val herePlaying = playback.hasDevice && current?.here == true
            val hereListed = others.any { it.here }

            if (!herePlaying && !hereListed) {
                item {
                    DeviceRow(BuildConfig.ICON_PHONE, "This phone",
                        if (setupReady) "Play here with Spotify Soloist" else "Set up Spotify Soloist to play here", true) {
                        if (setupReady) Core.transferHere() else openSetup()
                    }
                }
            }

            items(others, key = { it.id }) { device ->
                val detail = when {
                    device.restricted -> "Can't be controlled from this app"
                    device.here -> "This app"
                    else -> "Spotify Connect"
                }
                DeviceRow(deviceIcon(device), if (device.here) "This phone" else device.name, detail, device.canTransfer) {
                    Core.transferTo(device)
                }
            }

            if (others.isEmpty() && herePlaying) {
                item {
                    Text("No other devices found. Open Spotify on a computer, speaker or TV to see it here.",
                        fontSize = 13.sp, color = Palette.muted)
                }
            }

            // Only while Soloist runs (or is starting) on this phone is there anything to stop.
            if (core.engine in setOf(BuildConfig.ENGINE_STARTING, BuildConfig.ENGINE_PAIRING, BuildConfig.ENGINE_READY)) {
                item {
                    TextButton(onClick = { Core.engineStop(); dismiss() }, Modifier.padding(top = 12.dp)) {
                        Text("Stop playing on this phone", color = Palette.muted)
                    }
                }
            }
        }
    }
}

@Composable
private fun DeviceRow(icon: Int, name: String, detail: String, enabled: Boolean, select: () -> Unit) {
    val color = if (enabled) Palette.text else Palette.muted.copy(alpha = 0.5f)

    Row(Modifier.fillMaxWidth().clip(RoundedCornerShape(6.dp)).clickable(enabled = enabled, role = Role.Button, onClick = select)
        .padding(vertical = 10.dp, horizontal = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        DesktopIcon(icon, 26.dp, color)
        Spacer(Modifier.width(14.dp))
        Column {
            Text(name, fontSize = 15.sp, color = color, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(detail, fontSize = 12.sp, color = Palette.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

/** The desktop queue panel as a screen: what plays now, what you queued, and what comes next. */
@Composable
internal fun QueueScreen(core: CoreState, actions: SongActions) {
    val queue = core.queue
    val playback = core.playback
    val state = rememberLazyListState()

    // The core refreshes the queue only while it is on screen, as with the desktop panel.
    DisposableEffect(Unit) {
        Core.queueVisible(true)
        onDispose { Core.queueVisible(false) }
    }

    LazyColumn(state = state, contentPadding = PaddingValues(bottom = 24.dp)) {
        val source = playingFrom(core)

        if (source.isNotEmpty()) {
            item {
                Row {
                    Text("Playing from ", fontSize = 13.sp, color = Palette.muted)
                    Text(source, fontSize = 13.sp, fontWeight = FontWeight.Bold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        }

        item { QueueHeading("Now playing") }
        item {
            if (playback.title.isEmpty()) {
                Text("Nothing playing", fontSize = 13.sp, color = Palette.muted)
            } else {
                Row(Modifier.fillMaxWidth().padding(vertical = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                    Cover(playback.artwork, Icons.Filled.MusicNote, Modifier.size(48.dp), radius = 4.dp)
                    Column(Modifier.weight(1f).padding(horizontal = 12.dp)) {
                        Text(playback.title, fontSize = 15.sp, color = Palette.accent, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(playback.artist, fontSize = 13.sp, color = Palette.muted, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    if (playback.durationMs > 0) Text(clockText(playback.durationMs), fontSize = 13.sp, color = Palette.muted)
                }
            }
        }

        item {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.weight(1f)) {
                    QueueHeading(when {
                        queue.loading -> "Loading recommendations…"
                        queue.queued > 0 -> "Next in queue"
                        else -> "Next up"
                    })
                }

                if (queue.queued > 0) {
                    TextButton(onClick = Core::clearQueue) { Text("Clear queue", color = Palette.muted) }
                }
            }
        }

        if (queue.loading) {
            items(5) { SongRow(null, actions) {} }
            return@LazyColumn
        }

        if (queue.total == 0) {
            item { Text("No upcoming tracks returned by Spotify.", fontSize = 13.sp, color = Palette.muted) }
        }

        itemsIndexed(queue.items, key = { index, item -> "${queue.first + index}:${item.id}" }) { index, item ->
            val absolute = queue.first + index
            val queued = absolute < queue.queued

            Row(verticalAlignment = Alignment.CenterVertically) {
                // A green edge marks songs you queued yourself, as on the desktop.
                Box(Modifier.width(3.dp).height(36.dp).clip(RoundedCornerShape(2.dp))
                    .background(if (queued) Palette.accent else Color.Transparent))
                Spacer(Modifier.width(8.dp))
                Box(Modifier.weight(1f)) {
                    SongRow(item, actions) { if (queued) Core.playQueued(it) else Core.playUpcoming(it) }
                }

                if (queued) {
                    IconControl(BuildConfig.ICON_CLOSE, "Remove ${item.title} from queue", 16.dp, Palette.muted) {
                        Core.dequeue(item)
                    }
                }
            }
        }
    }
}

@Composable
private fun QueueHeading(text: String) {
    Text(text, Modifier.padding(top = 20.dp, bottom = 8.dp).semantics { heading() }, fontSize = 16.sp, fontWeight = FontWeight.Bold)
}

/** Synced lyrics from the core; the core picks the highlighted line and a tap seeks to it. */
@Composable
internal fun LyricsScreen(core: CoreState) {
    val lyrics = core.lyrics
    val playback = core.playback
    val list = rememberLazyListState()
    var follow by remember { mutableStateOf(true) }

    DisposableEffect(Unit) {
        Core.lyricsVisible(true)
        onDispose { Core.lyricsVisible(false) }
    }

    LaunchedEffect(lyrics.active, follow) {
        if (follow && lyrics.active >= 0) {
            list.animateScrollToItem((lyrics.active - 2).coerceAtLeast(0))
        }
    }

    when (lyrics.state) {
        BuildConfig.LYRICS_SYNCED -> Column {
            TextButton(onClick = { follow = !follow }) { Text(if (follow) "Stop following" else "Follow playback") }
            LazyColumn(state = list, verticalArrangement = Arrangement.spacedBy(6.dp), contentPadding = PaddingValues(bottom = 48.dp)) {
                itemsIndexed(lyrics.lines) { index, line ->
                    val active = index == lyrics.active
                    Text(line.text.ifBlank { "♪" }, Modifier.fillMaxWidth().clip(RoundedCornerShape(6.dp))
                        .clickable { Core.seekLyric(playback.uri, line.timeMs) }.padding(vertical = 8.dp),
                        fontSize = 24.sp, fontWeight = FontWeight.Bold,
                        color = when {
                            active -> Palette.text
                            index < lyrics.active -> Palette.muted
                            else -> Palette.muted.copy(alpha = 0.6f)
                        })
                }
            }
        }
        BuildConfig.LYRICS_PLAIN -> LazyColumn(contentPadding = PaddingValues(bottom = 48.dp)) {
            item { Text(lyrics.text, fontSize = 20.sp, lineHeight = 30.sp) }
        }
        else -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            Text(when {
                playback.uri.isEmpty() -> "Play a song first."
                lyrics.state == BuildConfig.LYRICS_INSTRUMENTAL -> "Instrumental"
                lyrics.state == BuildConfig.LYRICS_ERROR -> "Could not load lyrics."
                lyrics.state == BuildConfig.LYRICS_MISSING -> "Lyrics unavailable."
                else -> "Finding lyrics…"
            }, style = MaterialTheme.typography.titleMedium, color = Palette.muted)
        }
    }
}

/** The mini-player's line under the title: the artist, or what the engine is doing. */
internal fun miniSubtitle(core: CoreState): String = when {
    core.playback.artist.isNotEmpty() && core.playback.hasDevice -> core.playback.artist
    core.engineMessage.isNotEmpty() -> core.engineMessage
    core.status.isNotEmpty() -> core.status
    else -> core.playback.artist
}
