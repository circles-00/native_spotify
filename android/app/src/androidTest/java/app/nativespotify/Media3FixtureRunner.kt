package app.nativespotify

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.SystemClock
import androidx.media3.common.Player
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.math.PI
import kotlin.math.sin
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull

class Media3FixtureRunner : Instrumentation() {
    override fun onCreate(arguments: Bundle?) {
        super.onCreate(arguments)
        start()
    }

    override fun onStart() {
        try {
            check(targetContext.packageName.endsWith(".fixture")) { "Build with -Pmedia3Fixture=true" }
            checkServicePlayback()
            runFixture()
            finish(Activity.RESULT_OK, Bundle().apply {
                putString("stream", "\nPASS: service-owned Media3, C/JNI transport, and fixture playback\n")
            })
        } catch (failure: Exception) {
            finish(Activity.RESULT_CANCELED, Bundle().apply {
                putString("stream", "\nFAIL: ${failure.stackTraceToString()}\n")
            })
        }
    }

    private fun checkServicePlayback() = runBlocking {
        val uri = Uri.parse("spotify:track:unavailable-fixture-id")
        check(!NativeCore.dispatchLocalPlayback(LocalPlaybackAction.PLAY.wireValue, uri.toString(), 1, 0))
        check(runCatching { SpotifyPlaybackSource.refresh(targetContext, null) }
            .exceptionOrNull()?.message == "Spotify playback authorization is not configured")
        check(runCatching {
            SpotifyPlaybackSource.resolve(uri, PlaybackGrant("fixture-only", 0))
        }.exceptionOrNull() is LocalPlaybackUnavailable)

        val audio = File(targetContext.cacheDir, PlaybackService.FIXTURE_FILE)
        val intent = Intent(targetContext, PlaybackService::class.java).setAction(PlaybackService.START_FIXTURE)
        writeTone(audio)

        try {
            targetContext.startForegroundService(intent)
            check(withTimeoutOrNull(10_000) {
                while (PlaybackService.localGeneration() == 0L) {
                    delay(20)
                }
                true
            } == true) { "Fixture service never enabled the native route" }

            val generation = PlaybackService.localGeneration()
            val signedIn = Engine.signInForLocalPlayback(targetContext)
            check(PlaybackService.localSession().refresh().expiresAtMs == signedIn.expiresAtMs)

            check(NativeCore.dispatchLocalPlayback(LocalPlaybackAction.PLAY.wireValue, PlaybackService.FIXTURE_URI, 1, 0))
            awaitState(generation, 1, LocalPlaybackState.PLAYING)
            check(NativeCore.dispatchLocalPlayback(LocalPlaybackAction.PAUSE.wireValue, null, 1, 0))
            awaitState(generation, 1, LocalPlaybackState.PAUSED)
            check(NativeCore.dispatchLocalPlayback(LocalPlaybackAction.SEEK.wireValue, null, 1, 2_000))
            check(withTimeoutOrNull(10_000) {
                while (NativeCore.localPlaybackPosition(generation, 1) != 2_000L) {
                    delay(20)
                }
                true
            } == true) { "Paused seek did not reach 2000 ms in the core" }
            awaitState(generation, 1, LocalPlaybackState.PAUSED)
            check(NativeCore.dispatchLocalPlayback(LocalPlaybackAction.RESUME.wireValue, null, 1, 0))
            awaitState(generation, 1, LocalPlaybackState.PLAYING)

            check(NativeCore.dispatchLocalPlayback(LocalPlaybackAction.PLAY.wireValue, PlaybackService.FIXTURE_URI, 2, 4_000))
            check(!CoreHost.reportLocalPlaybackEvent(LocalPlaybackEvent(generation, 1, LocalPlaybackState.ENDED, 5_000, 5_000)))
            awaitState(generation, 2, LocalPlaybackState.ENDED)
            check(!CoreHost.reportLocalPlaybackEvent(LocalPlaybackEvent(generation, 2, LocalPlaybackState.ENDED, 5_000, 5_000)))

            check(NativeCore.dispatchLocalPlayback(LocalPlaybackAction.PLAY.wireValue, uri.toString(), 3, 0))
            awaitState(generation, 3, LocalPlaybackState.ERROR)
        } finally {
            targetContext.stopService(intent)
            try {
                withTimeout(10_000) {
                    while (PlaybackService.localGeneration() != 0L) {
                        delay(20)
                    }
                }
            } finally {
                audio.delete()
            }
        }
    }

