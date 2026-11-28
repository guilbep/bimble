#!/usr/bin/env bash
# A crash in a session log made readable (ticket NUI2): the raw addresses the crash handler wrote into
# <session folder>/session.log ("bimble(+0x2ae70) [...]") as functions and lines, against the debug file of the
# same build (its build id, logged at start; ccore/out/app/debug/<build id>.debug, kept by native/build.sh app).
#   ccore/native/symbolize.sh [session folder]     (default: bimble --last-log's)  [DEBUG_DIR=ccore/out/app/debug]
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:-$("$here/out/app/bimble" --last-log)}
log=$dir/session.log
[[ -f $log ]] || { echo "symbolize: no $log" >&2; exit 2; }
bid=$(grep -o 'build id [0-9a-f]*' "$log" | head -1 | awk '{print $3}')
dbg=${DEBUG_DIR:-$here/out/app/debug}/$bid.debug
[[ -n $bid && -f $dbg ]] || { echo "symbolize: no debug file for build id '${bid:-?}' in ${DEBUG_DIR:-$here/out/app/debug}" >&2; exit 3; }
sym=$(command -v llvm-symbolizer || true)
grep -E 'bimble\(\+0x[0-9a-f]+\)' "$log" | sed -E 's/.*bimble\(\+(0x[0-9a-f]+)\).*/\1/' | while read -r off; do
    if [[ -n $sym ]]; then printf '%s  ' "$off"; "$sym" --obj="$dbg" --relative-address "$off" | head -2 | paste -sd' '
    else printf '%s  ' "$off"; addr2line -f -C -e "$dbg" "$off" | paste -sd' '; fi
done
