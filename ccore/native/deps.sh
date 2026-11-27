#!/usr/bin/env bash
# The native app's third parties (ticket NUI1, ADR 0084), the pattern of ccore/deps.sh for manifold:
# fetched at pinned commits into ccore/out/native/deps/ (not committed), every file compiled or
# included checked against its SHA-256 below (a mismatch stops the build), then built once into
#   out/native/libbimble_imgui.a   Dear ImGui (docking), its SDL3 and SDL_GPU backends, FreeType font
#                                  loading (WOFF2 through the system's FreeType with brotli), cimgui (the C
#                                  API) and our backend shim (ui/backend.cpp): the release links it static;
#   out/native/libbimble_imgui.so  the same for the edit loop (tcc links a shared C++ library, not an archive).
# nanosvg is two headers (ui/icons.c includes them).
#
#   ccore/native/deps.sh            fetch if missing, check, build if stale
#   ccore/native/deps.sh --check    check only (CI's cache key is this file's hash)
#
# Sources (licences read from each: imgui/LICENSE.txt MIT, cimgui/LICENSE MIT, nanosvg/LICENSE.txt zlib):
#   https://github.com/ocornut/imgui     6d32174508fb8c518e7a832cc1d571fadd5cba76 (1.93.0 docking, 15 Oct 2026)
#   https://github.com/cimgui/cimgui     2a6a93fccd10f275ee9206167dc9698f6f00fac1 (docking_inter, 18 Oct 2026; generated for 1.93.0)
#   https://github.com/memononen/nanosvg 239e102ec2c691f2902e20ace2ed36ee4a35cfe6 (10 Jul 2026)
# System libraries (pkg-config): SDL3 >= 3.4 (the GPU API, SDL_SavePNG), FreeType 2 (with brotli).
set -euo pipefail
cd "$(dirname "$0")/.."
D=out/native/deps
mkdir -p "$D"
CHECK=0
[[ ${1:-} == --check ]] && CHECK=1
SUMS='
9542082a1eaeffcab7b3d8069e707839ce1714d167005e9f273ac9f656b415ed imgui/imgui.cpp
22e9ee38114cb901be73109fee402ffc1626e5c3bc51d6aca9583c11ea718386 imgui/imgui_demo.cpp
490ead9d69c1a2f9c3fe509454a119159d8b287128ec8517a2db03a34e302536 imgui/imgui.h
61bc50172d907cc931159cdce48fbff1db616cda33251c9c3469e316056939e6 imgui/imgui_internal.h
fa91f606da783669ffd485f3f9e0cd0d92cddeac459efce4c2b9327cce7a3233 imgui/imconfig.h
fff3bb04ca11e32041144e83a37b89ec92f4d5c62d681eb7ef29ee6cd7663e84 imgui/imgui_draw.cpp
36635c1cf2ad1b7c88cdced456cceba3a1d02266d78a63d77e9cce92f4d48d61 imgui/imgui_tables.cpp
d2d388f711688f6a5ce1ae3b30168c1bf4f954ead40235b0b2b9667452850c93 imgui/imgui_widgets.cpp
889b396795202d1457560a797a7242e96f6f132d4b88ca2d69be58bf05e1771f imgui/imstb_rectpack.h
a985f5fa0ed97353d493b497961e9eef52082edcd045cf6954b69990ec9d0741 imgui/imstb_textedit.h
c51a0f7e7ea760f2366bd3752635ec58e21fccfec4a832501639990ba6ce0528 imgui/imstb_truetype.h
306060dffa276b574f0541194e8e079c1685c29c1095602a3972de0bb23171e7 imgui/backends/imgui_impl_sdl3.cpp
fa184f17d59d31e986df2e827ef964d7b3c28bcba7789643a4aaaed344cb868e imgui/backends/imgui_impl_sdl3.h
e277f6088569b0b9e78bc2faf1f745d834a4358a710461918fcecfc9a73011f2 imgui/backends/imgui_impl_sdlgpu3.cpp
fa70b2dbf9d2e5d640282a84cb8d2293ecec23c763c0f7a0c9b09ca7490f4ff4 imgui/backends/imgui_impl_sdlgpu3.h
a9b14825fc3f8b4809d914553130f6a2ea9b45c86db21eb72896edb72611a897 imgui/backends/imgui_impl_sdlgpu3_shaders.h
ed6caaea65fdb141f4f919c98005ee09f1908e9fef05fb916643b58f4d78aedb imgui/misc/freetype/imgui_freetype.cpp
f290a0d40662b70987f510652d391aca12e436235eb51152e55aeb9f7d3f45d9 imgui/misc/freetype/imgui_freetype.h
173506a2d6f7fb67990d257fb2507f188690eca39060c39469ae7bef43aae2a3 imgui/LICENSE.txt
ef3386866edd8afd0ba850092de09091ffe38851e123f8e736ea996ac56ea7cc cimgui/cimgui.cpp
99e9d2b0e5ab5f4d51317766cf2a916aca7e8afda856b470b87ce4da593b47ea cimgui/cimgui.h
cf6a4390b1ae5b4dfbc79a4ab33c58852db4f17b3f5c6d9300f278050223884d cimgui/LICENSE
e34fd5d084be106cea972d19ce5d27fd96d17ba89f8d06bdceee058420c8b2b0 nanosvg/src/nanosvg.h
79a9c5f4db19debf9f3a648a1589e96d92854f245a5cb4f3d823f263785234d8 nanosvg/src/nanosvgrast.h
e82bc3dd03400aecabe12201219ba14750dbc4b36faab58663a7a6068548d372 nanosvg/LICENSE.txt
'
fetch() { # name url commit
    local dir="$D/$1"
    if [[ -d "$dir/.git" ]] && [[ $(git -C "$dir" rev-parse HEAD) == "$3" ]]; then return; fi
    ((CHECK)) && return
    rm -rf "$dir"
    mkdir -p "$dir"
    git -C "$dir" init -q && git -C "$dir" remote add origin "$2"
    git -C "$dir" fetch -q --depth 1 origin "$3" && git -C "$dir" checkout -q FETCH_HEAD
    echo "native deps: $1 $(git -C "$dir" rev-parse HEAD)"
}
fetch imgui https://github.com/ocornut/imgui.git 6d32174508fb8c518e7a832cc1d571fadd5cba76
fetch cimgui https://github.com/cimgui/cimgui.git 2a6a93fccd10f275ee9206167dc9698f6f00fac1
fetch nanosvg https://github.com/memononen/nanosvg.git 239e102ec2c691f2902e20ace2ed36ee4a35cfe6
# every file checked
bad=0
while read -r sum f; do
    [[ -n $sum ]] || continue
    got=$(sha256sum "$D/$f" 2> /dev/null | cut -c1-64 || true)
    if [[ $got != "$sum" ]]; then echo "FAIL native deps: $f: sha256 ${got:-missing}, expected $sum" >&2; bad=1; fi
