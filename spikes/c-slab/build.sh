#!/usr/bin/env bash
# Builds the C slab port: build.sh tcc | native | wasm | wasm-dev (wasm: freestanding, main_wasm.c includes slab.c)
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
src=(slab.c main_native.c)
case "${1:-}" in
  tcc)    tcc "${src[@]}" -lm -o build/slab_tcc ;;
  native) clang -std=c99 -O0 -g0 "${src[@]}" -lm -o build/slab ;;
  wasm)     clang --target=wasm32 -std=c99 -ffreestanding -nostdlib -O2 -Wl,--no-entry main_wasm.c -o build/slab.wasm ;;
  wasm-dev) clang --target=wasm32 -std=c99 -ffreestanding -nostdlib -O0 -g0 -Wl,--no-entry main_wasm.c -o build/slab_dev.wasm ;;
  *) echo "usage: $0 tcc|native|wasm|wasm-dev" >&2; exit 2 ;;
esac
