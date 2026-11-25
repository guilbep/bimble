#!/usr/bin/env bash
# C slab spike timings: edit → rebuild (hyperfine, 4 builds) and edit → pixels in Chromium.
# Writes results/bench.md (hyperfine JSON and the pixels JSON in results/out/).
#   RUNS=10 bench/bench.sh
set -euo pipefail
cd "$(dirname "$0")/.."
RUNS=${RUNS:-10}
mkdir -p results/out
cp slab.c results/out/slab.c.orig
cp main_wasm.c results/out/main_wasm.c.orig
restore() {
  cp results/out/slab.c.orig slab.c
  cp results/out/main_wasm.c.orig main_wasm.c
  rm -f results/out/slab.c.orig results/out/main_wasm.c.orig
}
trap restore EXIT

# median and min in ms, peak RSS (MB, max over runs) from hyperfine 2.0 JSON
stat_line() { python3 -c "
import json,sys
s=json.load(open(sys.argv[1]))['results'][0]['summary']
t=s['time_wall_clock']; m=s['memory_peak_resident']
print(f\"| {sys.argv[2]} | {t['median']*1000:.0f} | {t['min']*1000:.0f} | {m['max']/2**20:.1f} |\")" "$1" "$2"; }

targets=(tcc native wasm-dev wasm)
for t in "${targets[@]}"; do ./build.sh "$t"; done  # warm caches, fail early
for t in "${targets[@]}"; do
  echo "== edit→rebuild $t" >&2
  hyperfine -N --runs "$RUNS" --prepare bench/touch.sh --export-json "results/out/rebuild-$t.json" "./build.sh $t" >&2
done
cp results/out/slab.c.orig slab.c
for t in "${targets[@]}"; do ./build.sh "$t"; done  # leave clean builds behind

echo "== edit→pixels" >&2
node bench/pixels_loop.mjs "$RUNS" results/out/pixels.json >/dev/null

chromium_v=$(python3 -c "import json;print(json.load(open('results/out/pixels.json'))['chromium'])")
tcc_v=$(tcc -v 2>&1 | head -1)
{
  echo "# C slab spike: edit → rebuild, edit → pixels"
  echo
  echo "$(date -I), $(uname -m), $(nproc) cores. $(clang --version | head -1). $tcc_v. $chromium_v (headless, SwiftShader). $RUNS runs each."
  echo
  echo "## edit→rebuild (hyperfine, prepare: bench/touch.sh edits slab.c)"
  echo
  echo "| build | median ms | min ms | peak RSS MB |"
  echo "|---|---|---|---|"
  stat_line results/out/rebuild-tcc.json "tcc (native)"
  stat_line results/out/rebuild-native.json "clang -O0 (native)"
  stat_line results/out/rebuild-wasm-dev.json "clang wasm -O0 (wasm-dev)"
  stat_line results/out/rebuild-wasm.json "clang wasm -O2 (wasm)"
  echo
  echo "## edit→pixels (SPIKE_K flip, build.sh wasm-dev, slab.reload, first changed frame)"
  echo
  python3 - results/out/pixels.json <<'PY'
import json,sys
r=json.load(open(sys.argv[1]))
print("| step | median ms | p95 ms |")
print("|---|---|---|")
for k,name in [("build_ms","build (t1−t0)"),("pixels_ms","reload→pixels (t2−t1)"),("total_ms","total edit→pixels (t2−t0)")]:
    print(f"| {name} | {r[k]['median']:.0f} | {r[k]['p95']:.0f} |")
PY
} > results/bench.md
cat results/bench.md
