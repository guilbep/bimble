#!/usr/bin/env bash
# Builds ccore: build.sh tcc | host | native | wasm. Output in ccore/out/<mode>/. Prints ms per step.
#   tcc    debug library (libccore.a), test binaries and the host (bimble-ccore), for the edit loop
#   host   only the host, one tcc call over every source (no archive, no tests): dev.sh's loop
#   native clang -O2, same outputs
#   wasm   clang freestanding wasm32 of src/ (no libc) and the host's call layer (HOST_CALLS:
#          the native host's call code, without its HTTP server and main), ccore.wasm exporting
#          the API of src/wasm_api.h (test: node test/wasm_run.mjs, test/lab_wasm.mjs)
set -euo pipefail
cd "$(dirname "$0")"
# Every src/*.c is the library (wasm_stub.c only in the wasm build); every test/<name>_test.c is a suite.
LIB=(); for f in src/*.c; do [[ $f == src/wasm_stub.c ]] || LIB+=("$f"); done
TESTS=(); for f in test/*_test.c; do t=${f#test/}; TESTS+=("${t%_test.c}"); done
INC=(-Isrc -Ivendor/ryu)
HOST_CALLS=(host/calls.c host/session.c host/mesh.c host/mesh_model.c)

ms() { date +%s%N | cut -b1-13; }
step() { # step <label> <command...>
    local t0 t1
    t0=$(ms)
    "${@:2}"
    t1=$(ms)
    printf '  %-28s %5d ms\n' "$1" $((t1 - t0))
}

lib_and_tests() { # <dir> <cc> <flags...>
    local dir=$1 cc=$2
    shift 2
    mkdir -p "$dir"
    local objs=()
    for f in "${LIB[@]}"; do objs+=("$dir/$(basename "${f%.c}").o"); done
    compile_lib() {
        local i=0
        for f in "${LIB[@]}"; do
            "$cc" "$@" "${INC[@]}" -c "$f" -o "${objs[$i]}"
            i=$((i + 1))
        done
    }
    step "compile library ($cc)" compile_lib "$@"
    if [[ $cc == tcc ]]; then step "archive libccore.a" tcc -ar rcs "$dir/libccore.a" "${objs[@]}"
    else step "archive libccore.a" llvm-ar rcs "$dir/libccore.a" "${objs[@]}"; fi
    for t in "${TESTS[@]}"; do
        step "test binary $t" "$cc" "$@" "${INC[@]}" "test/${t}_test.c" "$dir/libccore.a" -o "$dir/test_$t"
    done
    # The native host (not in wasm): every host/*.c on top of the library.
    step "host bimble-ccore" "$cc" "$@" "${INC[@]}" -Ihost host/*.c "$dir/libccore.a" -o "$dir/bimble-ccore"
}

t_start=$(ms)
case "${1:-}" in
  tcc)
    echo "build.sh tcc -> out/tcc"
    lib_and_tests out/tcc tcc -g -Wall -DRYU_ONLY_64_BIT_OPS ;;
  host)
    echo "build.sh host -> out/tcc/bimble-ccore"
    mkdir -p out/tcc
    step "host bimble-ccore (tcc, one call)" tcc -g -Wall -DRYU_ONLY_64_BIT_OPS "${INC[@]}" -Ihost "${LIB[@]}" host/*.c \
        -o out/tcc/bimble-ccore ;;
  native)
    echo "build.sh native -> out/native"
    lib_and_tests out/native clang -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function ;;
  wasm)
    echo "build.sh wasm -> out/wasm/ccore.wasm"
    mkdir -p out/wasm
    step "clang wasm32 link" clang --target=wasm32 -std=c11 -ffreestanding -nostdlib -O2 \
        -Wall -Werror -Wno-unused-function -DRYU_ONLY_64_BIT_OPS -isystem src/freestanding "${INC[@]}" \
        -Ihost -Wl,--no-entry "${LIB[@]}" src/wasm_stub.c "${HOST_CALLS[@]}" -o out/wasm/ccore.wasm
    printf '  %-28s %5d bytes\n' "ccore.wasm" "$(wc -c < out/wasm/ccore.wasm)"
    printf '  %-28s %5d bytes\n' "ccore.wasm gzip -9" "$(gzip -9 -c out/wasm/ccore.wasm | wc -c)" ;;
  *) echo "usage: $0 tcc|host|native|wasm" >&2; exit 2 ;;
esac
printf '  %-28s %5d ms\n' "total" $(($(ms) - t_start))
