# ADR 0067 — A C core for the lab's dev loop; Rust stays the product and the oracle

- Status: Proposed
- Decided by: to be decided by Pierre. Proposed by Claude on Pierre's request ("start working on
  the C core slab rewrite… tcc in dev, clang for release… build on our work of this night"),
  25 Nov 2026. Code by builder agents on branch `rewrite/c-core`; each number checked from its log.
- Reversal cost: low for the lab (one `?gl=1` branch and one transport in `bimble.ts`; delete
  `ccore/`). The cost grows with every feature ported to C.
- Extends: ADR 0065 (fast dev builds), ADR 0066 (headless native twin), ADR 0048 (Vue lab)

## Context / Problem

One edit, until the lab page shows it, measured on the dev machine:

- about 40 s with the release wasm (before ADR 0065);
- 4.66 s with the dev wasm and a page reload (ADR 0065);
- 1.66 s with the headless Rust twin and mold, no reload (ADR 0066, Proposed).

On 24 Nov the C slab spike reached edit → pixels in 118 ms in C and 209 ms in Rust at the same
scope (one slab, no wasm-bindgen). Fable's strategy note (`spikes/compile-speed/results/path-upward.md`
on `spike/compile-speed`) ranked a C rewrite last (rung 9, "Recommend: no"). It said the floor
comes from the size of what is rebuilt, not from the language, and estimated 300–600 h for all
of Bimble. Pierre chose to try a C core for the lab anyway. The question: can a small C core,
built with tcc, sit behind the unchanged Vue lab, match Rust byte for byte, and make the loop
much shorter?

## Decision (proposed)

1. A C core in `ccore/` serves the Vue lab in dev. It is the lab's native host (`?native=1`) and
   draws through a WebGL2 viewport (`?gl=1`). It may later back a small online viewer
   (the 220 KB wasm module). It is not the product.
2. Rust stays the product and the oracle. The desktop, drawings, PDF, IFC export, releases and
   the online viewer of record stay in Rust.
3. **The golden rule.** A feature enters the C core only after Rust dumps its golden.
   `crates/bimble-api/examples/ccore_golden.rs` writes `ccore/golden/`; C output must equal it
   byte for byte. No golden, no C code.
4. CI runs `cargo run -p bimble-api --example ccore_golden -- --check` and `ccore/test/run.sh`
   on every pull request that touches `crates/` or `ccore/`. A Rust change that changes a golden
   fails CI until the golden is regenerated and the C core follows, or the feature leaves C.
5. Dev builds use tcc (`ccore/build.sh host`, one tcc call). Release and wasm32 builds use clang.
6. The project rules hold in C: IFC names, Commands, canonical output, stable GlobalIds, files
   as untrusted input.

## What exists (branch `rewrite/c-core`, `wc -l`)

