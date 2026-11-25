#!/usr/bin/env bash
# Builds ccore: build.sh tcc | native | wasm. Output in ccore/out/<mode>/. Prints ms per step.
#   tcc    debug library (libccore.a) and test binaries, for the edit loop
#   native clang -O2, same outputs
#   wasm   clang freestanding wasm32 of src/ (no libc), ccore.wasm with a dummy export
set -euo pipefail
cd "$(dirname "$0")"
LIB=(src/arena.c src/buf.c src/fmt.c src/json_w.c)
TESTS=(fmt json)
INC=(-Isrc -Ivendor/ryu)

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
}

t_start=$(ms)
case "${1:-}" in
  tcc)
    echo "build.sh tcc -> out/tcc"
    lib_and_tests out/tcc tcc -g -Wall -DRYU_ONLY_64_BIT_OPS ;;
  native)
    echo "build.sh native -> out/native"
    lib_and_tests out/native clang -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function ;;
  wasm)
    echo "build.sh wasm -> out/wasm/ccore.wasm"
    mkdir -p out/wasm
    step "clang wasm32 link" clang --target=wasm32 -std=c11 -ffreestanding -nostdlib -O2 \
        -Wall -Werror -Wno-unused-function -DRYU_ONLY_64_BIT_OPS -isystem src/freestanding "${INC[@]}" \
        -Wl,--no-entry "${LIB[@]}" src/wasm_stub.c -o out/wasm/ccore.wasm
    printf '  %-28s %5d bytes\n' "ccore.wasm" "$(wc -c < out/wasm/ccore.wasm)" ;;
  *) echo "usage: $0 tcc|native|wasm" >&2; exit 2 ;;
esac
printf '  %-28s %5d ms\n' "total" $(($(ms) - t_start))
