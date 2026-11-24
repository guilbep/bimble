#!/usr/bin/env bash
# Rust baseline for the C slab spike: edit → rebuild in crates/bimble-geom, the code that holds
# extrude_tris, mesh_from_tris and the slab path. Mirrors bench.sh: hyperfine, a real code edit
# before each run (a fresh spike_touch fn at the end of lib.rs), lib.rs restored by a trap.
# Writes the hyperfine JSON to results/out/rust-*.json; results/rust.md is written by hand from it.
#   MOLD_DIR=<mold libexec/mold dir> CMAKE_BIN=<cmake bin dir> RUNS=5 bench/rust_loop.sh
# cargo (stable) must be on PATH.
set -euo pipefail
: "${MOLD_DIR:?set MOLD_DIR to the directory that holds the mold 'ld'}"
: "${CMAKE_BIN:?set CMAKE_BIN to the directory that holds cmake}"
export PATH="$CMAKE_BIN:$PATH"
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_RUSTFLAGS="-C link-arg=-B$MOLD_DIR"
spike=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$spike/../.." && pwd)
cd "$root"
RUNS=${RUNS:-5}
out="$spike/results/out"
mkdir -p "$out"
lib=crates/bimble-geom/src/lib.rs
cp "$lib" "$out/lib.rs.orig"
restore() { cp "$out/lib.rs.orig" "$lib"; rm -f "$out/lib.rs.orig"; }
trap restore EXIT

touch_cmd="sed -i '/^#\[allow(dead_code)\] fn spike_touch()/d' $lib && n=\$RANDOM\$RANDOM && echo \"#[allow(dead_code)] fn spike_touch() -> u32 { \$n }\" >> $lib"

declare -A cmds=(
  [geom]="cargo build -q -p bimble-geom"
  [slab_dump]="cargo build -q -p bimble-geom --example slab_dump && target/debug/examples/slab_dump scene > /dev/null"
  [wasm_dev]="cd web-ui && npm run -s wasm:dev"
)
for k in geom slab_dump wasm_dev; do
  echo "== edit→rebuild $k" >&2
  hyperfine -S bash --warmup 1 --runs "$RUNS" --prepare "$touch_cmd" \
    --export-json "$out/rust-$k.json" "${cmds[$k]}" >&2
done
restore; trap - EXIT
cargo build -q -p bimble-geom --example slab_dump  # leave a clean build behind

stat_line() { python3 -c "
import json,sys
s=json.load(open(sys.argv[1]))['results'][-1]['summary']  # [-1]: with -S, hyperfine 2.0 adds the bare shell as results[0]
t=s['time_wall_clock']; m=s['memory_peak_resident']
print(f\"| {sys.argv[2]} | {t['median']*1000:.0f} | {t['min']*1000:.0f} | {m['max']/2**20:.1f} |\")" "$1" "$2"; }
echo "$(date -I), $(uname -m), $(nproc) cores. $(rustc --version). $RUNS runs each, 1 warm-up."
echo "| build | median ms | min ms | peak RSS MB |"
echo "|---|---|---|---|"
stat_line "$out/rust-geom.json" "(1) cargo build -p bimble-geom"
stat_line "$out/rust-slab_dump.json" "(2) slab_dump example + run"
stat_line "$out/rust-wasm_dev.json" "(3) npm run wasm:dev"
