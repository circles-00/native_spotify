# Installed in Termux private storage by the app. The key, when needed, arrives on stdin.
set -eu
umask 077

base="$HOME/.local/share/native-spotify-android"
guest=debian
# Soloist runs directly on Termux's glibc; PRoot only serves setup, which borrows Debian's libraries.
native="$base/native"
loader="$PREFIX/glibc/lib/ld-linux-aarch64.so.1"
mkdir -p "$base"
cd "$base"

setup() {
    setup_stage repositories
    pkg update
    pkg install -y proot-distro pulseaudio util-linux glibc-repo
    pkg install -y glibc-runner

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
        apt-get install -y ca-certificates curl libatomic1 libpulse0 libpipewire-0.3-0t64 patchelf
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

        # Everything but glibc itself, which Termux provides outside PRoot.
        printf "%s\n" "NATIVE_SPOTIFY_STAGE=native-libraries" >&2
        staging=$1/native.new
        rm -rf "$staging"
        mkdir -p "$staging/lib"
        cp soloist "$staging/soloist"

        # Point Soloist at Termux'\''s glibc loader; the path only has to exist outside the guest.
        patchelf --set-interpreter "$2" "$staging/soloist"
        pulse=$(ldconfig -p | awk '\''$1 == "libpulse.so.0" { print $NF; exit }'\'')
        for library in $( { ldd soloist; ldd "$pulse"; } | awk '\''$3 ~ /^\// { print $3 }'\'' | sort -u) "$pulse"; do
            case ${library##*/} in
                libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libresolv.so.*|ld-linux*) continue ;;
            esac
            cp -L "$library" "$staging/lib/"
        done
        cp -L /etc/ssl/certs/ca-certificates.crt "$staging/ca-certificates.crt"
        cp -L /etc/resolv.conf "$staging/resolv.conf" 2>/dev/null || :
    ' native "$base" "$loader"

    setup_stage native-engine

    # Android has no /etc/resolv.conf; Termux's glibc reads its own.
    if [ ! -s "$PREFIX/glibc/etc/resolv.conf" ]; then
        mkdir -p "$PREFIX/glibc/etc"
        if [ -s native.new/resolv.conf ]; then
            cp native.new/resolv.conf "$PREFIX/glibc/etc/resolv.conf"
        else
            printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > "$PREFIX/glibc/etc/resolv.conf"
        fi
    fi

    mkdir -p native.new/home native.new/data native.new/cache
    [ ! -d "$native/data" ] || cp -a "$native/data/." native.new/data/

    # A fresh data folder means Soloist must be paired again.
    if [ -z "$(ls -A native.new/data)" ]; then
        rm -f paired
    fi

    rm -rf "$native"
    mv native.new "$native"

    # On failure, show why: the loader's view of the libraries and Soloist's own exit status.
    local result=0
    soloist --version >/dev/null || result=$?

    if [ "$result" -ne 0 ]; then
        printf 'NATIVE_SPOTIFY_DIAG exit=%s\n' "$result" >&2
        LD_LIBRARY_PATH="$PREFIX/glibc/lib:$native/lib" "$loader" --list "$native/soloist" >&2 || :
        ls -l "$loader" "$native/lib" >&2 || :
        return "$result"
    fi
}

# Runs Soloist without PRoot. The subshell execs it, so a background PID is Soloist itself.
soloist() (
    exec env -u LD_PRELOAD \
        LD_LIBRARY_PATH="$PREFIX/glibc/lib:$native/lib" \
        SSL_CERT_FILE="$native/ca-certificates.crt" \
        PULSE_SERVER=tcp:127.0.0.1:14713 \
        HOME="$native/home" \
        XDG_DATA_HOME="$native/data" \
        XDG_CACHE_HOME="$native/cache" \
        "$native/soloist" "$@"
)

setup_stage() {
    printf 'NATIVE_SPOTIFY_STAGE=%s\n' "$1" >&2
}

# Reports fixed NATIVE_SPOTIFY_* lines only; never prints keys or Soloist output.
status() {
    install=ready
    session=none
    active=none

    if ! command -v proot-distro >/dev/null || ! command -v flock >/dev/null || [ ! -x "$loader" ]; then
        install=packages
    else
        result=0
        bash ./audio.sh probe >/dev/null 2>&1 || result=$?

        # 5 means no bridge is running, which is expected while idle; 6 is a running bridge without a sink.
        if [ "$result" -ne 0 ] && [ "$result" -ne 5 ] && [ "$result" -ne 6 ]; then
            install=audio
        fi
    fi

    if [ "$install" = ready ] && [ ! -x "$native/soloist" ]; then
        install=engine
    fi

    data="$native/data/soloist"
    if [ -d "$data" ] && find "$data" -mindepth 1 -maxdepth 1 \
        ! -name soloist.pid ! -name ws.addr ! -name ws.port | grep -q .; then
        session=stored
    fi

    # The marker is written after a successful pairing.
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

soloist_diagnostics() {
    # Forward only fixed diagnostic categories, never raw Soloist output or credentials.
    awk '
            { line = tolower($0) }
            line ~ /could not resolve|cannot resolve|failed to resolve|getaddrinfo|name resolution/ { report("dns") }
            line ~ /certificate verify|ssl connect error|tls handshake/ { report("tls") }
            line ~ /connection refused|network is unreachable|connection timed out/ { report("network") }
            line ~ /invalid api key|unauthorized|authentication failed|login failed/ { report("authentication") }
            line ~ /zeroconf|mdns/ && line ~ /error|failed|unable/ { report("discovery") }
            function report(category) {
                if (!seen[category]++) system("/system/bin/log -t NativePair " category)
            }
        '
}

# Pairing waits for a choice in Spotify; give up after 10 minutes instead of lingering.
pair() {
    soloist --device-name "Native Spotify Phone" --api-key "$key" --pair 9>&- \
        > >(soloist_diagnostics 9>&-) 2>&1 &
    local pairing=$!
    /system/bin/log -t NativePair started || :
    ( sleep 600; kill -TERM "$pairing" 2>/dev/null ) 9>&- </dev/null >/dev/null 2>&1 &
    local timer=$!
    local result=0
    wait "$pairing" || result=$?
    /system/bin/log -t NativePair "exit=$result" || :

    # The timer only exits on its own after the 10 minutes ran out.
    if ! kill "$timer" 2>/dev/null; then
        return 124
    fi

    return "$result"
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
socket=()
port=${2:-0}
if [[ $port =~ ^[0-9]{1,5}$ ]] && (( 10#$port >= 1024 && 10#$port <= 65535 )); then
    socket=(--ws "127.0.0.1:$port")
fi
soloist --device-name "Native Spotify Phone" --api-key "$key" "${socket[@]}" 8>&- 9>&- \
    > >(soloist_diagnostics 8>&- 9>&-) 2>&1 &
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
