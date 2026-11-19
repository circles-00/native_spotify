# Installed in Termux private storage by the app. The key, when needed, arrives on stdin.
set -eu
umask 077

base="$HOME/.local/share/native-spotify-android"
guest=debian
mkdir -p "$base"
cd "$base"

setup() {
    setup_stage repositories
    pkg update
    pkg install -y proot-distro pulseaudio util-linux

    setup_stage audio-package
    bash ./audio.sh probe >/dev/null 2>&1 || {
        status=$?
        [ "$status" -eq 5 ] || [ "$status" -eq 6 ] || bash ./audio.sh install
    }

    setup_stage debian
    if ! proot-distro login "$guest" -- true >/dev/null 2>&1; then
        proot-distro install debian:trixie --name "$guest"
    fi

    proot-distro login "$guest" -- bash -c '
        set -eu
        printf "%s\n" "NATIVE_SPOTIFY_STAGE=debian-packages" >&2
        apt-get update -qq
        apt-get install -y ca-certificates curl libatomic1 libpulse0 libpipewire-0.3-0t64
        mkdir -p "$HOME/soloist-test"
        cd "$HOME/soloist-test"
        archive=$(mktemp)
        trap '\''rm -f "$archive" soloist.new'\'' EXIT
        printf "%s\n" "NATIVE_SPOTIFY_STAGE=download" >&2
        curl --fail --silent --show-error --location --max-time 180 \
            https://soloist-builds.spotifycdn.com/soloist_release_arm64.tar.gz -o "$archive"

        member=$(tar -tzf "$archive" | awk '\''$0 == "soloist" || $0 == "./soloist" { print }'\'')
        [ "$member" = soloist ] || [ "$member" = ./soloist ] || exit 11
        tar -xOf "$archive" "$member" >soloist.new
        printf "%s\n" "NATIVE_SPOTIFY_STAGE=binary" >&2
        chmod 700 soloist.new
        ./soloist.new --version
        mv -f soloist.new soloist
    '
}

setup_stage() {
    printf 'NATIVE_SPOTIFY_STAGE=%s\n' "$1" >&2
}

# Reports fixed NATIVE_SPOTIFY_* lines only; never prints keys or Soloist output.
status() {
    install=ready
    session=none
    active=none

    if ! command -v proot-distro >/dev/null || ! command -v flock >/dev/null; then
        install=packages
    else
        result=0
        bash ./audio.sh probe >/dev/null 2>&1 || result=$?

        # 5 means no bridge is running, which is expected while idle; 6 is a running bridge without a sink.
        if [ "$result" -ne 0 ] && [ "$result" -ne 5 ] && [ "$result" -ne 6 ]; then
            install=audio
        fi
    fi

    # Ask the guest itself: proot-distro's rootfs location differs between versions.
    guest_state=
    if [ "$install" = ready ]; then
        guest_state=$(proot-distro login "$guest" -- bash -c '
            if [ -x "$HOME/soloist-test/soloist" ]; then
                printf "%s\n" binary
            fi

            data="${XDG_DATA_HOME:-$HOME/.local/share}/soloist"
            if [ -d "$data" ] && find "$data" -mindepth 1 -maxdepth 1 \
                ! -name soloist.pid ! -name ws.addr ! -name ws.port | grep -q .; then
                printf "%s\n" session
            fi
        ' 2>/dev/null) || guest_state=

        case $guest_state in
            *binary*) ;;
            *) install=engine ;;
        esac
    fi

    # The marker is written after a successful pairing; older pairings only left Soloist's own data.
    case $guest_state in
        *session*) session=stored ;;
    esac

    if [ -f paired ]; then
        session=stored
    fi

    if [ -e session.lock ] && command -v flock >/dev/null && ! flock -n session.lock true; then
        active=$(cat session.mode 2>/dev/null || printf unknown)
    fi

    printf 'NATIVE_SPOTIFY_INSTALL=%s\n' "$install"
    printf 'NATIVE_SPOTIFY_SESSION=%s\n' "$session"
    printf 'NATIVE_SPOTIFY_ACTIVE=%s\n' "$active"
}

