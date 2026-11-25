# Client screenshots

[Back to README](../../README.md)

These images come from the current Linux client and its native Cairo/Pango
renderer. They are not design mockups or Windows runtime captures.

| Image | Source |
| --- | --- |
| [Home and resized queue panel](desktop-home-queue.png) | Offline renderer fixture |
| [Search](desktop-search.png) | Offline renderer fixture |
| [Album details](desktop-album.png) | Offline renderer fixture |
| [Login](desktop-login.png) | Running client captured on Xvfb |

Fixture views use synthetic test data and fallback artwork. Their visible
“OFFLINE TEST FIXTURE” banner is intentional; they do not demonstrate account
access or live playback. No tokens, keys or personal account data are included.

## Reproduce

Build the Linux client first; see [building and testing](../BUILDING.md).
From the repository root:

```sh
mkdir -p /tmp/opencode/native-spotify-renders
(cd build && NATIVE_RENDER_OUTPUT=/tmp/opencode/native-spotify-renders \
  ctest --output-on-failure -V)

cp /tmp/opencode/native-spotify-renders/native-resized-panels-fixture.png docs/screenshots/desktop-home-queue.png
cp /tmp/opencode/native-spotify-renders/native-search-fixture.png docs/screenshots/desktop-search.png
cp /tmp/opencode/native-spotify-renders/native-album-detail-fixture.png docs/screenshots/desktop-album.png

python3 tests/measure_linux.py build/native_spotify docs/screenshots/desktop-login.png
```

The login capture needs Python 3, Xvfb and ImageMagick. The measurement helper
launches an isolated display with credential restoration disabled.
