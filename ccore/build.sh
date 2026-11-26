#!/usr/bin/env bash
# Builds ccore: build.sh tcc | host | native | wasm | release. Output in ccore/out/<mode>/. Prints ms per step.
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
#   asan   only the host, clang -O1 with ASan and UBSan: out/asan/bimble-ccore (ci/run.sh san)
#   (tcc and native also build tools/*.c, e.g. ifc_dump: README "Comparing with Rust")
#   wasm   clang freestanding wasm32 of src/ (no libc) and the session layer (session/*.c: the
#          native host's call code, without the host's HTTP server, platform and main), ccore.wasm
#          exporting the API of src/wasm_api.h (test: node test/wasm_run.mjs, test/lab_wasm.mjs)
#   wasm-view  the viewer-only wasm (ADR 0069 rule 3, ticket W1): out/wasm/ccore-view.wasm, the
#          wasm build without VIEW_DENY (verbs, the verb table, the IFC import and writer), with
#          -DCC_VIEW_ONLY (session/view_only.c, wasm_api.c VIEW_ONLY, wasm_stub.c's stubs) and
#          VIEW_OPT; then wasm-opt VIEW_OPT (binaryen, required: $WASM_OPT or wasm-opt); fails over
#          VIEW_MAX bytes (test: node test/wasm_view_run.mjs). Clang runs wasm-opt by itself when it
#          finds one, and says nothing: the size depended on what was installed (426 KB on the first
#          CI runner, 352 KB on the dev machine, ticket K2). So the link asks clang not to
#          (--no-wasm-opt, clang 20 and later) and the step is explicit, timed and versioned.
#   release  the release archive (ticket R1, ADR 0069 rule 2): the host with clang -O2 and
#          the boolean kernel static (fails without it), the lab built by Vite (web-ui/, needs
#          node_modules), then out/release/bimble-ccore-<VERSION>-<arch>-<os>.tar.gz holding
#          bimble-ccore, web/ (no font: SIZE1, the PDF is Helvetica by default), LICENSE, README.txt,
#          THIRD-PARTY.txt and licences/ (FIXDOC; Linux: hardened, -z relro -z now)
#          (test: node test/release_test.mjs). <os> is
#          linux (libstdc++ static) or macos (ticket MAC1: libc++ is the system's, stripped then
#          signed ad hoc; CMAKE_OSX_ARCHITECTURES=x86_64 builds the Intel one on an arm64 Mac, with
#          deps.sh run under the same variable; MACOSX_DEPLOYMENT_TARGET sets the oldest macOS)
#   wasm-size [view]  bytes per object file in ccore.wasm (or ccore-view.wasm), from a link map
#          (out/wsize/): code and data, largest first (results/wasm-size.md)
# Boolean kernel (ADR 0068, 0069): manifold's C binding from $MANIFOLD_DIR or out/deps/manifold
# (deps.sh). Found: -DCC_HAVE_MANIFOLD, clang links the static libraries (+ libstdc++, libc++ on
# macOS), tcc the shared one (it cannot link C++ archives; none on macOS, where tcc is not used).
# Not found, or CC_NO_KERNEL=1, or wasm: no kernel.
# Portability (ticket MAC1, portable.sh): runs under macOS's /bin/bash 3.2 and BSD tools for
# native, release and the suites (not tcc, host, wasm: no tcc and no wasm-ld on a Mac).
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
VIEW_DENY=(src/verbs.c src/verb_table.c src/verbs_concrete.c src/verbs_sheets.c src/ifc_in.c src/ifc_out.c)
VIEW_DENY+=(src/draw_cut.c src/paper.c src/draw_style.c src/draw_plan.c src/draw_annot.c src/draw_view.c) # drawings on paper (D1-D4): not in the viewer
VIEW_DENY+=(src/draw_sheet.c) # sheets on paper (D5)
VIEW_DENY+=(src/pdf_out.c) # the PDF writer (D6): not in the viewer
VIEW_DENY+=(src/repo.c src/diff.c) # the history store (H1): no history folder in the browser (session/versions.c)
VIEW_DENY+=(src/repo_write.c) # H2a: writing the history store (no folder in the browser)
VIEW_DENY+=(src/merge.c src/repo_merge.c) # H2b: merging history branches (no folder in the browser)
VIEW_DENY+=(src/pdf_in.c src/ref_read.c) # L1: reading a PDF is link_pdf, an edit (view_only.c): the viewer shows the stored references only
VIEW_DENY+=(src/dxf_in.c) # L2: reading a DXF is link_dxf, an edit, as link_pdf
VIEW_DENY+=(src/verbs_views.c) # SEC1: view_add, view_remove, view_edit are verbs (the table is not in the viewer)
VIEW_OPT=-Os
VIEW_MAX=1000000 # Pierre, 25 Nov 2026: up to 1-2 MB is acceptable for the viewer; watch performance (Lighthouse), not bytes
HOST=(host/*.c)
# The version (`bimble-ccore --version`, the release archive's name): ccore/VERSION, one line.
CC_VER=$(< VERSION)
[[ $CC_VER =~ ^[0-9A-Za-z.+-]+$ ]] || { echo "build.sh: ccore/VERSION is not a version: \"$CC_VER\"" >&2; exit 2; }
VDEF="-DCC_RELEASE_VERSION=\"$CC_VER\""
source ./deps.sh
kernel_setup
# The archiver of clang's objects: llvm-ar where clang comes from LLVM, else the system's ar (macOS).
AR=${AR:-$(command -v llvm-ar || echo ar)}

ms() { ms_now; }
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
    set -- "$@" ${KFLAGS[@]+"${KFLAGS[@]}"} ${PLATFORM_CFLAGS[@]+"${PLATFORM_CFLAGS[@]}"}
    local klink=(${KLINK_CLANG[@]+"${KLINK_CLANG[@]}"})
    [[ $cc == tcc ]] && klink=(${KLINK_TCC[@]+"${KLINK_TCC[@]}"})
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
    else step "archive libccore.a" "$AR" rcs "$dir/libccore.a" "${objs[@]}"; fi
    # The session layer (L5) and the native platform: an archive the suites and the host link.
    local sobjs=()
    # WIN1: host/platform_win.c is the Windows one (empty elsewhere), platform_native.c the POSIX one.
    for f in "${SESSION[@]}" host/platform_native.c host/platform_win.c; do sobjs+=("$dir/$(basename "${f%.c}").o"); done
    compile_session() {
        local i=0
        for f in "${SESSION[@]}" host/platform_native.c host/platform_win.c; do
            "$cc" "$@" "${INC[@]}" -Ihost -c "$f" -o "${sobjs[$i]}"
            i=$((i + 1))
        done
    }
    step "compile session ($cc)" compile_session "$@"
    if [[ $cc == tcc ]]; then step "archive libccore_session.a" tcc -ar rcs "$dir/libccore_session.a" "${sobjs[@]}"
    else step "archive libccore_session.a" "$AR" rcs "$dir/libccore_session.a" "${sobjs[@]}"; fi
    for t in "${TESTS[@]}"; do
        step "test binary $t" "$cc" "$@" "${INC[@]}" -Ihost "test/${t}_test.c" "$dir/libccore_session.a" "$dir/libccore.a" ${klink[@]+"${klink[@]}"} -o "$dir/test_$t"
    done
    # Tools (tools/*.c, one binary each): on the library only.
    for f in tools/*.c; do
        [[ -e $f ]] || continue
        local tn
        tn=$(basename "${f%.c}")
        step "tool $tn" "$cc" "$@" "${INC[@]}" "$f" "$dir/libccore.a" ${klink[@]+"${klink[@]}"} -o "$dir/$tn"
    done
    # The native host (not in wasm): every host/*.c on top of session/ and the library.
    step "host bimble-ccore" "$cc" "$@" "$VDEF" "${INC[@]}" -Ihost "${HOST[@]}" "${SESSION[@]}" "$dir/libccore.a" ${klink[@]+"${klink[@]}"} \
        ${PLATFORM_LIBS[@]+"${PLATFORM_LIBS[@]}"} -o "$dir/bimble-ccore" # WIN1: Winsock on Windows
}

# build.sh host (see the header). Cache keys: sha1 of the file, of every header (and the vendored
# ryu sources, included by fmt.c) and of the flags; objects out/hostmix/o2/<name>.<key>.o.
HOST_SRCS=("${LIB[@]}" "${SESSION[@]}" "${HOST[@]}")
TCC_FLAGS=(-g -Wall -DRYU_ONLY_64_BIT_OPS "$VDEF" ${KFLAGS[@]+"${KFLAGS[@]}"})
O2_FLAGS=(-std=c11 -O2 -g0 -ffp-contract=off -DRYU_ONLY_64_BIT_OPS -Wall -Wno-unused-function "$VDEF" ${KFLAGS[@]+"${KFLAGS[@]}"})
# clang on aarch64 makes atomics (src/jobs.h) calls to libgcc's outline helpers
# (__aarch64_cas8_relax...), which the tcc link has not: inline them.
[[ $(uname -m) == aarch64 ]] && O2_FLAGS+=(-mno-outline-atomics)
host_keys() { # prints "<key> <file>" per source
    local hh
    hh=$( { printf '%s\n' "${O2_FLAGS[*]}" "${TCC_FLAGS[*]}"; cat src/*.h session/*.h host/*.h vendor/ryu/ryu/*; } | sha1_16)
    local sum f
    while read -r sum f; do echo "${sum:0:16}$hh $f"; done < <(sha1_each "${HOST_SRCS[@]}")
}
host_mixed() {
    if [[ ${HOST_MIX:-1} == 0 ]]; then
        mkdir -p out/tcc
        step "host bimble-ccore (tcc, one call)" tcc "${TCC_FLAGS[@]}" "${INC[@]}" -Ihost "${HOST_SRCS[@]}" ${KLINK_TCC[@]+"${KLINK_TCC[@]}"} -o out/tcc/bimble-ccore
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
    step "link (tcc; others clang -O2)" tcc -o out/tcc/bimble-ccore "${objs[@]}" -lm ${KLINK_TCC[@]+"${KLINK_TCC[@]}"}
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
                ls -t "$M/o2/$b".*.o 2> /dev/null | grep -v '\.tmp\.' | tail -n +5 | while read -r old; do rm -f "$old"; done
            ) &
        done
        wait
        echo "refreshed: ${changed[*]}"
    ) &
    disown
}

# build.sh asan (ticket FIXDOC): the native host alone, clang -O1 with ASan and UBSan, the kernel
# when found (manifold is not instrumented). For ci/run.sh san: test/san_host.sh runs the host's
# node suites on it and fails on any sanitizer report. One clang call per file, two at a time
# (the dev machine has 16 GB).
host_asan() {
    local d=out/asan f objs=() pids=()
    mkdir -p $d/o
    local flags=(-std=c11 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -Wall -Wno-unused-function
        -DRYU_ONLY_64_BIT_OPS "$VDEF" ${KFLAGS[@]+"${KFLAGS[@]}"} ${PLATFORM_CFLAGS[@]+"${PLATFORM_CFLAGS[@]}"})
    compile_asan() {
        local n=0 rc=0
        for f in "${HOST_SRCS[@]}"; do
            local o="$d/o/$(basename "$(dirname "$f")")_$(basename "${f%.c}").o"
            objs+=("$o")
            clang "${flags[@]}" "${INC[@]}" -Ihost -c "$f" -o "$o" &
            pids+=($!)
            n=$((n + 1))
            if ((n % 2 == 0)); then for p in "${pids[@]}"; do wait "$p" || rc=1; done; pids=(); fi
        done
        for p in ${pids[@]+"${pids[@]}"}; do wait "$p" || rc=1; done
        return $rc
    }
    step "compile (clang -O1, ASan+UBSan)" compile_asan
    step "link bimble-ccore (ASan+UBSan)" clang -fsanitize=address,undefined -pthread "${objs[@]}" \
        ${KLINK_CLANG[@]+"${KLINK_CLANG[@]}"} -lm ${PLATFORM_LIBS[@]+"${PLATFORM_LIBS[@]}"} -o $d/bimble-ccore
}

# The wasm builds. VIEW_LIB: the library without VIEW_DENY.
VIEW_LIB=()
for f in "${LIB[@]}"; do [[ " ${VIEW_DENY[*]} " == *" $f "* ]] || VIEW_LIB+=("$f"); done
WASM_FLAGS=(--target=wasm32 -std=c11 -ffreestanding -nostdlib -Wall -Werror -Wno-unused-function
    -DRYU_ONLY_64_BIT_OPS -isystem src/freestanding "${INC[@]}" "$VDEF") # SET1: $VDEF, query about's build
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
        clang "${WASM_FLAGS[@]}" "$opt" ${defs[@]+"${defs[@]}"} -c "$f" -o "$o" &
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

# build.sh release (see the header). The objects in parallel (at most $JOBS clang at once, default 4:
# the dev machine has 16 GB), one link with the kernel's archives and libstdc++ static, stripped.
release() {
    if ((HAVE_KERNEL == 0)); then
        echo "FAIL build.sh release needs the boolean kernel: run ccore/deps.sh (or MANIFOLD_DIR=... ccore/deps.sh)" >&2
        exit 1
    fi
    local arch os=linux osname=Linux archf=() R=out/release
    arch=$(uname -m)
    if [[ $CC_OS == Darwin ]]; then
        os=macos osname=macOS
        if [[ -n ${CMAKE_OSX_ARCHITECTURES:-} ]]; then
            [[ $CMAKE_OSX_ARCHITECTURES =~ ^(arm64|x86_64)$ ]] || {
                echo "FAIL build.sh release: CMAKE_OSX_ARCHITECTURES=$CMAKE_OSX_ARCHITECTURES: one of arm64, x86_64" >&2
                exit 1
            }
            arch=$CMAKE_OSX_ARCHITECTURES
        fi
        archf=(-arch "$arch")
        export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
    fi
    [[ $CC_OS == Windows ]] && os=windows osname=Windows # WIN2: MSYS2 CLANG64, a .zip
    local name="bimble-ccore-$CC_VER-$arch-$os"
    local stage="$R/stage/$name" obj="$R/obj"
    rm -rf "$R/stage" "$obj" "$R/$name.tar.gz" "$R/$name.zip"
    mkdir -p "$stage" "$obj"
    local flags=(-std=c11 -O2 -g0 -ffp-contract=off -DRYU_ONLY_64_BIT_OPS -Wall -Wno-unused-function -pthread
        "$VDEF" ${KFLAGS[@]+"${KFLAGS[@]}"} ${PLATFORM_CFLAGS[@]+"${PLATFORM_CFLAGS[@]}"} ${archf[@]+"${archf[@]}"}
        "${INC[@]}" -Ihost)
    # SIZE1: one section per function and datum, and the link drops the unused ones (--gc-sections;
    # deps.sh builds manifold the same way): 3,674,408 -> 2,232,352 bytes on aarch64 Linux, the
    # read benchmark unchanged (results/native-size.md). Linux only: macOS's -dead_strip is not measured.
    [[ $CC_OS == Darwin ]] || flags+=(-ffunction-sections -fdata-sections)
    # FIXDOC: hardening on Linux: checked libc calls where the size is known, stack canaries in
    # functions with arrays, and (at the link) the relocations read-only and resolved at start
    # (RELRO, BIND_NOW: test/release_test.mjs checks it with readelf -d).
    local harden_link=()
    if [[ $CC_OS == Linux ]]; then
        flags+=(-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2 -fstack-protector-strong)
        harden_link=(-Wl,-z,relro,-z,now)
    fi
    compile_release() { # at most $JOBS at once: wait for the oldest (bash 3.2 has no `wait -n`)
        local f o q=() rc=0 pid
        for f in "${LIB[@]}" "${SESSION[@]}" "${HOST[@]}"; do
            if ((${#q[@]} >= ${JOBS:-4})); then
                wait "${q[0]}" || rc=1
                q=("${q[@]:1}")
            fi
            o="$obj/$(basename "${f%.c}").o"
            clang "${flags[@]}" -c "$f" -o "$o" &
            q+=($!)
        done
        for pid in "${q[@]}"; do wait "$pid" || rc=1; done
        return $rc
    }
    step "compile (clang -O2)" compile_release
    local kd bin="$stage/bimble-ccore"
    kd=$(dirname "${KLINK_CLANG[0]}")
    if [[ $CC_OS == Darwin ]]; then
        # libc++ is part of macOS (no static one to link); ld64 has no -Bstatic. Stripping breaks
        # the linker's ad hoc signature, which arm64 needs to run: sign again, ad hoc.
        step "link bimble-ccore (kernel static)" clang -pthread "${archf[@]}" "$obj"/*.o "$kd/libmanifoldc.a" \
            "$kd/libmanifold.a" "$kd/libClipper2.a" -lc++ -lm -o "$bin"
        step "strip, sign ad hoc" bash -c 'strip "$1" && codesign --force --sign - "$1"' _ "$bin"
        printf '  %-28s %s, macOS %s or newer\n' "arch" "$(lipo -archs "$bin")" "$MACOSX_DEPLOYMENT_TARGET"
    elif [[ $CC_OS == Windows ]]; then
        # WIN2: -static: libc++, libunwind and winpthreads inside the program (no DLL of MSYS2's
        # to ship); the UCRT and the system DLLs stay imported. Winsock: PLATFORM_LIBS.
        bin="$stage/bimble-ccore.exe"
        step "link bimble-ccore.exe (kernel static)" clang -pthread -static -s "$obj"/*.o "$kd/libmanifoldc.a" \
            "$kd/libmanifold.a" "$kd/libClipper2.a" -lc++ -lm ${PLATFORM_LIBS[@]+"${PLATFORM_LIBS[@]}"} \
            -Wl,--gc-sections -Wl,-Map="$R/bimble-ccore.map" -o "$bin"
        if command -v llvm-objdump > /dev/null; then
            printf '  %-28s %s\n' "DLLs" "$(llvm-objdump -p "$bin" | sed -n 's/^ *DLL Name: //p' | tr '\n' ' ')"
        fi
    else
        # SIZE1: the link map (out/release/bimble-ccore.map) gives tools/weight.mjs the bytes per object.
        step "link bimble-ccore (kernel static)" clang -pthread -s "$obj"/*.o "$kd/libmanifoldc.a" "$kd/libmanifold.a" \
            "$kd/libClipper2.a" -Wl,-Bstatic -lstdc++ -Wl,-Bdynamic -static-libgcc -lm -Wl,--gc-sections \
            ${harden_link[@]+"${harden_link[@]}"} -Wl,-Map="$R/bimble-ccore.map" -o "$bin"
    fi
    local web
    web=$(cd "$stage" && pwd)/web
    # The lab: web-ui's own build script (prebuild included), into the archive's web/.
    step "web-ui build (vite)" bash -c 'cd ../web-ui && npm run --silent build -- --outDir "$1" --emptyOutDir --logLevel warn' _ "$web"
    # SIZE1: no font in the archive (the PDF is Helvetica by default; Roboto is optional, read from
    # $BIMBLE_FONTS or <config dir>/bimble/fonts, host/fonts.c). An older copy-ccore.mjs leftover
    # in public/ccore/fonts/ would ship them: removed.
    rm -rf "$web/ccore/fonts" "$web/fonts"
    cp ../LICENSE "$stage/LICENSE"
    # FIXDOC: the third parties (THIRD-PARTY.txt) and the licence texts the repository holds.
    cp THIRD-PARTY.txt "$stage/THIRD-PARTY.txt"
    mkdir -p "$stage/licences"
    local lf
    for lf in LICENSE_CC0 LICENSE_A2 LICENSE_A2LLVM; do cp "vendor/blake3/$lf" "$stage/licences/BLAKE3-$lf"; done
    for lf in LICENSE-Apache2 LICENSE-Boost; do cp "vendor/ryu/$lf" "$stage/licences/Ryu-$lf"; done
    cp vendor/earcutr/LICENSE "$stage/licences/earcutr-LICENSE"
    for lf in OFL-Sora.txt OFL-SourceSans3.txt OFL-DMMono.txt; do cp "../web-ui/src/brand/$lf" "$stage/licences/$lf"; done
    local needs="Linux with glibc 2.35 or newer and a browser with WebGL2. Nothing is installed."
    local last=""
    if [[ $CC_OS == Darwin ]]; then
        needs="macOS $MACOSX_DEPLOYMENT_TARGET or newer and a browser with WebGL2. Nothing is installed."
        last="Signed: ad hoc only. Unpacked from a browser download: xattr -dr com.apple.quarantine ."
    fi
    if [[ $CC_OS == Windows ]]; then # WIN2: its own README (bimble-ccore.exe; CRLF, for Notepad), a .zip
        cat > "$stage/README.txt" << EOF
bimble-ccore $CC_VER ($arch, $osname): the C core of Bimble and its lab, a preview build.
Bimble is open-source structural BIM (MIT licence, see LICENSE): https://github.com/guilbep/bimble

Run:    bimble-ccore.exe open model.ifc     (also .bimble or .json; no model: an empty lab)
        It serves the lab on 127.0.0.1 only, prints its address and opens it in your browser.
        --no-browser: only print the address. --port N: this port, not a free one.
Stop:   Ctrl-C.
Files:  bimble-ccore.exe, web\ (the lab: keep it next to the program), THIRD-PARTY.txt, licences\.
Needs:  Windows 10 or newer (x64) and a browser with WebGL2. Nothing is installed. Not signed.
EOF
        sed -i 's/$/\r/' "$stage/README.txt"
        step "archive (zip)" bash -c 'cd "$1" && zip -q -X -r "../$2.zip" "$2"' _ "$R/stage" "$name"
        printf '  %-28s %s, %d bytes\n' "archive" "$R/$name.zip" "$(wc -c < "$R/$name.zip")"
        return
    fi
    cat > "$stage/README.txt" << EOF
bimble-ccore $CC_VER ($arch, $osname): the C core of Bimble and its lab, a preview build.
Bimble is open-source structural BIM (MIT licence, see LICENSE): https://github.com/guilbep/bimble

Run:    ./bimble-ccore open model.ifc       (also .bimble or .json; no model: an empty lab)
        It serves the lab on 127.0.0.1 only, prints its address and opens it in your browser.
        --no-browser: only print the address. --port N: this port, not a free one.
Stop:   Ctrl-C.
Files:  bimble-ccore, web/ (the lab: keep it next to the program), THIRD-PARTY.txt, licences/.
Needs:  $needs
EOF
    [[ -z $last ]] || echo "$last" >> "$stage/README.txt"
    # GNU tar: sorted, owner 0. BSD tar (macOS) has neither option; COPYFILE_DISABLE keeps
    # macOS's ._ metadata files out.
    if [[ $(tar --version 2> /dev/null) == *"GNU tar"* ]]; then
        step "archive" tar -C "$R/stage" --sort=name --owner=0 --group=0 --numeric-owner -czf "$R/$name.tar.gz" "$name"
    else
        step "archive" env COPYFILE_DISABLE=1 tar -C "$R/stage" -czf "$R/$name.tar.gz" "$name"
    fi
    printf '  %-28s %s, %d bytes\n' "archive" "$R/$name.tar.gz" "$(wc -c < "$R/$name.tar.gz")"
}

t_start=$(ms)
if [[ ${1:-} == wasm* ]]; then echo "  boolean kernel: none in wasm (openings and boolean results drawn uncut)"
elif ((HAVE_KERNEL)); then echo "  boolean kernel: manifold (out/deps/manifold)"
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
    wopt=${WASM_OPT:-wasm-opt}
    if ! command -v "$wopt" > /dev/null; then
        echo "FAIL $wopt (binaryen) not found: ccore-view.wasm needs it for its budget (ADR 0069;" \
            "ci/binaryen.sh installs the pinned one, or set WASM_OPT)" >&2
        exit 1
    fi
    echo "  $(clang --version | head -n 1); $("$wopt" --version)"
    # clang's own wasm-opt run off (when it knows the flag); the target_features section kept, so
    # wasm-opt reads the features the code uses (bulk memory...) instead of assuming none.
    nowo=()
    clang --target=wasm32 --no-wasm-opt -fsyntax-only -x c /dev/null 2> /dev/null && nowo=(--no-wasm-opt)
    wasm_build ccore-view.wasm "$VIEW_OPT" ${nowo[@]+"${nowo[@]}"} -Wl,--keep-section=target_features \
        -DCC_VIEW_ONLY "${VIEW_LIB[@]}"
    step "wasm-opt $VIEW_OPT" "$wopt" "$VIEW_OPT" out/wasm/ccore-view.wasm -o out/wasm/ccore-view.wasm
    wasm_sizes ccore-view.wasm
    bytes=$(wc -c < out/wasm/ccore-view.wasm)
    if ((bytes > VIEW_MAX)); then
        echo "FAIL ccore-view.wasm is $bytes bytes, over the budget of $VIEW_MAX (ADR 0069)" >&2
        exit 1
    fi ;;
  wasm-size)
    wasm_size_table "${2:-}" ;;
  release)
    echo "build.sh release -> out/release/"
    release ;;
  asan)
    echo "build.sh asan -> out/asan/bimble-ccore"
    host_asan ;;
  *) echo "usage: $0 tcc|host|native|asan|wasm|wasm-view|wasm-size [view]|release" >&2; exit 2 ;;
esac
printf '  %-28s %5d ms\n' "total" $(($(ms) - t_start))
