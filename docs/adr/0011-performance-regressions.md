# ADR 0011 — Performance regressions: deterministic geometry budgets gate, timing trend warns

- Status: Accepted (5 Nov 2026)
- Decided by: Pierre asked for performance regression tests; design and evidence by Claude
- Reversal cost: low (one test, one CI job, one script)

## Problem
Speed on old machines is the product's main promise (ROADMAP, ADR 0001). A change that doubles the
triangles of every circle, or makes loading quadratic, must not land unnoticed. But CI runs on shared
cloud runners with a software GPU (lavapipe): wall-clock times move by tens of percent between runs
and runner generations, so a hard time limit would either be loose enough to miss real regressions
or fail randomly.

## Decision
Two layers.

1. **Geometry budgets — fail the build.** `crates/bimble-ifc/tests/budgets.rs` builds fixed models
   (generated frames of 98 to 51 301 elements, and the synthetic IFC fixtures including `curves.ifc`:
   circle profile, filleted I-section, bent bar) and compares element, triangle and edge counts and
   the GPU bytes they imply with `tests/budgets.json`. These numbers are exact and are what drives
   frame time and memory on weak GPUs. An intended change regenerates the file
   (`BIMBLE_UPDATE_BUDGETS=1 cargo test -p bimble-ifc --test budgets`) and commits it, so the cost
   shows in the PR diff and a reviewer accepts it explicitly.
2. **Timing trend — warn.** The `perf` CI job runs `bimble bench --json` three times. Pushes to main
   upload the results as the `bench-main` artifact (90 days). Pull requests download the latest main
   baseline and run `scripts/bench_compare.py`, which takes the best of the runs on each side and
   emits a GitHub warning and a job-summary table for load, upload, frame median/p95 and memory
   that are both more than 25 % slower and above a small absolute slack (2–20 ms or MB).

## Alternatives considered
- **Hard time limits in tests:** flaky on shared runners; rejected.
- **criterion micro-benchmarks:** good for local tuning of one function, but no history in CI without
  extra infrastructure and the same noise problem. Can be added for hot loops later.
- **Instruction counts (iai/cachegrind, Valgrind):** deterministic CPU cost, but Linux-only, slow, and
  blind to the GPU. A good next step if the trend proves too noisy for the reader and kernel.
- **Dedicated benchmark machine (e.g. an old office laptop as a self-hosted runner):** the real target
  hardware and quiet. Best long-term; needs a machine and someone to keep it up.

## Evidence
- A full bench run on lavapipe takes 8.5 s (four frames of 1 147 to 51 301 elements), so three runs
  per PR are cheap.
- The budget test was checked to fail on old/new code: making `MAX_STEP` in the arc tessellation
  π/16 instead of π/12 changes `curves.ifc` from 872 to 1 064 triangles and the test fails; before
  `curves.ifc` was added the same change passed unnoticed, because no fixture went through that path.
- `bench_compare.py` was checked with synthetic results: +50 % frame time and load time are flagged;
  doubling a 3 ms upload is not (below slack); a slow run is ignored when another run is clean.
- `curves.ifc` agrees with IfcOpenShell (`compare_ifcopenshell.py`: worst volume 0.32 %, bbox 0.013 mm).

## Revisit when
- Warnings fire often without a cause (noise): switch the trend to instruction counts or a
  self-hosted runner.
- The old-laptop benchmark (Pierre) exists: record it as a reference and consider a runner on it.
- Real-file timings matter in CI: add a large synthetic IFC (never client files, ADR 0008).
