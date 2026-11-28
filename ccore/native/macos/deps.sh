#!/usr/bin/env bash
# The macOS app's system libraries, built from source at pinned commits into one prefix, all STATIC (ticket MACAPP,
# ADR 0088): the bundle carries one executable and no dylib, so no @rpath and nothing for Gatekeeper to check twice.
#   SDL 3      ci/sdl3.sh (the same pin as Linux; static on macOS: Cocoa video, the Metal GPU backend, the offscreen driver)
#   brotli     WOFF2's compression (the lab's fonts are WOFF2: FreeType reads them through brotli, as on Linux)
#   FreeType   ImGui's font rasteriser (imgui_freetype), the same as Linux: the same text. No zlib (none is needed for
#              WOFF2 or TTF), no PNG, bzip2 or HarfBuzz.
#   native/macos/deps.sh <prefix>      then PKG_CONFIG_PATH=<prefix>/lib/pkgconfig (native/build.sh reads --static)
# CMAKE_OSX_ARCHITECTURES (arm64 | x86_64) and MACOSX_DEPLOYMENT_TARGET (default 11.0) from the environment, as
# ccore/build.sh release. Licences: SDL zlib, brotli MIT, FreeType FTL (licences/ in the bundle: native/macos/bundle.sh).
set -euo pipefail
cd "$(dirname "$0")/../.."
[[ $(uname -s) == Darwin ]] || { echo "FAIL native/macos/deps.sh: macOS only (Linux takes the system's FreeType and ci/sdl3.sh)" >&2; exit 1; }
prefix=${1:?usage: native/macos/deps.sh <prefix>}
mkdir -p "$prefix"
prefix=$(cd "$prefix" && pwd)
BROTLI=db24a98aed38a16de78cc05ca15cdd8dc2a0d92d   # google/brotli v1.2.1
FREETYPE=0a0221a1347e2f1e07c395263540026e9a0aa7c7 # freetype/freetype VER-2-14-3
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
arch=${CMAKE_OSX_ARCHITECTURES:-$(uname -m)}
export CMAKE_OSX_ARCHITECTURES=$arch
j=$(sysctl -n hw.ncpu 2> /dev/null || echo 4)
work=$(mktemp -d "${TMPDIR:-/tmp}/macdeps.XXXXXX")
trap 'rm -rf "$work"' EXIT
fetch() { # dir url commit
    git -C "$work" init -q "$1"
    git -C "$work/$1" remote add origin "$2"
    git -C "$work/$1" fetch -q --depth 1 origin "$3"
    git -C "$work/$1" checkout -q FETCH_HEAD
    [[ $(git -C "$work/$1" rev-parse HEAD) == "$3" ]] || { echo "FAIL native/macos/deps.sh: $1 is not $3" >&2; exit 1; }
}
cm() { # name src build-dir cmake-args...
    local n=$1 s=$2 b=$3
    shift 3
    cmake -S "$s" -B "$b" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
        -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" -DBUILD_SHARED_LIBS=OFF "$@" > "$work/$n.log" 2>&1 ||
        { tail -60 "$work/$n.log" >&2; exit 1; }
    cmake --build "$b" -j "$j" >> "$work/$n.log" 2>&1 || { tail -80 "$work/$n.log" >&2; exit 1; }
    cmake --install "$b" >> "$work/$n.log" 2>&1 || { tail -40 "$work/$n.log" >&2; exit 1; }
}
ci/sdl3.sh "$prefix"
fetch brotli https://github.com/google/brotli.git "$BROTLI"
cm brotli "$work/brotli" "$work/brotli-build" -DBROTLI_DISABLE_TESTS=ON -DBROTLI_BUILD_TOOLS=OFF
fetch freetype https://github.com/freetype/freetype.git "$FREETYPE"
cm freetype "$work/freetype" "$work/freetype-build" -DFT_REQUIRE_BROTLI=ON -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON \
    -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON
export PKG_CONFIG_PATH=$prefix/lib/pkgconfig
for l in sdl3 freetype2; do
    printf 'native/macos/deps.sh: %-9s %s  %s\n' "$l" "$(pkg-config --modversion "$l")" "$(pkg-config --static --libs "$l" | tr '\n' ' ')"
done
for a in "$prefix"/lib/*.a; do printf '  %s %s\n' "$(basename "$a")" "$(lipo -archs "$a" 2> /dev/null || echo ?)"; done
echo "native/macos/deps.sh: $arch, macOS $MACOSX_DEPLOYMENT_TARGET or newer, in $prefix (brotli ${BROTLI:0:10}, FreeType ${FREETYPE:0:10})"
