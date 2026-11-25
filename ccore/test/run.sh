#!/usr/bin/env bash
# Runs ccore test suites: test/run.sh [suite...] (default: all). Builds with tcc, or with $CC.
# Exits non-zero if any check fails.
#
# The library (src/*.c but the wasm stub, session/*.c, host/platform_native.c) is compiled once,
# one object per file, in parallel. Then each suite is compiled, linked and run as a job of its
# own, its output in a log of its own. The logs are printed whole, one suite after the other in
# the order asked (never interleaved), each followed by a "time" line: seconds to compile and link
# the suite, seconds to run it. A suite fails on a compile error, a non-zero exit or a signal (a
# sanitizer report); every suite runs, even after a failure. The last line gives the totals.
#   CCORE_TEST_JOBS   suites at once (default: the number of CPUs; 1 runs them one by one)
#   CC, CFLAGS        the compiler and extra flags, e.g.
#                     CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh file
# Portable (ticket MAC1): macOS's /bin/bash 3.2 and BSD tools. portable.sh (via deps.sh) gives the
# clock (us_now), the job limit without `wait -n` (jobs_throttle) and -D_DARWIN_C_SOURCE on macOS;
# an array that may be empty is expanded as ${a[@]+"${a[@]}"}; no mapfile.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
ALL=(); for f in test/*_test.c; do t=${f#test/}; ALL+=("${t%_test.c}"); done
if (($#)); then suites=("$@"); else suites=("${ALL[@]}"); fi
CC=${CC:-tcc}
case "$CC" in
  tcc) FLAGS=(-g -Wall -DRYU_ONLY_64_BIT_OPS) ;;
  *)   FLAGS=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function) ;;
esac
if [[ -n ${CFLAGS:-} ]]; then read -r -a extra <<< "$CFLAGS"; FLAGS+=(${extra[@]+"${extra[@]}"}); fi
# A sanitizer report fails the suite (UBSan only prints by default: a report once passed unseen).
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"
JOBS=${CCORE_TEST_JOBS:-$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 2)}
# The boolean kernel when found (build.sh, deps.sh): clang links it static, tcc shared.
source ./deps.sh
kernel_setup
FLAGS+=(${KFLAGS[@]+"${KFLAGS[@]}"} ${PLATFORM_CFLAGS[@]+"${PLATFORM_CFLAGS[@]}"})
LINK=(${KLINK_CLANG[@]+"${KLINK_CLANG[@]}"})
[[ $(basename "$CC") == tcc ]] && LINK=(${KLINK_TCC[@]+"${KLINK_TCC[@]}"})
# One directory per compiler and flags: a clang run and a clang+ASan run never share objects.
tag=$(basename "$CC")
[[ -n ${CFLAGS:-} ]] && tag+=-$(printf '%s' "$CFLAGS" | cksum | cut -d' ' -f1)
out="out/run-$tag"
rm -rf "$out/obj" "$out/log"
mkdir -p "$out/obj" "$out/log"
INC=(-Isrc -Ivendor/ryu -Isession -Ihost)
t_all=$(us_now)

# us <start> <end>: the microseconds between two us_now values.
us() { echo $(($2 - $1)); }
# secs <microseconds>: "12.34".
secs() { printf '%d.%02d' $(($1 / 1000000)) $(($1 % 1000000 / 10000)); }

# The library: every src/*.c but the wasm stub, as build.sh compiles it, the session layer and
# the native platform. Each suite links all the objects, as the one command line of before did.
SRC=(); for f in src/*.c; do [[ $f == src/wasm_stub.c ]] || SRC+=("$f"); done
SRC+=(session/*.c host/platform_native.c host/platform_win.c) # WIN1: platform_win.c is empty but on Windows
OBJ=()
jobs_reset
for f in "${SRC[@]}"; do
    o="$out/obj/$(basename "$(dirname "$f")")_$(basename "${f%.c}").o"
    OBJ+=("$o")
    { "$CC" "${FLAGS[@]}" "${INC[@]}" -c "$f" -o "$o" > "$o.log" 2>&1 || echo "$f $o" >> "$out/obj/failed"; } &
    jobs_throttle "$JOBS"
done
wait
t_lib=$(us_now)
# Warnings (tcc's -Wall prints and goes on), once for the library.
cat "$out"/obj/*.o.log
if [[ -f $out/obj/failed ]]; then
    while read -r f o; do echo "FAIL compile $f (log above)"; done < "$out/obj/failed"
    exit 1
fi

# one <suite>: compile and link, then run; the output in log/<suite>, "<exit> <compile us> <run us>"
# in log/<suite>.rc (exit 98: the compile failed, 99: no such suite).
one() {
    local s=$1 log="$out/log/$1" t0 t1 t2 rc=0
    t0=$(us_now)
    if [[ ! -f "test/${s}_test.c" ]]; then
        echo "unknown suite: $s" > "$log"
        echo "99 0 0" > "$log.rc"
        return
    fi
    if ! "$CC" "${FLAGS[@]}" "${INC[@]}" "${OBJ[@]}" "test/${s}_test.c" ${LINK[@]+"${LINK[@]}"} -o "$out/test_$s" > "$log" 2>&1; then
        echo "98 $(us "$t0" "$(us_now)") 0" > "$log.rc"
        return
    fi
    t1=$(us_now)
    "$out/test_$s" golden/floats >> "$log" 2>&1 < /dev/null || rc=$?
    t2=$(us_now)
    echo "$rc $(us "$t0" "$t1") $(us "$t1" "$t2")" > "$log.rc"
}

# Longest first (by the times of the last run, if any): a long suite never starts last.
order=("${suites[@]}")
if [[ -f $out/times ]]; then
    order=()
    while read -r s; do order+=("$s"); done < <(for s in "${suites[@]}"; do
        printf '%s %s\n' "$(awk -v s="$s" '$1 == s { t = $2 } END { print t + 0 }' "$out/times")" "$s"
    done | sort -k1,1nr | awk '{ print $2 }')
fi
jobs_reset
for s in "${order[@]}"; do
    # The job's stderr to the log too: bash's own line for a signal ("Aborted") lands there.
    one "$s" 2>> "$out/log/$s" &
    jobs_throttle "$JOBS"
done
wait

# The roll-up, in the order asked: each log whole, then its time line.
status=0 nfail=0 sum=0
: > "$out/times.new"
for s in "${suites[@]}"; do
    if [[ ! -f $out/log/$s.rc ]]; then # the job died before it wrote its result
        echo "FAIL $s: no result"
        status=1 nfail=$((nfail + 1))
        continue
    fi
    read -r rc tc tr < "$out/log/$s.rc"
    cat "$out/log/$s"
    case $rc in
        0) ;;
        98) echo "FAIL $s: compile" ;;
        99) echo "FAIL $s: no test/${s}_test.c" ;;
        *) echo "FAIL $s: exit $rc" ;;
    esac
    ((rc != 0)) && status=1 nfail=$((nfail + 1))
    printf '  time %-16s compile %6s s  run %6s s\n' "$s" "$(secs "$tc")" "$(secs "$tr")"
    echo "$s $((tc + tr))" >> "$out/times.new"
    sum=$((sum + tc + tr))
done
mv "$out/times.new" "$out/times"
printf 'run.sh: %d suites (%s%s), %d failing; library %s s, %s s in all, %s s of suite time on %d jobs\n' \
    "${#suites[@]}" "$CC" "${CFLAGS:+ $CFLAGS}" "$nfail" \
    "$(secs "$(us "$t_all" "$t_lib")")" "$(secs "$(us "$t_all" "$(us_now)")")" "$(secs "$sum")" "$JOBS"
exit $status
