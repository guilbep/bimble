#!/usr/bin/env bash
# The C core's edit -> pixels loop in the Vue lab (ticket T12): RUNS=10 MODES="tcc native" bench/lab_loop.sh
# Runs bench/lab_loop.mjs (method in its header and in results/bench.md); output in out/t12-lab_loop.log,
# raw runs in out/t12-lab_loop.json. Fails if the run fails or leaves an edit in src/ or host/.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p out
command -v tcc > /dev/null && command -v clang > /dev/null && [[ -x /usr/bin/chromium ]] && [[ -d ../web-ui/node_modules ]] || {
    echo "lab_loop: needs tcc, clang, /usr/bin/chromium and web-ui/node_modules (npm ci)" >&2
    exit 2
}
dirty_before=$(git status --porcelain -- src host)
status=0
RUNS=${RUNS:-10} node bench/lab_loop.mjs 2>&1 | tee out/t12-lab_loop.log || status=1
if [[ $(git status --porcelain -- src host) != "$dirty_before" ]]; then
    echo "lab_loop: FAIL: the run left a change in src/ or host/" >&2
    git status --porcelain -- src host >&2
    status=1
fi
exit $status
