package app.nativespotify

import android.net.Uri

internal class Media3FixtureSource(private val tone: Uri) {
    init {
        require(tone.scheme == "file")
    }

    fun resolve(contentUri: String): Uri? {
        return tone.takeIf { contentUri == TONE_URI }
    }

    companion object {
        const val TONE_URI = "fixture:tone"
    }
}
