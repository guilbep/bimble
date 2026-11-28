#!/usr/bin/env bash
# SDL_shadercross (SDL's own SPIR-V -> MSL translator, on SPIRV-Cross) at pinned commits, built into a prefix
# (ticket MACAPP, ADR 0088). Only to CHANGE a shader: the MSL it makes is committed beside the SPIR-V
# (native/shaders/gen/*.msl.h, `native/build.sh shaders` writes them when this tool is built), so neither the
# macOS build nor the release job needs it. A portable C/C++ tool: it builds on Linux (aarch64, x86_64) and macOS.
#   native/macos/shadercross.sh [prefix]     fetch, build, install (default prefix: out/native/shadercross)
#   native/macos/shadercross.sh --version    the version string written in each .msl.h's second line
# Needs: git, cmake, a C/C++ compiler, SDL 3 (>= 3.2, pkg-config / CMake: shadercross links it; the system's
# or ci/sdl3.sh's prefix through CMAKE_PREFIX_PATH). Licences: shadercross zlib, SPIRV-Cross Apache-2.0.
set -euo pipefail
cd "$(dirname "$0")/../.."
SC=1ff05bec573988a98ef9e0260b4da44f512b8367   # libsdl-org/SDL_shadercross main, 3 Sep 2026
SPVC=1a6169566c73d3da552748fc372fe2bbb856e46e # KhronosGroup/SPIRV-Cross: shadercross's submodule at that commit
VERSION="SDL_shadercross ${SC:0:10} SPIRV-Cross ${SPVC:0:10}"
if [[ ${1:-} == --version ]]; then echo "$VERSION"; exit 0; fi
prefix=${1:-out/native/shadercross}
mkdir -p "$prefix"
prefix=$(cd "$prefix" && pwd)
if [[ -x $prefix/bin/shadercross && "$(cat "$prefix/VERSION" 2> /dev/null)" == "$VERSION" ]]; then echo "shadercross: $VERSION in $prefix"; exit 0; fi
work=$(mktemp -d "${TMPDIR:-/tmp}/shadercross.XXXXXX")
trap 'rm -rf "$work"' EXIT
fetch() { # dir url commit
    git -C "$work" init -q "$1"
    git -C "$work/$1" remote add origin "$2"
    git -C "$work/$1" fetch -q --depth 1 origin "$3"
    git -C "$work/$1" checkout -q FETCH_HEAD
    [[ $(git -C "$work/$1" rev-parse HEAD) == "$3" ]] || { echo "FAIL shadercross.sh: $1 is not $3" >&2; exit 1; }
}
fetch spvc https://github.com/KhronosGroup/SPIRV-Cross.git "$SPVC"
fetch sc https://github.com/libsdl-org/SDL_shadercross.git "$SC"
j=${JOBS:-2}
cmake -S "$work/spvc" -B "$work/spvc-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DSPIRV_CROSS_SHARED=ON -DSPIRV_CROSS_STATIC=OFF -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF > "$work/spvc.log" 2>&1 ||
    { tail -40 "$work/spvc.log" >&2; exit 1; }
cmake --build "$work/spvc-build" -j "$j" >> "$work/spvc.log" 2>&1 || { tail -60 "$work/spvc.log" >&2; exit 1; }
cmake --install "$work/spvc-build" > /dev/null
cmake -S "$work/sc" -B "$work/sc-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_PREFIX_PATH="$prefix${CMAKE_PREFIX_PATH:+;$CMAKE_PREFIX_PATH}" -DCMAKE_INSTALL_RPATH="$prefix/lib" \
    -DSDLSHADERCROSS_DXC=OFF -DSDLSHADERCROSS_VENDORED=OFF -DSDLSHADERCROSS_SPIRVCROSS_SHARED=ON -DSDLSHADERCROSS_SHARED=ON \
    -DSDLSHADERCROSS_STATIC=OFF -DSDLSHADERCROSS_CLI=ON -DSDLSHADERCROSS_INSTALL=ON > "$work/sc.log" 2>&1 ||
    { tail -40 "$work/sc.log" >&2; exit 1; }
cmake --build "$work/sc-build" -j "$j" >> "$work/sc.log" 2>&1 || { tail -60 "$work/sc.log" >&2; exit 1; }
cmake --install "$work/sc-build" > /dev/null
echo "$VERSION" > "$prefix/VERSION"
echo "shadercross: $VERSION in $prefix"