    private suspend fun awaitState(generation: Long, request: Long, state: LocalPlaybackState) {
        check(withTimeoutOrNull(10_000) {
            while (NativeCore.localPlaybackState(generation, request) != state.wireValue) {
                delay(20)
            }
            true
        } == true) { "Request $request: expected $state, got ${NativeCore.localPlaybackState(generation, request)}" }
    }

    private fun runFixture() {
        val events = LinkedBlockingQueue<Media3FixturePlayer.State>()
        val audio = File.createTempFile("media3-fixture-", ".wav", targetContext.cacheDir)

        try {
            writeTone(audio)
            val source = Media3FixtureSource(Uri.fromFile(audio))
            lateinit var player: Media3FixturePlayer
            runOnMainSync { player = Media3FixturePlayer(targetContext, source) { events.add(it) } }

            try {
                var result: Media3FixturePlayer.PlayResult? = null
                runOnMainSync { result = player.play(Media3FixtureSource.TONE_URI, 1) }
                check(result == Media3FixturePlayer.PlayResult.ACCEPTED)

                await(events, "first playback") { it.requestId == 1L && it.playing && it.seekable }

                runOnMainSync { player.pause() }
                await(events, "pause") { it.requestId == 1L && it.phase == Player.STATE_READY && !it.playing }

                runOnMainSync { result = player.play("spotify:track:unavailable-fixture-id", 99) }
                check(result == Media3FixturePlayer.PlayResult.SOURCE_UNAVAILABLE)

                var accepted = false
                runOnMainSync { accepted = player.seek(1, 2_000) }
                check(accepted) { "Current seek was refused" }
                await(events, "paused seek") { it.requestId == 1L && !it.playing && it.positionMs == 2_000L }

                runOnMainSync {
                    player.resume()
                }
                await(events, "resume") { it.requestId == 1L && it.playing && it.positionMs >= 2_000L }

                runOnMainSync {
                    result = player.play(Media3FixtureSource.TONE_URI, 2)
                    accepted = player.seek(1, 3_000)
                }
                check(result == Media3FixturePlayer.PlayResult.ACCEPTED)
                check(!accepted) { "Seek for the replaced request was accepted" }
                await(events, "replacement playback") { it.requestId == 2L && it.playing }
                await(events, "end of replacement") { it.requestId == 2L && it.phase == Player.STATE_ENDED }
            } finally {
                runOnMainSync { player.close() }
            }
        } finally {
            audio.delete()
        }
    }

    private fun await(
        events: LinkedBlockingQueue<Media3FixturePlayer.State>,
        label: String,
        matches: (Media3FixturePlayer.State) -> Boolean,
    ) {
        val deadline = SystemClock.elapsedRealtime() + 10_000L

        while (true) {
            val remaining = (deadline - SystemClock.elapsedRealtime()).coerceAtLeast(0L)
            check(remaining > 0) { "Timed out waiting for $label" }
            val state = checkNotNull(events.poll(remaining, TimeUnit.MILLISECONDS)) { "Timed out waiting for $label" }
            check(state.errorCode == null) { "Player error ${state.errorCode} while waiting for $label" }

            if (matches(state)) {
                return
            }
        }
    }

    private fun writeTone(file: File) {
        val sampleRate = 16_000
        val frames = sampleRate * 5
        val dataSize = frames * Short.SIZE_BYTES
        val wav = ByteBuffer.allocate(44 + dataSize).order(ByteOrder.LITTLE_ENDIAN)

        wav.put("RIFF".toByteArray()).putInt(36 + dataSize).put("WAVEfmt ".toByteArray())
        wav.putInt(16).putShort(1).putShort(1).putInt(sampleRate).putInt(sampleRate * 2)
        wav.putShort(2).putShort(16).put("data".toByteArray()).putInt(dataSize)

        repeat(frames) { frame ->
            val fade = minOf(frame, frames - 1 - frame, 160) / 160.0
            wav.putShort((sin(2 * PI * 440 * frame / sampleRate) * 2_048 * fade).toInt().toShort())
        }

        file.writeBytes(wav.array())
    }
}
