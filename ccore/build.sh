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
#   (tcc and native also build tools/*.c, e.g. ifc_dump: README "Comparing with Rust")
#   wasm   clang freestanding wasm32 of src/ (no libc) and the session layer (session/*.c: the
#          native host's call code, without the host's HTTP server, platform and main), ccore.wasm
#          exporting the API of src/wasm_api.h (test: node test/wasm_run.mjs, test/lab_wasm.mjs)
#   wasm-view  the viewer-only wasm (ADR 0069 rule 3, ticket W1): out/wasm/ccore-view.wasm, the
#          wasm build without VIEW_DENY (verbs, the verb table, the IFC import and writer), with
#          -DCC_VIEW_ONLY (session/view_only.c, wasm_api.c VIEW_ONLY, wasm_stub.c's stubs) and
#          VIEW_OPT; fails over VIEW_MAX bytes (test: node test/wasm_view_run.mjs)
#   wasm-size [view]  bytes per object file in ccore.wasm (or ccore-view.wasm), from a link map
#          (out/wsize/): code and data, largest first (results/wasm-size.md)
# Boolean kernel (ADR 0068, 0069): manifold's C binding from $MANIFOLD_DIR or out/deps/manifold
# (deps.sh). Found: -DCC_HAVE_MANIFOLD, clang links the static libraries (+ libstdc++), tcc the
# shared one (it cannot link C++ archives). Not found, or CC_NO_KERNEL=1, or wasm: no kernel.
# Layers (README "Layers"): src/ is L0-L3 (and the wasm carrier, wasm_api.c), session/ is L5, host/
# is the native carrier (L6). Test suites link the library, session/ and host/platform_native.c.
set -euo pipefail
cd "$(dirname "$0")"
# Every src/*.c is the library (wasm_stub.c only in the wasm build); every test/<name>_test.c is a suite.
LIB=(); for f in src/*.c; do [[ $f == src/wasm_stub.c ]] || LIB+=("$f"); done
TESTS=(); for f in test/*_test.c; do t=${f#test/}; TESTS+=("${t%_test.c}"); done
INC=(-Isrc -Ivendor/ryu -Isession)
SESSION=(session/*.c)
# The viewer-only wasm: the files it leaves out, its optimisation and its budget (bytes).
VIEW_DENY=(src/verbs.c src/verb_table.c src/ifc_in.c src/ifc_out.c)
VIEW_OPT=-Os
VIEW_MAX=400000
HOST=(host/*.c)
source ./deps.sh
kernel_setup

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
    set -- "$@" "${KFLAGS[@]}"
    local klink=("${KLINK_CLANG[@]}")
    [[ $cc == tcc ]] && klink=("${KLINK_TCC[@]}")
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
    # The session layer (L5) and the native platform: an archive the suites and the host link.
    local sobjs=()
    for f in "${SESSION[@]}" host/platform_native.c; do sobjs+=("$dir/$(basename "${f%.c}").o"); done
    compile_session() {
        local i=0
        for f in "${SESSION[@]}" host/platform_native.c; do
            "$cc" "$@" "${INC[@]}" -Ihost -c "$f" -o "${sobjs[$i]}"
            i=$((i + 1))
        done
    }
    step "compile session ($cc)" compile_session "$@"
    if [[ $cc == tcc ]]; then step "archive libccore_session.a" tcc -ar rcs "$dir/libccore_session.a" "${sobjs[@]}"
    else step "archive libccore_session.a" llvm-ar rcs "$dir/libccore_session.a" "${sobjs[@]}"; fi
    for t in "${TESTS[@]}"; do
        step "test binary $t" "$cc" "$@" "${INC[@]}" -Ihost "test/${t}_test.c" "$dir/libccore_session.a" "$dir/libccore.a" "${klink[@]}" -o "$dir/test_$t"
    done
    # Tools (tools/*.c, one binary each): on the library only.
    for f in tools/*.c; do
        [[ -e $f ]] || continue
        local tn
        tn=$(basename "${f%.c}")
        step "tool $tn" "$cc" "$@" "${INC[@]}" "$f" "$dir/libccore.a" "${klink[@]}" -o "$dir/$tn"
    done
    # The native host (not in wasm): every host/*.c on top of session/ and the library.
    step "host bimble-ccore" "$cc" "$@" "${INC[@]}" -Ihost "${HOST[@]}" "${SESSION[@]}" "$dir/libccore.a" "${klink[@]}" -o "$dir/bimble-ccore"
}

# build.sh host (see the header). Cache keys: sha1 of the file, of every header (and the vendored
# ryu sources, included by fmt.c) and of the flags; objects out/hostmix/o2/<name>.<key>.o.
HOST_SRCS=("${LIB[@]}" "${SESSION[@]}" "${HOST[@]}")
TCC_FLAGS=(-g -Wall -DRYU_ONLY_64_BIT_OPS "${KFLAGS[@]}")
O2_FLAGS=(-std=c11 -O2 -g0 -ffp-contract=off -DRYU_ONLY_64_BIT_OPS -Wall -Wno-unused-function "${KFLAGS[@]}")
# clang on aarch64 makes atomics (src/jobs.h) calls to libgcc's outline helpers
# (__aarch64_cas8_relax...), which the tcc link has not: inline them.
[[ $(uname -m) == aarch64 ]] && O2_FLAGS+=(-mno-outline-atomics)
host_keys() { # prints "<key> <file>" per source
    local hh
    hh=$( { printf '%s\n' "${O2_FLAGS[*]}" "${TCC_FLAGS[*]}"; cat src/*.h session/*.h host/*.h vendor/ryu/ryu/*; } | sha1sum | cut -c1-16)
    local sum f
    while read -r sum f; do echo "${sum:0:16}$hh $f"; done < <(sha1sum "${HOST_SRCS[@]}")
}
host_mixed() {
    if [[ ${HOST_MIX:-1} == 0 ]]; then
        mkdir -p out/tcc
        step "host bimble-ccore (tcc, one call)" tcc "${TCC_FLAGS[@]}" "${INC[@]}" -Ihost "${HOST_SRCS[@]}" "${KLINK_TCC[@]}" -o out/tcc/bimble-ccore
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
    step "link (tcc; others clang -O2)" tcc -o out/tcc/bimble-ccore "${objs[@]}" -lm "${KLINK_TCC[@]}"
    ((${#changed[@]})) || return 0
    # The -O2 objects of the changed files, in the background: each keyed by the content it was
    # compiled from (checked again after the compile: a file saved meanwhile is not cached).
    (
        exec > $M/refresh.log 2>&1 < /dev/null
        for f in "${changed[@]}"; do
            (
                b=$(basename "${f%.c}")
                k1=$(host_keys | awk -v f="$f" '$2 == f {print $1}')
                # Compile a snapshot, not $f: clang maps its input, and a file truncated in place
                # meanwhile (an editor or a test saving it) made clang die with SIGBUS.
                snap="$M/o2/$b.src.$$.$BASHPID.c"
                cp "$f" "$snap" || exit 0
                clang "${O2_FLAGS[@]}" -iquote "$(dirname "$f")" "${INC[@]}" -Ihost -c "$snap" \
                    -o "$M/o2/$b.tmp.$$.$BASHPID.o" || { rm -f "$snap"; exit 0; }
                rm -f "$snap"
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

# The wasm builds. VIEW_LIB: the library without VIEW_DENY.
VIEW_LIB=()
for f in "${LIB[@]}"; do [[ " ${VIEW_DENY[*]} " == *" $f "* ]] || VIEW_LIB+=("$f"); done
WASM_FLAGS=(--target=wasm32 -std=c11 -ffreestanding -nostdlib -Wall -Werror -Wno-unused-function
    -DRYU_ONLY_64_BIT_OPS -isystem src/freestanding "${INC[@]}")
wasm_build() { # <out name> <opt> [-D...] <library files...>: one clang call, the session layer added
    local out=$1 opt=$2
    shift 2
    mkdir -p out/wasm
    step "clang wasm32 link" clang "${WASM_FLAGS[@]}" "$opt" -Wl,--no-entry "$@" src/wasm_stub.c \
        "${SESSION[@]}" -o "out/wasm/$out"
}
wasm_sizes() {
    printf '  %-28s %5d bytes\n' "$1" "$(wc -c < "out/wasm/$1")"
    printf '  %-28s %5d bytes\n' "$1 gzip -9" "$(gzip -9 -c "out/wasm/$1" | wc -c)"
}
# wasm_size_table [view]: the same build in two steps (one object per file in out/wsize/, then a link
# with a map), and the map summed per object: code bytes, data bytes (rodata and data, not bss);
# "<internal>" is the merged string literals. The link is the shipped one (same flags and files).
wasm_size_table() {
    local opt=-O2 defs=() files=("${LIB[@]}") d=out/wsize objs=() f o
    [[ ${1:-} == view ]] && opt=$VIEW_OPT defs=(-DCC_VIEW_ONLY) files=("${VIEW_LIB[@]}")
    rm -rf "$d" && mkdir -p "$d"
    for f in "${files[@]}" src/wasm_stub.c "${SESSION[@]}"; do
        o="$d/$(basename "${f%.c}").o"
        clang "${WASM_FLAGS[@]}" "$opt" "${defs[@]}" -c "$f" -o "$o" &
        objs+=("$o")
    done
    wait
    clang --target=wasm32 -nostdlib "$opt" -Wl,--no-entry -Wl,-Map=$d/map.txt "${objs[@]}" -o $d/size.wasm
    printf '%s: %d bytes, gzip -9 %d bytes\n' "${1:-full}" "$(wc -c < $d/size.wasm)" "$(gzip -9 -c $d/size.wasm | wc -c)"
    printf '| %-20s | %7s | %7s | %7s |\n|---|---:|---:|---:|\n' file total code data
    awk 'function hex(s,  i, n) { for (i = 1; i <= length(s); i++) n = n * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1; return n }
        / CODE$/ {sec = "c"; next} / DATA$/ {sec = "d"; next} / \.bss$/ || / CUSTOM/ {sec = ""; next}
        sec != "" && match($0, /[^ ]+:\(/) {
            o = substr($0, RSTART, RLENGTH - 2); sub(/.*\//, "", o); split($0, a, " ")
            n = hex(a[3]); if (sec == "c") c[o] += n; else dd[o] += n; seen[o] = 1 }
        END { for (o in seen) printf "| %-20s | %7d | %7d | %7d |\n", o, c[o] + dd[o], c[o], dd[o] }' $d/map.txt |
        sort -t'|' -k3 -rn
}

t_start=$(ms)
if [[ ${1:-} == wasm* ]]; then echo "  boolean kernel: none in wasm (openings and boolean results drawn uncut)"
elif ((${#KFLAGS[@]})); then echo "  boolean kernel: manifold (out/deps/manifold)"
else echo "  boolean kernel: none (openings and boolean results drawn uncut)"; fi
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
    wasm_build ccore.wasm -O2 "${LIB[@]}"
    wasm_sizes ccore.wasm ;;
  wasm-view)
    echo "build.sh wasm-view -> out/wasm/ccore-view.wasm"
    wasm_build ccore-view.wasm "$VIEW_OPT" -DCC_VIEW_ONLY "${VIEW_LIB[@]}"
    wasm_sizes ccore-view.wasm
    bytes=$(wc -c < out/wasm/ccore-view.wasm)
    if ((bytes > VIEW_MAX)); then
        echo "FAIL ccore-view.wasm is $bytes bytes, over the budget of $VIEW_MAX (ADR 0069)" >&2
        exit 1
    fi ;;
  wasm-size)
    wasm_size_table "${2:-}" ;;
  *) echo "usage: $0 tcc|host|native|wasm|wasm-view|wasm-size [view]" >&2; exit 2 ;;
esac
printf '  %-28s %5d ms\n' "total" $(($(ms) - t_start))
