#!/usr/bin/env bash
# Ticket J1: builds bench/json_bench.c (clang -O2 on out/native, run `build.sh native` first) and
# runs it on the synthetic models of bench/large_model.py (2k / 20k / 60k; out/j1/, not
# committed), then the page-side JSON.parse of the protocol answers (bench/json_parse.mjs).
#   bench/json_bench.sh [targets…]   (default: 2000 20000 60000). Raw JSON: out/j1/<target>.json
set -euo pipefail
cd "$(dirname "$0")/.."
source ./deps.sh
kernel_setup
mkdir -p out/j1
clang -std=gnu11 -O2 -g0 -ffp-contract=off -DRYU_ONLY_64_BIT_OPS "${KFLAGS[@]}" -Isrc -Ivendor/ryu -Isession -Ihost \
    bench/json_bench.c out/native/libccore_session.a out/native/libccore.a "${KLINK_CLANG[@]}" -lm -o out/j1/json_bench
targets=("$@")
((${#targets[@]})) || targets=(2000 20000 60000)
for t in "${targets[@]}"; do
    [[ -f out/j1/m$t.bimble ]] || python3 bench/large_model.py "$t" "out/j1/m$t.bimble" > /dev/null
    mkdir -p "out/j1/w$t"
    out/j1/json_bench "out/j1/m$t.bimble" "out/j1/w$t" 5 > "out/j1/$t.json" 2> "out/j1/$t.log"
    node bench/json_parse.mjs "out/j1/w$t" > "out/j1/$t.parse.json"
    echo "== $t"; cat "out/j1/$t.json" "out/j1/$t.parse.json"
done
