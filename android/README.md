# Native Spotify for Android

This is an ARM64 Android debug app. Soloist and Debian run on the phone inside Termux, not inside the APK or on another device. No Soloist executable or account credential is bundled.

## Build

Use JDK 17, Gradle 8.13, Android SDK 35, NDK 27.1.12297006, and CMake 3.22.1. Open `android/` in Android Studio or run `gradle :app:assembleDebug` from that directory. The APK is `app/build/outputs/apk/debug/app-debug.apk`.

Judge smoothness and animations with `gradle :app:assembleRelease` instead (`app/build/outputs/apk/release/app-release.apk`). Compose runs much slower in debug builds. The release build is shrunk with R8 and signed with your local debug key, so it installs over a debug install and keeps its data. It is still a local test build, not one for distribution.

## First run on the phone

1. Install and open the APT-based F-Droid or GitHub release of Termux. In Termux, add the line `allow-external-apps = true` to `~/.termux/termux.properties` and run `termux-reload-settings`.
2. Install Native Spotify **after** Termux; otherwise Android can't grant Termux's permission and you must reinstall Native Spotify.
3. Open the app. Like the desktop app, it shows a loading screen while it checks your saved Spotify login, then a **Log in to Spotify** screen if there is none. Sign-in opens in your browser and uses the same public client ID and loopback redirect as the desktop app (`SPOTIFY_CLIENT_ID` in the root `CMakeLists.txt`).
4. After login the app asks Termux what is installed (`player.sh status`). If anything is missing, the **Play on this phone** setup screen opens by itself and walks through: allow Termux, install the engine, paste your Soloist key, and pair. Each check runs again when you return from Termux or Android settings. Engine install progress appears in Termux; **Copy setup command** is the manual fallback.
5. Pairing starts automatically only when no Soloist session is stored and no pairing or playback session is already running. If a pairing session is already running, the app waits for it instead of starting another. Open Spotify, tap the devices icon, and choose **Native Spotify Phone**. Pairing gives up after 10 minutes. If Soloist later reports no login despite a stored session, the app stops trusting that session and shows pairing again.
6. Close the setup screen at any time; **Settings → Playback setup** reopens it. Starting playback while setup is incomplete also reopens it. **Quit** stops the managed processes; Android can still interrupt Termux, and the bridge shuts down if the app stops renewing its lease. **Log out** also removes the Soloist key, as on desktop.

## How it is built

The app uses the same C core as the desktop app (`src/core`), compiled with the NDK. Compose draws the screens and sends commands. Sign-in and session, Home, Library, Search, search history, albums, playlists, Liked Songs, artist pages, discography filters, song radio, likes, saving albums, list sorting and filtering, profile, and lyrics all run in the core. The Android platform adapter (`src/platform/android_system.c`, with `posix_common.c` shared with Linux) supplies HTTPS through OkHttp, Keystore-backed secrets, SHA-256, and opening the browser. The JNI bridge (`app/src/main/cpp/bridge.c`) exposes typed commands and bounded JSON snapshots, never core structs. Lists follow the desktop contract: the core keeps up to 1,500 rows and each snapshot carries the 64 rows around the scroll position.

Sign-in is the desktop flow: the core opens Spotify in the browser and waits on the loopback redirect. Builds before the playback move ask you to log in once more, because the older sign-in did not include Spotify's playback scopes.

Playback uses the same C core as desktop. Soloist runs in Termux as the Spotify Connect device "Native Spotify Phone". The core starts it with the session when the app opens, as on desktop, through the Android adapter, which hands the session to `PlaybackService`. That service keeps the Termux lease alive, mirrors the core's playback into Android's media controls, and reports when the session ends. Quit stops Soloist; the next play starts it again. Now Playing and the mini-player open the shared device picker: move playback to any Spotify Connect device, or back to "This phone".

## Scope and verification

Android sends play, pause, seek, skips, and other local controls directly to Soloist and uses its playback events for the phone's position and playing state. The Web API is still used for remote devices and operations the Soloist socket cannot express, such as playing a playlist at a specific track, supplying a custom track list, or restoring a saved position with its queue. The Soloist control socket is bound only to loopback on a per-session port; it has no authentication, so the port is not a security boundary against other apps on the phone.

Run `gradle :app:testDebugUnitTest` from `android/` for snapshot parsing, navigation, Home rules, setup decisions and queue confirmation. The core's behavior is covered by the desktop CTest suite.

Successful APK assembly does not prove live playback. The earlier S24 Ultra test reached PulseAudio's dummy output; the updated OpenSL ES/AAudio startup path still needs an audible test on the phone. See [the audio helper](termux/README.md) for separate diagnostics if the app reports that neither driver started.
