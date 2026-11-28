#!/usr/bin/env bash
# Builds `bimble`, the native app (ticket NUI1, ADR 0084; macOS: ticket MACAPP, ADR 0088): the C core, the native
# renderer (render.c, scene_load.c, loader.c) and the shell (ui/*.c on Dear ImGui through cimgui), one binary.
#   native/build.sh app      clang -O2 (-Wall -Wextra -Werror on the app's own C), ImGui and the boolean
#                            kernel static: out/app/bimble (ccore/build.sh native-app runs this)
#   native/build.sh dev      the app's C files by tcc (one object each, cached), the core's from the clang
#                            -O2 cache, linked by tcc with libbimble_imgui.so: out/app-dev/bimble (the edit loop)
#   native/build.sh asan     clang -O1 ASan + UBSan, everything but ImGui instrumented: out/app-asan/bimble
#   native/build.sh watch [cmd...]   dev on every save of a native/*.c, *.h or ui/data file, then `cmd`
#                            (default: an offscreen shot of the fixture); prints "edit -> pixels N ms"
#   native/build.sh shaders [--check|--msl]   the shaders' SPIR-V (native/shaders/gen/*.h, committed) from their
#                            sources with glslc, and its MSL (*.msl.h, committed) with SDL_shadercross
#                            (native/macos/shadercross.sh); --check: every committed header made from today's sources
#                            (their signatures: no tool needed), made again when the tools are here (bytes compared
#                            when their version is the one that made them); --msl: only the MSL, from the SPIR-V
# macOS (MACAPP): `app` and `asan` only, clang for CMAKE_OSX_ARCHITECTURES (arm64 | x86_64) and MACOSX_DEPLOYMENT_TARGET
# (default 11.0), SDL 3 and FreeType static from native/macos/deps.sh's prefix (PKG_CONFIG_PATH); a .dSYM per build.
# native/macos/bundle.sh then makes bimble.app and the .dmg.
# Needs: clang, SDL3 >= 3.4 and FreeType (pkg-config), git (native/deps.sh fetches ImGui, cimgui, nanosvg).
# glslc (shaderc) only to change a shader: the SPIR-V is committed (NUI3: Ubuntu 22.04, where the release
# archives are built for their old glibc, has no glslc package).
set -euo pipefail
cd "$(dirname "$0")/.."
# MACAPP: bash 3.2 and BSD tools on macOS (portable.sh: ms_now, sha1_16, sha1_each; no associative array)
source ./portable.sh
MODE=${1:-app}
ms() { ms_now; }
t0=$(ms)
# 1. shaders -> SPIR-V as C arrays, committed in native/shaders/gen: each header's first line is its signature (the
# source's and common.glsl's sha1, the defines); a stale one is made again when glslc is here, else the build fails.
# MACAPP (ADR 0088): beside each, <name>.msl.h, the Metal Shading Language SDL's Metal backend takes, translated from
# THAT SPIR-V by SDL_shadercross (native/macos/shadercross.sh, pinned): its first line the same signature, its second
# the tool's version and the SPIR-V header's sha1. Made when the tool is built (out/native/shadercross or
# $SHADERCROSS); the macOS build fails on a stale one, Linux does not compile them in.
SG=native/shaders/gen
SH=() # output | source | defines
for s in native/shaders/*.vert native/shaders/*.frag native/shaders/*.comp; do
    b=$(basename "$s")
    [[ $b == face.frag ]] || SH+=("$b.h|$s|")
done
# NUI2: the section box by clip distances (vertex stage) and the fragment stage without discard, measured against discard
SH+=("face_id.frag.h|native/shaders/face.frag|-DWITH_ID" "face_id_nodiscard.frag.h|native/shaders/face.frag|-DWITH_ID -DNO_DISCARD")
for v in face_world face_inst face_proxy; do SH+=("${v}_clip.vert.h|native/shaders/$v.vert|-DCLIP"); done
SUMS=$(sha1_each native/shaders/*.vert native/shaders/*.frag native/shaders/*.comp native/shaders/common.glsl)
sum_of() { echo "$SUMS" | awk -v f="$1" '$2 == f {print substr($1, 1, 16)}'; }
sig() { echo "/* native/build.sh shaders: $2 ${3:-(no defines)}; sha1 $(sum_of "$2") common $(sum_of native/shaders/common.glsl) */"; }
glslc_ver() { glslc --version 2> /dev/null | head -n 1; }
shader_make() { # out src defines -> $SG/out (or $4 a folder)
    local dir=${4:-$SG} tmp
    tmp=$(mktemp "$dir/.$1.XXXXXX")
    # shellcheck disable=SC2086
    glslc -O $3 -mfmt=c -o "$tmp" "$2" || { rm -f "$tmp"; return 1; }
    { sig "$1" "$2" "$3"; echo "/* $(glslc_ver) */"; cat "$tmp"; } > "$dir/$1"
    rm -f "$tmp"
}
# MACAPP: the MSL of a SPIR-V header
SC_DIR=${SHADERCROSS:-out/native/shadercross}
sc_ver() { native/macos/shadercross.sh --version; }
have_sc() { [[ -x $SC_DIR/bin/shadercross && "$(cat "$SC_DIR/VERSION" 2> /dev/null)" == "$(sc_ver)" ]]; }
msl_of() { echo "${1%.h}.msl.h"; }
msl_line2() { echo "/* $(sc_ver); from $1 sha1 $(sha1_16 < "$SG/$1") */"; } # the SPIR-V header it was made from
msl_make() { # spv-header src defines [folder] -> <folder>/<name>.msl.h
    local dir=${4:-$SG} o stage w
    o=$(msl_of "$1")
    case $1 in *.vert.h) stage=vertex ;; *.frag.h) stage=fragment ;; *) stage=compute ;; esac
    w=$(mktemp -d "${TMPDIR:-/tmp}/msl.XXXXXX")
    perl -ne 'next if $. <= 2; print pack("V*", map { hex } /0x([0-9a-fA-F]{8})/g)' "$SG/$1" > "$w/s.spv"
    if ! LD_LIBRARY_PATH="$SC_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" DYLD_LIBRARY_PATH="$SC_DIR/lib" \
        "$SC_DIR/bin/shadercross" "$w/s.spv" -s SPIRV -d MSL -t "$stage" -o "$w/s.msl" > "$w/log" 2>&1; then
        cat "$w/log" >&2; rm -rf "$w"; return 1
    fi
    { sig "$o" "$2" "$3"; msl_line2 "$1"
      sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$/\\n"/' "$w/s.msl"; } > "$dir/$o"
    rm -rf "$w"
}
if [[ $MODE == shaders ]]; then
    if [[ ${2:-} == --check ]]; then
        bad=0 cmp_note="" msl_note=""
        work=$(mktemp -d)
        trap 'rm -rf "$work"' EXIT
        for e in "${SH[@]}"; do
            IFS='|' read -r o src def <<< "$e"
            if [[ ! -f $SG/$o || "$(head -n 1 "$SG/$o")" != "$(sig "$o" "$src" "$def")" ]]; then
                echo "FAIL native/build.sh shaders --check: $SG/$o is not made from today's $src (native/build.sh shaders)" >&2
                bad=1
                continue
            fi
            m=$(msl_of "$o")
            if [[ ! -f $SG/$m || "$(head -n 1 "$SG/$m")" != "$(sig "$m" "$src" "$def")" ||
                "$(sed -n 2p "$SG/$m" | sed 's/.*; from //')" != "$(msl_line2 "$o" | sed 's/.*; from //')" ]]; then
                echo "FAIL native/build.sh shaders --check: $SG/$m is not made from today's $SG/$o (native/macos/shadercross.sh, then native/build.sh shaders)" >&2
                bad=1
            elif have_sc; then
                msl_make "$o" "$src" "$def" "$work" || { echo "FAIL native/build.sh shaders --check: $o does not translate to MSL" >&2; bad=1; }
                cmp -s "$SG/$m" "$work/$m" || { echo "FAIL native/build.sh shaders --check: $SG/$m differs from what $(sc_ver) makes" >&2; bad=1; }
            fi
            if command -v glslc > /dev/null; then
                shader_make "$o" "$src" "$def" "$work" || { echo "FAIL native/build.sh shaders --check: $src $def does not compile" >&2; bad=1; continue; }
                if [[ "$(sed -n 2p "$SG/$o")" == "/* $(glslc_ver) */" ]]; then
                    cmp -s "$SG/$o" "$work/$o" || { echo "FAIL native/build.sh shaders --check: $SG/$o differs from what $(glslc_ver) makes" >&2; bad=1; }
                else cmp_note=" (compiled with $(glslc_ver); the committed bytes come from $(sed -n 2p "$SG/$o" | sed 's#/\* ##; s# \*/##'): bytes not compared)"; fi
            fi
        done
        ((bad)) && exit 1
        command -v glslc > /dev/null || cmp_note=" (no glslc here: signatures only)"
        have_sc && msl_note=", MSL bytes compared ($(sc_ver))" || msl_note=", MSL signatures only (native/macos/shadercross.sh not built)"
        echo "native/build.sh shaders --check: ${#SH[@]} headers and their MSL made from today's sources$cmp_note$msl_note"
        exit 0
    fi
    if [[ ${2:-} == --msl ]]; then # MACAPP: only the MSL, from the committed SPIR-V (no glslc needed)
        have_sc || { echo "FAIL native/build.sh shaders --msl: SDL_shadercross not built (native/macos/shadercross.sh)" >&2; exit 1; }
        for e in "${SH[@]}"; do IFS='|' read -r o src def <<< "$e"; msl_make "$o" "$src" "$def"; done
        echo "native/build.sh shaders --msl: ${#SH[@]} MSL headers in $SG ($(sc_ver))"
        exit 0
    fi
    command -v glslc > /dev/null || { echo "FAIL native/build.sh shaders: glslc (shaderc) not found (Debian/Ubuntu: apt install glslc; Arch: pacman -S shaderc)" >&2; exit 1; }
    have_sc || { echo "FAIL native/build.sh shaders: SDL_shadercross not built (native/macos/shadercross.sh: the MSL beside the SPIR-V)" >&2; exit 1; }
    for e in "${SH[@]}"; do IFS='|' read -r o src def <<< "$e"; shader_make "$o" "$src" "$def" && msl_make "$o" "$src" "$def"; done
    echo "native/build.sh shaders: ${#SH[@]} headers and their MSL in $SG ($(glslc_ver); $(sc_ver))"
    exit 0
