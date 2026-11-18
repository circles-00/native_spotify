# Native Spotify

A Windows-first Spotify client with a C17 core and a **modern Spotify-style UI
rendered natively**. Music plays on this computer through **Spotify Soloist**,
Spotify's official background player, which the app downloads from Spotify and runs
for you; the Spotify desktop app does not need to be open. This is not a complete
Spotify replacement, and authenticated playback has not been verified end to end.

- **Windows 10/11:** custom GDI drawing, Win32 text input, WinHTTP, BCrypt,
  Credential Manager and a dark native title bar.
- **Linux:** Xlib/XIM, Cairo software surfaces, Pango/Fontconfig, libcurl, OpenSSL
  and Secret Service. X11 or XWayland required; direct Wayland is unfinished.
- **macOS:** not implemented. The core and presentation model are portable;
  an AppKit/native drawing, networking and Keychain adapter is still needed.

No Electron, browser UI/runtime, WebView, GTK, Qt or application OpenGL. The
system browser opens only for OAuth, the Soloist key page or an explicitly selected
Spotify link. GLib/GObject are used by Pango and Secret Service, not GTK widgets.

## Visual direction

See [`design/REFERENCES.md`](design/REFERENCES.md) for actual Spotify screenshots
that were fetched and inspected. The interface uses black outer chrome,
rounded split panes, a library rail, pill search/filters, collection cards,
an artwork-led collection header, a track table, and a persistent bottom player.

Layout, drawing commands, hit testing and animation state are shared between
platforms. Hover colors settle in 80ms; cards do not move, and navigation has no
crossfade or screen-transition animation. Frame timers stop when hover effects
settle. Windows supersamples shapes/artwork but composites text at native pixel
resolution with ClearType, preserving modal occlusion and glyph overhangs instead
of downsampling text with the rest of the page. There is no permanent rendering
loop at rest. Shapes and native-resolution text are both composed in a reusable
off-screen final buffer before one window blit, so hover repainting cannot expose
the background-only intermediate frame.

DM Sans is included as a freely distributable typographic substitute, under its
included OFL license. **Keep the `assets/` directory beside the executable.**
CMake copies it automatically. Spotify's proprietary font is not bundled.
Album, playlist and player artwork loads asynchronously through native decoders;
gradient collection icons remain fallbacks. This is not pixel-identical to Spotify
or its private Home feed, and no fake listening history is presented.

## Implemented

- PKCE S256 login through the external browser, cryptographic randomness and
  state validation, bounded IPv4 loopback callback with a 120-second deadline.
- Refresh tokens saved in the OS credential store; automatic access-token
  refresh before requests, including refresh-token rotation. No client secret,
  token logging, or plaintext credential fallback.
- Search has All/Songs/Artists/Albums filters, a top-result card, four songs beside
  it and artist/album shelves matching the supplied Spotify reference's structure.
  Queries update after a 180ms typing pause; Enter submits immediately.
  Typed search expressions are passed to Spotify unchanged. Ranking comes from
  the Web API, not Spotify desktop's private personalized ranking; the All view
  prefers its first artist as the top result when one is returned. Artist results
  open that artist's albums/singles; album and playlist results open track lists.
  Non-song cards browse instead of starting playback, and do not show decorative
  play buttons that suggest otherwise.
  Request generations prevent an old search/page response replacing newer results.
- Saved tracks/albums, current-user playlists and collection
  drill-down as **infinite scroll**: the next page loads before you reach the end,
  up to 5,000 rows per list. Only the rows around the scroll position are copied
  to the UI each frame. Failed loads keep what is already shown.
- Home has a scrollable **Jump back in** grid of up to eight unique albums from
  the listener's 50 most recently played tracks, plus saved-album and playlist
  shelves. Card dimensions no longer shrink with the available window height;
  scrolling reveals the complete shelves and captions. This is real recent
  listening, not Spotify's private personalized recommendation feed.
  Recent history needs `user-read-recently-played`; an existing session may need
  the Home **Enable recently played** button to reauthorize. This does not remove
  the saved Soloist key. API/account restrictions can still prevent access. When
  history is unavailable or empty, Home uses clearly labeled browse shortcuts.
