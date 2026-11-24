#!/usr/bin/env bash
# Rust slab spike (rust-slab/, the same scope as slab.c): edit → rebuild (hyperfine) and edit → pixels.
# Mirrors bench.sh: a real code edit before each run (a fresh dead spike_touch fn appended to
# rust-slab/src/lib.rs), lib.rs restored by a trap. Writes hyperfine JSON and the pixels JSON to
# results/out/rust-slab-*.json and prints the tables; results/rust-slab.md is written by hand from them.
#   RUNS=10 bench/rust_slab_bench.sh      (cargo stable with the wasm32-unknown-unknown target on PATH)
set -euo pipefail
spike=$(cd "$(dirname "$0")/.." && pwd)
crate="$spike/rust-slab"
out="$spike/results/out"
RUNS=${RUNS:-10}
mkdir -p "$out"
lib="$crate/src/lib.rs"
cp "$lib" "$out/rust-slab-lib.rs.orig"
restore() { cp "$out/rust-slab-lib.rs.orig" "$lib"; rm -f "$out/rust-slab-lib.rs.orig"; }
trap restore EXIT
cd "$crate"

touch_cmd="sed -i '/^fn spike_touch_[0-9]*() -> u32/d' src/lib.rs && n=\$RANDOM\$RANDOM && echo \"fn spike_touch_\$n() -> u32 { \$n }\" >> src/lib.rs"
W=wasm32-unknown-unknown
declare -A cmds=(
  [native]="cargo build -q && ./target/debug/slab_dump scene > /dev/null"
  [wasm_dev]="cargo build -q --target $W"
  [wasm_dev_lib]="cargo build -q --target $W --lib"
  [wasm_release]="cargo build -q --release --target $W"
)
order=(native wasm_dev wasm_dev_lib wasm_release)
cargo build -q && cargo build -q --target $W && cargo build -q --release --target $W  # warm, fail early
for k in "${order[@]}"; do
  echo "== edit→rebuild $k" >&2
  hyperfine -S bash --warmup 1 --runs "$RUNS" --prepare "$touch_cmd" \
    --export-json "$out/rust-slab-$k.json" "${cmds[$k]}" >&2
done
restore; trap - EXIT
cargo build -q && cargo build -q --target $W && cargo build -q --release --target $W  # clean builds

echo "== edit→pixels" >&2
node "$spike/bench/rust_pixels_loop.mjs" "$RUNS" "$out/rust-slab-pixels.json" >/dev/null

stat_line() { python3 -c "
import json,sys
s=json.load(open(sys.argv[1]))['results'][-1]['summary']  # [-1]: with -S, hyperfine 2.0 adds the bare shell as results[0]
t=s['time_wall_clock']; m=s['memory_peak_resident']
print(f\"| {sys.argv[2]} | {t['median']*1000:.0f} | {t['min']*1000:.0f} | {m['max']/2**20:.1f} |\")" "$1" "$2"; }
echo "$(date -I), $(uname -m), $(nproc) cores. $(rustc --version). linker: $(cc --version | head -1), $(ld --version | head -1) (native); rust-lld (wasm). $RUNS runs each, 1 warm-up."
echo
echo "| build | median ms | min ms | peak RSS MB |"
echo "|---|---|---|---|"
stat_line "$out/rust-slab-native.json" "cargo build + run slab_dump (native)"
stat_line "$out/rust-slab-wasm_dev.json" "cargo build --target wasm32 (dev, lib + bin)"
stat_line "$out/rust-slab-wasm_dev_lib.json" "cargo build --target wasm32 --lib (dev, cdylib only)"
stat_line "$out/rust-slab-wasm_release.json" "cargo build --release --target wasm32"
echo
python3 - "$out/rust-slab-pixels.json" <<'PY'
import json,sys
r=json.load(open(sys.argv[1]))
print("| step | median ms | p95 ms |")
print("|---|---|---|")
for k,name in [("build_ms","build (t1−t0)"),("pixels_ms","reload→pixels (t2−t1)"),("total_ms","total edit→pixels (t2−t0)")]:
    print(f"| {name} | {r[k]['median']:.0f} | {r[k]['p95']:.0f} |")
PY