| Brick | Lines (`.c` + `.h`) |
|---|---|
| Base: arena, buffer, JSON writer and reader, f64 formatters, exact `strtod` | 85 + 104 + 203 + 405 + 156 + 996 |
| GlobalId (BLAKE3 `derived`) | 360 |
| Model, file load and save, material library | 598 + 793 + 109 |
| Commands, history, six verbs | 1,574 + 175 + 945 |
| Queries | 697 |
| Scene and meshes, earcut port | 1,234 + 655 |
| STEP parser, IFC import (Bimble's subset) | 815 + 1,707 |
| wasm32 API and allocator | 394 + 123 |
| **`ccore/src/` total** | **12,136** |
| Native host: HTTP, change feed, calls, session file, `/mesh` | 2,753 |
| Tests | 4,721 |
| Vendored files (Ryu, BLAKE3, their licences) | 3,974 |
| Rust golden dumper | 991 |
| Lab: `glviewport.ts` (new), `bimble.ts` (+262 −8) | 462 |

Suites (`ccore/test/run.sh`, all pass in the final orchestrator run): fmt 24,997 checks,
json 39, file 83, guid 3,575, mesh 121, query 155, verbs 61, ifc_in 28, strtod 1,247,467.
Node suites: host_test 72, lab_boot 5, lab_place 9, mesh_http_test 26, lab_gl 13,
session_test 22, dev_test 18, wasm_run 25,121 (the same goldens through the wasm module).

## Measurements

Method, machine and raw rows: `ccore/results/bench.md`. Apple M1 Pro (8 cores), Asahi Linux
aarch64, 15.2 GiB; tcc 0.9.28rc, clang 23.1.3, headless Chromium 153 with SwiftShader (CPU GL).
10 runs per loop after one warm-up. The lab is opened once and never reloaded. The edit changes a
C constant that changes every element's colour (pixels) and the status bar (text).

| Edit → visible in the lab page | Median | p95 | Source |
|---|---|---|---|
| Rust release wasm | ≈ 40 s | — | ADR 0065 |
| Rust dev wasm + reload, value | 4.66 s | — | ADR 0065 |
| Rust twin + mold, value | 1.66 s | — | ADR 0066 |
| C core, `build.sh host` (tcc), value | 106 ms | 121 ms | bench.md |
| C core, `build.sh host`, text | 166 ms | 214 ms | bench.md |
| **C core, `build.sh host`, pixels** | **233 ms** | **277 ms** | bench.md |
| C core, `build.sh tcc` (all), pixels | 414 ms | 453 ms | bench.md |
| C core, clang -O2, pixels | 5.53 s | 5.65 s | bench.md |
| C core before the reconnect fix, text / pixels | 1.33 s / 1.40 s | | bench.md |

Other measured values: the tcc host build 73 ms median; host start with session restore
10 ms; wasm module 220,183 bytes (102,473 gzip); 1,000 open/mesh/verb/undo/close rounds in wasm do
not grow memory.

What the numbers do **not** show:

- The Rust twin with the same reconnect fix: not measured. It uses the same `pollChanges`, so its
  text and pixel times likely gain too.
- A thin Rust twin (path-upward rung 4, estimated 0.6–1.2 s): not built, not measured.
- Equal scope. The C core is about 12k lines and does a fraction of what Rust does; Rust
  rebuilds incrementally, `build.sh` rebuilds all.
- Large models: only the 72-element frame was timed. A real GPU, macOS and Windows: not measured.
- The agent loop (clippy and tests): not measured.
- The cost of keeping two cores in step: not measured.

## Consequences

- In the lab, an edit to C shows in pixels in 0.23 s; the Rust twin shows a value in 1.66 s.
  Different scopes (above).
- Two cores can drift. The goldens catch drift only where a golden exists. A Rust change with
  no golden is silent in C.
- Every feature costs twice: Rust, then a golden, then C. Bugs fixed in Rust must be fixed in C.
- The C core lacks: drawings and PDF (the product), openings and booleans, IFC export (ticket
  T10 not built; `export_ifc4` is off), sheets, schedules and views, collaboration, map
  projections, and eight of the 14 verbs. A file with openings, views, sheets, schedules or
  georef is refused, not silently cut.
- CI must add: tcc and clang on the runner, `ccore/test/run.sh` (tcc, and `CC=clang`), the golden
  `--check`, `test/host.sh` and the Chromium suites, and `build.sh wasm` with `wasm_run.mjs`.
  None of this runs in CI today.
- `bimble.ts` gains a reconnect backoff (20 ms, doubling to 1 s). It helps the Rust twin too.

## Alternatives

- **ADR 0066 alone**, the Rust twin, then the thin-twin split of `bimble-viewer`
  (`path-upward.md` rungs 1, 2 and 4). One core, no drift. Measured 1.66 s; the split is
  estimated at 0.6–1.2 s. This was Fable's recommendation.
- **Nothing beyond ADR 0065.** The dev wasm loop, 4.66 s, with a reload. No new code.

## Not decided here

- Replacing the Rust core with C: no. Rust stays the product and the oracle.
- An online viewer on the 220 KB wasm module: a separate experiment, with its own ADR.
- Merging the `bimble.ts` reconnect fix and the `?gl=1` viewport to `main`: separate pull
  requests, each on its own merit.
- Which features may enter C after the lab surface. Each needs a golden first (rule 3).
