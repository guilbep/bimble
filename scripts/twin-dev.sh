#!/usr/bin/env bash
# The Rust dev loop behind the Vue lab (ADR 0066): scripts/twin-dev.sh [model]
#   Builds `cargo build -p bimble-cli` (dev profile), starts the headless native twin and Vite
#   (web-ui/, --strictPort), prints the lab URL, then watches crates/ (*.rs). On every save: rebuild
#   and, when the build passes, restart the twin on the same port, token and model. The open lab
#   page sees the new epoch in the change feed and re-syncs by itself (no reload). When the build
#   fails, the compiler output is printed and the old twin keeps running.
#
#   model             default: `bimble demo` written to target/twin-dev/frame.bimble
#   TWIN_DEV_PORT     twin port, default 47361
#   TWIN_DEV_VITE     Vite port, default 5173 (another port is passed to the twin in BIMBLE_ORIGINS)
#   BIMBLE_TOKEN      the token, default: random, one per run
#   TWIN_DEV_LINKER   mold | lld | default; default: mold if found, else lld, else the default linker
#   TWIN_DEV_JOBS     cargo -j, default: cargo's default
#
# Linker (ADR 0066 decision 5): only this script sets it, for the native host target only, through
# CARGO_TARGET_<host triple>_RUSTFLAGS (the wasm target, CI and release builds do not change).
# mold is found with `mise where mold` (its libexec/mold/ld, passed with -B), else on PATH
# (-fuse-ld=mold); lld when `ld.lld` is on PATH (-fuse-ld=lld). A value of that variable already
# set in the environment is kept. Note: cargo rebuilds when these flags change, so a `cargo build`
# without them (clippy, tests) and this script rebuild each other's artefacts.
#
# Ctrl-C or SIGTERM stops Vite, the twin and the watcher. Needs cargo, inotifywait
# (inotify-tools), and web-ui/node_modules (`npm ci` in web-ui/). Logs go to target/twin-dev/.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
UI=$ROOT/web-ui
PORT=${TWIN_DEV_PORT:-47361}
VITE=${TWIN_DEV_VITE:-5173}
DEV=$ROOT/target/twin-dev
BIN=$ROOT/target/debug/bimble
mkdir -p "$DEV"

for tool in cargo rustc inotifywait npx curl; do
    command -v "$tool" > /dev/null || { echo "twin-dev: $tool not found" >&2; exit 2; }
done
[[ -d $UI/node_modules ]] || { echo "twin-dev: no web-ui/node_modules: run npm ci in web-ui/" >&2; exit 2; }

# The linker, for the host target only.
HOST=$(rustc -vV | sed -n 's/^host: //p')
HOSTVAR="CARGO_TARGET_$(echo "$HOST" | tr 'a-z-' 'A-Z_')_RUSTFLAGS"
find_mold() { # prints rustflags for mold, or nothing
    local d ld
    if command -v mise > /dev/null && d=$(mise where mold 2> /dev/null) && [[ -d $d ]]; then
        ld=$(find "$d" -path '*/libexec/mold/ld' -print -quit 2> /dev/null)
        [[ -n $ld ]] && { echo "-C link-arg=-B$(dirname "$ld")"; return; }
    fi
    command -v mold > /dev/null && echo "-C link-arg=-fuse-ld=mold"
}
LINKER=default
if [[ -n ${!HOSTVAR:-} ]]; then
    LINKER="kept from the environment"
else
    want=${TWIN_DEV_LINKER:-auto}
    flags=""
    if [[ $want == auto || $want == mold ]]; then
        flags=$(find_mold) && [[ -n $flags ]] && LINKER=mold
    fi
    if [[ -z $flags && ($want == auto || $want == lld) ]] && command -v ld.lld > /dev/null; then
        flags="-C link-arg=-fuse-ld=lld" LINKER=lld
    fi
    [[ $want != auto && $want != default && $LINKER != "$want" ]] && { echo "twin-dev: linker $want not found" >&2; exit 2; }
    [[ $want == default ]] && flags="" LINKER=default
    [[ -n $flags ]] && export "$HOSTVAR=$flags"
fi
JOBS=()
[[ -n ${TWIN_DEV_JOBS:-} ]] && JOBS=(-j "$TWIN_DEV_JOBS")

