#!/usr/bin/env bash
# The native host's checks (tickets 9a, 9b): builds with tcc, runs host_test.mjs, then lab_boot.mjs
# (the unchanged Vue lab against the host in headless Chromium). lab_boot needs web-ui/node_modules
# (`npm ci` in web-ui/) and /usr/bin/chromium; set HOST_LAB=0 to skip it. Logs in out/t9b-*.log.
# Exits non-zero if a check fails. CCORE_PORT_BASE (test/ports.mjs) passes through to both tests.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p out
sfx=${CCORE_PORT_BASE:+-$CCORE_PORT_BASE} # one set of logs per port base: parallel runs in one tree
./build.sh tcc > out/t9b-build-tcc$sfx.log
grep -E 'host bimble-ccore|total' out/t9b-build-tcc$sfx.log
status=0
node test/host_test.mjs > out/t9b-host_test$sfx.log 2>&1 || status=1
grep -E '^(FAIL|host start|query state latency|verb place latency|query element latency)|passed, ' out/t9b-host_test$sfx.log || true
if [[ ${HOST_LAB:-1} != 0 ]]; then
    node test/lab_boot.mjs > out/t9b-lab_boot$sfx.log 2>&1 || status=1
    grep -E '^(FAIL|page holds)|passed, ' out/t9b-lab_boot$sfx.log || true
fi
exit $status
