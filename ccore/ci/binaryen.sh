#!/usr/bin/env bash
# Installs binaryen (wasm-opt) at a pinned release into <prefix>: ccore/ci/binaryen.sh <prefix>
# ccore/ci/binaryen.sh --version prints the pinned version (the workflow's cache key).
# Why (ticket K2): `build.sh wasm-view` runs wasm-opt after the link, and the 400,000-byte budget of
# ccore-view.wasm (ADR 0069) holds only with it: 426,434 bytes without it, 353,477 with it (clang
# 21.1, results/wasm-size.md). The first CI run had no wasm-opt (426,448 bytes, FAIL); the dev
# machine had one that clang ran without saying so. Ubuntu 26.04 ships binaryen 120; the pin is the
# dev machine's (Arch's binaryen 130), so CI and builders optimise alike. The release tarball is
# checked against its sha256 below (from the release's .sha256 files).
set -euo pipefail
BINARYEN_VERSION=130
declare -A SHA256=(
    [x86_64-linux]=0a18362361ad05465118cd8eeb72edaeec89de6894bc283576ef4e07aa3babcc
    [aarch64-linux]=e6ae6e09ac40f4e14bc5be6f687c58e2995c84170013975fa641809dd3b480a0
)
if [[ ${1:-} == --version ]]; then echo "$BINARYEN_VERSION"; exit 0; fi
[[ $# -eq 1 ]] || { echo "usage: $0 <prefix> | --version" >&2; exit 2; }
prefix=$1
arch=$(uname -m)-linux
sum=${SHA256[$arch]:-}
[[ -n $sum ]] || { echo "binaryen.sh: no pinned tarball for $arch" >&2; exit 1; }
name=binaryen-version_$BINARYEN_VERSION-$arch.tar.gz
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL --retry 3 -o "$tmp/$name" \
    "https://github.com/WebAssembly/binaryen/releases/download/version_$BINARYEN_VERSION/$name"
echo "$sum  $tmp/$name" | sha256sum -c --quiet
tar -xzf "$tmp/$name" -C "$tmp"
mkdir -p "$prefix"
cp -a "$tmp/binaryen-version_$BINARYEN_VERSION"/. "$prefix"/
"$prefix/bin/wasm-opt" --version
