#!/usr/bin/env bash
# A crash in a session log made readable (ticket NUI2): the raw addresses the crash handler wrote into
# <session folder>/session.log ("bimble(+0x2ae70) [...]") as functions and lines, against the debug file of the
# same build (its build id, logged at start; ccore/out/app/debug/<build id>.debug, kept by native/build.sh app).
# macOS: atos against the build's .dSYM (out/app/debug/<UUID>.dSYM; DSYM=<path> for a release's), below.
#   ccore/native/symbolize.sh [session folder]     (default: bimble --last-log's)  [DEBUG_DIR=ccore/out/app/debug]
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
dir=${1:-$("$here/out/app/bimble" --last-log)}
log=$dir/session.log
[[ -f $log ]] || { echo "symbolize: no $log" >&2; exit 2; }
if [[ $(uname -s) == Darwin ]]; then
    # MACAPP (ADR 0088): macOS's backtrace lines hold absolute addresses ("bimble 0x0000000102f1a2b4 ..."); the log's
    # build id is the executable's LC_UUID and its base the Mach-O header's address: atos against the .dSYM of that
    # UUID (out/app/debug/<UUID>.dSYM, or DSYM= one unpacked from bimble-<v>-macos-<arch>.dSYM.zip)
    uuid=$(grep -o 'build id [0-9A-F-]*' "$log" | head -1 | awk '{print $3}')
    base=$(grep -o 'executable base 0x[0-9a-f]*' "$log" | head -1 | awk '{print $3}')
    ds=${DSYM:-${DEBUG_DIR:-$here/out/app/debug}/$uuid.dSYM}
    dw=$(ls "$ds"/Contents/Resources/DWARF/* 2> /dev/null | head -1)
    [[ -n $uuid && -n $base && -f $dw ]] || { echo "symbolize: no .dSYM for UUID '${uuid:-?}' (base ${base:-?}) at $ds" >&2; exit 3; }
    arch=$(dwarfdump --uuid "$dw" | grep "$uuid" | sed -E 's/.*\(([a-z0-9_]+)\).*/\1/' | head -1)
    grep -E '^[0-9]+ +bimble +0x[0-9a-f]+' "$log" | awk '{print $3}' | while read -r addr; do
        printf '%s  ' "$addr"; atos -o "$dw" -arch "${arch:-$(uname -m)}" -l "$base" "$addr"
    done
    exit 0
fi
bid=$(grep -o 'build id [0-9a-f]*' "$log" | head -1 | awk '{print $3}')
dbg=${DEBUG_DIR:-$here/out/app/debug}/$bid.debug
[[ -n $bid && -f $dbg ]] || { echo "symbolize: no debug file for build id '${bid:-?}' in ${DEBUG_DIR:-$here/out/app/debug}" >&2; exit 3; }
sym=$(command -v llvm-symbolizer || true)
grep -E 'bimble\(\+0x[0-9a-f]+\)' "$log" | sed -E 's/.*bimble\(\+(0x[0-9a-f]+)\).*/\1/' | while read -r off; do
    if [[ -n $sym ]]; then printf '%s  ' "$off"; "$sym" --obj="$dbg" --relative-address "$off" | head -2 | paste -sd' '
    else printf '%s  ' "$off"; addr2line -f -C -e "$dbg" "$off" | paste -sd' '; fi
done