done <<< "$SUMS"
((bad == 0)) || exit 1
((CHECK)) && { echo "native deps: checked"; exit 0; }
# cimgui includes "./imgui/imgui.h": its submodule folder points at ours
rmdir "$D/cimgui/imgui" 2> /dev/null || true
[[ -e "$D/cimgui/imgui" ]] || ln -s ../imgui "$D/cimgui/imgui"
command -v pkg-config > /dev/null && pkg-config --exists 'sdl3 >= 3.4.0' freetype2 || {
    echo "FAIL native deps: SDL3 >= 3.4.0 and FreeType 2 are needed (pkg-config sdl3 freetype2: the system's, or SDL built from source: ci/sdl3.sh)" >&2
    exit 1
}
LIB=out/native/libbimble_imgui
SRC=("$D"/imgui/imgui.cpp "$D"/imgui/imgui_demo.cpp "$D"/imgui/imgui_draw.cpp "$D"/imgui/imgui_tables.cpp "$D"/imgui/imgui_widgets.cpp
     "$D"/imgui/backends/imgui_impl_sdl3.cpp "$D"/imgui/backends/imgui_impl_sdlgpu3.cpp
     "$D"/imgui/misc/freetype/imgui_freetype.cpp "$D"/cimgui/cimgui.cpp native/ui/backend.cpp native/ui/backend.h)
newest=$(ls -t "${SRC[@]}" native/deps.sh | head -1)
if [[ -f $LIB.a && -f $LIB.so && $LIB.a -nt $newest && $LIB.so -nt $newest ]]; then exit 0; fi
mkdir -p out/native/imgui-obj
CXXF=(-std=c++17 -O2 -fPIC -g0 -DIMGUI_ENABLE_FREETYPE -DIMGUI_ENABLE_STB_TRUETYPE -DIMGUI_DISABLE_DEMO_WINDOWS
      -I"$D"/imgui -I"$D"/imgui/backends -I"$D"/cimgui -Inative/ui $(pkg-config --cflags sdl3 freetype2))
objs=()
pids=()
for f in "${SRC[@]}"; do
    [[ $f == *.h ]] && continue
    o=out/native/imgui-obj/$(basename "${f%.cpp}").o
    objs+=("$o")
    if [[ ! $o -nt $f || native/deps.sh -nt $o ]]; then clang++ "${CXXF[@]}" -c "$f" -o "$o" & pids+=($!); fi
    if ((${#pids[@]} >= ${JOBS:-3})); then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
done
for p in ${pids[@]+"${pids[@]}"}; do wait "$p"; done
rm -f "$LIB.a"
${AR:-$(command -v llvm-ar || echo ar)} rcs "$LIB.a" "${objs[@]}"
clang++ -shared -o "$LIB.so" "${objs[@]}" $(pkg-config --libs sdl3 freetype2)
echo "native deps: $LIB.a $(stat -c %s "$LIB.a") bytes, $LIB.so $(stat -c %s "$LIB.so") bytes"
