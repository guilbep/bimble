#!/usr/bin/env bash
# The C core's checks in one script, for CI and for builders alike (tickets K1, K3):
#   ccore/ci/run.sh [fast|full|unit|san|lab|clang|native]    (default fast)
#   fast  the gate of every ticket: build.sh tcc, test/run.sh (tcc), test/run.sh (clang with
#         ASan and UBSan), build.sh wasm, test/wasm_run.mjs, test/host.sh (host_test, lab_boot).
#   unit  all but the sanitizers and the Chromium lab suites (the job ccore-linux): the gate's
#         builds, test/run.sh with tcc, wasm_run, host.sh, build.sh wasm-view and
#         release, the weight table (tools/weight.mjs, ticket SIZE1), every test/*.mjs suite but
#         lab_* (a new test/<name>.mjs is picked up without an
#         edit here; dev_test last: it rebuilds out/tcc/bimble-ccore through dev.sh); before them
#         the README's Status block against the committed reports (tools/status.mjs --check) and
#         the parity tool's check of the C host (tools/parity.mjs --check; ticket FIXDOC).
#   san   test/run.sh with clang, ASan and UBSan (the job ccore-san); then the host program itself
#         under ASan and UBSan (build.sh asan) through its node suites without a browser:
#         test/san_host.sh host_test, fs_test, mcp_test, mesh_http_test, files_test (ticket FIXDOC),
#         session_test, session_log_test, versions_test, links_test, verbs_session_test (ticket
#         FIXSES), one at a time, each failing on any sanitizer report.
#   lab   build.sh tcc and wasm, then every test/lab_*.mjs but lab_boot (host.sh runs it): the
#         Chromium suites (the job ccore-lab); then the performance report (tools/perf.mjs,
#         ticket PERF1: out/ci/perf.md and its change against results/perf.md).
#   full  fast, the lab suites, then the rest of unit: every suite once (test/native_app_test.mjs among
#         them: it says "skipped" without SDL3 >= 3.4 or a Vulkan device).
#   native  the native app `bimble` (ticket NUI1, ADR 0084; the job ccore-native): its third parties
#         checked (native/deps.sh --check), the shaders' committed SPIR-V against their sources (native/build.sh
#         shaders --check), the shell's data against the TypeScript sources, build.sh
#         native-app, then test/native_app_test.mjs with CCORE_NATIVE_REQUIRED=1 (no skip: a missing
#         SDL3 or Vulkan device fails), and the same under ASan and UBSan (native/build.sh asan).
#   clang for a machine with neither tcc nor wasm-ld (macOS, ticket MAC1, the job ccore-macos):
#         test/run.sh with clang (the kernel when deps.sh built it), then test/host.sh on the host
#         built by build.sh native (clang -O2 -Werror, every suite and tool too).
# Prints one line per suite (ok or FAIL, seconds, the "N passed, M failed" sums of its log) and a
# last line "ci: N suites, M failed". A suite fails on a non-zero exit, a line that starts with
# FAIL, or a "failed" count above 0. Every suite runs, even after a failure. Exits 1 on a failure.
# Logs: ccore/out/ci/<suite>.log (CCORE_CI_LOGS: another directory). CCORE_PORT_BASE defaults to
# auto (test/ports.mjs): every port of every test is locked and bind-tested, so two runs at once, in
# one worktree or two, never share a port (ticket TEST1). CCORE_CI_BUILD=0 skips the build-* suites
# and takes out/ as built: for a second run at the same time in the same tree (a build would rewrite
# the binaries the first run is executing). CCORE_CI_TIMEOUT: seconds per suite (default 900).
# The lab tests need web-ui/node_modules (npm ci in web-ui/) and /usr/bin/chromium; dev_test
# needs inotifywait (inotify-tools); build-wasm-view needs wasm-opt (binaryen, ci/binaryen.sh).
# Portable (ticket MAC1): macOS's /bin/bash 3.2; no GNU `timeout` there, so a shell watchdog
# (below); the clock of portable.sh.
set -uo pipefail
cd "$(dirname "$0")/.."
source ./portable.sh
mode=${1:-fast}
case "$mode" in fast | full | unit | san | lab | clang | native) ;; *) echo "usage: $0 [fast|full|unit|san|lab|clang|native]" >&2; exit 2 ;; esac
export CCORE_PORT_BASE=${CCORE_PORT_BASE:-auto}
LIMIT=${CCORE_CI_TIMEOUT:-900}
LOGS=${CCORE_CI_LOGS:-out/ci}
mkdir -p "$LOGS"

