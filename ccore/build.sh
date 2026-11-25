#!/usr/bin/env bash
# Builds ccore: build.sh tcc | host | native | wasm. Output in ccore/out/<mode>/. Prints ms per step.
#   tcc    debug library (libccore.a), test binaries and the host (bimble-ccore), for the edit loop
#   host   only the host (no archive, no tests): dev.sh's loop. Every source whose clang -O2 object
#          for its exact content (and the headers') is cached in out/hostmix/ is linked from it; the
#          others are compiled by tcc (one call each), then linked by tcc. After the link, clang -O2
#          objects of the tcc-compiled files are made in the background (detached; logs in
#          out/hostmix/refresh.log), so an edit to one file runs it in tcc code until the next build
#          and the rest (scene, loader) at -O2. Same defines for both: -DRYU_ONLY_64_BIT_OPS, and
#          clang with -ffp-contract=off (no fused multiply-add: the same floats as tcc). HOST_MIX=0:
#          one tcc call over every source (the build before ticket T18).
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

# build.sh host (see the header). Cache keys: sha1 of the file, of every header (and the vendored
# ryu sources, included by fmt.c) and of the flags; objects out/hostmix/o2/<name>.<key>.o.
HOST_SRCS=("${LIB[@]}" host/*.c)
TCC_FLAGS=(-g -Wall -DRYU_ONLY_64_BIT_OPS)
O2_FLAGS=(-std=c11 -O2 -g0 -ffp-contract=off -DRYU_ONLY_64_BIT_OPS -Wall -Wno-unused-function)
host_keys() { # prints "<key> <file>" per source
    local hh
    hh=$( { printf '%s\n' "${O2_FLAGS[*]}" "${TCC_FLAGS[*]}"; cat src/*.h host/*.h vendor/ryu/ryu/*; } | sha1sum | cut -c1-16)
    local sum f
    while read -r sum f; do echo "${sum:0:16}$hh $f"; done < <(sha1sum "${HOST_SRCS[@]}")
}
host_mixed() {
    if [[ ${HOST_MIX:-1} == 0 ]]; then
        mkdir -p out/tcc
        step "host bimble-ccore (tcc, one call)" tcc "${TCC_FLAGS[@]}" "${INC[@]}" -Ihost "${HOST_SRCS[@]}" -o out/tcc/bimble-ccore
        return
    fi
    local M=out/hostmix t0 t1 key f b objs=() changed=()
    mkdir -p $M/o2 $M/tcc out/tcc
    t0=$(ms)
    while read -r key f; do
        b=${f##*/} b=${b%.c}
        if [[ -f $M/o2/$b.$key.o ]]; then objs+=("$M/o2/$b.$key.o")
        else
            tcc "${TCC_FLAGS[@]}" "${INC[@]}" -Ihost -c "$f" -o "$M/tcc/$b.o"
            objs+=("$M/tcc/$b.o") changed+=("$f")
        fi
    done < <(host_keys)
    t1=$(ms)
    printf '  %-28s %5d ms\n' "tcc: ${#changed[@]} of ${#HOST_SRCS[@]} files" $((t1 - t0))
    step "link (tcc; others clang -O2)" tcc -o out/tcc/bimble-ccore "${objs[@]}" -lm
    ((${#changed[@]})) || return 0
    # The -O2 objects of the changed files, in the background: each keyed by the content it was
    # compiled from (checked again after the compile: a file saved meanwhile is not cached).
    (
        exec > $M/refresh.log 2>&1 < /dev/null
        for f in "${changed[@]}"; do
            (
                b=$(basename "${f%.c}")
                k1=$(host_keys | awk -v f="$f" '$2 == f {print $1}')
                clang "${O2_FLAGS[@]}" "${INC[@]}" -Ihost -c "$f" -o "$M/o2/$b.tmp.$$.$BASHPID.o" || exit 0
                k2=$(host_keys | awk -v f="$f" '$2 == f {print $1}')
                if [[ $k1 == "$k2" ]]; then mv "$M/o2/$b.tmp.$$.$BASHPID.o" "$M/o2/$b.$k1.o"
                else rm -f "$M/o2/$b.tmp.$$.$BASHPID.o"; fi
                # Keep the 4 newest objects per file.
                ls -t "$M/o2/$b".*.o 2> /dev/null | grep -v '\.tmp\.' | tail -n +5 | xargs -r rm -f
            ) &
        done
        wait
        echo "refreshed: ${changed[*]}"
    ) &
    disown
}

t_start=$(ms)
case "${1:-}" in
  tcc)
    echo "build.sh tcc -> out/tcc"
    lib_and_tests out/tcc tcc -g -Wall -DRYU_ONLY_64_BIT_OPS ;;
  host)
    echo "build.sh host -> out/tcc/bimble-ccore"
    host_mixed ;;
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
