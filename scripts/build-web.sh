#!/usr/bin/env bash
# Build Bimble for the browser into web/dist (ADR 0020).
#   scripts/build-web.sh                    release build: viewer and editor
#   PROFILE=dev scripts/build-web.sh        dev build: viewer and editor
#   PROFILE=dev LAB=1 scripts/build-web.sh  dev build of the editor only, for the Vue lab (ADR 0065);
#                                           `npm run wasm:dev` in web-ui/ runs it and copies the files
# Needs: rustup target add wasm32-unknown-unknown; wasm-bindgen-cli matching the wasm-bindgen
# crate version in Cargo.lock (the script says which); npm (fetches the manifold kernel's
# WebAssembly build, pinned below); wasm-opt (binaryen) optional.
set -euo pipefail
cd "$(dirname "$0")/.."
PROFILE="${PROFILE:-release}"
MANIFOLD_VERSION=3.5.4
want=$(awk '/^name = "wasm-bindgen"$/ {getline; gsub(/version = |"/, ""); print; exit}' Cargo.lock)
have=$(wasm-bindgen --version 2>/dev/null | awk '{print $2}' || true)
if [ "$want" != "$have" ]; then
  echo "wasm-bindgen-cli $want needed (found: ${have:-none}): cargo install wasm-bindgen-cli --version $want" >&2
  exit 1
fi
# Release uses the size profile [profile.web] (Cargo.toml) and drops the wasm name section
# (function names, ~4 MB): panics in the browser console lose their symbol names.
# PROFILE=dev keeps them for debugging.
if [ "$PROFILE" = release ]; then flag="--profile web"; dir=web; strip=--remove-name-section
else flag=; dir=debug; strip=; fi
# Two builds (ADR 0050): web/dist is the viewer (no DXF/PDF reference plans), what Pages and the
# release zip publish; web/dist/editor is the full editor. The viewer is built last into web/dist,
# so the editor is built first into a temporary folder and moved under it.
deps=target/web-deps/manifold-3d-$MANIFOLD_VERSION
if [ ! -f "$deps/package/manifold.wasm" ]; then
  mkdir -p "$deps"
  (cd "$deps" && npm pack --silent "manifold-3d@$MANIFOLD_VERSION" > /dev/null && tar xzf manifold-3d-*.tgz)
fi
build() { # build <out dir> [cargo flags...]
  local out=$1; shift
  cargo build -p bimble-web --target wasm32-unknown-unknown $flag "$@"
  rm -rf "$out" && mkdir -p "$out"
  wasm-bindgen --target web --no-typescript $strip --out-dir "$out" \
    "${CARGO_TARGET_DIR:-target}/wasm32-unknown-unknown/$dir/bimble_web.wasm"
  if command -v wasm-opt >/dev/null && [ "$PROFILE" = release ]; then
    wasm-opt -O2 --enable-bulk-memory --enable-nontrapping-float-to-int \
      "$out/bimble_web_bg.wasm" -o "$out/bimble_web_bg.wasm"
  fi
  cp web/index.html web/viewport.html web/manifold-bridge.js "$out/"
  # The page's icon: one source each, under packaging/.
  cp packaging/logo-mark.svg "$out/favicon.svg"
  cp packaging/icon-256.png "$out/favicon.png"
  # The geometry kernel for booleans (Apache-2.0), cached in target/.
  cp "$deps/package/manifold.js" "$deps/package/manifold.wasm" "$out/"
  cp "$deps/package/LICENSE" "$out/manifold-LICENSE"
}
if [ "${LAB:-}" = 1 ]; then
  # The lab uses only the editor (web-ui/scripts/copy-wasm.mjs reads web/dist/editor): one build,
  # one wasm-bindgen run. web/dist itself (the viewer) is left as it is.
  build web/dist/editor --features editor
  ls -l web/dist/editor
  exit 0
fi
build target/web-editor --features editor
build web/dist
mv target/web-editor web/dist/editor
out=web/dist
ls -l "$out" "$out/editor"
