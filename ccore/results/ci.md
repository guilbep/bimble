# CI for the C core (tickets K1, K3)

One script holds the checks, `ccore/ci/run.sh [fast|full|unit|san|lab]`. Builders and CI run the
same script. `.github/workflows/ccore.yml` runs its three parts on Linux as three parallel jobs:
`ccore-linux` (`unit`), `ccore-san` (`san`), `ccore-lab` (`lab`); `full` is the three in one run. It
also runs `test/run.sh` on macOS as a probe (`ccore-macos`, `continue-on-error`). The probe log
shows what a port needs.

## K3: a faster CI (25 Nov 2026)

GitHub run 45662558516 (`ci/run.sh full`, one job): 1264 s, of which `run-clang-asan` 929 s.

### What dominated

Not the checks: the compile. The old `test/run.sh` built each suite with one command line that
held the whole library (`src/*.c`, `session/*.c`, `host/platform_native.c`: 54 files, 31,000
lines). So it compiled the library 26 times, one suite after the other. With tcc that costs little
(0.3 s for the library). With clang `-O1` and ASan+UBSan, it costs 25 s each time, about 650 of the
789 s of the old sanitizer run. The suites themselves run 55 s under the sanitizers, one by one.

`test/run.sh` now compiles the library once, one object per file, in parallel. Then it compiles,
links and runs each suite as a job of its own, as many at once as there are CPUs, longest first
(the times of the last run, in `out/run-<cc>/times`). Each suite writes its own log. The roll-up
prints the logs whole, in name order, each with a line `time <suite> compile X s run Y s`, then
one total line. A suite fails on a FAIL check, a non-zero exit, a signal or a compile error. Every
suite runs, and the exit code is 1 if one fails. `CCORE_TEST_JOBS=1` runs one suite at a time.

No sample knob: the suites that the ticket suspected (`strtod`, `fmt`, `draw`) are not the long
ones (3.4, 0.01 and 2.4 s under ASan). The long ones are `ifc_read` and `ifc_in` (18 s each under
ASan). They import every golden IFC file twice and compare it in full, so a sample would drop
golden comparisons. Every suite runs every check in every mode: 1,288,845 checks with tcc and
with ASan.

### Per suite, one at a time (`CCORE_TEST_JOBS=1`)

Dev machine: 16 GB aarch64, 8 cores, tcc 0.9.28rc (b39cbc70), clang 23.1.3. Other builders were
busy: load average 5 to 6 at the start of each run. Seconds. "compile" is the suite's own file
and the link; the library is compiled once before (tcc 0.4 s, clang 8.2 s, clang+ASan 25.4 s, one
file at a time). Sorted by the ASan run time.

| Suite | tcc compile | tcc run | clang compile | clang run | ASan compile | ASan run |
|---|---:|---:|---:|---:|---:|---:|
| ifc_read | 0.01 | 11.20 | 0.19 | 5.65 | 0.38 | 18.33 |
| ifc_in | 0.01 | 6.84 | 0.25 | 2.94 | 0.55 | 17.75 |
| viewsheet | 0.01 | 2.78 | 0.22 | 1.42 | 0.42 | 5.71 |
| strtod | 0.02 | 7.38 | 0.20 | 2.22 | 0.38 | 3.39 |
| file2 | 0.01 | 2.29 | 0.18 | 1.13 | 0.41 | 3.31 |
| draw | 0.01 | 2.56 | 0.22 | 1.68 | 0.64 | 2.38 |
| viewfile | 0.01 | 0.72 | 0.24 | 0.26 | 0.57 | 1.66 |
| step | 0.01 | 0.66 | 0.17 | 0.19 | 0.35 | 0.74 |
| file | 0.01 | 0.30 | 0.20 | 0.17 | 0.41 | 0.67 |
| mesh | 0.02 | 0.44 | 0.24 | 0.14 | 0.51 | 0.38 |
| sheets | 0.02 | 0.41 | 0.18 | 0.03 | 0.36 | 0.20 |
| georef | 0.01 | 0.12 | 0.29 | 0.04 | 0.54 | 0.19 |
| ifc_out | 0.01 | 0.12 | 0.19 | 0.05 | 0.54 | 0.16 |
| relation | 0.01 | 0.08 | 0.19 | 0.05 | 0.39 | 0.13 |
| grid | 0.01 | 0.07 | 0.26 | 0.02 | 0.56 | 0.10 |
| verbs | 0.01 | 0.01 | 0.27 | 0.00 | 0.43 | 0.06 |
| quantity | 0.01 | 0.02 | 0.19 | 0.01 | 0.39 | 0.05 |
| boolean | 0.02 | 0.00 | 0.18 | 0.00 | 0.39 | 0.04 |
| query | 0.01 | 0.01 | 0.14 | 0.00 | 0.37 | 0.04 |
| verbs_all | 0.01 | 0.02 | 0.18 | 0.01 | 0.39 | 0.04 |
| tool | 0.01 | 0.01 | 0.21 | 0.00 | 0.40 | 0.03 |
| annotation | 0.01 | 0.00 | 0.16 | 0.00 | 0.35 | 0.01 |
| fmt | 0.02 | 0.01 | 0.16 | 0.00 | 0.32 | 0.01 |
| json | 0.01 | 0.00 | 0.14 | 0.00 | 0.32 | 0.01 |
| two_sessions | 0.01 | 0.00 | 0.15 | 0.00 | 0.31 | 0.01 |
| guid | 0.01 | 0.00 | 0.21 | 0.00 | 0.40 | 0.00 |
| **sum** | 0.31 | 36.05 | 5.21 | 16.01 | 11.08 | 55.40 |

