package app.nativespotify

import android.graphics.Bitmap
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Desktop palette roles, generated from src/ui/dark_palette.h at build time. */
internal object Palette {
    val body = Color(BuildConfig.DARK_BODY)
    val chrome = Color(BuildConfig.DARK_CHROME)
    val tile = Color(BuildConfig.DARK_TILE)
    val hover = Color(BuildConfig.DARK_HOVER)
    val selected = Color(BuildConfig.DARK_SELECTED)
    val border = Color(BuildConfig.DARK_BORDER)
    val text = Color(BuildConfig.DARK_TEXT)
    val muted = Color(BuildConfig.DARK_MUTED)
    val accent = Color(BuildConfig.DARK_ACCENT)
    val brand = Color(BuildConfig.DARK_BRAND)
    val progress = Color(BuildConfig.DARK_PROGRESS)
    val error = Color(BuildConfig.DARK_ERROR)
    val control = Color(BuildConfig.DARK_CONTROL)
    val sheet = Color(BuildConfig.DARK_SHEET)
}

/** Android's own UI font (Roboto, or the phone maker's system font). */
internal val AppFont = FontFamily.Default

@Composable
internal fun NativeTheme(content: @Composable () -> Unit) {
    val colors = darkColorScheme(
        primary = Palette.accent, onPrimary = Color.Black,
        secondary = Palette.text, onSecondary = Color.Black,
        background = Palette.body, onBackground = Palette.text,
        surface = Palette.chrome, onSurface = Palette.text,
        surfaceVariant = Palette.tile, onSurfaceVariant = Palette.muted,
        secondaryContainer = Palette.selected, onSecondaryContainer = Palette.text,
        outline = Palette.border, outlineVariant = Palette.border,
        // Material 3 surface tiers, from the darkest page to raised sheets and menus.
        surfaceContainerLowest = Palette.body, surfaceContainerLow = Palette.chrome,
        surfaceContainer = Palette.chrome, surfaceContainerHigh = Palette.tile,
        surfaceContainerHighest = Palette.hover,
        error = Palette.error, onError = Color.Black,
    )
    val base = Typography()
    val type = base.copy(
        headlineSmall = base.headlineSmall.copy(fontWeight = FontWeight.Bold),
        titleLarge = base.titleLarge.copy(fontWeight = FontWeight.Bold),
        titleMedium = base.titleMedium.copy(fontWeight = FontWeight.SemiBold),
    )
    MaterialTheme(colorScheme = colors, typography = type, content = content)
}

internal object NativeBrand {
    init { System.loadLibrary("native_spotify_android") }

    /** The desktop window icon, rasterized by the shared C code as ARGB rows. */
    external fun pixels(size: Int): IntArray?
}

@Composable
internal fun BrandLogo(size: Dp) {
    val pixels = with(LocalDensity.current) { size.roundToPx() }.coerceIn(16, 1024)
    val image = remember(pixels) {
        NativeBrand.pixels(pixels)?.let {
            Bitmap.createBitmap(it, pixels, pixels, Bitmap.Config.ARGB_8888).asImageBitmap()
        }
    }

    if (image == null) {
        Box(Modifier.size(size).clip(CircleShape).background(Palette.brand))
        return
    }

    Image(image, contentDescription = null, modifier = Modifier.size(size))
}

private fun style(size: Int, bold: Boolean = false, color: Color = Palette.text) = TextStyle(
    fontFamily = AppFont, fontSize = size.sp, fontWeight = if (bold) FontWeight.Bold else FontWeight.Normal,
    color = color, textAlign = TextAlign.Center)

/** Shown while the saved session is checked and the first page loads, like the desktop intro. */
@Composable
internal fun IntroScreen(authenticated: Boolean) {
    val label = if (authenticated) "Loading your music…" else "Connecting to Spotify…"

    Box(Modifier.fillMaxSize().background(Palette.body).semantics(mergeDescendants = true) {
        contentDescription = "Native Spotify. $label"
    }, contentAlignment = Alignment.Center) {
        Column(Modifier.widthIn(max = 360.dp).fillMaxWidth().padding(horizontal = 16.dp)
            .clip(RoundedCornerShape(20.dp)).background(lerp(Palette.body, Palette.tile, 0.35f))
            .padding(vertical = 36.dp), horizontalAlignment = Alignment.CenterHorizontally) {
            BrandLogo(92.dp)
            Spacer(Modifier.height(18.dp))
            Text("Native Spotify", style = style(26, bold = true))
            Spacer(Modifier.height(6.dp))
            Text(label, style = style(13, color = Palette.muted))
            Spacer(Modifier.height(28.dp))
            LoadingBar()
        }
    }
}

