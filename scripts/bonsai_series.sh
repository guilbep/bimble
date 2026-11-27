#!/bin/bash
# The measurement series of docs/research/2026-11-27-bonsai-vs-bimble-loading.md, one file after the
# other in ascending size: IfcOpenShell (parse + geometry), Bimble's C port (link-convert), Bonsai
# load_project, Bonsai link (first = cache creation, second = from the cache).
# Each run: started only with MemAvailable >= 9 GiB (waited for outside the lock), under the shared
# lock taken with flock -n for the run only, `free -g` logged, in a cgroup scope with MemoryMax=8G and no swap (systemd-run --user --scope), 20 minutes. A tool stops after two failures in a row (the wall, then the
# confirmation on the next file).
# Usage: bonsai_series.sh <first nn> <last nn>    (files are $W/files/fNN.ifc, anonymous copies)
set -u
W=$HOME/.cache/bimble-bonsai/work
S=$(cd "$(dirname "$0")" && pwd)
PY=$HOME/.cache/bimble-ifc-corpus/venv/bin/python
FR=$HOME/.cache/bimble-bonsai/bin/fedora-run.sh
BIM=$HOME/bimble-test/bimble-ccore
LOCK=$HOME/.cache/bimble-measure.lock
TOOLS="ios bimble bload blink"
mkdir -p "$W/res" "$W/logs" "$W/bimble-cache"
declare -A fails stopped
for t in $TOOLS; do fails[$t]=0; stopped[$t]=0; done

run() { # tool nn outname -- cmd...
  local tool=$1 nn=$2 name=$3; shift 4
  rm -f "$W/res/$name.m.json"
  # Wait for 9 GiB available WITHOUT the lock; then take the shared lock only for the run itself
  # (flock -n: if busy, release nothing, wait and retry). The run is in its own cgroup scope:
  # MemoryMax=8G, no swap; an oom-kill there is a result.
  until [ -f "$W/res/$name.m.json" ]; do
    until [ "$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)" -ge 9 ]; do sleep 20; done
    flock -n "$LOCK" bash -c '
      [ "$(awk "/MemAvailable/{print int(\$2/1048576)}" /proc/meminfo)" -ge 9 ] || exit 0
      echo "== $(date -Is) $1" >> "$2/logs/free.log"; free -g >> "$2/logs/free.log"
      shift 2; exec "$@"' _ "$name" "$W" \
      systemd-run --user --scope -q -p MemoryMax=8G -p MemorySwapMax=0 -p OOMPolicy=continue \
      python3 -I "$S/measure_run.py" --timeout-s 1200 --out "$W/res/$name.m.json" -- "$@" \
      > "$W/logs/$name.log" 2>&1
    [ -f "$W/res/$name.m.json" ] || sleep 30
  done
  local rc
  rc=$(python3 -c "import json,sys;d=json.load(open(sys.argv[1]));print(1 if d['exit_code'] or d['timed_out'] else 0)" "$W/res/$name.m.json" 2>/dev/null || echo 1)
  echo "$(date +%T) $name rc=$rc $(python3 -c "import json,sys;d=json.load(open(sys.argv[1]));print({k:d[k] for k in ['wall_s','max_rss_mb','cgroup_memory_peak_mb','cgroup_oom_kills','exit_code','mem_available_gb_at_start']})" "$W/res/$name.m.json" 2>/dev/null)" >> "$W/logs/series.log"
  return "$rc"
}

step() { # tool nn name -- cmd...
  local tool=$1
  if [ "${stopped[$tool]}" = 1 ]; then return; fi
  if run "$@"; then fails[$tool]=0; else
    fails[$tool]=$((fails[$tool] + 1))
    if [ "${fails[$tool]}" -ge 2 ]; then stopped[$tool]=1; echo "STOP $tool at $2" >> "$W/logs/series.log"; fi
  fi
}

for n in $(seq -w "$1" "$2"); do
  f=$W/files/f$n.ifc
  step ios "$n" "ios_f$n" -- "$PY" -I "$S/ios_geom_bench.py" "$f" --json "$W/res/ios_f$n.json"
  step bimble "$n" "bim_f$n" -- "$BIM" link-convert --cache-dir "$W/bimble-cache" "$f"
  step bload "$n" "bload_f$n" -- "$FR" blender -b --python /scripts/bonsai_load_bench.py -- \
    "/work/files/f$n.ifc" --mode load --force-large --save "/work/bonsai-run/f$n.blend" --json "/work/res/bload_f$n.json"
  ls -l "$W/bonsai-run/f$n.blend" >> "$W/logs/series.log" 2>/dev/null; rm -f "$W/bonsai-run/f$n.blend" "$W/bonsai-run/f$n.blend1"
  if [ "${stopped[blink]}" = 0 ]; then
    step blink "$n" "blink1_f$n" -- "$FR" blender -b --python /scripts/bonsai_load_bench.py -- \
      "/work/files/f$n.ifc" --mode link --json "/work/res/blink1_f$n.json"
    if [ -f "$W/files/f$n.ifc.cache.blend" ]; then
      step blink "$n" "blink2_f$n" -- "$FR" blender -b --python /scripts/bonsai_load_bench.py -- \
        "/work/files/f$n.ifc" --mode link --json "/work/res/blink2_f$n.json"
    fi
    ls -l "$W/files/f$n.ifc".cache.* >> "$W/logs/series.log" 2>/dev/null
    rm -f "$W/files/f$n.ifc".cache.*
  fi
done
echo "DONE $1-$2 $(date +%T)" >> "$W/logs/series.log"
