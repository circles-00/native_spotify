package app.nativespotify

import android.net.Uri

internal class LocalPlaybackUnavailable(operation: String) : UnsupportedOperationException(operation)

internal enum class LocalPlaybackAction(val wireValue: Int) {
    PLAY(BuildConfig.LOCAL_PLAYBACK_PLAY),
    PAUSE(BuildConfig.LOCAL_PLAYBACK_PAUSE),
    RESUME(BuildConfig.LOCAL_PLAYBACK_RESUME),
    SEEK(BuildConfig.LOCAL_PLAYBACK_SEEK),
    STOP(BuildConfig.LOCAL_PLAYBACK_STOP),
}

internal data class LocalPlaybackCommand(
    val action: LocalPlaybackAction,
    val contentUri: Uri?,
    val generation: Long,
    val requestId: Long,
    val positionMs: Long,
)

internal enum class LocalPlaybackState(val wireValue: Int) {
    BUFFERING(BuildConfig.LOCAL_PLAYBACK_BUFFERING),
    PLAYING(BuildConfig.LOCAL_PLAYBACK_PLAYING),
    PAUSED(BuildConfig.LOCAL_PLAYBACK_PAUSED),
    SEEKED(BuildConfig.LOCAL_PLAYBACK_SEEKED),
    ENDED(BuildConfig.LOCAL_PLAYBACK_ENDED),
    ERROR(BuildConfig.LOCAL_PLAYBACK_ERROR),
}

internal data class LocalPlaybackEvent(
    val generation: Long,
    val requestId: Long,
    val state: LocalPlaybackState,
    val positionMs: Long,
    val durationMs: Long,
)
