# ADR 0066 — A headless native twin for the dev loop; mold for native dev builds

- Status: Proposed
- Decided by: to be decided by Pierre. Measurements and draft by Claude (Fable 5.2 drafted, the
  orchestrator checked each number), 24 Nov 2026, night run.
- Reversal cost: low (one transport in `bimble.ts`, three files in `bimble-viewer`, one linker setting)
- Extends: ADR 0065 (fast dev builds), ADR 0048 (Vue lab), ADR 0020 (web version)

## Context / Problem

After ADR 0065, one edit costs 3.3–4.4 s until the wasm files are on disk. The page reload and the
state restore come on top and are not measured. wasm-bindgen runs on every edit and peaks at
1.27 GB. Three experiments tested a native process behind the Vue lab (branches
`spike/native-twin`, `spike/native-transport`, `spike/frame-stream`; journal 24 Nov, "Night run").

## Decision

1. In dev, the Vue lab can talk to a native Bimble process. `bimble.ts` has a second transport
   (`?native=1&port=&token=`): synchronous XHR to 127.0.0.1, and a long-poll for changes. The
   wasm path is unchanged and stays the default.
2. The twin is **headless**: no window, a plain loop, a `Viewer` on a headless wgpu device. A
   windowed twin is not supported: it does not answer when nothing is drawn (measured: every call
   timed out while the panel was off).
3. `calls.rs` holds every bridge call, with no target `cfg`. The wasm bridge and the twin both
   call it.
4. `serve.rs` listens on 127.0.0.1 only (std, no new crate). It checks a token, `Origin` and
   `Host`, and bounds every read. It is a dev tool: it is not part of a release.
5. Native dev builds on Linux link with mold, or with lld when mold is absent. Release builds do
   not change.
6. Wasm-only: the online viewer, Pages, releases, the performance budgets and the e2e suite in
   CI. CI builds and tests the wasm on every pull request.
7. The dependencies stay statically linked (no dev dylib).

## Measurements (24 Nov, aarch64 Asahi, Apple M1 Pro GPU, 8 cores, 16 GB)

| | Value |
|---|---|
| Native edit loop, GNU ld | 5.6–6.8 s |
| Native edit loop, lld | 1.5–2.7 s |
| Native edit loop, mold | 1.3–2.4 s, 0.4 GB peak |
| Dependencies as one dylib | 0.1–0.2 s less; no mimalloc; opens 9 % slower |
| Twin call, p95 (1000 calls) | 1.6–2.0 ms |
| Edit → new value over HTTP (twin) | 1.72 s, 549 MB peak |
| Edit → wasm files ready (`wasm:dev`) | 3.75 s, 1267 MB peak; reload not included |
| **Edit → new value in the lab page, wasm** (build + reload) | **4.66 s** (3.67 + 0.98) |
| **Edit → new value in the lab page, twin** (build + restart, no reload) | **1.66 s** (1.57 + 0.09): 2.8× shorter |
| The lab in Chromium against the twin (`e2e:native-lab`) | ready in 0.63–0.75 s; selection and properties shown |
| Raw 1080p frame, request → pixels | p50 19–22 ms (32–33 ms at CPU ×4), p95 38–50 ms |
| PNG frame | 45–50 ms |

## Consequences

- The loop to the value in the lab page is 2.8× shorter (4.66 s → 1.66 s), and the page does not
  reload. The build uses less than half the memory.
- The twin has no 3D or paper viewport. Work on the viewport uses the wasm path until frames are
  streamed.
- Each rebuild restarts the twin. The model is opened again: about 1.0 s for 13,000 elements.
- Wasm bugs appear in CI, not on the developer's machine. `bimble view` was broken for four days
  on main and nobody saw it (fix: `fix/viewer-main-thread`): an unused path breaks in silence.
- mold is a new dev dependency on Linux.

## Not decided here

- The viewport in the page (raw frames streamed from the twin; branch `spike/native-viewport`).
  Measured with drags (pointer → changed pixels, 50 drags, headless Chromium, software GL):
  full resolution p95 57 ms (72 elements) and 86 ms (20,715); half resolution while dragging
  p95 41–50 ms at 43–52 fps (72) and 55–56 ms at 29–30 fps (20,715). Workable at the edge for
  small models, short for large ones; the transfer, not the render, is most of the time.
- Session resume after a restart.
- The linker on macOS and Windows: not measured.
- `bimble serve` as a product (files over 4 GB, an agent and a person on one model).
- A small wasm module without wasm-bindgen. The same slab slice reached edit → pixels in 118 ms
  in C and 209 ms in Rust (no wasm-bindgen; Rust library to wasm 73 ms against clang 60 ms). The
  language is within 1.2–2× at equal scope: module size and wasm-bindgen are the levers, not C.
