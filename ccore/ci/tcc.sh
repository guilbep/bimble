#!/usr/bin/env bash
# Builds tcc from source at a pinned commit into <prefix> (default /usr/local): ccore/ci/tcc.sh [prefix]
# ccore/ci/tcc.sh --commit prints the pinned commit (the workflow's cache key).
# Why not the distribution's tcc: Ubuntu 24.04 ships tinycc 62c30a4a (Aug 2020), which has no
# stdatomic.h and no __atomic builtins (both came in 2021, f2e7742a); src/jobs.h needs them. The pin
# is the tcc of the dev machine (Arch's 0.9.28rc of 17 Jan 2026), so CI and builders compile alike.
# The workflow caches <prefix> under a key holding TCC_COMMIT. configure + make + install: 3.7 s on
# the dev machine (aarch64, -j2, 25 Nov 2026), plus the clone. On aarch64 with recent kernel headers
# libtcc1's bounds-checking and backtrace objects fail (asm/sigcontext.h's __u128): there it is
# configured without them (the core uses neither -b nor -bt). x86_64: upstream's default options.
set -euo pipefail
TCC_COMMIT=b39cbc70c4ccc0540c2ef65010b8241caf005ad4 # tinycc mob, 17 Jan 2026 ("0.9.28rc")
TCC_URL=https://repo.or.cz/tinycc.git
if [[ ${1:-} == --commit ]]; then echo "$TCC_COMMIT"; exit 0; fi
prefix=${1:-/usr/local}
src=$(mktemp -d)
trap 'rm -rf "$src"' EXIT
git -C "$src" init -q
git -C "$src" fetch -q --depth 1 "$TCC_URL" "$TCC_COMMIT" 2> /dev/null ||
    git -C "$src" fetch -q "$TCC_URL" # a server that refuses to fetch a commit by its id
git -C "$src" checkout -q "$TCC_COMMIT"
cd "$src"
cfg=(--prefix="$prefix")
[[ $(uname -m) == aarch64 ]] && cfg+=(--config-backtrace=no --config-bcheck=no)
./configure "${cfg[@]}" > /dev/null
make -j "${JOBS:-2}" > /dev/null
make install > /dev/null
"$prefix/bin/tcc" -v
