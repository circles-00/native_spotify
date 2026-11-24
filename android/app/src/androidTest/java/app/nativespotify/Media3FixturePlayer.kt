package app.nativespotify

import android.content.Context
import androidx.media3.common.AudioAttributes
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer

/** Test-only, single-item renderer. All calls and events run on ExoPlayer's application looper. */
internal class Media3FixturePlayer(
    context: Context,
    private val source: Media3FixtureSource,
    report: (State) -> Unit,
) : AutoCloseable {
    enum class PlayResult { ACCEPTED, SOURCE_UNAVAILABLE }

    data class State(
        val requestId: Long,
        val phase: Int,
        val playing: Boolean,
        val positionMs: Long,
        val seekable: Boolean,
        val errorCode: Int?,
    )

    private val player = ExoPlayer.Builder(context).build()
    private val listener = object : Player.Listener {
        override fun onEvents(player: Player, events: Player.Events) {
            // Read one coherent snapshot rather than assigning old callbacks to a new request.
            val requestId = player.currentMediaItem?.mediaId?.toLongOrNull() ?: return

            report(State(requestId, player.playbackState, player.isPlaying,
                player.currentPosition, player.isCurrentMediaItemSeekable, player.playerError?.errorCode))
        }
    }

    init {
        player.setAudioAttributes(AudioAttributes.DEFAULT, true)
        player.addListener(listener)
    }

    fun play(contentUri: String, requestId: Long): PlayResult {
        require(requestId > 0)

        val uri = source.resolve(contentUri) ?: return PlayResult.SOURCE_UNAVAILABLE

        player.setMediaItem(MediaItem.Builder().setUri(uri).setMediaId(requestId.toString()).build())
        player.prepare()
        player.play()
        return PlayResult.ACCEPTED
    }

    fun pause() = player.pause()

    fun resume() = player.play()

    fun seek(requestId: Long, positionMs: Long): Boolean {
        if (player.currentMediaItem?.mediaId != requestId.toString() || !player.isCurrentMediaItemSeekable) {
            return false
        }

        player.seekTo(positionMs.coerceIn(0L, player.duration))
        return true
    }

    override fun close() {
        player.removeListener(listener)
        player.release()
    }
}