fi
for e in "${SH[@]}"; do
    IFS='|' read -r o src def <<< "$e"
    if ! [[ -f $SG/$o && "$(head -n 1 "$SG/$o")" == "$(sig "$o" "$src" "$def")" ]]; then
        command -v glslc > /dev/null || { echo "FAIL native/build.sh: $SG/$o is older than $src and glslc (shaderc) is not here to make it" >&2; exit 1; }
        shader_make "$o" "$src" "$def"
    fi
    m=$(msl_of "$o")
    [[ -f $SG/$m && "$(head -n 1 "$SG/$m")" == "$(sig "$m" "$src" "$def")" && "$(sed -n 2p "$SG/$m" | sed 's/.*; from //')" == "$(msl_line2 "$o" | sed 's/.*; from //')" ]] && continue
    if have_sc; then msl_make "$o" "$src" "$def"
    elif [[ $CC_OS == Darwin ]]; then echo "FAIL native/build.sh: $SG/$m is older than $SG/$o and SDL_shadercross is not built (native/macos/shadercross.sh)" >&2; exit 1
    else echo "native/build.sh: warning: $SG/$m is older than $SG/$o (the macOS build needs it: native/macos/shadercross.sh, then native/build.sh shaders)" >&2; fi
done
source ./deps.sh
kernel_setup
native/deps.sh
D=out/native/deps
G=out/app/gen
mkdir -p "$G"
# 2. the embedded files: the shell's data (exported from the web UI's TypeScript, committed) and the lab's fonts
BRAND=../web-ui/src/brand
EMB=(native/ui/data/ribbon.json native/ui/data/keys.json native/ui/data/machine.json native/ui/data/strings.fr.json
     native/ui/data/strings.en.json native/ui/data/themes.json)
