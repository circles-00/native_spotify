# Android app plan and cross-platform rules

**Status: Audible on-phone playback confirmed (November 19, 2026).** On the Galaxy S24 Ultra, the app installed the engine through Termux and Soloist played a track through the phone itself ("This phone" in Android's media controls), with no other playback device. Still to verify on the device: a full track end to end, pause/resume and seek, queue transitions, restart with the saved session, and Quit stopping the managed processes. Librespot remains excluded, including forks, ports, and wrappers.

## Decisions

- **One repo, two frontends, one shared C17 core.** Put the Android Gradle project in `android/`. Use Kotlin and Jetpack Compose for Android UI and the Android NDK with CMake to build the shared core. Keep the existing native Windows and Linux frontends. Do not create a separate library repo or require desktop developers to install the Android SDK.
- **Share behavior and visual identity, not layouts.** Search, library, queue, settings, and playback actions should mean the same thing across clients. A phone is not a scaled-down desktop window. Use Android navigation and controls instead of porting the desktop canvas or putting the desktop UI in a WebView.
- **On-phone streaming is required.** For this personal setup, Soloist may run as a separate process on the same phone. The official Spotify app is acceptable for initial pairing (or re-pairing), but must not render audio or be required for normal playback. No second device or remote audio routing is required. Both clients offer the shared Spotify Connect device picker (decided November 19, 2026): Android lists the same devices and moves playback through the same core calls, and shows its own Soloist as "This phone".

## Shared library boundary

```text
Desktop: native scene/controller ──────────┐
                                            ├── Shared C17 core ── platform adapters
Android: Compose → ViewModel → Kotlin → JNI ┘
```

| Layer | Owns | Must not own |
| --- | --- | --- |
| Shared C core ([`src/core/`](../src/core/)) | Catalog, search, library, queue rules, lyrics fetching and matching, playback commands and reconciliation, account/session state, shared settings and capability checks | Compose or desktop drawing, navigation, OS headers, platform-specific process or credential APIs |
| Platform adapters ([`src/platform/`](../src/platform/), plus Android adapters) | Networking/TLS implementation, secure storage, browser auth handoff, time/wake services, file locations, and platform playback integration | A second implementation of queue, search, or playback policy |
| Android bridge | JNI lifecycle, argument validation, UTF-8 conversion, bounded snapshot copies, and typed error mapping | Business logic, screen state, or UI resource choices |
| Frontends | Layout, navigation, focus, input, scroll position, menus, accessibility, and transient drafts such as scrubbing | Direct Spotify API calls, token storage, authoritative queue/playback state, or platform capability guesses |

### Library API and usage rules

1. Use the command/state pattern already present in [`app.h`](../src/core/app.h) (`app_action`, typed operations, `app_snapshot_into`). Add a narrow boundary for Android: explicit initialization/shutdown, typed commands, read-only snapshots or bounded pages, and a change notification. Do not pass the full internal `View`, raw pointers to mutable core data, or a struct's memory layout across JNI.
2. Use stable content IDs, typed capabilities and errors, and documented units such as milliseconds. Validate arguments and document ownership, thread safety, callback lifetime, and whether a returned result means **accepted** or **confirmed**. Show pending state until playback or another remote operation is confirmed or reconciled.
3. Keep native/network work off Android's main thread. Deliver change notifications through a lifecycle-aware Kotlin wrapper and expose immutable state to Compose, e.g. via `StateFlow` and `collectAsStateWithLifecycle`. Composables render state and emit actions; recomposition must not start new fetches or commands. Use bounded pagination and revision-based updates, not whole-library copies or polling per frame.
4. Start with one process-scoped core owner. Activity recreation must not create another session or queue. Explicitly stop workers and detach callbacks on shutdown. Keep UI collection scoped to visibility; playback that continues in the background has its own Android lifecycle.
5. Share data contracts, not storage locations. Use browser-based OAuth with PKCE and a provider-supported Android return flow; verify that flow early rather than copying desktop loopback assumptions. Use Android Keystore-backed credential storage and app-private paths. Never send credentials through UI snapshots, write plaintext fallback credentials, or log secrets.
6. Keep Gradle and NDK versions pinned and select the Android adapters in CMake without linking X11, GDI, or desktop process-launch code. Package the native library in the APK. Keep SDK paths, signing material, APKs/AABs, and build outputs out of Git.

### Shared fetching and feature logic

- The core decides **what to fetch and when**. Keep provider requests, parsing, matching, pagination, and shared cache/retry policy in C for lyrics, search, library, and other shared data. The platform adapter supplies HTTP/TLS and other OS services; Compose and desktop screens consume results rather than calling providers directly.
- Lyrics already follow this split: [`lyrics.c`](../src/core/lyrics.c) fetches from LRCLIB via `platform_http`, matches results, parses timed or plain lyrics, and provides active-line calculation. Reuse that code on Android. Pass the current track's metadata and playback position from the in-app player into the shared logic; send only the metadata needed for lookup, never Spotify credentials. Ignore stale results after a track change.
- The UI owns when the lyrics surface is visible, scrolling, highlighting, and accessibility. A tap on a timed line requests a seek through the shared playback API. Do not add a second lyrics client or timed-lyrics parser in Kotlin.

## Android playback feasibility

**November 20 replacement decision:** [Native Media3 playback is designed but blocked on an authorized Spotify source](ANDROID_LOCAL_PLAYBACK.md). The Soloist observations below describe earlier runs; a subsequent launch did not establish a Connect session. Keep the existing route until a replacement passes the Galaxy checks.

Reviewed Spotify's public documentation and the current repo on November 19, 2026. The S24 Ultra test below is a partial on-device result, not proof of audible playback.

| Candidate | Finding | Decision |
| --- | --- | --- |
| [Spotify Android SDK](https://developer.spotify.com/documentation/android) | App Remote delegates playback to the installed Spotify app. The [old native streaming SDK was phased out in 2022](https://developer.spotify.com/blog/2022-07-15-mobile-streaming-sdks-update). | Not in-app playback. |
| [Spotify Web API](https://developer.spotify.com/documentation/web-api/reference/start-a-users-playback) | `PUT /me/player/play` starts playback on an existing playback device; it does not return a full-track audio stream. | Use for supported data features, not as a streaming backend. |
| [Spotify Soloist](https://developer.spotify.com/documentation/soloist/reference/downloads-and-updates) | Spotify publishes Linux, macOS, and Windows executables, not an Android binary or NDK library. The [Linux ARM64 build targets Linux with glibc and uses PipeWire or PulseAudio](https://developer.spotify.com/documentation/soloist/tutorials/getting-started). | Use the official Linux ARM64 download in Termux's Debian environment; keep it out of the APK and Git. |
| [Spotify Web Playback SDK](https://developer.spotify.com/documentation/web-playback-sdk) | Can stream in a supported Android **browser** with eligible Premium. Spotify documents a browser player, not a native SDK or supported Android WebView integration. | A separate web product, not the native Compose app in this plan. |
| [Spotify Embedded SDK](https://developer.spotify.com/documentation/commercial-hardware) | Restricted to approved commercial hardware partners. No access or authorization for this Android app has been established. | Not an available backend. |
| [librespot](https://github.com/librespot-org/librespot/issues/1649) | Confirmed not to work for this project. Upstream also documents account-dependent playback failures. | Excluded as a backend, including through forks, ports, and wrappers. |

The repo already separates playback control from audio rendering: [`engine.c`](../src/core/engine.c) manages the external Soloist executable, and [`api.c`](../src/core/api.c) sends playback commands to Spotify. Neither is an in-process audio renderer. Rebuilding the C core with the NDK will not provide one. Put the selected engine behind an Android playback adapter that reports confirmed track, position, and playback state back to the shared core. Keep desktop Soloist and Connect behavior separate.

### Evidence from existing Android apps

| Project | What it demonstrates | What it does not establish |
| --- | --- | --- |
| [Sidespot](https://github.com/jtaekman/sidespot) | Closest match: Compose, JNI, an in-process Rust/librespot engine, and Android `AudioTrack`. It explicitly does not expose a Spotify Connect device. It has a [released APK](https://github.com/jtaekman/sidespot/releases/tag/v0.4.1) and an [independent community review](https://forum.mudita.com/t/sidespot-review-by-nilss/14590) of an older version. | The review is not a current full-track playback test across accounts. The engine is unofficial and requires Premium; Android support in the APK does not imply Spotify authorization. |
| [Outify](https://github.com/iTomKo/Outify/releases/tag/2.1.0) | A second project ships Android APKs using Rust/librespot. | A [Pixel user reported playback stuck at zero](https://github.com/iTomKo/Outify/issues/47). The maintainer cites [upstream account-dependent audio key denial](https://github.com/librespot-org/librespot/issues/1649) as one possible cause, not a diagnosis of that specific report. |
| [Quackify](https://github.com/23fpsz/Quackify) | A separate native Compose project, not a WebView. A [Realme 8 user reported about 30 minutes of continuous playback](https://github.com/23fpsz/Quackify/issues/8), and [another user reported about an hour before noticing the same problem](https://github.com/23fpsz/Quackify/issues/34#issuecomment-6971260504). | Both users reported playback stopping. The November 18 thread includes reports from multiple phones, and the issue remains open. This is firsthand evidence of playback, not a dependable backend. |
| [Orpheon](https://github.com/kota-zo/Orpheon) | Ships a Compose APK that renders Spotify audio locally using the official web player inside a hidden WebView. A [user report describes audio playing before track-switch failures](https://github.com/kota-zo/Orpheon/issues/311). | It depends on a browser runtime and therefore does not meet this repo's native-only constraint. It is not evidence of a supported native playback SDK. |

**Assessment:** Sidespot and Outify are historical examples, not candidates. Quackify's open playback failures make it a poor choice for this prototype. The partial on-device Soloist result gives us a more concrete issue to solve first.

### Personal setup and automation

- **Observed on the S24 Ultra:** Soloist `1.3.10.83` ran inside Debian under Termux. After pairing, the PulseAudio bridge showed a stereo `float32le` stream, but its only sink was `auto_null`. Termux PulseAudio `17.0-3` logged an Android linker failure loading `module-sles-sink`; the [upstream OpenSL ES loading fix](https://github.com/termux/termux-packages/commit/2cff1b326dd55dbf4ca19e743975728abf829743) landed in `17.0-4`. Installing that fix and hearing audio still need device verification.
- **Manual once:** Install/open Termux from F-Droid or GitHub and explicitly grant this app [Termux command execution](https://github.com/termux/termux-app/wiki/RUN_COMMAND-Intent). Termux's `allow-external-apps` setting also requires user opt-in. Pair in the official Spotify app only when the locally saved Soloist session is missing or invalid. Do not put the API key into Intent arguments, logs, or Git.
- **Automate after consent:** Check the Termux build and package versions; install dependencies from the configured repositories, with a signed official Termux fallback for an obsolete audio package without changing saved mirrors. Install rootless Debian Trixie in Termux's private storage and fetch Soloist from Spotify. Preserve existing Debian files and pairing. No rooting, OS replacement, or remote server.
- **First implementation:** [`audio.sh`](../android/termux/audio.sh) checks the minimum PulseAudio package version, starts an isolated, loopback-only bridge, and distinguishes an Android speaker sink from a dummy sink. A sink check is not an audible test. Treat missing packages, outdated repositories, a failed driver, and unconfirmed sound as different setup states.
- **Day to day:** Start the app-owned bridge and Soloist with the session when the app opens (decided November 19, 2026, so playback is ready when you press Play), using fixed Termux commands. The shared core controls it through the Spotify Web API, as on desktop; Soloist's local control port is not opened. A new playback session replaces an earlier one that is still running; a pairing session is never interrupted. On Quit, stop only the processes that this app started; do not stop unrelated Termux sessions. Do not start at boot. Keep playback alive through rotation and screen lock; use an idle timeout as a fallback when the app dies without a clean Quit.

### Go/no-go criteria

1. **Go for the prototype.** Implement setup diagnostics and on-demand process control based on the observed PCM stream. An obsolete package or `auto_null` must never be reported as ready.
2. **Gate the full app on audible playback.** On the S24 Ultra, pair if needed, then start playback from the local Soloist process with the official app closed. Hear one full track through the phone speaker or headphones. Verify that a restart preserves the session, that pause/resume and seek work through local control, and that Quit stops the managed processes.
3. **Keep verification scoped.** Account login, playback notifications, and PCM reaching a dummy sink do not count as audible playback. Report the actual phone and package versions; do not infer support for other Android devices or accounts.

## Visual identity rules

- Use the existing [desktop references](REFERENCES.md) and [`dark_palette.h`](../src/ui/dark_palette.h) as the baseline. Share semantic roles such as background, surface, primary text, muted text, accent, error, and focus. Keep the charcoal palette, green active states, Inter typography, artwork crop conventions, and recognizable app icons. Use Android system icons for system affordances.
- Before building the full mobile screens, establish one checked-in source for shared tokens under `design/`. Start with existing desktop values; generate or validate the C and Compose mappings against it. Change a shared token and its affected components together. Keep platform-specific dimensions and component placement in the frontend; do not translate desktop pixels to Android `dp` one-for-one.
- Share labels, state meanings, content hierarchy, and artwork aspect ratios, not exact coordinates. Define selected, pressed, focused, disabled, loading, empty, error, and unavailable states for equivalent controls. Desktop hover is a hint, never the only way to expose an action. Android uses touch feedback, TalkBack semantics, and visible entry points.
- Use short, purposeful motion. Do not add decorative animation loops or render timers while idle. Respect the platform's reduced-motion setting and avoid transitions that delay actions or reset scroll.
- Review equivalent flows side by side using the same deterministic fixtures: playing and paused states, missing artwork, long titles, empty library, loading, and API errors. Compare hierarchy and behavior, not pixels across platforms. Dev fixtures must never be presented as real account content.

## Translating desktop features to mobile

Use the available window width, not the device model or orientation alone. On compact widths use one main pane; on wider windows or tablets consider a navigation rail or list/detail arrangement if both panes remain usable. Keep the playback model and navigation state stable when the layout changes.

| Desktop pattern | Compact Android default |
| --- | --- |
| Top search and persistent library rail | Home, Search, and Library bottom tabs. Put the search field, history, and filters on the Search screen; preserve each tab's destination and scroll position. |
| Persistent transport bar | Mini-player above bottom navigation. Tap it to open full-screen Now Playing with scrubber, transport, and entry points to Queue and Lyrics. Avoid showing the same transport controls twice on one screen. |
| Right-side Queue or Lyrics panel | Dedicated Now Playing destination for long, scrollable tasks. Android Back returns to the previous player or browse screen without resetting playback. |
| Spotify Connect Devices panel and “Playing on” badge | A device sheet opened from Now Playing and the mini-player: the current device with its controls and volume, then "Select another device". Playback on another device shows that device's name in green. Android's own output (speaker, headset, Bluetooth) stays in the system controls. |
| Dense playlist table with sortable columns | Artwork + title and artist in a short list row. Surface sort and filter separately; move secondary metadata to details rather than silently removing the information. |
| Right click or hover actions | Visible overflow for actions such as Add to Queue or Start Radio; optionally mirror it on long press. Swipes may be shortcuts but cannot be the only access path. |
| Profile menu, settings, and window controls | Avatar opens an account/settings destination. Do not copy Windows caption controls or drag regions to mobile. Use Android Back and system bars. |
| Taskbar transport and keyboard media controls | Android `MediaSession` and system media controls when playback is supported. Support audio focus, output changes, and current foreground-service requirements for ongoing playback. |

### Mobile interaction rules

1. **Choose the surface by task.** A frequent primary action is visible; a short choice or secondary action fits in a sheet; a task requiring reading, typing, or sustained scrolling gets a screen. Dialogs are for blocking decisions, not navigation.
2. **Make touch and accessibility work by default.** Use at least 48 × 48 dp touch targets, scalable `sp` text, sufficient contrast, meaningful labels and state semantics, and logical focus order. Do not communicate an action's state through color alone. Test with large font scales and TalkBack, not just screenshots.
3. **Honor Android navigation and insets.** Support system Back, edge-to-edge insets, the keyboard, and gesture navigation. Close the active surface or return one step instead of resetting the app. Preserve navigation and scroll through rotation; restore durable state after process death, without replaying old commands.
4. **Design for real device constraints.** Use lazy lists and bounded image requests. Keep essential controls reachable. Do not fetch or animate just because an offscreen view exists. Show loading, offline, empty, retry, and unavailable states distinctly; never suggest that unavailable account or device capabilities are supported.
5. **Keep desktop native too.** Retain mouse/keyboard affordances, dense layouts where readable, and panels where simultaneous visibility helps. Mobile adaptations should not force desktop into bottom tabs, larger hit areas, or full-screen destinations.

## Feature parity and review rules

- For every feature PR, name the shared behavior, desktop entry point, Android entry point, supported capabilities, and success/error states. Implement behavior once in C and test it there; add frontend tests for how users discover and invoke it on each platform.
- Once Android is active, add user-facing shared features to both clients in the same PR by default. If an entry point is deferred or genuinely platform-specific, record the reason, affected platform, and follow-up issue in the PR or parity tracker. An unavailable control must not appear to work. Before Android ships, use the desktop screens and the translation table as the initial parity inventory.
- Example: **Move to top** is one queue command that takes a stable queue-entry ID, not a visible row index. Desktop adds it to a context menu; Android adds it to visible overflow. Both observe the same queue update and handle the same failure. A Windows drag region or Android audio-focus handling is platform-specific and needs no equivalent UI.

## Milestones and acceptance checks

1. **Prototype setup and playback control.** Build the Termux setup helper first, then a minimal Android launcher/diagnostic flow. Once the audible playback gate passes, add the full Compose shell and wire the shared core through JNI without changing the desktop builds.
2. **Build one complete flow.** Sign in → search → play a result → open Now Playing → add to queue. Verify navigation, background/resume behavior, secure storage, and actionable errors. Set up the shared token baseline before multiplying screens.
3. **Expand parity.** Add Home, Library, collection details, Queue, Lyrics, settings, and the device picker. Review compact phone, landscape, and wider layouts. Verify gesture/Back behavior, large text, TalkBack, offline/retry, sign-out, and process recreation.
4. **Package and verify.** Build a signed test APK, verify install/upgrade and a real account/device session, then decide distribution format and ABI/API support. Run desktop CTest from the build directory, Android/JNI contract tests, and Compose tests. Report cross-build, emulator, and physical-device results separately; an APK compiling is not evidence of live Spotify playback.
