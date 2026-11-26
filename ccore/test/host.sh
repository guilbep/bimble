#!/usr/bin/env bash
# The native host's checks (tickets 9a, 9b): builds with tcc, runs host_test.mjs, then lab_boot.mjs
# (the unchanged Vue lab against the host in headless Chromium). lab_boot needs web-ui/node_modules
# (`npm ci` in web-ui/) and Chromium (CHROMIUM, test/ports.mjs); set HOST_LAB=0 to skip it. Logs in out/t9b-*.log.
# Exits non-zero if a check fails. CCORE_PORT_BASE (test/ports.mjs) passes through to both tests.
# HOST_BUILD=native: the host of build.sh native (clang) instead of tcc (macOS has no tcc: MAC1).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p out
sfx=${CCORE_PORT_BASE:+-$CCORE_PORT_BASE} # one set of logs per port base: parallel runs in one tree
[[ ${CCORE_PORT_BASE:-} == auto ]] && sfx=-auto$$ # auto: one set per run (TEST1)
mode=${HOST_BUILD:-tcc}
case "$mode" in tcc | native) ;; *) echo "HOST_BUILD=$mode: tcc or native" >&2; exit 2 ;; esac
export HOST_BIN=${HOST_BIN:-$PWD/out/$mode/bimble-ccore}
./build.sh "$mode" > out/t9b-build-$mode$sfx.log || { tail -n 40 out/t9b-build-$mode$sfx.log; exit 1; }
grep -E 'host bimble-ccore|total' out/t9b-build-$mode$sfx.log
status=0
node test/host_test.mjs > out/t9b-host_test$sfx.log 2>&1 || status=1
grep -E '^(FAIL|host start|query state latency|verb place latency|query element latency)|passed, ' out/t9b-host_test$sfx.log || true
if [[ ${HOST_LAB:-1} != 0 ]]; then
    node test/lab_boot.mjs > out/t9b-lab_boot$sfx.log 2>&1 || status=1
    grep -E '^(FAIL|page holds)|passed, ' out/t9b-lab_boot$sfx.log || true
fi
exit $status