# Lists Soloist processes this app started; its device name keeps unrelated Soloist runs out.
engine_pids() {
    local dir command

    for dir in /proc/[0-9]*; do
        command=$(tr '\0' ' ' < "$dir/cmdline" 2>/dev/null) || continue

        case $command in
            "soloist --device-name Native Spotify Phone "*|/*"/soloist --device-name Native Spotify Phone "*)
                printf '%s\n' "${dir#/proc/}"
                ;;
        esac
    done
}

# Stops engines left behind when a session was killed before it could clean up.
stop_engines() {
    local pids attempt

    pids=$(engine_pids)
    [ -n "$pids" ] || return 0

    # shellcheck disable=SC2086
    kill -TERM $pids 2>/dev/null || :

    for (( attempt=0; attempt<10; attempt++ )); do
        pids=$(engine_pids)
        [ -n "$pids" ] || return 0
        sleep 0.5
    done

    # shellcheck disable=SC2086
    kill -KILL $pids 2>/dev/null || :
}

# Pairing waits for a choice in Spotify; give up after 10 minutes instead of lingering.
pair() {
    timeout 600 proot-distro login "$guest" -- bash -c '
        set -eu
        IFS= read -r key
        export PULSE_SERVER=tcp:127.0.0.1:14713
        exec "$HOME/soloist-test/soloist" --device-name "Native Spotify Phone" --api-key "$key" "$@"
    ' soloist --pair <<< "$key"
}

case ${1:-} in
    setup)
        setup
        exit
        ;;
    status)
        status
        exit
        ;;
    stop|keepalive)
        if [ -p stop ]; then
            timeout 3s bash -c 'printf "%s\n" "$1" > stop' signal "$1"
        fi
        exit
        ;;
    pair|start) ;;
    *) exit 2 ;;
esac

command -v flock >/dev/null || { printf '%s\n' 'Run setup first.' >&2; exit 3; }
exec 9>session.lock
if ! flock -n 9; then
    # A new playback session replaces an earlier one (e.g. after the app restarted); pairing is never cut short.
    if [ "$1" != start ] || [ "$(cat session.mode 2>/dev/null)" != start ]; then
        printf '%s\n' 'A playback or pairing session is already running.' >&2
        exit 7
    fi

    if [ -p stop ]; then
        timeout 3s bash -c 'printf "%s\n" stop > stop' || :
    fi

    flock -w 15 9 || { printf '%s\n' 'The earlier playback session did not stop.' >&2; exit 7; }
fi
printf '%s\n' "$1" > session.mode

# Holding the lock means no live session owns an engine, so any engine still running is orphaned.
stop_engines
IFS= read -r key
[[ $key =~ ^spak_[a-zA-Z0-9_-]+$ ]] || exit 2

if [ "$1" = pair ]; then
    setup_stage pairing
    rm -f paired
    pair
    touch paired
    exit
fi

audio_pid=
player_pid=
cleanup() {
    [ -z "$player_pid" ] || kill -TERM "$player_pid" 2>/dev/null || :

    # Killing PRoot's launcher does not always stop Soloist inside it.
    stop_engines
    [ -z "$audio_pid" ] || kill -TERM "$audio_pid" 2>/dev/null || :
    rm -f stop
}
trap cleanup EXIT
trap 'exit 0' INT TERM HUP
rm -f stop
mkfifo -m 600 stop
exec 8<>stop

setup_stage audio-output
if bash ./audio.sh probe >/dev/null 2>&1; then
    printf '%s\n' 'The audio bridge port is already in use. Stop the manual bridge first.' >&2
    exit 7
else
    result=$?
    if [ "$result" -ne 5 ]; then bash ./audio.sh probe >&2; fi
fi

for backend in sles aaudio; do
    bash ./audio.sh start "$backend" 8>&- 9>&- &
    audio_pid=$!

    for (( attempt=0; attempt<5; attempt++ )); do
        kill -0 "$audio_pid" 2>/dev/null || break
        if bash ./audio.sh probe >/dev/null 2>&1; then break 2; fi
        sleep 1
    done

    kill -KILL "$audio_pid" 2>/dev/null || :
    wait "$audio_pid" 2>/dev/null || :
    audio_pid=
done

[ -n "$audio_pid" ] || { printf '%s\n' 'Neither Android audio driver started.' >&2; exit 5; }

setup_stage launch
# Use exec so the managed PID is PRoot rather than an extra shell running it.
proot-distro login "$guest" -- bash -c '
    set -eu
    IFS= read -r key
    export PULSE_SERVER=tcp:127.0.0.1:14713
    port=$1
    set --
    if [[ $port =~ ^[0-9]{1,5}$ ]] && (( 10#$port >= 1024 && 10#$port <= 65535 )); then
        set -- --ws "127.0.0.1:$port"
    fi
    exec "$HOME/soloist-test/soloist" --device-name "Native Spotify Phone" --api-key "$key" "$@"
' soloist "${2:-0}" <<< "$key" 8>&- 9>&- &
player_pid=$!
deadline=$((SECONDS + 100))
while kill -0 "$player_pid" 2>/dev/null && kill -0 "$audio_pid" 2>/dev/null; do
    if IFS= read -r -t 1 command <&8; then
        [ "$command" != stop ] || exit 0
        [ "$command" != keepalive ] || deadline=$((SECONDS + 100))
    fi
    [ "$SECONDS" -lt "$deadline" ] || exit 0
done
if ! kill -0 "$player_pid" 2>/dev/null; then wait "$player_pid"; fi
exit 5
