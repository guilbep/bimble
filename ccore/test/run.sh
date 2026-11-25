#!/usr/bin/env bash
# Runs ccore test suites: test/run.sh [suite...] (default: all). Builds with tcc, or with $CC.
# Exits non-zero if any check fails.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
ALL=(); for f in test/*_test.c; do t=${f#test/}; ALL+=("${t%_test.c}"); done
suites=("$@")
[[ ${#suites[@]} -eq 0 ]] && suites=("${ALL[@]}")
CC=${CC:-tcc}
case "$CC" in
  tcc) FLAGS=(-g -Wall -DRYU_ONLY_64_BIT_OPS) ;;
  *)   FLAGS=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function) ;;
esac
# Extra flags, e.g. CC=clang CFLAGS="-fsanitize=address,undefined -g" test/run.sh file
if [[ -n ${CFLAGS:-} ]]; then read -r -a extra <<< "$CFLAGS"; FLAGS+=("${extra[@]}"); fi
out="out/run-$(basename "$CC")"
mkdir -p "$out"
# Every src/*.c but the wasm stub, as build.sh compiles the library.
SRC=(); for f in src/*.c; do [[ $f == src/wasm_stub.c ]] || SRC+=("$f"); done
status=0
for s in "${suites[@]}"; do
    if [[ ! -f "test/${s}_test.c" ]]; then echo "unknown suite: $s" >&2; status=1; continue; fi
    "$CC" "${FLAGS[@]}" -Isrc -Ivendor/ryu "${SRC[@]}" \
        "test/${s}_test.c" -o "$out/test_$s"
    if ! "$out/test_$s" golden/floats; then status=1; fi
done
exit $status
