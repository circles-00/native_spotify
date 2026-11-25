# Building and testing

[Back to README](../README.md)

Desktop builds require CMake 3.20+ and a C17 compiler. Keep `assets/` beside the
executable: it contains the bundled fonts, licenses and application icons.
CMake copies it automatically and includes it in desktop ZIP packages.

## Windows

Install CMake and Visual Studio 2022 Build Tools with **Desktop development with
C++**, including the Windows SDK. Run from the repository root in a developer
PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Release

Push-Location build
ctest -C Release --output-on-failure -V
Pop-Location

.\build\Release\native_spotify.exe
```

Create a ZIP containing the executable and assets:

```powershell
cmake --build build --config Release --target package
```

Set `NATIVE_CONTROLLER_SMOKE=1` for a window smoke check that opens, paints and
closes without network access or credentials. Unset it before normal use.

## Linux

On Debian/Ubuntu:

```sh
sudo apt-get install build-essential cmake pkg-config libx11-dev libcairo2-dev \
  libpango1.0-dev libfontconfig1-dev libjpeg-dev libpng-dev \
  libcurl4-openssl-dev libssl-dev libsecret-1-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
(cd build && ctest --output-on-failure -V)

./build/native_spotify
```

An X11 session or XWayland is required. Sign-in persistence needs an unlocked
Secret Service provider, such as GNOME Keyring or a compatible KWallet setup.
`libwebp-dev` is optional and enables WebP decoding when available.

Address/undefined behavior sanitizers:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
(cd build-asan && ctest --output-on-failure -V)
```

### Cross-build Windows from Linux

Use Zig 0.14.1 and LLVM ar/ranlib:

```sh
cmake -S . -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/zig-windows.cmake \
  -DZIG_EXECUTABLE=/absolute/path/to/zig -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build-windows --parallel
cmake --build build-windows --target package
```

Cross-compilation checks the build, not Windows runtime behavior or performance.

## Android — work in progress

Use the separate [Android build and setup guide](../android/README.md).
It covers the SDK/NDK versions, Gradle commands, Termux permissions, Soloist
pairing and current audio limitations. The root CMake build targets desktop only.

## Testing and previews

Run CTest **from a build directory**, not the repository root. Seven desktop
suites cover authentication primitives, parsers, playback requests, artwork,
engine process handling, layout, rendering and interaction. They run without
Spotify account consent or writing user credentials.

The [CI workflow](../.github/workflows/build.yml) builds and tests on Windows and
Linux and runs native window smoke checks. A successful build or test run is not
proof of authenticated playback, audible output or successful Soloist pairing.

For disconnected Linux measurements, install Python 3, Xvfb and ImageMagick:

```sh
python3 tests/measure_linux.py build/native_spotify /tmp/opencode/native-login.png
```

This measures startup, RSS and idle CPU without account data or artwork; it
excludes the X server and shared-library disk footprint. For a login hover video,
also install ffmpeg and libXtst:

```sh
python3 tests/preview_linux.py build/native_spotify /tmp/opencode/native-preview.mp4
```

Both helpers use `NATIVE_CONTROLLER_OFFLINE=1` to avoid restoring credentials.
See [screenshot capture details](screenshots/README.md) for renderer fixtures.

## Authentication notes

The public client ID is configured in the root `CMakeLists.txt`. Packagers can
override it using `-DSPOTIFY_CLIENT_ID=...`; listeners do not need to configure it.
The browser callback is `http://127.0.0.1:43821/callback`.

If an older session lacks a permission, reauthorize using the relevant in-app
prompt. Refreshing a token cannot add permissions, and reconnecting does not
remove Spotify account, Premium, quota or endpoint restrictions.
