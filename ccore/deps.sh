#!/usr/bin/env bash
# The boolean kernel of the C core (ADR 0068, 0069): manifold's C binding (manifoldc), static,
# in out/deps/manifold/ (include/manifold/*.h, lib/libmanifoldc.a libmanifold.a libClipper2.a,
# and lib/libmanifoldc.so for tcc, which cannot link C++ static archives). Nothing is committed:
# vendor/manifold/SOURCE records the pinned version and the licence.
#   deps.sh            clone the pinned tag and build it with cmake (the options of the
#                      manifold-csg-sys crate Rust uses: no TBB, static, C binding on). Minutes.
#   MANIFOLD_DIR=<dir> deps.sh
#                      no build: take the libraries of an existing build. <dir> is the `out`
#                      directory of the manifold-csg-sys crate's cargo build (build/ and
#                      manifold-src/ inside), or a manifold source tree with its cmake build in
#                      <dir>/build.
# Sourced (build.sh, test/run.sh): defines kernel_setup, which imports $MANIFOLD_DIR when set (as
# above, only when it changed) and sets KFLAGS (compile), KLINK_CLANG and KLINK_TCC (link) and
# HAVE_KERNEL=1 when out/deps/manifold is usable; else the arrays stay empty (HAVE_KERNEL=0) and the core builds without a kernel
# (openings and boolean results drawn uncut, with a warning). CC_NO_KERNEL=1 forces that.
# macOS (ticket MAC1): no shared library (only tcc needs it, and macOS builds with clang), so
# KLINK_TCC stays empty there; clang links libc++, not libstdc++. CMAKE_OSX_ARCHITECTURES (cmake
# reads it from the environment) builds manifold for another arch, as release.yml does.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/portable.sh"

MANIFOLD_TAG=v3.5.3 # manifold-csg-sys 3.5.105 (build.rs MANIFOLD_VERSION)
MANIFOLD_URL=https://github.com/elalish/manifold.git
DEPS_OUT=out/deps/manifold

# deps_import <cmake build dir> <source tree>: copy the libraries and the header, make the .so
# (not on macOS: see the header).
deps_import() {
    local b=$1 s=$2 d=$DEPS_OUT
    local libs=("$b/bindings/c/libmanifoldc.a" "$b/src/libmanifold.a" "$b/_deps/clipper2-build/libClipper2.a")
    local stamp
    stamp=$(file_sig "${libs[@]}" "$s/bindings/c/include/manifold/manifoldc.h" | sha1_16)
    [[ -f $d/stamp && $(cat "$d/stamp") == "$stamp" ]] && return 0
    rm -rf "$d"
    mkdir -p "$d/lib" "$d/include/manifold"
    cp "${libs[@]}" "$d/lib/"
    cp "$s"/bindings/c/include/manifold/*.h "$d/include/manifold/"
    # One shared library for tcc (the static ones were built with -fPIC).
    if [[ $CC_OS != Darwin ]]; then
        clang++ -shared -o "$d/lib/libmanifoldc.so" -Wl,--whole-archive "$d/lib/libmanifoldc.a" -Wl,--no-whole-archive \
            "$d/lib/libmanifold.a" "$d/lib/libClipper2.a" -lm
    fi
    echo "$stamp" > "$d/stamp"
}

# deps_from_dir <dir>: 0 when <dir> holds a manifold build (either layout), imported.
deps_from_dir() {
    local m=$1
    if [[ -f $m/build/bindings/c/libmanifoldc.a && -d $m/manifold-src/bindings/c/include ]]; then
        deps_import "$m/build" "$m/manifold-src"
    elif [[ -f $m/build/bindings/c/libmanifoldc.a && -d $m/bindings/c/include ]]; then
        deps_import "$m/build" "$m"
    else
        return 1
    fi
}

kernel_setup() {
    KFLAGS=() KLINK_CLANG=() KLINK_TCC=() HAVE_KERNEL=0
    [[ ${CC_NO_KERNEL:-0} == 1 ]] && return 0
    if [[ -n ${MANIFOLD_DIR:-} ]] && ! deps_from_dir "$MANIFOLD_DIR"; then
        echo "  MANIFOLD_DIR=$MANIFOLD_DIR holds no manifold C binding build (ignored)" >&2
    fi
    local d=$DEPS_OUT
    [[ -f $d/lib/libmanifoldc.a && -f $d/include/manifold/manifoldc.h ]] || return 0
    [[ $CC_OS == Darwin || -f $d/lib/libmanifoldc.so ]] || return 0
    local abs cxx=-lstdc++
    abs=$(cd "$d" && pwd)
    [[ $CC_OS == Darwin ]] && cxx=-lc++
    KFLAGS=(-DCC_HAVE_MANIFOLD -isystem "$abs/include") HAVE_KERNEL=1
    KLINK_CLANG=("$abs/lib/libmanifoldc.a" "$abs/lib/libmanifold.a" "$abs/lib/libClipper2.a" "$cxx" -lm)
    [[ $CC_OS == Darwin ]] || KLINK_TCC=("-L$abs/lib" -lmanifoldc "-Wl,-rpath=$abs/lib")
}

deps_main() {
    cd "$(dirname "$0")"
    if [[ -n ${MANIFOLD_DIR:-} ]]; then
        deps_from_dir "$MANIFOLD_DIR" || { echo "MANIFOLD_DIR=$MANIFOLD_DIR: no manifold build there" >&2; exit 1; }
        echo "manifold imported from MANIFOLD_DIR into $DEPS_OUT"
        return
    fi
    local src=out/deps/src/manifold bld=out/deps/build/manifold
    [[ -d $src/.git ]] || git clone --depth 1 --branch "$MANIFOLD_TAG" "$MANIFOLD_URL" "$src"
    cmake -S "$src" -B "$bld" -DCMAKE_BUILD_TYPE=Release -DMANIFOLD_TEST=OFF -DMANIFOLD_PYBIND=OFF \
        -DMANIFOLD_JSBIND=OFF -DMANIFOLD_CBIND=ON -DMANIFOLD_CROSS_SECTION=ON -DMANIFOLD_USE_BUILTIN_CLIPPER2=ON \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DMANIFOLD_PAR=OFF \
        ${CMAKE_OSX_ARCHITECTURES:+"-DCMAKE_OSX_ARCHITECTURES=$CMAKE_OSX_ARCHITECTURES"}
    cmake --build "$bld" -j "${JOBS:-2}"
    deps_import "$bld" "$src"
    echo "manifold $MANIFOLD_TAG built into $DEPS_OUT"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then deps_main "$@"; fi
