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
# above, only when it changed) and sets KFLAGS (compile), KLINK_CLANG and KLINK_TCC (link) when
# out/deps/manifold is usable; else the arrays stay empty and the core builds without a kernel
# (openings and boolean results drawn uncut, with a warning). CC_NO_KERNEL=1 forces that.
set -euo pipefail

MANIFOLD_TAG=v3.5.3 # manifold-csg-sys 3.5.105 (build.rs MANIFOLD_VERSION)
MANIFOLD_URL=https://github.com/elalish/manifold.git
DEPS_OUT=out/deps/manifold

# deps_import <cmake build dir> <source tree>: copy the libraries and the header, make the .so.
deps_import() {
    local b=$1 s=$2 d=$DEPS_OUT
    local libs=("$b/bindings/c/libmanifoldc.a" "$b/src/libmanifold.a" "$b/_deps/clipper2-build/libClipper2.a")
    local stamp
    stamp=$(stat -c '%n %Y %s' "${libs[@]}" "$s/bindings/c/include/manifold/manifoldc.h" | sha1sum | cut -c1-16)
    [[ -f $d/stamp && $(cat "$d/stamp") == "$stamp" ]] && return 0
    rm -rf "$d"
    mkdir -p "$d/lib" "$d/include/manifold"
    cp "${libs[@]}" "$d/lib/"
    cp "$s"/bindings/c/include/manifold/*.h "$d/include/manifold/"
    # One shared library for tcc (the static ones were built with -fPIC).
    clang++ -shared -o "$d/lib/libmanifoldc.so" -Wl,--whole-archive "$d/lib/libmanifoldc.a" -Wl,--no-whole-archive \
        "$d/lib/libmanifold.a" "$d/lib/libClipper2.a" -lm
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
    KFLAGS=() KLINK_CLANG=() KLINK_TCC=()
    [[ ${CC_NO_KERNEL:-0} == 1 ]] && return 0
    if [[ -n ${MANIFOLD_DIR:-} ]] && ! deps_from_dir "$MANIFOLD_DIR"; then
        echo "  MANIFOLD_DIR=$MANIFOLD_DIR holds no manifold C binding build (ignored)" >&2
    fi
    local d=$DEPS_OUT
    [[ -f $d/lib/libmanifoldc.a && -f $d/lib/libmanifoldc.so && -f $d/include/manifold/manifoldc.h ]] || return 0
    local abs
    abs=$(cd "$d" && pwd)
    KFLAGS=(-DCC_HAVE_MANIFOLD -isystem "$abs/include")
    KLINK_CLANG=("$abs/lib/libmanifoldc.a" "$abs/lib/libmanifold.a" "$abs/lib/libClipper2.a" -lstdc++ -lm)
    KLINK_TCC=("-L$abs/lib" -lmanifoldc "-Wl,-rpath=$abs/lib")
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
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DMANIFOLD_PAR=OFF
    cmake --build "$bld" -j "${JOBS:-2}"
    deps_import "$bld" "$src"
    echo "manifold $MANIFOLD_TAG built into $DEPS_OUT"
}

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then deps_main "$@"; fi