for f in source-sans-3-latin-400 source-sans-3-latin-ext-400 source-sans-3-latin-600 source-sans-3-latin-ext-600 sora-latin-700 \
    sora-latin-ext-700 dm-mono-latin-400 dm-mono-latin-ext-400; do EMB+=("$BRAND/$f-normal.woff2"); done
newest=$(ls -t "${EMB[@]}" native/embed.sh | head -1)
[[ $G/embed.c -nt $newest ]] || native/embed.sh "$G/embed.c" "${EMB[@]}"
t1=$(ms)
# 3. the core (src, session, the native platform, the dialog's file lists) with clang, cached by content
CORE=()
for f in src/*.c session/*.c host/platform_native.c host/platform_win.c host/fs.c; do
    [[ $f == src/wasm_stub.c || $f == src/wasm_api.c ]] && continue
    CORE+=("$f")
done
VDEF="-DCC_RELEASE_VERSION=\"$(< VERSION)\""
case $MODE in
  app | dev | watch) CF=(-std=c11 -O2 -g0 -ffp-contract=off -DRYU_ONLY_64_BIT_OPS -Wall -Wno-unused-function) O=out/app ;;
  asan) CF=(-std=c11 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -ffp-contract=off -DRYU_ONLY_64_BIT_OPS -Wall -Wno-unused-function) O=out/app-asan ;;
  *) echo "usage: native/build.sh app|dev|asan|watch [cmd...] | shaders [--check]" >&2; exit 2 ;;
esac
[[ $(uname -m) == aarch64 ]] && CF+=(-mno-outline-atomics)
CF+=("$VDEF" ${KFLAGS[@]+"${KFLAGS[@]}"} ${PLATFORM_CFLAGS[@]+"${PLATFORM_CFLAGS[@]}"})
# MACAPP (ADR 0088): macOS builds one architecture (CMAKE_OSX_ARCHITECTURES, arm64 or x86_64, default this Mac's) for
# MACOSX_DEPLOYMENT_TARGET (default 11.0), as ccore/build.sh release; no tcc there (app and asan only)
MAC_ARCH=""
if [[ $CC_OS == Darwin ]]; then
    MAC_ARCH=${CMAKE_OSX_ARCHITECTURES:-$(uname -m)}
    [[ $MAC_ARCH =~ ^(arm64|x86_64)$ ]] || { echo "FAIL native/build.sh: CMAKE_OSX_ARCHITECTURES=$MAC_ARCH: one of arm64, x86_64" >&2; exit 1; }
    export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-11.0}
    CF+=(-arch "$MAC_ARCH" -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET")
    [[ $MODE == app || $MODE == asan ]] || { echo "FAIL native/build.sh $MODE: not on macOS (no tcc): app or asan" >&2; exit 2; }
fi
# the core as ccore/build.sh compiles it (-I: its own math.h is meant); the app takes the core's folders as
# quote-only (-iquote) so that src/math.h never hides the system's <math.h>
CORE_INC=(-Isrc -Ivendor/ryu -Isession -Ihost)
APP_INC=(-iquote src -iquote vendor/ryu -iquote session -iquote host)
mkdir -p "$O/core"
hsig=$( { printf '%s\n' "${CF[*]}"; cat src/*.h session/*.h host/*.h vendor/ryu/ryu/*; } | sha1_16 | cut -c1-12)
core_objs=()
pids=()
for f in "${CORE[@]}"; do
    o=$O/core/$(echo "${f%.c}" | tr / _).$hsig.o
    core_objs+=("$o")
    if [[ ! $o -nt $f ]]; then clang "${CF[@]}" "${CORE_INC[@]}" -c "$f" -o "$o" & pids+=($!); fi
    if ((${#pids[@]} >= ${JOBS:-4})); then wait "${pids[0]}" || exit 1; pids=("${pids[@]:1}"); fi
done
for p in ${pids[@]+"${pids[@]}"}; do wait "$p" || exit 1; done
find "$O/core" -name "*.o" ! -name "*.$hsig.o" -delete 2> /dev/null || true
t2=$(ms)
# 4. the app
APP=(native/render.c native/tiles_gpu.c native/scene_load.c native/loader.c native/ui/app.c native/ui/core.c native/ui/view3d.c native/ui/shell.c
     native/ui/panels.c native/ui/home.c native/ui/tools.c native/ui/page_quantities.c native/ui/drive.c native/ui/theme.c
     native/ui/icons.c native/ui/machine.c native/ui/slog.c native/ui/nset.c native/ui/compare.c native/ui/desktop.c "$G/embed.c")
INC=(-I"$D"/cimgui -I"$D"/nanosvg/src -Inative -Inative/ui -I"$SG" $(pkg-config --cflags sdl3 freetype2))
SDL_LIBS=$(pkg-config --libs sdl3)
FT_LIBS=$(pkg-config --libs freetype2)
LINK_EXTRA=()
STDCXX=(-lstdc++)
if [[ $CC_OS == Darwin ]]; then # MACAPP: the platform's own pieces; SDL 3 and FreeType static (native/macos/deps.sh)
    APP+=(native/macos/mac.c)
    INC+=(-Inative/macos)
    SDL_LIBS=$(pkg-config --static --libs sdl3)
    FT_LIBS=$(pkg-config --static --libs freetype2)
    STDCXX=(-lc++)
fi
build_app_clang() { # <out> <extra flags...>
    local out=$1
    shift
    local objs=() f o
    mkdir -p "$O/obj"
    for f in "${APP[@]}"; do
        o=$O/obj/$(basename "${f%.c}").o
        objs+=("$o")
        clang "${CF[@]}" "$@" -Wextra -Wno-unused-parameter -Wno-missing-field-initializers "${APP_INC[@]}" "${INC[@]}" -c "$f" -o "$o" &
    done
    wait
    for o in "${objs[@]}"; do [[ -f $o ]] || exit 1; done
    local kl=()
    for f in ${KLINK_CLANG[@]+"${KLINK_CLANG[@]}"}; do [[ $f == -lstdc++ ]] || kl+=("$f"); done
    clang "${CF[@]}" -pthread "${objs[@]}" "${core_objs[@]}" out/native/libbimble_imgui.a ${kl[@]+"${kl[@]}"} "${STDCXX[@]}" \
        $SDL_LIBS $FT_LIBS -lm ${LINK_EXTRA[@]+"${LINK_EXTRA[@]}"} -o "$out"
}
case $MODE in
  app)
    if [[ $CC_OS == Darwin ]]; then
        # MACAPP: libc++ is macOS's; ld64 drops what nothing reaches. The debug information: a .dSYM per build, named by
        # the executable's LC_UUID (the session log prints it; native/symbolize.sh runs atos on it); the binary
        # stripped, then signed ad hoc again (strip breaks the linker's signature; arm64 runs nothing unsigned)
        LINK_EXTRA=(-Wl,-dead_strip)
        build_app_clang out/app/bimble -Werror -g
        uuid=$(dwarfdump --uuid out/app/bimble | awk '{print $2; exit}')
        mkdir -p out/app/debug
        rm -rf "out/app/debug/$uuid.dSYM" out/app/bimble.dSYM
        dsymutil out/app/bimble -o "out/app/debug/$uuid.dSYM"
        cp -R "out/app/debug/$uuid.dSYM" out/app/bimble.dSYM
        strip out/app/bimble
        codesign --force --sign - out/app/bimble
        ls -td out/app/debug/*.dSYM 2> /dev/null | tail -n +21 | while read -r old; do rm -rf "$old"; done
        printf 'native/build.sh app: %s, macOS %s or newer, UUID %s\n' "$(lipo -archs out/app/bimble)" "$MACOSX_DEPLOYMENT_TARGET" "$uuid"
    else
    # libstdc++ and libgcc inside (as bimble-ccore), relocations read-only and bound at start (FIXDOC)
    STDCXX=(-Wl,-Bstatic -lstdc++ -Wl,-Bdynamic -static-libgcc)
    LINK_EXTRA=(-Wl,-z,relro,-z,now -Wl,--gc-sections)
    # -Werror on the app's own files (nanosvg's sign-compare is silenced at its include)
    LINK_EXTRA+=(-Wl,--build-id=sha1)
    build_app_clang out/app/bimble -Werror -g
    # NUI2: the debug information kept beside, named by build id (the session log prints it; native/symbolize.sh
    # turns a crash's raw addresses into functions); the binary shipped is stripped and points to it
    bid=$(readelf -n out/app/bimble | awk '/Build ID/ {print $3}')
    mkdir -p out/app/debug
    objcopy --only-keep-debug out/app/bimble "out/app/debug/$bid.debug"
    cp "out/app/debug/$bid.debug" out/app/bimble.debug
    strip out/app/bimble
    objcopy --add-gnu-debuglink="out/app/debug/$bid.debug" out/app/bimble
    ls -t out/app/debug/*.debug 2> /dev/null | tail -n +21 | xargs -r rm -f # the last 20 builds' kept
    fi ;;
  asan)
    build_app_clang out/app-asan/bimble ;;
  dev | watch)
    mkdir -p out/app-dev/obj
    TF=(-O1 -Wall -DRYU_ONLY_64_BIT_OPS "$VDEF" -I/usr/include -Isrc -Isession -Ihost -Ivendor/ryu "${INC[@]}")
    dev_build() {
        local objs=() f o
        for f in "${APP[@]}"; do
            o=out/app-dev/obj/$(basename "${f%.c}").o
            objs+=("$o")
            if [[ ! $o -nt $f || native/ui/ui.h -nt $o || native/render.h -nt $o || native/native.h -nt $o || native/loader.h -nt $o ]]; then
                tcc "${TF[@]}" -c "$f" -o "$o" || return 1
            fi
        done
        tcc "${objs[@]}" "${core_objs[@]}" -Lout/native -lbimble_imgui -Wl,-rpath,"$PWD/out/native" ${KLINK_TCC[@]+"${KLINK_TCC[@]}"} \
            $SDL_LIBS -lm -lpthread -o out/app-dev/bimble
    }
    if [[ $MODE == dev ]]; then dev_build
    else
        shift
        cmd=("$@")
        [[ ${#cmd[@]} -gt 0 ]] || cmd=(out/app-dev/bimble --offscreen --quit-after 2 --config-dir out/app-dev/conf --shot out/app-dev/shot.png golden/frame.json)
        command -v inotifywait > /dev/null || { echo "FAIL native/build.sh watch: inotifywait (inotify-tools) not found" >&2; exit 1; }
        dev_build && echo "watch: ready; save a file under ccore/native/ to rebuild"
        while inotifywait -qq -e close_write -e moved_to -r native --include '.*\.(c|h|json)$'; do
            sleep 0.05
            w0=$(ms)
            if dev_build; then
                w1=$(ms)
                "${cmd[@]}" > /dev/null 2>&1 || true
                w2=$(ms)
                echo "watch: built in $((w1 - w0)) ms, edit -> pixels $((w2 - w0)) ms (${cmd[*]})"
            else echo "watch: the build failed (the previous binary stays)"; fi
        done
    fi ;;
esac
t3=$(ms)
bin=$O/bimble
[[ $MODE == dev || $MODE == watch ]] && bin=out/app-dev/bimble
printf 'native/build.sh %s: shaders and data %d ms, core %d ms, app %d ms -> %s (%d bytes)\n' "$MODE" $((t1 - t0)) $((t2 - t1)) $((t3 - t2)) \
    "$bin" "$(wc -c < "$bin")"