The slowest library files under clang+ASan: `guid.c` 4.1 s, `ifc_in.c` 2.7 s, `ifc_read.c` 2.5 s,
then about 1 s each. In parallel the library takes 6.6 s.

### Before and after, same machine, same session

| Run | Before (old `test/run.sh`, serial) | After (8 jobs) | Load at start (before / after) |
|---|---:|---:|---|
| `test/run.sh` (tcc) | 39.3 s | 11.9 s (twice: 11.87, 11.85) | 3.9 / 9.0 |
| `CC=clang test/run.sh` | 240.1 s | 8.5 s | 4.5 / 4.7 |
| clang + ASan + UBSan | 789.5 s (13 min 10 s) | 29.1 s | 3.8 to 8.8 / 8.4 |

The tcc run is bound by its longest suite: `ifc_read`, 11.2 s alone. Targets: tcc under 15 s
(reached: 11.9 s), sanitizers under 3 min (reached: 29 s).

### The lab suites: a settle that always timed out

`lab_dims`, `lab_viewcube`, `lab_section` and `lab_plan` each had a `settle()` that waited for one
more frame: `waitFor("a new frame", 5000 /* 3000 in lab_viewcube */).catch(() => {})`, then 100 ms.
The viewport draws only when something changes. Each call came after the change had been drawn,
so no frame came: every call waited the full timeout. Traced with a log line in `waitFor` (one run
each): 9 timeouts of 5 s in `lab_dims`, 15 of 3 s in `lab_viewcube`, 6 of 5 s in `lab_section` and
in `lab_plan`.

`settle()` now returns when no new frame came for 400 ms (at most the old timeout). No check
changed. One wait was not waste. In `lab_viewcube`, the Top turn must show in-between angles. When
the turn starts less than about 2 s after the column selection, its first frame takes about
400 ms (headless Chromium, SwiftShader): the turn ends in one frame, 0 in-between angles, in 6 of 9
runs (quiet window 400 ms, or a 1 s wait). A wait of 2 s or more passed 3 of 3. The old settle
waited 3.1 s there, so that one site keeps it (`sleep(2600)` after the settle). Then 4 more runs of
`lab_viewcube` and 1 of each other suite: all passed.

| Lab suite | Before (s) | After (s) | GitHub before (s) |
|---|---:|---:|---:|
| lab_dims | over 51.4 | 9 to 10 | 51.9 |
| lab_viewcube | over 54.2 | 17 to 19 | 51.5 |
| lab_section | over 37.8 | 10 to 11 | 36.8 |
| lab_plan | over 37.1 | 9 to 10.5 | 36.3 |

"Before" is the time of the last timeout in the traced run (the suite ended soon after). "After" is
the range of the runs since the change (`ci/run.sh full` and the repeats above).