TOKEN=${BIMBLE_TOKEN:-$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')}
export BIMBLE_TOKEN=$TOKEN
export BIMBLE_ORIGINS="http://localhost:$VITE,http://127.0.0.1:$VITE"

now_ms() { local t=${EPOCHREALTIME/[.,]/}; echo $((t / 1000)); }

twin_pid="" vite_pid="" watch_pid=""
FIFO=$DEV/watch.fifo
cleanup() {
    trap - INT TERM EXIT
    [[ -n $watch_pid ]] && kill "$watch_pid" 2> /dev/null
    [[ -n $twin_pid ]] && kill "$twin_pid" 2> /dev/null
    [[ -n $vite_pid ]] && kill -TERM -- "-$vite_pid" 2> /dev/null # Vite runs in its own process group
    local i
    for i in $(seq 50); do
        { [[ -n $twin_pid ]] && kill -0 "$twin_pid" 2> /dev/null; } ||
            { [[ -n $vite_pid ]] && kill -0 -- "-$vite_pid" 2> /dev/null; } || break
        sleep 0.05
    done
    [[ -n $twin_pid ]] && kill -KILL "$twin_pid" 2> /dev/null
    [[ -n $vite_pid ]] && kill -KILL -- "-$vite_pid" 2> /dev/null
    wait 2> /dev/null
    rm -f "$FIFO"
    echo "twin-dev: stopped"
}
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap cleanup EXIT

build() { # prints the errors on failure; returns cargo's status
    cargo build "${JOBS[@]}" -p bimble-cli --manifest-path "$ROOT/Cargo.toml" > "$DEV/build.log" 2>&1 && return 0
    echo "twin-dev: build failed, the old twin keeps running:"
    grep -v '^ *\(Compiling\|Building\|Blocking\)' "$DEV/build.log"
    return 1
}

start_twin() { # spawns the headless twin and waits for "listening"; non-zero if it exits first
    env -u WAYLAND_DISPLAY -u DISPLAY BIMBLE_SERVE="$PORT" BIMBLE_HEADLESS=1 \
        "$BIN" view "$MODEL" > "$DEV/twin.log" 2>&1 < /dev/null &
    twin_pid=$!
    local i
    for i in $(seq 3000); do
        grep -q 'bimble twin: listening' "$DEV/twin.log" && return 0
        kill -0 "$twin_pid" 2> /dev/null || break
        sleep 0.005
    done
    echo "twin-dev: the twin did not start:"
    cat "$DEV/twin.log"
    kill -KILL "$twin_pid" 2> /dev/null
    twin_pid=""
    return 1
}

stop_twin() {
    [[ -n $twin_pid ]] || return 0
    kill -TERM "$twin_pid" 2> /dev/null
    wait "$twin_pid" 2> /dev/null
    twin_pid=""
}

echo "twin-dev: building bimble-cli (dev profile, linker: $LINKER)..."
t0=$(now_ms)
build || exit 1
echo "twin-dev: built in $(($(now_ms) - t0)) ms"

if [[ $# -ge 1 ]]; then
    [[ -f $1 ]] || { echo "twin-dev: no model file $1" >&2; exit 2; }
    MODEL=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
else
    MODEL=$DEV/frame.bimble
    [[ -f $MODEL ]] || "$BIN" demo "$MODEL" > /dev/null || { echo "twin-dev: bimble demo failed" >&2; exit 1; }
fi

start_twin || exit 1
(cd "$UI" && exec setsid npx vite --port "$VITE" --strictPort) > "$DEV/vite.log" 2>&1 < /dev/null &
vite_pid=$!
for i in $(seq 600); do
    curl -sf -o /dev/null "http://localhost:$VITE/" && break
    if ! kill -0 "$vite_pid" 2> /dev/null; then
        echo "twin-dev: Vite did not start:"; cat "$DEV/vite.log"; exit 1
    fi
    sleep 0.05
done
echo "lab: http://localhost:$VITE/?native=1&port=$PORT&token=$TOKEN"
echo "twin-dev: model $MODEL; twin http://127.0.0.1:$PORT; logs in target/twin-dev/; watching crates/ (Ctrl-C stops)"

rm -f "$FIFO"
mkfifo "$FIFO"
inotifywait -m -r -q -e close_write,moved_to,create --format '%w%f' "$ROOT/crates" > "$FIFO" 2> /dev/null &
watch_pid=$!
exec 3< "$FIFO"

while true; do
    read -r -u 3 path || { echo "twin-dev: the watcher stopped" >&2; exit 1; }
    [[ $path == *.rs ]] || continue
    # Debounce: an editor's save is often several events; wait for 50 ms of quiet.
    while read -r -t 0.05 -u 3 _; do :; done
    t0=$(now_ms)
    build || continue
    t1=$(now_ms)
    stop_twin
    if start_twin; then
        echo "rebuilt in $((t1 - t0)) ms, twin up in $(($(now_ms) - t1)) ms"
    fi
done