/** The desktop's sliding pill. It only runs while this screen is visible. */
@Composable
private fun LoadingBar() {
    val phase by rememberInfiniteTransition(label = "intro").animateFloat(0f, 1f,
        infiniteRepeatable(tween(1300, easing = LinearEasing), RepeatMode.Restart), label = "pill")

    Canvas(Modifier.width(192.dp).height(4.dp)) {
        val radius = CornerRadius(size.height / 2)
        drawRoundRect(Palette.tile, cornerRadius = radius)

        val pill = 64.dp.toPx()
        val x = -pill / 2 + phase * (size.width + pill / 2)
        val left = x.coerceAtLeast(0f)
        val right = (x + pill).coerceAtMost(size.width)

        if (right > left) {
            drawRoundRect(Palette.progress, Offset(left, 0f), Size(right - left, size.height), radius)
        }
    }
}

@Composable
internal fun LoginScreen(connecting: Boolean, status: String, login: () -> Unit) {
    Box(Modifier.fillMaxSize().background(Palette.body).safeDrawingPadding().verticalScroll(rememberScrollState()),
        contentAlignment = Alignment.Center) {
        Column(Modifier.padding(16.dp).widthIn(max = 480.dp).fillMaxWidth().clip(RoundedCornerShape(16.dp))
            .background(Brush.verticalGradient(listOf(Palette.tile, Palette.body)))
            .padding(horizontal = 32.dp, vertical = 30.dp), horizontalAlignment = Alignment.CenterHorizontally) {
            BrandLogo(48.dp)
            Spacer(Modifier.height(8.dp))
            Text("Native Spotify", style = style(21, bold = true))
            Spacer(Modifier.height(24.dp))
            Text("Log in to Spotify", Modifier.semantics { heading() }, style = style(32, bold = true))
            Spacer(Modifier.height(12.dp))
            Text("Your music and playlists. A lighter native player.", style = style(14, color = Palette.muted))
            Spacer(Modifier.height(28.dp))
            PillButton(if (connecting) "Connecting…" else "Continue with Spotify", filled = true,
                enabled = !connecting, modifier = Modifier.widthIn(max = 320.dp).fillMaxWidth(), onClick = login)
            Spacer(Modifier.height(16.dp))
            Text("Secure sign-in opens in your browser.", style = style(12, color = Palette.muted))

            if (status.isNotBlank()) {
                Spacer(Modifier.height(16.dp))
                Text(status, style = style(12, color = Palette.muted))
            }
        }
    }
}

/** Desktop pill buttons: white with black text when filled, otherwise a dark control pill. */
@Composable
internal fun PillButton(label: String, filled: Boolean, modifier: Modifier = Modifier, enabled: Boolean = true,
                        onClick: () -> Unit) {
    val background = when {
        !enabled -> Palette.control.copy(alpha = 0.6f)
        filled -> Palette.text
        else -> Palette.control
    }
    val foreground = when {
        !enabled -> Palette.muted
        filled -> Color.Black
        else -> Palette.text
    }

    Box(modifier.heightIn(min = 48.dp).clip(CircleShape).background(background)
        .clickable(enabled = enabled, role = Role.Button, onClick = onClick).padding(horizontal = 24.dp),
        contentAlignment = Alignment.Center) {
        Text(label, style = style(14, bold = true, color = foreground))
    }
}

private enum class StepMark { Done, Current, Pending }

@Composable
private fun StepRow(number: Int, title: String, mark: StepMark) {
    val (fill, text) = when (mark) {
        StepMark.Done -> Palette.accent to Color.Black
        StepMark.Current -> Palette.text to Color.Black
        StepMark.Pending -> Palette.control to Palette.muted
    }
    val state = when (mark) {
        StepMark.Done -> "done"
        StepMark.Current -> "current step"
        StepMark.Pending -> "not started"
    }

    Row(Modifier.fillMaxWidth().padding(vertical = 6.dp).semantics(mergeDescendants = true) {
        contentDescription = "Step $number, $title, $state"
    }, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Box(Modifier.size(28.dp).clip(CircleShape).background(fill), contentAlignment = Alignment.Center) {
            Text(if (mark == StepMark.Done) "✓" else "$number", style = style(13, bold = true, color = text))
        }
        Text(title, style = style(15, bold = mark != StepMark.Pending,
            color = if (mark == StepMark.Pending) Palette.muted else Palette.text).copy(textAlign = TextAlign.Start))
    }
}

/** Actions the setup screen asks the activity to perform; they need Android APIs or launchers. */
internal class SetupActions(
    val grantTermux: () -> Unit, val getTermux: () -> Unit, val openTermux: () -> Unit,
    val copySetup: () -> Unit, val getKey: () -> Unit, val pasteKey: () -> Unit, val openSpotify: () -> Unit,
)

/**
 * First-time playback setup, on its own screen rather than mixed into Settings. It follows
 * the desktop "Play on this computer" panel: key first, then the engine, then pairing.
 */