`lab_pages` (20 s) has no such wait: 14.5 s of it is `npm run build:ccore`, the Vite build that it
checks. The lab suites still run one after the other. Each one starts its own Vite and Chromium, and
their pixel and animation checks depend on the CPU. One shared Vite would save about 2 s a suite, but
the change would touch every lab file. Not done: the lab now runs in its own job, beside the others.

### `ci/run.sh full` after K3, dev machine

`ccore/ci/run.sh full`, 19:02, load average 5.3 at the start: 33 suites, 0 failed, **197 s** in all.
build-tcc 0.9, run-tcc 13.7 (1,288,845 checks), run-clang-asan 27.6 (1,288,845), build-wasm 7.2,
wasm_run 2.6, host 4.9, the 11 lab suites 98.3 (lab_pages 19.6, lab_viewcube 17.4), build-wasm-view
5.3, build-release 3.5, the other .mjs suites 27.4 (session_log_test 13.0, wasm_view_run 12.0),
dev_test 6.1. The same run before K3 was 633 s (K1, below) and would be about 1100 s now, from
the parts measured above.

### The jobs on GitHub (expected; the first run gives the real times)

`ubuntu-latest` has 4 vCPUs. The old run there was 1.2 to 2 times slower than the dev machine.

| Job | Mode | What | Expected |
|---|---|---|---|
| `ccore-linux` | `unit` | apt, tcc, binaryen, manifold, npm ci; builds; C suites with tcc (bound by `ifc_read`); wasm, host, release, the .mjs suites | 1.5 to 2 min of steps after 1.5 to 2 min of setup: about 4 min |
| `ccore-san` | `san` | apt (clang, cmake), manifold; the C suites under ASan+UBSan | library about 10 s, suites about 25 to 40 s: about 2 to 3 min with setup |
| `ccore-lab` | `lab` | as `ccore-linux` without inotify; build tcc and wasm; 11 Chromium suites | about 1.5 to 2 min of suites (225 s before): about 4 min |

The jobs run side by side, so the workflow takes as long as the slowest: about 4 minutes instead of
about 23. `ccore-prerelease` waits for all three and `ccore-release`. On a cache miss each job builds
manifold itself (the three caches have the same key; the first to finish saves it).

## Times on the dev machine (K1, before K3)

`ccore/ci/run.sh full`, 25 Nov 2026, 16 GB aarch64, tcc 0.9.28rc (b39cbc70), clang 23.1.3, Node 26,
Chromium of the distribution. Other builders were busy (load average 5.0 at the start). One run, in
seconds. Each suite has `CCORE_PORT_BASE=auto`.

| Suite | What | s | Checks |
|---|---|---:|---:|
| build-tcc | `build.sh tcc` | 0.9 | |
| run-tcc | `test/run.sh`, 19 C suites with tcc | 34.6 | 1,277,937 |
| run-clang-asan | the same suites, clang with ASan and UBSan | 469.0 | 1,277,937 |
| build-wasm | `build.sh wasm` | 6.7 | |
| wasm_run | `test/wasm_run.mjs` | 2.5 | 25,428 |
| host | `test/host.sh`: host_test, lab_boot | 4.6 | 78 |
| **fast (the gate)** | | **518** | |
| lab_gl | | 5.1 | 13 |
| lab_tool | | 4.7 | 17 |
| lab_place | | 2.9 | 16 |
| lab_section | | 37.6 | 18 |
| lab_plan | | 36.5 | 25 |
| lab_wasm | | 5.8 | 26 |
| lab_links | | 5.2 | 17 |
| files_test | | 0.2 | 59 |
| large_test | | 0.5 | 15 |
| links_test | | 0.2 | 43 |
| mesh_http_test | | 0.2 | 26 |
| session_log_test | | 13.1 | 14 |
| session_test | | 0.2 | 22 |
| verbs_session_test | | 0.4 | 30 |
| views_test | | 0.1 | 41 |
| visibility_test | | 0.1 | 65 |
| dev_test | | 2.3 | 18 (20 since K2) |
| **full** | 23 suites, 0 failed | **633** | |

The ASan run is 91 % of the gate (469 of 518 s). An earlier run gave the same order: 488 of
537 s. `lab_links` passed in both runs (U1 owns its flaky pixel test).

