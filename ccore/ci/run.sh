#!/usr/bin/env bash
# The C core's checks in one script, for CI and for builders alike (ticket K1):
#   ccore/ci/run.sh [fast|full]          (default fast)
#   fast  the gate of every ticket: build.sh tcc, test/run.sh (tcc), test/run.sh (clang with
#         ASan and UBSan), build.sh wasm, test/wasm_run.mjs, test/host.sh (host_test, lab_boot).
#   full  fast, then the Chromium lab tests, build.sh wasm-view and every other test/*.mjs suite,
#         one after the other (a new test/<name>.mjs is picked up without an edit here; dev_test
#         last: it rebuilds out/tcc/bimble-ccore through dev.sh).
# Prints one line per suite (ok or FAIL, seconds, the "N passed, M failed" sums of its log) and a
# last line "ci: N suites, M failed". A suite fails on a non-zero exit, a line that starts with
# FAIL, or a "failed" count above 0. Every suite runs, even after a failure. Exits 1 on a failure.
# Logs: ccore/out/ci/<suite>.log. CCORE_PORT_BASE defaults to auto (test/ports.mjs), so runs in two
# worktrees never share a port. CCORE_CI_TIMEOUT: seconds per suite (default 900).
# The lab tests need web-ui/node_modules (npm ci in web-ui/) and /usr/bin/chromium; dev_test
# needs inotifywait (inotify-tools).
set -uo pipefail
cd "$(dirname "$0")/.."
mode=${1:-fast}
case "$mode" in fast | full) ;; *) echo "usage: $0 [fast|full]" >&2; exit 2 ;; esac
export CCORE_PORT_BASE=${CCORE_PORT_BASE:-auto}
LIMIT=${CCORE_CI_TIMEOUT:-900}
LOGS=out/ci
mkdir -p "$LOGS"

n=0 failed=0 t_all=$(date +%s%N)
# suite <name> <command...>: runs the command with its output in out/ci/<name>.log.
suite() {
    local name=$1 log="$LOGS/$1.log" t0 t1 rc bad=0 sums
    shift
    t0=$(date +%s%N)
    timeout -k 15 "$LIMIT" "$@" > "$log" 2>&1 < /dev/null
    rc=$?
    t1=$(date +%s%N)
    # The checks of the log: the sum of its "N passed, M failed" lines (one per C suite), or its
    # last such line when that one is the total of the others (wasm_run's sub-suites).
    sums=$(grep -oE '[0-9]+ passed, [0-9]+ failed' "$log" | awk '
        { p[NR] = $1; f[NR] = $3; sp += $1; sf += $3 }
        END { if (NR == 0) exit
              if (NR > 1 && p[NR] == sp - p[NR] && f[NR] == sf - f[NR]) { sp = p[NR]; sf = f[NR] }
              printf "%d passed, %d failed", sp, sf }')
    ((rc != 0)) && bad=1
    grep -qE '^FAIL' "$log" && bad=1
    # Any "M failed" above 0 makes the sum above 0. (No `grep | grep -q`: under pipefail the
    # writer's SIGPIPE, when -q stops early, can turn a match into false.)
    [[ $sums =~ ([0-9]+)\ failed$ ]] && ((BASH_REMATCH[1] > 0)) && bad=1
    ((rc == 124)) && sums="timed out after $LIMIT s"
    n=$((n + 1))
    if ((bad)); then
        failed=$((failed + 1))
        printf 'FAIL  %-20s %7.1f s  %s (exit %d, log ccore/%s)\n' "$name" "$(((t1 - t0) / 1000000))e-3" "${sums:-}" "$rc" "$log"
    else
        printf 'ok    %-20s %7.1f s  %s\n' "$name" "$(((t1 - t0) / 1000000))e-3" "${sums:-}"
    fi
}

# The gate.
suite build-tcc ./build.sh tcc
suite run-tcc test/run.sh
# The sanitizer run is the long one (8-10 min on the dev machine, more than 15 on a GitHub runner).
LIMIT=${CCORE_CI_ASAN_TIMEOUT:-2700} suite run-clang-asan env CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh
suite build-wasm ./build.sh wasm
suite wasm_run node test/wasm_run.mjs
suite host test/host.sh # host_test.mjs and lab_boot.mjs

if [[ $mode == full ]]; then
    # Run by the gate already (host.sh: host_test, lab_boot) or not a suite (ports.mjs).
    skip=" ports host_test lab_boot wasm_run dev_test "
    labs=(lab_gl lab_tool lab_place lab_section lab_plan lab_wasm lab_links)
    for t in "${labs[@]}"; do suite "$t" node "test/$t.mjs"; done
    # The viewer-only wasm and its 400 KB budget (ADR 0069), for test/wasm_view_run.mjs below.
    suite build-wasm-view ./build.sh wasm-view
    for f in test/*.mjs; do
        t=$(basename "${f%.mjs}")
        [[ $skip == *" $t "* || " ${labs[*]} " == *" $t "* ]] && continue
        suite "$t" node "$f"
    done
    suite dev_test node test/dev_test.mjs
fi

printf 'ci: %s, %d s in all, logs in ccore/%s\n' "$mode" $((($(date +%s%N) - t_all) / 1000000000)) "$LOGS"
printf 'ci: %d suites, %d failed\n' "$n" "$failed"
((failed == 0))