- Toggleable **Devices**, **Queue**, **Friend Activity** and **Lyrics** panels.
  Devices lists every Spotify Connect device; click one to move playback there.
  A blue **Playing on <device>** badge appears in the header while another
  device plays. **Add to queue** keeps songs in this app and hands them to
  Spotify one at a time just before they are due, because Spotify's API cannot
  remove queued songs. Starting a playlist, album or song replaces the queue, and
  **Clear queue** empties it; the queue is saved between sessions. Lyrics come from
  [LRCLIB](https://lrclib.net/); when the lyrics panel is open, only track
  metadata (title, artist, album and duration) is sent to look up lyrics, not
  Spotify credentials. Friend Activity isn't available through Spotify's public
  API. One panel is open at a time; wide windows reflow and compact windows
  use a closable overlay.
- Login/session restoration automatically loads playback, devices and library
  previews. The left playlist library loads every API page in the background,
  preserving the returned order (up to 5,000 entries), and scrolls independently
  when the pointer is over the library pane. Queue lives in the bottom player,
  leaving more room for playlists. Spotify's desktop-specific custom,
  folder, pinned and recent ordering is not exposed by this API.
  Loaded pages are reused from a 16-entry in-memory cache for five
  minutes, silently; Refresh reloads the open list from Spotify.
- Track/episode playback commands, current playback metadata, and explicit
  Spotify attribution links for selected items. Album/playlist playback sends
  the context and selected URI so Spotify continues the collection.
- The queue shows up to 50 entries from Spotify and refreshes while open. Context
  progression is delegated to Spotify, not a locally simulated queue. Shuffle and
  off/context/track repeat buttons use Spotify's device-targeted player APIs.
  **Not exact desktop parity:** the documented playback API accepts album, artist
  and playlist contexts, not Liked Songs; saved-track clicks still play individual
  tracks. A standalone track URI does not guarantee Spotify's personalized radio
  or Autoplay. These are not presented as verified supported contexts.
- Device discovery: inspect up to 32 returned entries, select a target in the
  device popup, and transfer playback while preserving play/pause state.
- Seek and volume controls, enabled only when the last confirmed playback
  device supports them. Requests target that device explicitly, not an inactive
  device merely selected in the popup. Seek is clamped before the item's end.
- Network work runs on three independent lanes: playback commands, status/device
  reads, and content. A slow page or playback-state read cannot occupy the command
  worker. Buttons are not disabled merely because a request is running. Repeated seeks,
  volume drags, polls and list loads coalesce so only the latest is sent.
- **Optimistic playback:** play, pause, seek, volume, next/previous and choosing
  a track update the player instantly; Spotify's confirming read then settles or
  reverts it. Reads started before or during a newer command are discarded.
  Seeks retain their locally advancing position until confirmation, rejection,
  track/device change or a five-second post-acceptance deadline. Keeping keyboard
  focus on the seek control no longer freezes its progress display.
- HTTPS verification, redirects disabled, bounded response bodies, timeouts and
  Retry-After backoff. Lanes share OAuth tokens under a dedicated lock, so token
  refresh happens once and never stalls drawing. A 401 gets one refresh retry;
  non-idempotent commands are not retried on ambiguous network failure.
- Playback synchronization: three quick reads after a command, then five-second
  playing or fifteen-second paused refreshes, backing off on failures. Polling
  stops while hidden. Progress is estimated locally and painted once per second.
- Bounded artwork downloads from allowlisted Spotify CDN URLs, without bearer
  tokens. PNG/JPEG decoding uses WIC on Windows and libpng/libjpeg on Linux.
  Downloads are capped at 1 MiB, source dimensions at 2048, cached images at
  320 pixels per side and 64 entries (at most 25 MiB decoded pixels), enough for
  the bounded visible main, library and queue-panel rows together.
  Visible covers are pinned before drawing so they cannot evict each other; stale
  queued downloads can be replaced while scrolling, recent requests take priority,
  and failed downloads can retry after 30 seconds on a later repaint. Viewport
  snapshots are updated when scrolling, not one paint later. No artwork is written to disk.
- Unicode metadata/input, mouse and keyboard navigation, and Windows per-monitor
  DPI-aware layout/text.
Still missing: library/playlist mutations, queue additions,
Liked Songs context/radio parity, podcast search, live friend activity,
full accessibility, direct Wayland and macOS. Custom-painted controls do
not yet expose full screen-reader semantics.

API boundaries above were checked against Spotify's
[playback context documentation](https://developer.spotify.com/documentation/web-api/reference/start-a-users-playback),
[playlist paging endpoint](https://developer.spotify.com/documentation/web-api/reference/get-a-list-of-current-users-playlists)
and [Soloist WebSocket reference](https://developer.spotify.com/documentation/soloist/reference/websocket-api).
The latter exposes native queue/options events, but does not document a Liked
Songs context, radio-selection command or desktop library-order API. This build
keeps the working Web API playback transport rather than assuming those features.

### Playback on this computer (Spotify Soloist)

[Spotify Soloist](https://developer.spotify.com/documentation/soloist) is Spotify's
official headless player for x86_64 Windows and Linux. Spotify does not allow
redistributing it, so the app downloads the official build from
`soloist-builds.spotifycdn.com` into your user profile (`%LOCALAPPDATA%\NativeSpotify`
or `~/.local/share/native-spotify`), refreshes it monthly (builds expire after 90
days; an expired build's exit code 10 also triggers a fresh download) and runs it as
an owned background process that stops with the app. It shows up as **This
computer**; new playback goes there unless you transfer to another device.

One-time setup, guided by the **Play on this computer** panel:

1. **Get your key** opens Spotify's Soloist key page. Spotify requires each listener
   to create their own key there, with a Premium account.
2. Copy it and press **Paste key** (or Ctrl+V while the panel is open). The key is
   stored only in your OS credential store; **Remove key** deletes it, and so does
   logging out.
3. Pair once: Soloist only signs in after it is chosen from an official Spotify app,
   so open Spotify on your phone on the same network and pick **Native Spotify** in
   its device list. Soloist remembers the session afterwards.

If you pick a track before setup finishes, it starts as soon as Soloist is ready.

**Privacy note:** Soloist accepts its key only as a command-line argument. On
Windows, other standard user accounts cannot read another user's process command
lines. On Linux, local users can see process arguments unless `/proc` is mounted
with `hidepid`. Soloist discovers and is discovered over your local network, so
Windows may ask to allow it through the firewall.

Earlier versions of this project embedded librespot. That route is no longer used:
Spotify refuses librespot's audio-key requests for many accounts (librespot issue
#1649, locked in November 2026 with no known workaround).

## Usage

- **Home:** browse recent albums and saved shelves; use the mouse wheel to reach
  lower shelves, or Show all to open a complete collection.
- **Search:** Ctrl+L focuses search. Queries update after a 180ms typing pause;
  Enter submits immediately. Queries are bounded to 255 UTF-8 bytes. Spotify
  search filters such as `artist:Björk` are preserved through URL encoding.
- **Collections:** click artist/album/playlist cards to browse them. Select tracks and
  use the green play button or Enter; double-clicking a playable row also plays
  it. The external-link icon opens the selected item's Spotify page.
- **Scrolling:** use the mouse wheel or Up/Down. More rows load automatically near
  the end of a list.
- **Devices:** the monitor popup lists Spotify Connect devices; Soloist appears as
  **This computer**. Select a device and Transfer to play there, or select This
  computer to come back. Until Soloist is ready the button opens its setup panel.
- **Seek/volume:** drag and release the player bars. No request is sent during
  dragging. Keyboard Left/Right changes a focused bar's draft; Enter commits it.
- **Refresh:** the circular-arrow icon refreshes playback and the current API list.
  Queue reloads its own snapshot, and the Devices panel refreshes while it is open.
- **Panels:** use the bottom-player lyrics, queue and speaker (Devices) icons;
  the people icon in the header toggles Friend Activity. Click again, use the
  panel's close button, or press Escape to hide it.
- **Lyrics:** click the expand icon in the Lyrics sidebar to go fullscreen.
  Escape returns to the sidebar. Synced lyrics highlight and follow the current
  line; click any timed line to seek there on a device that supports seeking.
  If you scroll manually, use **Follow playback** to rejoin the current line.
  When timed lyrics are unavailable, matching plain lyrics are shown instead.

API acknowledgements are **not** proof that playback already changed; the
optimistic update is reconciled by scheduled reads. Slider drafts stay separate
from reported values, and unsupported/restricted devices remain disabled.
Selecting a queued item starts that URI; it does not implement queue reordering
or an atomic jump to a particular queue position.

Tab/Shift-Tab navigates controls; Up/Down navigates result rows. Escape closes
open panels/popups/focus rather than quitting the app. Both Windows and Linux
render the search pill directly in the shared scene, supporting clipboard paste,
Ctrl+A, Ctrl+U and Unicode Backspace (with XIM on Linux and `WM_CHAR` on
Windows). Search editing is currently append/backspace rather than a complete
cursor-editing widget.

Browse/queue/device snapshots and the list/artwork caches stay in memory and are
cleared on connect or disconnect. No account data is written to a disk cache.

## Sign in and play

1. Choose **Continue with Spotify** on the login screen.
2. Sign in and approve access in your normal browser. Return to the native app.
3. Choose a track. The first time, the **Play on this computer** panel walks you
   through the Soloist key and pairing; afterwards music starts immediately.

The application's public client ID is compiled in. Listeners do not register an
app, enter a client ID, set environment variables or provide a client secret.
The loopback callback remains `http://127.0.0.1:43821/callback`.

**Recently played needs an additional permission on older sessions.** Use Home's
Enable recently played button to authorize it without logging out or removing
your Soloist key. The requested scopes are
`user-read-playback-state`, `user-read-currently-playing`,
`user-modify-playback-state`, `user-library-read`, `playlist-read-private`,
`playlist-read-collaborative` and `user-read-recently-played`. Token refresh cannot add new permissions. A 403
can also reflect Premium, allowlist, endpoint or playlist-access restrictions;
reconnecting cannot remove those restrictions.

Linux needs an unlocked Secret Service provider, such as GNOME Keyring or a
compatible KWallet setup. Windows uses the current user's Credential Manager.
One account is stored per client ID. Log out deletes the local credential;
removing the app in Spotify account settings revokes its remote access.

The Web API is not an audio streaming SDK; local audio comes from Spotify Soloist.
This client does not support offline downloads. Spotify's
personalized Home feed, lyrics and full desktop parity are not publicly exposed.
Some device models and playlist contents are unavailable through the API.
Spotify's [Developer Policy III.11](https://developer.spotify.com/policy) also
restricts replacing its core experience without written permission. API access
does not itself authorize distributing a clone. See current
[quota rules](https://developer.spotify.com/documentation/web-api/concepts/quota-modes).

## Build

### Windows

Install CMake and Visual Studio 2022 Build Tools with the Desktop development
with C++ workload, which provides the C compiler and Windows SDK. In a developer
PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Release

Push-Location build
ctest -C Release --output-on-failure
Pop-Location

.\build\Release\native_spotify.exe

# Distributable ZIP containing the application and assets:
cmake --build build --config Release --target package
```

Keep `build/Release/assets/` with the executable. Set
`NATIVE_CONTROLLER_SMOKE=1` to open, paint and close a window without network
or credentials. The GitHub Actions workflow includes native Windows builds,
tests and a window smoke check; it has not been run in this local session.

### Linux

```sh
sudo apt-get install build-essential cmake pkg-config libx11-dev libcairo2-dev \
  libpango1.0-dev libfontconfig1-dev libjpeg-dev libpng-dev \
  libcurl4-openssl-dev libssl-dev libsecret-1-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
(cd build && ctest --output-on-failure -V)

./build/native_spotify
```

Sanitizers:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
(cd build-asan && ctest --output-on-failure -V)
```

Optional Linux-to-Windows cross-build using Zig 0.14.1 and LLVM ar/ranlib:

```sh
cmake -S . -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/zig-windows.cmake \
  -DZIG_EXECUTABLE=/absolute/path/to/zig -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build-windows --parallel
cmake --build build-windows --target package
```

Cross-compilation does not verify Windows runtime behavior or performance.

## Verification

Seven suites cover real OS cryptography and a real loopback listener, parsing,
token rotation, device restrictions, transfer JSON escaping, seek/volume URLs,
queue snapshots, native scene geometry/hit testing, scrolling and the setup panel,
Unicode input, refused commands without a session, cache eviction/expiry and
animation settling. Regression checks cover delayed/stale seek confirmations,
shuffle/repeat URLs, typed search parsing/cache separation, independent library
scrolling, visible artwork pinning, stale-download replacement and retry policy.
Linux render checks verify rounded panel corners, immediate navigation and search
layouts; Linux and Windows offscreen render tests measure label ink centering.
Windows additionally has a native-text composition/occlusion/descender test (it
requires Windows to execute; cross-building is not runtime verification).
Native PNG/JPEG decoders are tested with actual images. Engine
checks cover Soloist key handling, the update policy, device matching, real owned
processes (including Soloist's "expired" exit code 10 and prompt stopping), file
helpers and extraction of a real archive with the system `tar`.
No Spotify HTTP or credential
store is mocked. Fixtures exercise parsers and presentation state; they are not
presented as live account data. Tests run without contacting Spotify or writing
user credentials. Windows tests are cross-compiled here, not executed.

Disconnected Linux measurements (Python 3 and Xvfb):

```sh
python3 tests/measure_linux.py build/native_spotify
```

An optional second argument saves a screenshot using ImageMagick. This measures
startup to first completed paint, RSS and idle CPU, excluding the X server,
shared-library disk footprint and authenticated data/artwork. It does not measure
Windows or full logged-in usage. Bundled font assets add about 245 KB on disk.

Record the actual native login screen and hover transitions (Xvfb, libXtst, ffmpeg and
ImageMagick are development tools, not application dependencies):

```sh
python3 tests/preview_linux.py build/native_spotify /tmp/opencode/native-preview.mp4
```

The native button has also been exercised against a real isolated Brave browser,
reaching Spotify's login page with the compiled client ID. The optional
`tests/oauth_browser_linux.py` repeats that check without entering credentials;
it additionally needs Brave and Node 22+. It does not complete account consent.

Live token exchange, keyring persistence, Soloist pairing and authenticated
playback still require manual account consent and a Premium account.
Those paths and Windows runtime performance remain unverified. Preview and
measurement tools use `NATIVE_CONTROLLER_OFFLINE=1` to avoid restoring real
credentials. Packagers may override the public app ID with CMake's
`-DSPOTIFY_CLIENT_ID=...`; listeners never configure it.

## Structure and style

```text
src/main.c             Unified application entry point
src/core/              Portable domain, Spotify API, Soloist engine, worker lanes, artwork fetch
src/ui/                Portable presentation, scene layout/input/draw, controller, rasterizer, artwork cache
src/platform/          OS adapters: Win32/Linux system, image decode, engine, loopback, window, canvas
assets/                Fira Sans font and OFL license
design/REFERENCES.md   Inspected Spotify desktop references
tests/                 Parsers, interaction checks and preview tools
vendor/yyjson/         yyjson 0.12.0, MIT license included
```

Follow `.clang-format`. Keep blank lines between logical steps, including inside
blocks, and avoid compressed multi-statement code or unnecessary abstraction.
