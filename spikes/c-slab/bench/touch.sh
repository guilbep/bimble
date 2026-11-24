#!/usr/bin/env bash
# hyperfine --prepare: replaces the previous spike edit at the end of slab.c with a fresh one,
# so every run compiles a real code change. bench.sh restores slab.c with a trap.
set -euo pipefail
cd "$(dirname "$0")/.."
sed -i '/^int spike_touch_[0-9]*(void)/d' slab.c
n=$RANDOM$RANDOM
echo "int spike_touch_$n(void){return $n;}" >> slab.c