@Composable
internal fun SetupScreen(setup: EngineSetup, hasKey: Boolean, actions: SetupActions, retry: () -> Unit,
                         install: () -> Unit, pair: () -> Unit, forgetKey: () -> Unit, close: () -> Unit) {
    val order = listOf(SetupStep.Termux, SetupStep.Install, SetupStep.Key, SetupStep.Pairing)
    val current = when (setup.step) {
        SetupStep.Installing -> SetupStep.Install
        SetupStep.Checking, SetupStep.Failed -> null
        else -> setup.step
    }
    val reached = current?.let { order.indexOf(it) } ?: -1

    fun mark(step: SetupStep): StepMark = when {
        setup.step == SetupStep.Ready -> StepMark.Done
        current == step -> StepMark.Current
        reached > order.indexOf(step) -> StepMark.Done
        step == SetupStep.Key && hasKey && current != null -> StepMark.Done
        else -> StepMark.Pending
    }

    Column(Modifier.fillMaxSize().background(Palette.body).safeDrawingPadding()
        .verticalScroll(rememberScrollState()).padding(horizontal = 16.dp)) {
        Row(Modifier.fillMaxWidth().height(56.dp), verticalAlignment = Alignment.CenterVertically) {
            Spacer(Modifier.weight(1f))
            Box(Modifier.size(48.dp).clip(CircleShape)
                .clickable(role = Role.Button, onClickLabel = "Close setup", onClick = close),
                contentAlignment = Alignment.Center) {
                Text("✕", style = style(18, color = Palette.muted))
            }
        }

        Column(Modifier.fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Palette.sheet).padding(24.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Box(Modifier.size(12.dp).clip(CircleShape).background(Palette.accent))
                Text("Play on this phone", Modifier.semantics { heading() },
                    style = style(20, bold = true).copy(textAlign = TextAlign.Start))
            }
            Spacer(Modifier.height(16.dp))

            StepRow(1, "Allow Termux", mark(SetupStep.Termux))
            StepRow(2, "Install the playback engine", mark(SetupStep.Install))
            StepRow(3, "Add your Soloist key", mark(SetupStep.Key))
            StepRow(4, "Pair with Spotify", mark(SetupStep.Pairing))
            Spacer(Modifier.height(16.dp))

            val message = setup.message.ifBlank { defaultMessage(setup.step) }
            Text(message, style = style(14, color = if (setup.error) Palette.error else Palette.muted)
                .copy(textAlign = TextAlign.Start))
            Spacer(Modifier.height(20.dp))

            val wide = Modifier.fillMaxWidth()
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                when (setup.step) {
                    SetupStep.Checking -> LoadingBar()
                    SetupStep.Termux -> {
                        if (setup.message.startsWith("Termux is not installed")) {
                            PillButton("Get Termux", filled = true, modifier = wide, onClick = actions.getTermux)
                        } else if (setup.message.startsWith("Grant")) {
                            PillButton("Allow Termux access", filled = true, modifier = wide, onClick = actions.grantTermux)
                        } else {
                            PillButton("Try again", filled = true, modifier = wide, onClick = retry)
                            PillButton("Open Termux", filled = false, modifier = wide, onClick = actions.openTermux)
                        }
                    }
                    SetupStep.Install -> {
                        PillButton("Install engine", filled = true, modifier = wide, onClick = install)
                        PillButton("Copy setup command", filled = false, modifier = wide, onClick = actions.copySetup)
                    }
                    SetupStep.Installing -> {
                        LoadingBar()
                        PillButton("Open Termux", filled = false, modifier = wide, onClick = actions.openTermux)
                    }
                    SetupStep.Key -> {
                        PillButton("Paste key", filled = true, modifier = wide, onClick = actions.pasteKey)
                        PillButton("Get your key", filled = false, modifier = wide, onClick = actions.getKey)
                    }
                    SetupStep.Pairing -> {
                        LoadingBar()
                        PillButton("Open Spotify", filled = true, modifier = wide, onClick = actions.openSpotify)
                    }
                    SetupStep.Ready -> PillButton("Done", filled = true, modifier = wide, onClick = close)
                    SetupStep.Failed -> {
                        PillButton("Try again", filled = true, modifier = wide, onClick = retry)
                        PillButton("Install engine", filled = false, modifier = wide, onClick = install)
                        if (hasKey) PillButton("Pair again", filled = false, modifier = wide, onClick = pair)
                    }
                }
            }

            if (hasKey && setup.step != SetupStep.Checking) {
                Spacer(Modifier.height(12.dp))
                Box(wide.heightIn(min = 48.dp).clickable(role = Role.Button, onClick = forgetKey),
                    contentAlignment = Alignment.Center) {
                    Text("Remove key", style = style(12, color = Palette.muted))
                }
            }
        }
        Spacer(Modifier.height(24.dp))
    }
}

private fun defaultMessage(step: SetupStep): String = when (step) {
    SetupStep.Checking -> "Checking the playback engine…"
    SetupStep.Termux -> "Music plays through Termux on this phone. Allow this app to run commands in Termux."
    SetupStep.Install, SetupStep.Installing -> installMessage("engine")
    SetupStep.Key -> "Spotify plays music on this phone through Spotify Soloist, its official background " +
        "player. Create your free Soloist key (Premium account), copy it, then press Paste key. You only do this once."
    SetupStep.Pairing -> PAIRING_MESSAGE
    SetupStep.Ready -> "Ready. Music now plays on this phone."
    SetupStep.Failed -> "Setup could not finish."
}
