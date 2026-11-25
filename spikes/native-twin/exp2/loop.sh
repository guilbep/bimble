#!/usr/bin/env bash
# Experiment 2, T6: the dev loop end to end, native twin against wasm, same edit, same machine.
#
# Usage: MOLD_DIR=<mold libexec/mold dir> CMAKE_BIN=<cmake bin dir> spikes/native-twin/exp2/loop.sh [runs]
#
# The edit: a new random N in the "spike" field of the "state" query (crates/bimble-viewer/src/calls.rs).
#   native: edit -> cargo build -p bimble-cli (mold) -> restart the headless twin -> poll
#           POST /query/state until the body has "spike":N (every 20 ms).
#   wasm:   edit -> npm run wasm:dev (files ready) -> grep the marker in public/bimble/*.wasm.
#           The browser reload is NOT measured.
# One untimed warm-up of each path, then runs alternate native and wasm.
# calls.rs is restored on exit (trap). Every twin process is killed on exit.
set -euo pipefail

RUNS="${1:-5}"
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="$ROOT/spikes/native-twin/exp2/results.txt"
CALLS="$ROOT/crates/bimble-viewer/src/calls.rs"
cd "$ROOT"

eval "$(mise env -s bash rust@stable)"
export PATH="$HOME/.cargo/bin:${CMAKE_BIN:?set CMAKE_BIN to the directory of cmake}:$PATH"
MOLD_DIR=${MOLD_DIR:?set MOLD_DIR to the libexec/mold directory of mold, which holds an ld that is mold}
HOST=$(rustc -vV | sed -n 's/^host: //p')
HOSTVAR="CARGO_TARGET_$(echo "$HOST" | tr 'a-z-' 'A-Z_')_RUSTFLAGS"
export "$HOSTVAR=-C link-arg=-B$MOLD_DIR"   # host only: the wasm target is not affected

WORK="$ROOT/target/exp2-loop"
mkdir -p "$WORK"
FRAME="$WORK/frame.bimble"
PORT=7881
TWIN_PID=

BAK="$WORK/calls.rs.orig"
cp "$CALLS" "$BAK"
cleanup() {
  if [[ -n "$TWIN_PID" ]]; then kill "$TWIN_PID" 2>/dev/null || true; wait "$TWIN_PID" 2>/dev/null || true; fi
  pkill -f "^target/debug/bimble view " 2>/dev/null || true
  cp "$BAK" "$CALLS"
}
trap cleanup EXIT INT TERM

# edit <N>: the "state" query gets "spike": N (a JSON number). The digits are parsed at run
# time from a string literal with a marker prefix, so the wasm binary holds "bimblespike<N>"
# as bytes that grep can find (a number literal would be an immediate, not greppable).
edit() {
  cp "$BAK" "$CALLS"
  python3 -I - "$CALLS" "$1" <<'PY'
import sys
path, n = sys.argv[1], sys.argv[2]
s = open(path).read()
anchor = '            "version": env!("CARGO_PKG_VERSION"),\n'
assert s.count(anchor) == 1, "anchor not found once"
line = f'            "spike": "bimblespike{n}".trim_start_matches("bimblespike").parse::<u64>().ok(),\n'
open(path, "w").write(s.replace(anchor, anchor + line))
PY
}

# timed <label> <log> <cmd...>: wall time and peak RSS of the largest child (fresh python per call).
timed() {
  local label="$1" log="$2"; shift 2
  python3 -I -c '
import resource, subprocess, sys, time
label, log, cmd = sys.argv[1], sys.argv[2], sys.argv[3:]
t = time.perf_counter()
with open(log, "w") as f:
    rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode
dt = time.perf_counter() - t
rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss / 1024
print(f"{dt:.3f} {rss:.0f} {rc}")
sys.exit(rc)
' "$label" "$log" "$@"
}

now() { python3 -I -c 'import time; print(f"{time.perf_counter():.6f}")'; }
# perf_counter is CLOCK_MONOTONIC on Linux, so values from different processes compare.

