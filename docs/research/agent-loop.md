# Research: the agent loop of the Rust workspace, timed

_25 Nov 2026. MEASURE ticket T17 (Opus), on branch `feat/native-twin` at b77549e. First measurement
of the loop that CLAUDE.md asks of every change: edit → `cargo clippy --workspace --all-targets --
-D warnings` → `cargo test --workspace`._

## Summary

- One edit costs **about 21 s** (edit in `bimble-core`) and 18 s (edit in `bimble-viewer`).
- **Running the tests (10.7 s) costs more than building them (8.4 s).** Clippy costs 1–2 s.
- The test run is long because `cargo test` runs the 15 test binaries **one after the other**. Each
  binary takes as long as its slowest test. The sum is 9.9 s; the slowest binary is 2.7 s.
- The test build is mostly **linking**: mold makes it 2.5× faster (8.4 s → 3.3 s) and uses 40 % less
  memory (836 MB → 492 MB).
- mold + `cargo nextest` together: **21.0 s → 10.2 s** per edit in `bimble-core`, measured.

## Method

- Machine: 8-core aarch64 (Apple M1 Pro, 6 performance + 2 efficiency cores), 16 GB RAM, Arch
  Linux, kernel 7.2. rustc and clippy 1.100.0, cargo-nextest 0.9.152, mold 3.0.0, GNU ld 2.46.
- Profiles of ADR 0065 (`line-tables-only`, dependencies at opt-level "s"). `CARGO_BUILD_JOBS=6`.
- Warm target directory: the dependencies were already built. Only Bimble's own crates rebuild.
  The machine ran other work (one more agent, no cargo) during the runs.
- Edit sites. Each iteration writes a new value, so every run sees a real change:
  - `core-comment`: a comment line appended to `bimble-core/src/command.rs`.
  - `core-code`: `Vec::with_capacity(cmds.len())` → `cmds.len() + N` in a fn body of the same file.
  - `geom`: `Vec::with_capacity(tris.len() * 3)` → `… * 3 + N` in `bimble-geom/src/lib.rs`.
  - `viewer`: `PROXY_MIN_TRIANGLES` → `1_000_000 + N` in `bimble-viewer/src/lib.rs`.
- One iteration = edit, then the commands in the order of the loop. One warm-up iteration, then 5
  measured iterations. The tables give the median of the 5. Wall clock from a small Python wrapper.
- Peak RSS: the largest single process of the command (`getrusage(RUSAGE_CHILDREN).ru_maxrss`;
  `/usr/bin/time` is not installed). It is not the sum over parallel processes.
- "test build" = `cargo test --workspace --no-run`. "test run" = `cargo test --workspace` right
  after it: the cargo freshness check (0.1 s), the 15 test binaries and the doc-tests.
- Each edit was reverted with `git checkout` after its site. The tree is clean.

## 1. Baseline: the loop as CLAUDE.md asks it

Median of 5, seconds (peak RSS in MB).

| Edit site | clippy | test build | test run | **Loop** |
|---|---|---|---|---|
| `bimble-core`, comment only | 1.91 (411) | 8.50 (836) | 10.60 (205) | **21.0** |
| `bimble-core`, code | 1.93 (437) | 8.36 (836) | 10.68 (205) | **21.0** |
| `bimble-geom`, code | 1.58 (437) | 8.23 (836) | 10.77 (205) | **20.6** |
| `bimble-viewer`, code | 0.99 (423) | 6.33 (837) | 10.70 (205) | **18.0** |

- A comment-only edit costs the same as a code edit. Cargo rebuilds every crate that depends on the
  changed one; it does not stop early when the output is the same.
- An edit in the leaf crate rebuilds 8 crates and relinks all 15 test binaries, the `bimble` binary
  and the `hlod` example (`cargo test` builds examples too).
- The test run does not depend on the edit site: it always runs everything.

## 2. Where the time goes

### Test run: binaries in series

Each test binary, run alone from its package directory, median of 5 after one warm-up:

