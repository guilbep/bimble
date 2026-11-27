#!/usr/bin/env bash
# SDL 3 at a pinned release, built from source into a prefix (ticket NUI1: the native app needs SDL >= 3.4,
# which Ubuntu's runners do not package). For CI (the jobs ccore-native and ccore-release cache the prefix
# by this file's hash) and for a machine without a recent SDL 3:
#   ccore/ci/sdl3.sh <prefix>          builds and installs (then PKG_CONFIG_PATH=<prefix>/lib/pkgconfig,
#                                      LD_LIBRARY_PATH=<prefix>/lib)
#   ccore/ci/sdl3.sh --commit          prints the pin
# The video drivers built: offscreen, X11 and Wayland when their headers are there; the GPU API's Vulkan
# backend (SDL loads libvulkan at run time). zlib licence (LICENSE.txt in the sources).
set -euo pipefail
TAG=release-3.4.18
COMMIT=829a65d769d935c4852f8159e964312c0957260a
if [[ ${1:-} == --commit ]]; then echo "$TAG $COMMIT"; exit 0; fi
prefix=${1:?usage: ci/sdl3.sh <prefix> | --commit}
work=$(mktemp -d "${TMPDIR:-/tmp}/sdl3.XXXXXX")
trap 'rm -rf "$work"' EXIT
git -C "$work" init -q src
git -C "$work/src" remote add origin https://github.com/libsdl-org/SDL.git
git -C "$work/src" fetch -q --depth 1 origin "$COMMIT"
git -C "$work/src" checkout -q FETCH_HEAD
[[ $(git -C "$work/src" rev-parse HEAD) == "$COMMIT" ]] || { echo "FAIL ci/sdl3.sh: not $COMMIT" >&2; exit 1; }
cmake -S "$work/src" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF \
    -DSDL_X11_XTEST=OFF -DSDL_AUDIO=OFF -DSDL_CAMERA=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_SENSOR=OFF > "$work/cmake.log" 2>&1 ||
    { tail -60 "$work/cmake.log" >&2; exit 1; }
cmake --build "$work/build" -j "$(nproc)" > "$work/build.log" 2>&1 || { tail -80 "$work/build.log" >&2; exit 1; }
cmake --install "$work/build" > /dev/null
echo "sdl3: $TAG ($COMMIT) in $prefix"
