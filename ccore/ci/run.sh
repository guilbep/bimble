#!/usr/bin/env bash
# The C core's checks in one script, for CI and for builders alike (tickets K1, K3):
#   ccore/ci/run.sh [fast|full|unit|san|lab]          (default fast)
#   fast  the gate of every ticket: build.sh tcc, test/run.sh (tcc), test/run.sh (clang with
#         ASan and UBSan), build.sh wasm, test/wasm_run.mjs, test/host.sh (host_test, lab_boot).
#   unit  all but the sanitizers and the Chromium lab suites (the job ccore-linux): the gate's
#         builds, test/run.sh with tcc, wasm_run, host.sh, build.sh wasm-view and
#         release, every test/*.mjs suite but lab_* (a new test/<name>.mjs is picked up without an
#         edit here; dev_test last: it rebuilds out/tcc/bimble-ccore through dev.sh).
#   san   test/run.sh with clang, ASan and UBSan (the job ccore-san).
#   lab   build.sh tcc and wasm, then every test/lab_*.mjs but lab_boot (host.sh runs it): the
#         Chromium suites (the job ccore-lab).
#   full  fast, the lab suites, then the rest of unit: every suite once.
# Prints one line per suite (ok or FAIL, seconds, the "N passed, M failed" sums of its log) and a
# last line "ci: N suites, M failed". A suite fails on a non-zero exit, a line that starts with
# FAIL, or a "failed" count above 0. Every suite runs, even after a failure. Exits 1 on a failure.
# Logs: ccore/out/ci/<suite>.log. CCORE_PORT_BASE defaults to auto (test/ports.mjs), so runs in two
# worktrees never share a port. CCORE_CI_TIMEOUT: seconds per suite (default 900).
# The lab tests need web-ui/node_modules (npm ci in web-ui/) and /usr/bin/chromium; dev_test
# needs inotifywait (inotify-tools); build-wasm-view needs wasm-opt (binaryen, ci/binaryen.sh).
set -uo pipefail
cd "$(dirname "$0")/.."
mode=${1:-fast}
case "$mode" in fast | full | unit | san | lab) ;; *) echo "usage: $0 [fast|full|unit|san|lab]" >&2; exit 2 ;; esac
export CCORE_PORT_BASE=${CCORE_PORT_BASE:-auto}
LIMIT=${CCORE_CI_TIMEOUT:-900}
LOGS=out/ci
mkdir -p "$LOGS"

n=0 failed=0 t_all=$(date +%s%N)
done_=" "
# suite <name> <command...>: runs the command with its output in out/ci/<name>.log, once a run.
suite() {
    local name=$1 log="$LOGS/$1.log" t0 t1 rc bad=0 sums
    shift
    [[ $done_ == *" $name "* ]] && return 0
    done_+="$name "
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

# test/run.sh compiles the library once and runs its suites in parallel (CCORE_TEST_JOBS).
unit_c() { suite run-tcc test/run.sh; }
san() { suite run-clang-asan env CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh; }
gate_rest() {
    suite build-wasm ./build.sh wasm
    suite wasm_run node test/wasm_run.mjs
    suite host test/host.sh # host_test.mjs and lab_boot.mjs
}
labs() {
    suite build-tcc ./build.sh tcc
    suite build-wasm ./build.sh wasm # lab_wasm, lab_pages
    for f in test/lab_*.mjs; do
        t=$(basename "${f%.mjs}")
        [[ $t == lab_boot ]] && continue # host.sh
        suite "$t" node "$f"
    done
}
unit_rest() {
    # The viewer-only wasm and its 400 KB budget (ADR 0069), for test/wasm_view_run.mjs below.
    suite build-wasm-view ./build.sh wasm-view
    # The release archive (ticket R1), for test/release_test.mjs below.
    suite build-release ./build.sh release
    # Run already (host.sh: host_test, lab_boot; wasm_run), the lab suites, or not a suite (ports).
    local skip=" ports host_test lab_boot wasm_run dev_test "
    for f in test/*.mjs; do
        t=$(basename "${f%.mjs}")
        [[ $skip == *" $t "* || $t == lab_* ]] && continue
        suite "$t" node "$f"
    done
    suite dev_test node test/dev_test.mjs
}

case "$mode" in
    fast) suite build-tcc ./build.sh tcc; unit_c; san; gate_rest ;;
    unit) suite build-tcc ./build.sh tcc; unit_c; gate_rest; unit_rest ;;
    san) san ;;
    lab) labs ;;
    full) suite build-tcc ./build.sh tcc; unit_c; san; gate_rest; labs; unit_rest ;;
esac

printf 'ci: %s, %d s in all, logs in ccore/%s\n' "$mode" $((($(date +%s%N) - t_all) / 1000000000)) "$LOGS"
printf 'ci: %d suites, %d failed\n' "$n" "$failed"
((failed == 0))
