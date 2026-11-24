#!/usr/bin/env bash
# Builds the C slab port: build.sh tcc | native | wasm
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
src=(slab.c main_native.c)
case "${1:-}" in
  tcc)    tcc "${src[@]}" -lm -o build/slab_tcc ;;
  native) clang -std=c99 -O0 -g0 "${src[@]}" -lm -o build/slab ;;
  wasm)   echo "not yet (T3)" >&2; exit 2 ;;
  *) echo "usage: $0 tcc|native|wasm" >&2; exit 2 ;;
esac
