# Project conventions

- C17 shared core, with thin native OS frontends and platform-service adapters.
- Windows is the primary platform; keep portable code free of OS/UI headers.
- No browser UI/runtime, GTK, Qt or application OpenGL dependency.
- Native does NOT mean classic-looking OS buttons/list boxes. The UI must follow
  Spotify's modern desktop look: see `design/REFERENCES.md`. Keep shared visuals
  and event-driven animations consistent on Windows and Linux. No permanent
  animation/render timer when the interface is at rest.
- Code must be readable, not dense. Blank lines between logical steps are
  required, including inside functions and control-flow blocks.
- Expand error branches and avoid compressed multi-statement lines. Follow
  `.clang-format`; do not format vendored code.
- Never log tokens. Windows and Android use the OS credential store with no
  plaintext fallback; Linux intentionally stores credentials as 0600 plain text
  files under `$XDG_CACHE_HOME/native-spotify/credentials` (owner's choice).
- Run tests from a build directory: `ctest --output-on-failure -V`.
- Separate cross-build evidence from runtime verification. Do not claim live
  Spotify functionality has been verified without a configured account/device.