n=0 failed=0 t_all=$(ms_now)
# limit <seconds> <command...>: GNU timeout -k 15 where there is one; else the command in the
# background and a watchdog that sends TERM at the limit, KILL 15 s later. Exit 124 on a timeout.
if command -v timeout > /dev/null 2>&1; then
    limit() { timeout -k 15 "$@"; }
else
    limit() {
        local secs=$1 pid dog rc flag
        shift
        flag=$(mktemp "${TMPDIR:-/tmp}/ccore-ci.XXXXXX")
        rm -f "$flag"
        "$@" &
        pid=$!
        (
            trap 'kill "$s" 2> /dev/null; exit 0' TERM
            sleep "$secs" & s=$!
            wait "$s"
            : > "$flag"
            kill -TERM "$pid" 2> /dev/null
            sleep 15 & s=$!
            wait "$s"
            kill -KILL "$pid" 2> /dev/null
        ) &
        dog=$!
        wait "$pid"
        rc=$?
        kill -TERM "$dog" 2> /dev/null
        wait "$dog" 2> /dev/null
        if [[ -e $flag ]]; then rc=124; rm -f "$flag"; fi
        return $rc
    }
fi
done_=" "
# suite <name> <command...>: runs the command with its output in out/ci/<name>.log, once a run.
suite() {
    local name=$1 log="$LOGS/$1.log" t0 t1 rc bad=0 sums skip part
    shift
    [[ $done_ == *" $name "* ]] && return 0
    done_+="$name "
    [[ ${CCORE_CI_BUILD:-1} == 0 && $name == build-* ]] && { printf 'skip  %-20s (CCORE_CI_BUILD=0)\n' "$name"; return 0; }
    t0=$(ms_now)
    limit "$LIMIT" "$@" > "$log" 2>&1 < /dev/null
    rc=$?
    t1=$(ms_now)
    # The checks of the log: the sum of its "N passed, M failed" lines (one per C suite), or its
    # last such line when that one is the total of the others (wasm_run's sub-suites).
    sums=$(grep -oE '[0-9]+ passed, [0-9]+ failed' "$log" | awk '
        { p[NR] = $1; f[NR] = $3; sp += $1; sf += $3 }
        END { if (NR == 0) exit
              if (NR > 1 && p[NR] == sp - p[NR] && f[NR] == sf - f[NR]) { sp = p[NR]; sf = f[NR] }
              printf "%d passed, %d failed", sp, sf }')
    ((rc != 0)) && bad=1
    # FIXUI: a FAIL line, or one a suite re-prints from its sub-test ("  | FAIL …", lab_place).
    grep -qE '^([[:space:]]*\|[[:space:]]*)?FAIL([[:space:]:]|$)' "$log" && bad=1
    # Any "M failed" above 0 makes the sum above 0. (No `grep | grep -q`: under pipefail the
    # writer's SIGPIPE, when -q stops early, can turn a match into false.)
    [[ $sums =~ ([0-9]+)\ failed$ ]] && ((BASH_REMATCH[1] > 0)) && bad=1
    # GPU1: a suite that declares a skip says so in the roll-up: with no check at all and a line
    # "skipped: <why>" (link_large_test) it is "skipped", not a failure (FIXUI: a silent zero still
    # fails); a part skipped, a line "SKIP <what>: <why>" (lab_webgpu), is added to its counts.
    skip=$(grep -m1 -E '^skipped: ' "$log" | cut -c1-140)
    part=$(grep -m1 -E '^SKIP ' "$log" | cut -c1-140)
    if [[ $sums == "0 passed, 0 failed" ]]; then
        if [[ -n $skip ]]; then sums=$skip; else bad=1; sums="no check ran (0 passed, 0 failed)"; fi
    elif [[ -n $part && -n $sums ]]; then sums+=" ($part)"; fi
    ((rc == 124)) && sums="timed out after $LIMIT s"
    n=$((n + 1))
    if ((bad)); then
        failed=$((failed + 1))
        printf 'FAIL  %-20s %7.1f s  %s (exit %d, log ccore/%s)\n' "$name" "$((t1 - t0))e-3" "${sums:-}" "$rc" "$log"
    else
        printf 'ok    %-20s %7.1f s  %s\n' "$name" "$((t1 - t0))e-3" "${sums:-}"
    fi
}

# test/run.sh compiles the library once and runs its suites in parallel (CCORE_TEST_JOBS).
unit_c() { suite run-tcc test/run.sh; }
san() { suite run-clang-asan env CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh; }
# FIXDOC: the host program (server, files, MCP) under the sanitizers; one suite at a time.
san_host() {
    suite build-asan ./build.sh asan
    for t in host_test fs_test mcp_test mesh_http_test files_test \
        session_test session_log_test versions_test links_test verbs_session_test; do # FIXSES: the last five
        suite "san-$t" test/san_host.sh "$t"
    done
}
gate_rest() {
    suite build-wasm ./build.sh wasm
    suite wasm_run node test/wasm_run.mjs
    suite host test/host.sh # host_test.mjs and lab_boot.mjs
}
labs() {
    suite build-tcc ./build.sh tcc
    suite build-wasm ./build.sh wasm # lab_wasm, lab_pages
    # URL1: lab_open_url serves the page as Pages does, with the viewer-only wasm (the lab job alone has not built it)
    [[ -f out/wasm/ccore-view.wasm ]] || suite build-wasm-view-lab ./build.sh wasm-view
    for f in test/lab_*.mjs; do
        t=$(basename "${f%.mjs}")
        [[ $t == lab_boot ]] && continue # host.sh
        suite "$t" node "$f"
    done
    # PERF1: the lab's performance (tools/perf.mjs, short mode: 1 Lighthouse run per page and preset,
    # the sample model, no network) into out/ci/perf.md (uploaded with the logs); the change against
    # the committed results/perf.md is printed. Fails only when a page does not load or the tool
    # crashes, never on a slower number (runner noise). `node tools/perf.mjs --full` on a dev machine.
    suite perf node tools/perf.mjs --prebuilt --out "$LOGS/perf.md"
    grep '^perf: ' "$LOGS/perf.log" | sed 's/^/      /'
}
# FIXDOC: the README's Status block must equal what tools/status.mjs writes from the committed
# reports (before tools/weight.mjs below rewrites results/weight.md for this machine), and the
# parity tool runs the C host on its scenarios (fails on a crash, a 5xx or malformed JSON only).
docs_checks() {
    suite status node tools/status.mjs --check
    suite parity node tools/parity.mjs --check
    # NUI1: the native app's data (ribbon, keys, strings, machine, themes) equals what the TypeScript gives
    suite shell_data node ../web-ui/scripts/export-shell-data.mjs --check
}
native_suites() {
    suite shell_data node ../web-ui/scripts/export-shell-data.mjs --check
    suite native-shaders native/build.sh shaders --check
    suite build-native-app ./build.sh native-app
    suite native-deps native/deps.sh --check
    suite native_app env CCORE_NATIVE_REQUIRED=1 node test/native_app_test.mjs
    suite native_geo env CCORE_NATIVE_REQUIRED=1 node test/native_geo_test.mjs
    suite build-native-asan native/build.sh asan
    suite native_app_asan env CCORE_NATIVE_REQUIRED=1 NATIVE_BIN=out/app-asan/bimble ASAN_OPTIONS=detect_leaks=0 node test/native_app_test.mjs
}
unit_rest() {
    # The viewer-only wasm and its 400 KB budget (ADR 0069), for test/wasm_view_run.mjs below.
    suite build-wasm-view ./build.sh wasm-view
    # The release archive (ticket R1), for test/release_test.mjs below.
    suite build-release ./build.sh release
    # SIZE1: the weight table (results/weight.md) from the builds above; its change is printed.
    # Fails only on an object no feature claims (tools/weight.mjs FEATURES), never on growth.
    suite weight node tools/weight.mjs
    grep '^weight: ' "$LOGS/weight.log" | sed 's/^/      /'
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
    unit) suite build-tcc ./build.sh tcc; unit_c; gate_rest; docs_checks; unit_rest ;;
    san) san; san_host ;;
    lab) labs ;;
    full) suite build-tcc ./build.sh tcc; unit_c; san; san_host; gate_rest; docs_checks; labs; unit_rest ;;
    clang) suite run-clang env CC=clang test/run.sh; suite host env HOST_BUILD=native test/host.sh ;;
    native) native_suites ;;
esac

printf 'ci: %s, %d s in all, logs in ccore/%s\n' "$mode" $((($(ms_now) - t_all) / 1000)) "$LOGS"
printf 'ci: %d suites, %d failed\n' "$n" "$failed"
((failed == 0))