Steps that only CI does (they are cached, so they run only on a cache miss). Times are on the dev
machine:

| Step | s | Note |
|---|---:|---|
| `ci/tcc.sh` (fetch, configure, make, install) | 6.3 | the make alone: 3.7 |
| manifold (`deps.sh`): clone, cmake configure, build `-j 2` | 2.0 + 3.0 + 27.6 | then `deps_import` links the `.so` |
| `npm ci` in `web-ui/` (55 packages, 112 MB) | 0.6 | warm npm cache |

The 19 C suites also pass with the tcc that `ci/tcc.sh` builds (`build.sh tcc`, then
`test/run.sh`, 19 of 19 suites at 0 failed). Times on a GitHub runner: read them in the first run.

## Why tcc is built from source

Ubuntu 24.04 (the `ubuntu-latest` runner) ships tinycc `62c30a4a` of August 2020. That version has
no `stdatomic.h` and no `__atomic_*` builtins. Both came in January 2021 (`f2e7742a`).
`src/jobs.h` includes `stdatomic.h` and uses the builtins, so the apt package cannot compile the
core. `ci/tcc.sh` builds the dev machine's commit (`b39cbc70`) instead. The workflow caches it
under that commit. The ticket asked for apt `tcc`. This is the one change from it.

## What the Linux job assumes (proven only by the first push)

- `/usr/bin/chromium` is a link to the image's Google Chrome. The lab tests name that path, and no
  shared `CHROMIUM` variable exists in `ccore/test/`. One variable in one place is a later change
  for the owner of `test/*.mjs`.
- `kernel.apparmor_restrict_unprivileged_userns=0`, so Chrome's sandbox can start on Ubuntu 24.04.
  The tests do not pass `--no-sandbox`.
- clang from apt: `ubuntu-latest` is Ubuntu 26.04 since November 2026 (image `ubuntu-26.04`,
  20261116), so Ubuntu clang 21.1.8 and lld 21.1.6, not clang 18. test/run.sh compiles with
  `-Wall -Wextra -Werror`, and the dev machine has clang 23. A warning that only clang 21 gives
  would fail the job (none did in the first run).
- binaryen 130 (`wasm-opt`) from `ci/binaryen.sh`, cached like tcc: `build.sh wasm-view` needs it
  for the 400 KB budget (ticket K2, `results/wasm-size.md`). Ubuntu 26.04's own is 120.
- `lld` gives `wasm-ld` for `build.sh wasm`. `inotify-tools` is for `dev_test` (`dev.sh` watches
  with `inotifywait`).

## The first run on GitHub (ticket K2)

Run of `942f316` (25 Nov 2026, `ubuntu-26.04` image, x86_64): 28 suites, 3 failed, 1175 s in all.
build-tcc 1.1 s, run-tcc 40.6 s, the lab suites 5 to 53 s.

| Suite | Failure | Cause | Fix |
|---|---|---|---|
| run-clang-asan | timed out at 900 s | the sanitizer run is longer on a runner | its own limit, 2700 s (`c77bae2`) |
| build-wasm-view | 426,448 bytes, over 400,000 | no `wasm-opt` on the runner. On the dev machine clang ran `/usr/bin/wasm-opt -Os` after the link without saying so (344 KB). Not the clang version: clang 21 here gives 426,434 without wasm-opt, 353,477 with it | `build.sh wasm-view` runs wasm-opt as an explicit step and fails without it; the workflow installs binaryen 130 (`ci/binaryen.sh`, cached) |
| dev_test | "syntax error: no restart", "the old host still answers, same epoch" | the test, not dev.sh. After cycle 5 it restored `calls.c` and waited for 300 + 300 ms without a new "rebuilt in" line. On the runner a rebuild took 700 to 920 ms, so the restore's build was still running: the test wrote the syntax error, then saw the restore's restart (new epoch) and took it for the syntax error's. The revert check passed only by the same mistake (its epoch was the restore's) | `dev.sh` prints a line when a cycle starts ("dev.sh: session/calls.c changed, building"); every cycle ends with one line, so as many ends as starts is idle. The test waits for the cycle of each save, checks one build per save, and checks the revert gives exactly one restart after the syntax error |

