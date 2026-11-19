# Run with Termux bash, not from the Debian guest.
set -eu

server=tcp:127.0.0.1:14713
sink=native_spotify_phone
minimum=17.0-4

fail() {
    code=$1
    shift
    printf '%s\n' "$*" >&2
    exit "$code"
}

case ${1:-} in
    install|start|probe) action=$1 ;;
    *) fail 2 'Usage: bash audio.sh {install|start|probe}' ;;
esac

if [ "$(uname -m)" != aarch64 ] || [ -z "${PREFIX:-}" ] || ! command -v pkg >/dev/null; then
    fail 2 'The prototype requires ARM64 Termux with its APT package manager.'
fi

if [ "$action" = install ]; then
    pkg update || fail 3 'Could not refresh the configured Termux repositories.'
    pkg install -y pulseaudio || fail 3 'Could not install PulseAudio from the configured Termux repositories.'

    installed=$(dpkg-query -W -f='${Version}' pulseaudio)
    if ! dpkg --compare-versions "$installed" ge "$minimum"; then
        # Use a signed official source for this upgrade only; keep the user's mirrors unchanged.
        sources=$(mktemp -d)
        trap 'rm -f "$sources/soloist.list"; rmdir "$sources"' EXIT
        printf '%s\n' 'deb https://packages.termux.dev/apt/termux-main stable main' > "$sources/soloist.list"
        options=(-o "Dir::Etc::sourcelist=$sources/soloist.list" -o Dir::Etc::sourceparts=-)
        apt-get "${options[@]}" update && apt-get "${options[@]}" install -y pulseaudio ||
            fail 4 'An outdated PulseAudio was installed, and the official Termux update could not be applied.'
    fi
fi

version=$(dpkg-query -W -f='${Version}' pulseaudio 2>/dev/null) ||
    fail 3 'PulseAudio is missing. Run bash audio.sh install (uses your configured Termux repositories).'

if ! dpkg --compare-versions "$version" ge "$minimum"; then
    fail 4 "PulseAudio $version is too old; need $minimum or newer. Run install or check your Termux mirror."
fi

case "$action" in
    install)
        printf 'PulseAudio %s installed.\n' "$version"
        ;;
    start)
        umask 077
        export PULSE_RUNTIME_PATH="$PREFIX/tmp/native-spotify-pulse"
        mkdir -p "$PULSE_RUNTIME_PATH" || fail 5 'Could not create the private PulseAudio runtime directory.'

        case ${2:-sles} in
            sles) backend=module-sles-sink ;;
            aaudio) backend=module-aaudio-sink ;;
            *) fail 2 'Unknown Android audio backend.' ;;
        esac

        # Use a dedicated server; never replace the existing Termux PulseAudio config or kill its daemon.
        exec pulseaudio -n --fail=yes --daemonize=no --exit-idle-time=-1 --log-target=stderr \
            -L "$backend sink_name=$sink" \
            -L 'module-native-protocol-tcp listen=127.0.0.1 port=14713 auth-ip-acl=127.0.0.1'
        ;;
    probe)
        sinks=$(timeout 5s pactl -s "$server" list short sinks) ||
            fail 5 'The Native Spotify PulseAudio bridge did not respond.'

        if ! printf '%s\n' "$sinks" | awk -v sink="$sink" \
            '$2 == sink && ($3 == "module-sles-sink.c" || $3 == "module-aaudio-sink.c") { found = 1 } END { exit !found }'; then
            fail 6 'No Native Spotify Android output found. A dummy sink cannot play sound.'
        fi

        printf 'Android audio sink detected at %s (audible playback not yet verified).\n' "$server"
        ;;
esac
