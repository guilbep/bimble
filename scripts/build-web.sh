#!/usr/bin/env bash
# Build Bimble for the browser into web/dist (ADR 0020).
#   scripts/build-web.sh            release build
#   PROFILE=dev scripts/build-web.sh
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
if [ "$PROFILE" = release ]; then flag=--release; dir=release; else flag=; dir=debug; fi
cargo build -p bimble-web --target wasm32-unknown-unknown $flag
out=web/dist
rm -rf "$out" && mkdir -p "$out"
wasm-bindgen --target web --no-typescript --out-dir "$out" \
  "target/wasm32-unknown-unknown/$dir/bimble_web.wasm"
if command -v wasm-opt >/dev/null && [ "$PROFILE" = release ]; then
  wasm-opt -O2 --enable-bulk-memory --enable-nontrapping-float-to-int \
    "$out/bimble_web_bg.wasm" -o "$out/bimble_web_bg.wasm"
fi
cp web/index.html web/manifold-bridge.js "$out/"
# The geometry kernel for booleans (Apache-2.0), cached in target/.
deps=target/web-deps/manifold-3d-$MANIFOLD_VERSION
if [ ! -f "$deps/package/manifold.wasm" ]; then
  mkdir -p "$deps"
  (cd "$deps" && npm pack --silent "manifold-3d@$MANIFOLD_VERSION" > /dev/null && tar xzf manifold-3d-*.tgz)
fi
cp "$deps/package/manifold.js" "$deps/package/manifold.wasm" "$out/"
cp "$deps/package/LICENSE" "$out/manifold-LICENSE"
ls -l "$out"
