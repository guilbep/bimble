#!/bin/bash
# The measurement series of docs/research/2026-11-27-bonsai-vs-bimble-loading.md, one file after the
# other in ascending size: IfcOpenShell (parse + geometry), Bimble's C port (link-convert), Bonsai
# load_project, Bonsai link (first = cache creation, second = from the cache).
# Each run: started only with MemAvailable >= 9 GiB (waited for outside the lock), under the shared
# lock taken with flock -n for the run only, `free -g` logged, in a cgroup scope with MemoryMax=8G and no swap (systemd-run --user --scope), 20 minutes. A tool stops after two failures in a row (the wall, then the
# confirmation on the next file).
# This file: the runs in the order given, e.g. bonsai_queue.sh bload:12 blink:14 ios:15 bim:11
# (same run() as bonsai_series.sh; used when the machine was too busy for the whole series)
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

one() { # kind nn
  local k=$1 n=$2 f=$W/files/f$2.ifc
  case $k in
    ios) run ios "$n" "ios_f$n" -- "$PY" -I "$S/ios_geom_bench.py" "$f" --json "$W/res/ios_f$n.json" ;;
    bim) rm -f "$W/bimble-cache/links/"*"-f$n.ifc."*
         run bimble "$n" "bim_f$n" -- "$BIM" link-convert --cache-dir "$W/bimble-cache" "$f" ;;
    bload) run bload "$n" "bload_f$n" -- "$FR" blender -b --python /scripts/bonsai_load_bench.py -- \
             "/work/files/f$n.ifc" --mode load --force-large --save "/work/bonsai-run/f$n.blend" --json "/work/res/bload_f$n.json"
           ls -l "$W/bonsai-run/f$n.blend" >> "$W/logs/series.log" 2>/dev/null; rm -f "$W/bonsai-run/f$n.blend"* ;;
    blink) rm -f "$W/files/f$n.ifc".cache.*
           run blink "$n" "blink1_f$n" -- "$FR" blender -b --python /scripts/bonsai_load_bench.py -- \
             "/work/files/f$n.ifc" --mode link --json "/work/res/blink1_f$n.json"
           if [ -f "$W/files/f$n.ifc.cache.blend" ]; then
             run blink "$n" "blink2_f$n" -- "$FR" blender -b --python /scripts/bonsai_load_bench.py -- \
               "/work/files/f$n.ifc" --mode link --json "/work/res/blink2_f$n.json"
           fi
           ls -l "$W/files/f$n.ifc".cache.* >> "$W/logs/series.log" 2>/dev/null; rm -f "$W/files/f$n.ifc".cache.* ;;
  esac
}
for r in "$@"; do one "${r%%:*}" "${r##*:}"; done
echo "QUEUE DONE $* $(date +%T)" >> "$W/logs/series.log"