| Test binary | s | Slowest test in it | s |
|---|---|---|---|
| `bimble-ifc` `tests/budgets.rs` | 2.70 | `geometry_budgets_match` | 2.67 |
| `bimble-render` lib | 2.18 | `screen_edges_follow_the_edge_lines` (and 7 more GPU tests at 0.8–1.0 s) | 1.09 |
| `bimble-viewer` lib | 1.52 | `serve::tests::change_with_same_epoch_still_long_polls` | 1.50 |
| `bimble-ifc` `tests/review_regressions.rs` | 1.44 | `a_small_file_that_explodes_stops_at_the_expansion_guard` | 1.43 |
| `bimble-geom` lib | 1.43 | `hlod::tests::proxies_stay_within_their_error` | 1.42 |
| the other 10 binaries | 0.56 together | | |
| **Sum** | **9.93** | | |

Per-test times come from libtest's `--report-time` (needs `RUSTC_BOOTSTRAP=1` on stable; used for
this measurement only). Inside a binary the tests run in parallel, so each binary takes about as
long as its slowest test. `cargo test` runs the binaries in series, so the run is the **sum** of the
slowest tests: 9.9 s for 403 tests. The doc-tests add 0.74 s although the workspace has **no**
doc-tests: rustdoc starts once per library crate (8 crates).

The viewer test `change_with_same_epoch_still_long_polls` probably waits for a long-poll timeout
(1.5 s for one test). Not checked further.

### Test build: mostly linking

`cargo build --timings` of one edit in `bimble-core` (8.8 s wall): the longest units are the
binaries that link everything: `bimble` bin 5.7 s, `bimble_viewer` lib test 4.8 s, `bimble` bin
test 3.8 s, `hlod` example 3.5 s. Cargo's timings do not split codegen from linking, so the share
was measured with the linker alone (section 3a): changing only the linker removes 5.1 s of 8.4 s.
**About 60 % of the test build is the linker (GNU ld).** GNU ld is also the largest process
(836 MB).

### Clippy against check

| Edit site | `cargo check --workspace --all-targets` | `cargo clippy …` (baseline) | Clippy's own cost |
|---|---|---|---|
| `bimble-core` | 1.40 | 1.93 | +0.5 s |
| `bimble-viewer` | 0.70 | 0.99 | +0.3 s |

Clippy is not worth replacing: it is 10 % of the loop.

## 3. Levers, each against the same baseline

### a. mold for the test build

Native host target only, through `CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_RUSTFLAGS="-C
link-arg=-B<mold libexec dir>"`, the same flag that `scripts/twin-dev.sh` sets (ADR 0066). A
separate target directory, built once (cold, `cargo test --workspace --no-run`: mold 121 s, GNU ld
134 s). The binaries were checked: their `.comment` section names mold 3.0.0.

| Edit site | GNU ld | mold | Gain |
|---|---|---|---|
| `bimble-core`, test build | 8.36 s (836 MB) | **3.30 s** (492 MB) | −5.1 s, 2.5× |
| `bimble-viewer`, test build | 6.33 s (837 MB) | **1.81 s** (537 MB) | −4.5 s, 3.5× |
| `bimble-core`, test run | 10.68 s | 10.63 s | none (expected) |

Control: the same separate directory rebuilt with GNU ld gave 8.55 s for the `bimble-core` test
build, the same as the baseline (8.36 s). The gain is the linker, not the directory.

The two builds do not overwrite each other: with the flag, the units get other hashes, and both
sets stay in `target/` (seen: the GNU ld and the mold `bimble_viewer` test binaries side by side).
Switching the flag costs one cold build of the dependencies once, not on every switch. lld was not
measured here; ADR 0066 measured it at +0.2 s per edit against mold.

### b. cargo-nextest for the test run

`cargo install cargo-nextest --locked` took 85 s (under the 5 min limit). nextest runs each test
in its own process, all binaries in parallel. 403 tests, all pass.

