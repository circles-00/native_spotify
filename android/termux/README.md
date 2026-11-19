# Termux audio prototype

This helper is used by the [Android app](../README.md) and can also run separately for diagnostics. Running it directly does not install Debian or Soloist, change your mirrors, or touch your saved pairing.

Run the helper with `bash` in F-Droid or GitHub Termux, at the `~ $` prompt. Keep the script out of Debian:

1. `bash audio.sh install` installs or upgrades PulseAudio. It first uses your configured Termux repositories, then the signed official repository if the installed version is still below `17.0-4`. Skip this step if you're already up to date.
2. `bash audio.sh start` starts a dedicated foreground PulseAudio bridge on `127.0.0.1:14713`. Leave it running while testing. If the OpenSL ES sink fails to load, this command fails instead of starting a server with no audio output. `bash audio.sh start aaudio` tries the AAudio alternative.
3. In a second Termux session, run `bash audio.sh probe`. This confirms that the bridge exposes the expected Android sink, not that it is audible.
4. For the existing Soloist test in Debian, use `export PULSE_SERVER=tcp:127.0.0.1:14713` in place of the earlier port `4713`. Play a full track, using the same Soloist data directory as before. Stop the bridge with Ctrl+C in the session where you started it.

The loopback port is not exposed to Wi-Fi, but other apps on the phone may be able to reach it while the bridge is running. The eventual launcher should only keep its bridge and Soloist running for a playback session.

`probe` exits `0` for the expected sink, `5` if the bridge is unreachable, and `6` if the sink is missing. The setup commands also use `2` for an unsupported environment or invalid command, `3` for missing packages or an install failure, and `4` for an outdated PulseAudio package.
