#!/usr/bin/env bash
# The host program under ASan and UBSan (ticket FIXDOC): test/san_host.sh <suite> [<suite>...]
# runs each node suite (test/<suite>.mjs, one at a time) against out/asan/bimble-ccore (build.sh
# asan) instead of the tcc host. The sanitizers write their reports to files (log_path), not to
# the host's stderr, which the suites read; any report fails the suite and is printed whole.
# LeakSanitizer runs when a host exits normally (SIGTERM, the end of an MCP session).
# Prints the suite's own last lines, then "san_host <suite>: N passed, M failed" (M = the suite's
# failures plus 1 for a sanitizer report). ci/run.sh san runs it.
set -uo pipefail
cd "$(dirname "$0")/.."
bin=$PWD/out/asan/bimble-ccore
[[ -x $bin ]] || { echo "FAIL $bin missing: run ./build.sh asan first"; exit 1; }
status=0
for name in "$@"; do
    logs=$(mktemp -d "${TMPDIR:-/tmp}/ccore-san-$name.XXXXXX")
    out=$(HOST_BIN=$bin \
        ASAN_OPTIONS="log_path=$logs/asan:detect_leaks=1:halt_on_error=1:abort_on_error=0" \
        UBSAN_OPTIONS="log_path=$logs/ubsan:print_stacktrace=1:halt_on_error=0" \
        node "test/$name.mjs" 2>&1)
    rc=$?
    sums=$(grep -oE '[0-9]+ passed, [0-9]+ failed' <<< "$out" | tail -n 1)
    grep -E '^FAIL' <<< "$out" || true
    p=0 f=0
    [[ $sums =~ ^([0-9]+)\ passed,\ ([0-9]+)\ failed$ ]] && p=${BASH_REMATCH[1]} f=${BASH_REMATCH[2]}
    ((rc != 0 && f == 0)) && f=1
    reports=$(find "$logs" -type f -size +0c | sort)
    if [[ -n $reports ]]; then
        f=$((f + 1))
        echo "FAIL $name: sanitizer report(s) from the host:"
        while read -r r; do echo "---- $(basename "$r")"; cat "$r"; done <<< "$reports"
    fi
    echo "san_host $name: $p passed, $f failed"
    ((f)) && status=1
    rm -rf "$logs"
done
exit $status
