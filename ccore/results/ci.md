# CI for the C core (ticket K1)

One script holds the checks, `ccore/ci/run.sh [fast|full]`. Builders and CI run the same script.
`.github/workflows/ccore.yml` runs `full` on Linux (`ccore-linux`). It also runs `test/run.sh` on
macOS as a probe (`ccore-macos`, `continue-on-error`). The probe log shows what a port needs.

## Times on the dev machine

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
