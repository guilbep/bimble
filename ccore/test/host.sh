#!/usr/bin/env bash
# The native host's checks (ticket 9a): builds with tcc, runs host_test.mjs, then lab_boot.mjs
# (the unchanged Vue lab against the host in headless Chromium). lab_boot needs web-ui/node_modules
# (`npm ci` in web-ui/) and /usr/bin/chromium; set HOST_LAB=0 to skip it. Logs in out/t9a-*.log.
# Exits non-zero if a check fails.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p out
./build.sh tcc > out/t9a-build-tcc.log
grep -E 'host bimble-ccore|total' out/t9a-build-tcc.log
status=0
node test/host_test.mjs > out/t9a-host_test.log 2>&1 || status=1
grep -E '^(FAIL|host start|query state latency)|passed, ' out/t9a-host_test.log || true
if [[ ${HOST_LAB:-1} != 0 ]]; then
    node test/lab_boot.mjs > out/t9a-lab_boot.log 2>&1 || status=1
    grep -E '^(FAIL|page holds)|passed, ' out/t9a-lab_boot.log || true
fi
exit $status
