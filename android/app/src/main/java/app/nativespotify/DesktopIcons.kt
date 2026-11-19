package app.nativespotify

import android.graphics.Bitmap
import android.util.LruCache
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Dp

/** The desktop's icons (src/ui/icons.c), drawn by the shared C rasterizer. */
internal object NativeIcons {
    init { System.loadLibrary("native_spotify_android") }

    /** A desktop icon as a white alpha mask; `size` is in desktop units, `scale` is pixels per unit. */
    external fun render(icon: Int, size: Float, scale: Float): IntArray?

    private val cache = LruCache<String, ImageBitmap>(96)

    fun bitmap(icon: Int, size: Float, scale: Float): ImageBitmap? {
        val key = "$icon:$size:$scale"
        cache.get(key)?.let { return it }

        val side = (size * scale + 0.5f).toInt()
        val pixels = render(icon, size, scale) ?: return null
        val image = Bitmap.createBitmap(pixels, side, side, Bitmap.Config.ARGB_8888).asImageBitmap()
        cache.put(key, image)
        return image
    }
}

/** One desktop icon, tinted. Labels belong on the button that holds it. */
@Composable
internal fun DesktopIcon(icon: Int, size: Dp, tint: Color, modifier: Modifier = Modifier) {
    val scale = LocalDensity.current.density
    val image = remember(icon, size, scale) { NativeIcons.bitmap(icon, size.value, scale) }

    if (image == null) {
        Spacer(modifier.size(size))
        return
    }

    Image(image, contentDescription = null, modifier = modifier.size(size), colorFilter = ColorFilter.tint(tint))
}
