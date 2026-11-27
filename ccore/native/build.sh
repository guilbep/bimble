#!/usr/bin/env bash
# Builds `bimble`, the native Linux app (ticket NUI1, ADR 0084): the C core, the native renderer
# (render.c, scene_load.c, loader.c) and the shell (ui/*.c on Dear ImGui through cimgui), one binary.
#   native/build.sh app      clang -O2 (-Wall -Wextra -Werror on the app's own C), ImGui and the boolean
#                            kernel static: out/app/bimble (ccore/build.sh native-app runs this)
#   native/build.sh dev      the app's C files by tcc (one object each, cached), the core's from the clang
#                            -O2 cache, linked by tcc with libbimble_imgui.so: out/app-dev/bimble (the edit loop)
#   native/build.sh asan     clang -O1 ASan + UBSan, everything but ImGui instrumented: out/app-asan/bimble
#   native/build.sh watch [cmd...]   dev on every save of a native/*.c, *.h or ui/data file, then `cmd`
#                            (default: an offscreen shot of the fixture); prints "edit -> pixels N ms"
# Needs: clang, SDL3 >= 3.4 and FreeType (pkg-config), glslc (shaderc: the shaders become SPIR-V arrays at
# build time; the binary needs no glslc to run), git (native/deps.sh fetches ImGui, cimgui, nanosvg).
set -euo pipefail
cd "$(dirname "$0")/.."
MODE=${1:-app}
ms() { date +%s%3N; }
t0=$(ms)
source ./deps.sh
kernel_setup
command -v glslc > /dev/null || {
    echo "FAIL native/build.sh: glslc (shaderc) not found: the shaders are compiled to SPIR-V at build time" \
        "(Debian/Ubuntu: apt install glslc; Arch: pacman -S shaderc)" >&2
    exit 1
}
native/deps.sh
D=out/native/deps
G=out/app/gen
mkdir -p "$G/spv"
# 1. shaders -> SPIR-V as C arrays (glslc at build time only)
for s in native/shaders/*.vert native/shaders/*.frag native/shaders/*.comp; do
    b=$(basename "$s")
    [[ $b == face.frag ]] && continue
    [[ "$G/spv/$b.h" -nt "$s" && "$G/spv/$b.h" -nt native/shaders/common.glsl ]] || glslc -O -mfmt=c -o "$G/spv/$b.h" "$s"
done
[[ "$G/spv/face_id.frag.h" -nt native/shaders/face.frag ]] || glslc -O -DWITH_ID -mfmt=c -o "$G/spv/face_id.frag.h" native/shaders/face.frag
# NUI2: the section box by clip distances (vertex stage) and the fragment stage without discard, measured against discard
[[ "$G/spv/face_id_nodiscard.frag.h" -nt native/shaders/face.frag ]] || glslc -O -DWITH_ID -DNO_DISCARD -mfmt=c -o "$G/spv/face_id_nodiscard.frag.h" native/shaders/face.frag
for v in face_world face_inst face_proxy; do
    [[ "$G/spv/${v}_clip.vert.h" -nt native/shaders/$v.vert && "$G/spv/${v}_clip.vert.h" -nt native/shaders/common.glsl ]] ||
        glslc -O -DCLIP -mfmt=c -o "$G/spv/${v}_clip.vert.h" "native/shaders/$v.vert"
done
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
  *) echo "usage: native/build.sh app|dev|asan|watch [cmd...]" >&2; exit 2 ;;
esac
[[ $(uname -m) == aarch64 ]] && CF+=(-mno-outline-atomics)
CF+=("$VDEF" ${KFLAGS[@]+"${KFLAGS[@]}"})
# the core as ccore/build.sh compiles it (-I: its own math.h is meant); the app takes the core's folders as
# quote-only (-iquote) so that src/math.h never hides the system's <math.h>
CORE_INC=(-Isrc -Ivendor/ryu -Isession -Ihost)
APP_INC=(-iquote src -iquote vendor/ryu -iquote session -iquote host)
mkdir -p "$O/core"
hsig=$( { printf '%s\n' "${CF[*]}"; cat src/*.h session/*.h host/*.h vendor/ryu/ryu/*; } | sha1sum | cut -c1-12)
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
     native/ui/icons.c native/ui/machine.c native/ui/slog.c native/ui/nset.c native/ui/compare.c "$G/embed.c")
INC=(-I"$D"/cimgui -I"$D"/nanosvg/src -Inative -Inative/ui -I"$G/spv" $(pkg-config --cflags sdl3 freetype2))
SDL_LIBS=$(pkg-config --libs sdl3)
LINK_EXTRA=()
STDCXX=(-lstdc++)
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
        $SDL_LIBS $(pkg-config --libs freetype2) -lm ${LINK_EXTRA[@]+"${LINK_EXTRA[@]}"} -o "$out"
}
case $MODE in
  app)
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
    ls -t out/app/debug/*.debug 2> /dev/null | tail -n +21 | xargs -r rm -f ;; # the last 20 builds' kept
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
    "$bin" "$(stat -c %s "$bin")"
