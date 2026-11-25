# Building and testing

[Back to README](../README.md)

Desktop builds require CMake 3.20+ and a C17 compiler. Keep `assets/` beside the
executable: it contains the bundled fonts, licenses and application icons.
CMake copies it automatically and includes it in desktop ZIP packages.

## Automated releases

The [GitHub Actions workflow](../.github/workflows/build.yml) builds Windows x64
on Windows Server 2022 and Linux x64 on Ubuntu 22.04. Both jobs run CTest and
native window smoke checks before uploading ZIP artifacts containing the
executable, assets, documentation and licenses.

Successful pushes to `main` publish both ZIPs under [GitHub Releases](https://github.com/circles-00/native_spotify/releases).
The first release uses the CMake project version; later releases increment the
latest release tag's minor version, unless CMake specifies a newer version.
Release notes list commits since the previous version tag. Versions are tracked
by Git tags; the workflow does not commit version bumps back to the repository.
Rerunning an already released commit does not create another release.

Pull requests only build, test and upload artifacts. Use **Actions → Desktop
builds and releases → Run workflow** on `main` to build and publish manually.
Artifacts are retained for 14 days; published release assets remain available.
Publishing uses the built-in `GITHUB_TOKEN` with `contents: write` in the release
job only. Repository policy must allow that permission; no personal token or
Spotify credentials are needed.

### Prebuilt Linux packages

The Linux ZIP is not an AppImage or a fully static bundle. It targets Ubuntu
22.04 x64 (glibc 2.35+) and compatible systems with these runtime dependencies:

```sh
sudo apt-get install ca-certificates libx11-6 libcairo2 libpango-1.0-0 libpangocairo-1.0-0 \
  libfontconfig1 libjpeg-turbo8 libpng16-16 libwebp7 libcurl4 libssl3 libsecret-1-0
```

Package names vary by distribution. An X11/XWayland session and an unlocked
Secret Service provider are also needed. Windows packages use the static MSVC
runtime and require Windows 10/11. Soloist is downloaded at runtime on both platforms.

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
Use `NATIVE_RENDER_OUTPUT` when running CTest to save Linux renderer fixtures to
an existing directory.

## Authentication notes

The public client ID is configured in the root `CMakeLists.txt`. Packagers can
override it using `-DSPOTIFY_CLIENT_ID=...`; listeners do not need to configure it.
The browser callback is `http://127.0.0.1:43821/callback`.

If an older session lacks a permission, reauthorize using the relevant in-app
prompt. Refreshing a token cannot add permissions, and reconnecting does not
remove Spotify account, Premium, quota or endpoint restrictions.
