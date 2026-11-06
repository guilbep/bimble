# ADR 0020 — Bimble in a browser: the same Rust, compiled to WebAssembly

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre ("next release: make it work on the web, using WebAssembly"); approach and
  measurements by Claude
- Reversal cost: low (one crate, a page, a few `cfg`s; the desktop build is unchanged)

## Problem
Engineers and architects should be able to open a model without installing anything, and the
files must stay theirs: nothing uploaded.

## Decision
- **One code base.** All crates build for `wasm32-unknown-unknown`; `bimble-web` starts the viewer
  (`eframe::WebRunner`) in `web/index.html`. `scripts/build-web.sh` builds `web/dist/` (static
  files: any web server, GitHub Pages). Files are read in the page; nothing leaves the computer.
- **GPU:** WebGPU where the browser has an adapter, WebGL2 otherwise. Chrome on many Linux machines
  exposes `navigator.gpu` with no adapter behind it; wgpu then picks WebGPU and draws nothing, so
  the page asks for an adapter first and falls back to WebGL2 itself.
- **Booleans:** the manifold kernel is C++ and doesn't build for this target. The page loads
  manifold's own WebAssembly build (npm `manifold-3d`, pinned, Apache-2.0, 0.5 MB) and exposes it
  as `bimbleBoolean`, which `bimble-geom` calls synchronously. Positions go as f32 relative to the
  operation's centre (1 µm or better up to ±8 m). Without it the page still runs, elements uncut
  and reported.
- **Platform differences:**
  - files are picked or dropped and read from their bytes (`files.rs`);
  - saving and exports are downloads;
  - the model history (ADR 0017) stays desktop-only for now (it is a folder of files);
  - `std::time::Instant` → `web-time`;
  - no background threads.
  - Parallel loops run on one thread: rayon falls back to the current thread when it can't spawn.
- **Fonts:** rasterized plan previews fall back to the font egui ships when the system has none.

## Evidence (headless Chrome, SwiftShader WebGL2 on a 2-core container; 1 run each)
| Real file | Elements | Desktop load (3 runs) | Browser load |
|---|---|---|---|
| #1, 2.5 MB | 776 | 30–39 ms | 240 ms |
| 7 MB | 2 735 | 118–136 ms | 419 ms |
| 21 MB | 4 062 | 398–410 ms | 837 ms |
- Booleans fixture: 208 triangles in the browser and on the desktop (the door opening is cut).
  Real file #1 reports the same 3 uncut walls in both; triangle counts differ by under 2 % (f32
  input to the kernel tessellates a little differently). Not yet checked: volumes element by element.
- Download: `bimble_web_bg.wasm` 14 MB, 4.3 MB gzipped (no wasm-opt yet), plus manifold 0.5 MB.
- Demo frame, plan preview and PDF export (A3, as on the desktop) work in the page.

## Publishing
Each release attaches `bimble-vX-web.zip`; `pages.yml` then publishes it on GitHub Pages. It runs
after the release workflow (`workflow_run`), from the default branch: a workflow started by a tag
is refused by the `github-pages` environment, whose default rule allows the default branch only
(found on v0.5.0). No `gh-pages` branch, no setting to change. A release can be published again by
hand: Actions → pages → Run workflow, with its tag.

## Alternatives considered
- **A JavaScript viewer (three.js, web-ifc)**: a second code base, and the plans, styles and
  editing would have to be written twice.
- **Emscripten for everything** (to link manifold directly): eframe and wgpu target
  `wasm32-unknown-unknown`; mixing is fragile.
- **A server that renders**: files would leave the user's computer.

## Revisit when
- Models of 50k+ elements: browser memory is capped at 4 GB per page; measure, then consider
  threads (needs cross-origin isolation headers and a nightly build) or streaming.
- History in the browser (Origin Private File System) and opening a folder (File System Access).
- Download size: wasm-opt, splitting the IFC reader.
