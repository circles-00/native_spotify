package app.nativespotify

import android.app.Activity
import android.content.Context
import android.net.Uri
import java.util.UUID

internal data class PlaybackDrm(val scheme: UUID, val licenseUri: Uri)

internal class ResolvedPlayback(val mediaUri: Uri, val drm: PlaybackDrm?)

internal interface AuthorizedPlaybackSource {
    suspend fun signIn(context: Context): PlaybackGrant
    suspend fun refresh(context: Context, previous: PlaybackGrant?): PlaybackGrant
    suspend fun resolve(contentUri: Uri, grant: PlaybackGrant): ResolvedPlayback
}

internal object SpotifyPlaybackSource : AuthorizedPlaybackSource {
    override suspend fun signIn(context: Context): PlaybackGrant {
        val endpoints = requireWebEndpoints()
        val activity = context as? Activity ?: throw LocalPlaybackUnavailable("Playback sign-in needs an Activity")
        return exchange(PlaybackWebSession.signIn(activity, endpoints))
    }

    override suspend fun refresh(context: Context, previous: PlaybackGrant?): PlaybackGrant =
        exchange(PlaybackWebSession.refresh(context, requireWebEndpoints()))

    override suspend fun resolve(contentUri: Uri, grant: PlaybackGrant): ResolvedPlayback {
        // TODO(native-playback): Resolve Spotify media and DRM through an authorized provider interface.
        throw LocalPlaybackUnavailable("Spotify native playback source is unavailable")
    }

    private fun requireWebEndpoints(): PlaybackWebEndpoints {
        // TODO(native-playback): Configure supported playback authorization endpoints, if available.
        throw LocalPlaybackUnavailable("Spotify playback authorization is not configured")
    }

    @Suppress("UNUSED_PARAMETER")
    private suspend fun exchange(grant: PlaybackWebGrant): PlaybackGrant {
        // TODO(native-playback): Redeem the one-time code and verifier through the supported provider.
        throw LocalPlaybackUnavailable("Spotify playback code exchange is not configured")
    }
}
