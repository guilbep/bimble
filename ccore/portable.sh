# The few things the ccore scripts need that differ between Linux (GNU tools) and macOS (BSD
# tools, /bin/bash 3.2), in one place (ticket MAC1). Sourced by deps.sh (so by build.sh and
# test/run.sh) and by ci/run.sh; defines functions and two variables, runs nothing else.
# bash 3.2 rules for every script that sources this: no `wait -n`, no $BASHPID, no
# $EPOCHREALTIME without a fallback, and an array that may be empty is expanded as
# ${a[@]+"${a[@]}"} (bash before 4.4 calls "${a[@]}" of an empty array unbound under set -u).
#   CC_OS             Linux, Darwin... (uname -s)
#   PLATFORM_CFLAGS   C flags of this OS for every native compile: on macOS -D_DARWIN_C_SOURCE,
#                     because a file that defines _POSIX_C_SOURCE otherwise loses MAP_ANONYMOUS,
#                     SO_NOSIGPIPE, st_mtimespec... (the glibc build needs no flag)
#   ms_now            milliseconds since the epoch (GNU date %N, else bash 5's EPOCHREALTIME,
#                     else perl, else whole seconds)
#   us_now            microseconds since the epoch (EPOCHREALTIME without a fork, else as ms_now)
#   jobs_reset, jobs_throttle <n>   at most n background jobs: call jobs_throttle after each `&`.
#                     `wait -n` (bash 4.3) where there is one, else wait for the oldest job
#   sha1_16           the first 16 hex digits of the SHA-1 of stdin (sha1sum, else shasum)
#   sha1_each <f...>  "<sha1> <file>" per file, as sha1sum prints it
#   file_sig <f...>   "<name> <mtime s> <size>" per file (GNU stat -c, BSD stat -f)

CC_OS=$(uname -s)
PLATFORM_CFLAGS=()
[[ $CC_OS == Darwin ]] && PLATFORM_CFLAGS=(-D_DARWIN_C_SOURCE)

if [[ $(date +%N 2> /dev/null) =~ ^[0-9]+$ ]]; then
    ms_now() { local t; t=$(date +%s%N); echo "${t:0:13}"; }
elif [[ -n ${EPOCHREALTIME:-} ]]; then
    ms_now() { local t=${EPOCHREALTIME/[.,]/}; echo "${t:0:13}"; }
elif command -v perl > /dev/null 2>&1; then
    ms_now() { perl -MTime::HiRes=time -e 'printf "%d\n", time() * 1000'; }
else
    ms_now() { echo "$(date +%s)000"; }
fi

if [[ -n ${EPOCHREALTIME:-} ]]; then
    us_now() { local t=${EPOCHREALTIME/[.,]/}; echo "$((10#$t))"; }
elif [[ $(date +%N 2> /dev/null) =~ ^[0-9]+$ ]]; then
    us_now() { local t; t=$(date +%s%N); echo "${t:0:16}"; }
elif command -v perl > /dev/null 2>&1; then
    us_now() { perl -MTime::HiRes=time -e 'printf "%d\n", time() * 1000000'; }
else
    us_now() { echo "$(date +%s)000000"; }
fi

if ((BASH_VERSINFO[0] > 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] >= 3))); then
    jobs_reset() { _jobs_n=0; }
    jobs_throttle() { if ((++_jobs_n >= $1)); then wait -n || true; _jobs_n=$((_jobs_n - 1)); fi; }
else
    jobs_reset() { _jobs_q=(); }
    jobs_throttle() {
        _jobs_q+=("$!")
        if ((${#_jobs_q[@]} >= $1)); then
            wait "${_jobs_q[0]}" || true
            _jobs_q=("${_jobs_q[@]:1}")
        fi
    }
fi
jobs_reset

if command -v sha1sum > /dev/null 2>&1; then
    sha1_16() { sha1sum | cut -c1-16; }
    sha1_each() { sha1sum "$@"; }
else
    sha1_16() { shasum -a 1 | cut -c1-16; }
    sha1_each() { shasum -a 1 "$@"; }
fi

if stat -c %s / > /dev/null 2>&1; then
    file_sig() { stat -c '%n %Y %s' "$@"; }
else
    file_sig() { stat -f '%N %m %z' "$@"; }
fi