# restart_and_poll <N>: kill the previous twin, start a new one, poll until "spike":N. Writes the
# seconds to $WORK/restart.s. Not run in $( ): the twin must be a child of this shell (TWIN_PID),
# and its stdout must not hold a command substitution open.
restart_and_poll() {
  local n="$1" t
  if [[ -n "$TWIN_PID" ]]; then kill "$TWIN_PID" 2>/dev/null || true; wait "$TWIN_PID" 2>/dev/null || true; fi
  t=$(now)
  env -u WAYLAND_DISPLAY -u DISPLAY BIMBLE_SERVE=$PORT BIMBLE_TOKEN=t BIMBLE_HEADLESS=1 \
    target/debug/bimble view "$FRAME" >"$WORK/twin.out" 2>"$WORK/twin.err" &
  TWIN_PID=$!
  python3 -I - "$PORT" "$n" "$t" >"$WORK/restart.s" <<'PY'
import sys, time, urllib.request
port, n, t0 = sys.argv[1], sys.argv[2], float(sys.argv[3])
want = f'"spike":{n}'.encode()
deadline = time.perf_counter() + 60
while time.perf_counter() < deadline:
    try:
        req = urllib.request.Request(f"http://127.0.0.1:{port}/query/state?t=t", data=b"", method="POST",
                                     headers={"Content-Type": "text/plain"})
        with urllib.request.urlopen(req, timeout=1) as r:
            if want in r.read():
                print(f"{time.perf_counter() - t0:.3f}")
                sys.exit(0)
    except Exception:
        pass
    time.sleep(0.02)
sys.exit("timeout waiting for " + want.decode())
PY
}

native_run() { # native_run: sets RES="build restart total rss" (no $( ): the twin is our child)
  local n=$((RANDOM * 32768 + RANDOM)) t0 t2 b r
  t0=$(now); edit "$n"
  b=$(timed native "$WORK/native.log" cargo build -p bimble-cli)
  restart_and_poll "$n"; t2=$(now); r=$(cat "$WORK/restart.s")
  read -r bs rss rc <<<"$b"
  # total = t2 - t0 (includes the edit and the helper processes, ~0.1 s)
  RES=$(python3 -I -c "import sys; t0,t2,b,r=map(float,sys.argv[1:5]); print(f'{b:.3f} {r:.3f} {t2-t0:.3f} {sys.argv[5]}')" "$t0" "$t2" "$bs" "$r" "$rss")
}

wasm_run() { # wasm_run -> "build rss hits"
  local n=$((RANDOM * 32768 + RANDOM)) b hits
  edit "$n"
  b=$(cd web-ui && timed wasm "$WORK/wasm.log" npm run wasm:dev)
  hits=$(grep -c "bimblespike$n" web-ui/public/bimble/bimble_web_bg.wasm || true)
  [[ "$hits" -ge 1 ]] || { echo "marker bimblespike$n not in the wasm" >&2; exit 1; }
  read -r bs rss rc <<<"$b"
  echo "$bs $rss $hits"
}

[[ -f "$FRAME" ]] || { cargo build -p bimble-cli >/dev/null 2>&1; target/debug/bimble demo "$FRAME" >/dev/null; }

{
  echo "=== exp2/T6 dev loop  $(date -Is)"
  echo "uname -m: $(uname -m)  nproc: $(nproc)  $(rustc -V)"
  echo "native: $HOSTVAR=-C link-arg=-B<mold>; profile from Cargo.toml (ADR 0065)"
  free -m | sed -n 2p
} | tee -a "$OUT"

echo "warm-up (untimed)..." >&2
native_run
wasm_run >/dev/null

NAT=() WAS=()
for i in $(seq 1 "$RUNS"); do
  native_run; a=$RES; NAT+=("$a")
  echo "native run $i: build_s restart_to_value_s total_s peak_rss_mb = $a" | tee -a "$OUT"
  w=$(wasm_run); WAS+=("$w")
  echo "wasm   run $i: build_s peak_rss_mb marker_hits = $w" | tee -a "$OUT"
done

python3 -I - "${NAT[@]}" "--" "${WAS[@]}" <<'PY' | tee -a "$OUT"
import statistics as st, sys
a = sys.argv[1:]; i = a.index("--")
nat = [list(map(float, s.split())) for s in a[:i]]
was = [list(map(float, s.split())) for s in a[i+1:]]
m = lambda rows, k: st.median(r[k] for r in rows)
print(f"median native: build {m(nat,0):.3f} s, restart->value {m(nat,1):.3f} s, total {m(nat,2):.3f} s, peak RSS {max(r[3] for r in nat):.0f} MB (max)")
print(f"median wasm:   build {m(was,0):.3f} s (files ready; browser reload not measured), peak RSS {max(r[1] for r in was):.0f} MB (max)")
PY