| `bimble-core` edit, tests already built | `cargo test` (no doc-tests) | `cargo nextest run` | Gain |
|---|---|---|---|
| Run | 10.07 s | **4.11 s** | −6.0 s, 2.4× |

nextest does not run doc-tests. Today there are none; `cargo test --workspace --doc` costs 0.74 s
to find that out.

### a + b together

Same `bimble-core` edit, mold, then nextest (build and run in one command):

| Step | Baseline | mold + nextest |
|---|---|---|
| clippy | 1.93 | 1.87 |
| test build + run | 8.36 + 10.68 | **7.54** (one `cargo nextest run`) |
| doc-tests | (in the run) | 0.74 (`cargo test --workspace --doc`) |
| **Loop** | **21.0 s** | **10.2 s** |

### c. One package (`-p`) against the workspace

| Edit site | clippy `-p` | test build `-p` | test run `-p` | Loop `-p` | Loop `--workspace` |
|---|---|---|---|---|---|
| `bimble-core` | 0.57 | 1.44 | 0.36 | **2.4 s** | 21.0 s |
| `bimble-viewer` | 0.69 | 4.17 | 1.81 | **6.7 s** | 18.0 s |

`-p` is fast, but it does not test the crates that depend on the edited one. An edit in
`bimble-core` can break `bimble-ifc` or `bimble-viewer`, and `-p bimble-core` does not see it. It
is a good inner loop; the workspace run is still needed before claiming that a change works.

### d. check instead of clippy

See section 2: 0.3–0.5 s saved, against losing the lints that CI enforces. Not recommended.

### e. Linking against codegen

See section 2: about 60 % of the test build is linking (5.1 s of 8.4 s for a `bimble-core` edit),
measured by changing the linker only.

## Recommendations

1. **Link with mold for local test builds** (Linux): test build 8.4 s → 3.3 s for an edit in
   `bimble-core`, peak memory 836 MB → 492 MB. Today only `scripts/twin-dev.sh` sets it. A
   repository `.cargo/config.toml` cannot find mold on every machine, so the place to set it is the
   developer's environment (or a small `scripts/agent-check.sh` that sets the flag like
   `twin-dev.sh` and runs the loop). CI keeps the default linker.
2. **Run the tests with `cargo nextest run --workspace`** locally, plus `cargo test --workspace
   --doc` while doc-tests may exist: run 10.7 s → 4.1 s + 0.7 s.
3. Together: **21.0 s → 10.2 s** per edit, measured. With 10 loops an hour, about 2 minutes saved
   per hour of agent work, and a smaller peak memory on a 16 GB machine.
4. Use `-p <crate>` while iterating (2.4 s for `bimble-core`), and the workspace loop once, before
   claiming that the change works.
5. Not measured, worth a look: the five slow tests above set the floor of the parallel run
   (2.7 s for `geometry_budgets_match`). A 1.5 s wait in a viewer test is suspicious.

For the C port: the Rust loop to match is about 10 s with mold and nextest, of which clippy is
1.9 s, the incremental build 3.3 s and the tests 4.1 s. The tests, not the compiler, are the
largest part once the linker is fast.

## Suggested CLAUDE.md wording (not applied)

Replace the second bullet's test command with:

> `cargo fmt --all`, `cargo clippy --workspace --all-targets -- -D warnings` and the tests must
> pass before claiming something works. Locally, run the tests with `cargo nextest run --workspace`
> and `cargo test --workspace --doc`, and link with mold on Linux (see `docs/research/agent-loop.md`:
> 21 s → 10 s per edit). CI runs `cargo test --workspace`. While iterating, `-p <crate>` is enough;
> run the whole workspace once before claiming.

## Not measured

- lld (ADR 0066 has it), macOS and Windows linkers.
- A cold loop (after `cargo clean` or a toolchain update): 121–134 s for the test build alone.
- `rustup update stable` was not run, so that the warm target stayed valid; rustc 1.100.0 is the
  toolchain in use on this machine.
