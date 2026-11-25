#!/usr/bin/env bash
# The dev loop for the C core behind the Vue lab: ccore/dev.sh [model]
#   Builds the host with `build.sh host` (one tcc call), starts Vite (web-ui/, --strictPort) and the native host, prints
#   the lab URL, then watches src/ and host/ (*.c, *.h). On every save: rebuild with tcc and, when
#   the build passes, restart the host on the same port, token and model. The open lab page sees
#   the new epoch in the change feed and re-syncs by itself. When the build fails, the compiler
#   output is printed and the old host keeps running.
#   model           default: a scratch copy of golden/frame.json in out/dev/ (the golden is never written)
#   CCORE_DEV_PORT  host port, default 47400
#   CCORE_DEV_VITE  Vite port, default 5173 (the host allows the two :5173 origins without config)
#   BIMBLE_TOKEN    the token, default: random, one per dev.sh run
# Ctrl-C or SIGTERM stops Vite, the host and the watcher. Needs tcc, inotifywait (inotify-tools),
# and web-ui/node_modules (`npm ci` in web-ui/). Host output goes to out/dev/host.log.
set -uo pipefail
CCORE=$(cd "$(dirname "$0")" && pwd)
UI=$(cd "$CCORE/../web-ui" && pwd)
PORT=${CCORE_DEV_PORT:-47400}
VITE=${CCORE_DEV_VITE:-5173}
DEV=$CCORE/out/dev
mkdir -p "$DEV"

for tool in tcc inotifywait npx; do
    command -v "$tool" > /dev/null || { echo "dev.sh: $tool not found" >&2; exit 2; }
done
[[ -d $UI/node_modules ]] || { echo "dev.sh: no web-ui/node_modules: run npm ci in web-ui/" >&2; exit 2; }

if [[ $# -ge 1 ]]; then
    [[ -f $1 ]] || { echo "dev.sh: no model file $1" >&2; exit 2; }
    MODEL=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
else
    MODEL=$DEV/frame.json
    cp "$CCORE/golden/frame.json" "$MODEL"
fi
TOKEN=${BIMBLE_TOKEN:-$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')}
export BIMBLE_TOKEN=$TOKEN
export BIMBLE_ORIGINS="http://localhost:$VITE,http://127.0.0.1:$VITE"

now_ms() { local t=${EPOCHREALTIME/[.,]/}; echo $((t / 1000)); }

host_pid="" vite_pid="" watch_pid=""
FIFO=$DEV/watch.fifo
cleanup() {
    trap - INT TERM EXIT
    [[ -n $watch_pid ]] && kill "$watch_pid" 2> /dev/null
    [[ -n $host_pid ]] && kill "$host_pid" 2> /dev/null
    [[ -n $vite_pid ]] && kill -TERM -- "-$vite_pid" 2> /dev/null # Vite runs in its own process group
    local i
    for i in $(seq 50); do
        kill -0 "$host_pid" 2> /dev/null || kill -0 -- "-$vite_pid" 2> /dev/null || break
        sleep 0.05
    done
    [[ -n $host_pid ]] && kill -KILL "$host_pid" 2> /dev/null
    [[ -n $vite_pid ]] && kill -KILL -- "-$vite_pid" 2> /dev/null
    wait 2> /dev/null
    rm -f "$FIFO"
    echo "dev.sh: stopped"
}
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap cleanup EXIT

# The host runs from a copy of the binary: a build never writes the file of a running process, and a
# failed build leaves the running host untouched.
BIN=$CCORE/out/tcc/bimble-ccore
RUN=$DEV/bimble-ccore

build() { # prints the build output on failure; returns build.sh's status
    "$CCORE/build.sh" host > "$DEV/build.log" 2>&1 && return 0
    echo "dev.sh: build failed, the old host keeps running:"
    grep -v '^  .* ms$' "$DEV/build.log"
    return 1
}

start_host() { # spawns the host and waits for "listening"; returns non-zero if it exits first
    cp "$BIN" "$RUN.tmp" && mv -f "$RUN.tmp" "$RUN"
    "$RUN" "$PORT" "$MODEL" > "$DEV/host.log" 2>&1 &
    host_pid=$!
    local i
    for i in $(seq 2000); do
        grep -q listening "$DEV/host.log" && return 0
        kill -0 "$host_pid" 2> /dev/null || break
        sleep 0.002
    done
    echo "dev.sh: the host did not start:"
    cat "$DEV/host.log"
    host_pid=""
    return 1
}

stop_host() {
    [[ -n $host_pid ]] || return 0
    kill -TERM "$host_pid" 2> /dev/null
    wait "$host_pid" 2> /dev/null
    host_pid=""
}

build || exit 1
start_host || exit 1
(cd "$UI" && exec setsid npx vite --port "$VITE" --strictPort) > "$DEV/vite.log" 2>&1 < /dev/null &
vite_pid=$!
for i in $(seq 600); do
    curl -sf -o /dev/null "http://localhost:$VITE/" && break
    if ! kill -0 "$vite_pid" 2> /dev/null; then
        echo "dev.sh: Vite did not start:"; cat "$DEV/vite.log"; exit 1
    fi
    sleep 0.05
done
echo "lab: http://localhost:$VITE/?native=1&gl=1&port=$PORT&token=$TOKEN"
echo "dev.sh: model $MODEL; host log out/dev/host.log; watching src/ host/ (Ctrl-C stops)"

rm -f "$FIFO"
mkfifo "$FIFO"
inotifywait -m -q -e close_write,moved_to,create --format '%w%f' "$CCORE/src" "$CCORE/host" > "$FIFO" 2> /dev/null &
watch_pid=$!
exec 3< "$FIFO"

while true; do
    read -r -u 3 path || { echo "dev.sh: the watcher stopped" >&2; exit 1; }
    [[ $path == *.c || $path == *.h ]] || continue
    # Debounce: an editor's save is often several events; wait for 50 ms of quiet.
    while read -r -t 0.05 -u 3 _; do :; done
    t0=$(now_ms)
    build || continue
    t1=$(now_ms)
    stop_host
    if start_host; then
        echo "rebuilt in $((t1 - t0)) ms, host up in $(($(now_ms) - t1)) ms"
    fi
done