The dev_test failure is reproduced on the old code by a slower tcc (a `tcc` first in `PATH` that
sleeps 0.8 s before each call: the restore's build takes about 850 ms): the same two FAIL lines,
and the revert's epoch equals the syntax error's, as on the runner. The new check "revert: one
build, one restart since the syntax error" counts 2 restarts on the old synchronisation.

## macOS: what blocks

`ccore-macos` runs `test/run.sh` with Apple clang and `CC_NO_KERNEL=1`. When `/bin/bash` is 3.2,
the job installs a newer bash first. Its log is the evidence. The costs below are estimates from
the code; the job has not run yet.

| What blocks | Where | Cost |
|---|---|---|
| `getrandom()` is Linux only (`getentropy()` on macOS, same header) | `src/guid.c` | small: one `#ifdef` |
| bash 3.2: an empty array under `set -u` is an error (`"${KFLAGS[@]}"` without a kernel) | `test/run.sh`, `build.sh` | small: Homebrew bash, or guard the expansions |
| `stat -c`, `sha1sum` (BSD `stat -f`, `shasum`) | `deps.sh` (`deps_import`) | small |
| `-Wl,--whole-archive`, `.so`, `-Wl,-rpath=` (ld64: `-force_load`, `.dylib`, `-rpath,`) | `deps.sh` (`deps_import`, `kernel_setup`) | moderate: a second link path for the kernel |
| `date +%s%N` (BSD date has no `%N`), `llvm-ar`, `sha1sum` | `build.sh` | small, but `step` times and the host cache key use them |
| wasm32: Apple clang has no `wasm-ld` | `build.sh wasm` | small: Homebrew `llvm` and `lld` |
| `MSG_NOSIGNAL` (macOS: `SO_NOSIGPIPE`; SIGPIPE is already ignored) | `host/server.c` | small |
| `inotifywait`, `setsid` | `dev.sh` | moderate: `fswatch` or kqueue, another way to detach |
| GNU `timeout`, `date +%s%N` | `ci/run.sh` | small: coreutils from Homebrew |
| `/usr/bin/chromium` | the lab tests | small: one shared variable |
| tcc on macOS arm64: tinycc has Mach-O arm64 code (`tccmacho.c`), but it has never been built here | `ci/tcc.sh`, the edit loop | unknown: build it on a runner first |

POSIX threads, `sysconf`, `clock_gettime`, `open`/`rename` and the BSD sockets exist on macOS.
`jobs.c`, `platform_native.c`, `session.c` and most of `server.c` should compile as they are. The
probe log confirms this or shows what fails.

## Windows: what blocks

Nothing has been tried. These estimates are from reading the code.

| What blocks | Where | Cost |
|---|---|---|
| BSD sockets, `poll`, `sigaction`, `MSG_NOSIGNAL` (Winsock: `WSAStartup`, `closesocket`, `WSAPoll`, `SetConsoleCtrlHandler`) | `host/server.c` | large: a socket layer or a second server file |
| `rename` does not replace an existing file (`MoveFileExW`), `/dev/urandom` (`BCryptGenRandom`), UTF-8 paths need the wide-character API | `host/platform_native.c`, `session/session.c` (the session file), `host/main.c` (token) | moderate |
| `getrandom()` | `src/guid.c` | small: `BCryptGenRandom` |
| pthreads, `sysconf` | `src/jobs.c` | small with mingw (winpthreads), moderate with MSVC |
| `__atomic_*` builtins: MSVC has none | `src/jobs.h` | none with clang (llvm-mingw or clang-cl), moderate with MSVC |
| every script is bash with GNU tools | `build.sh`, `deps.sh`, `test/*.sh`, `ci/run.sh`, `dev.sh` | moderate: MSYS2 or Git Bash runs most of them; `dev.sh` needs another watcher |
| tcc: tinycc has an x86_64 Windows target, none for arm64 Windows | the edit loop | small on x86_64 |
| the kernel: `.so` and `--whole-archive` (`.dll`, `/WHOLEARCHIVE`) | `deps.sh` | moderate |
| `/usr/bin/chromium` | the lab tests | small: one shared variable |

The order of cost: macOS first, with small changes in the build scripts and one `#ifdef`. On
Windows the HTTP server is the large item. The wasm build does not need a native port: it runs in
any browser now.
