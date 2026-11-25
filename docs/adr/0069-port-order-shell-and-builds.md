# ADR 0069 — Port order, desktop shell and builds of the C core; supersedes ADR 0001's Rust core

- Status: Accepted, 25 Nov 2026
- Decided by: Pierre, 25 Nov 2026: "IFC export import first, edit/creation second, then whatever
  is the fastest"; "browser + local C server first so I can test and iterate super fast";
  "The app: one web UI, which is the app everywhere, on one C core"; "manifold is important for
  the 3D printing"; the PDF writer and paper later. Study: `docs/research/future-architecture.md`.
- Supersedes: ADR 0001 in its choice of a Rust core and an own wgpu renderer. ADR 0001's
  IFC-typed native format stays.
- Reversal cost: high once Rust is retired (milestone M4).

## Decision

1. **One web UI is the app everywhere**, on one C core. No native GUI; egui and wgpu are not
   ported.
2. **Desktop shell order**: first the user's browser plus a local C server (`bimble file.ifc`
   opens 127.0.0.1); then a thin window on the system webview on Windows and macOS; Electron
   only as a measured fallback.
3. **Three builds from one codebase**, selected by files, not by lines: a viewer wasm (budget
   400 KB), an editor wasm, a native `bimble` (threads, files, history, CLI, MCP). Plus the tcc
   dev host.
4. **Port order**: (1) IFC import and export, `.bimble` new/edit/save, export to the viewer;
   (2) editing and creation, 3D and the UI, the section box and cuts; (3) the dynamic orthogonal
   2D view with dimensions; then whatever is fastest. PDF, DXF and paper later.
5. **Keep manifold** (C++) through its C binding for openings, booleans and 3D printing. Do not
   port it.
6. **Performance work waits** unless it blocks iteration (Pierre: a working prototype first,
   "not spending token on early optimization"). The WebGL2 old-laptop gate is measured on real
   hardware once the viewer is deployed.

## Consequences

- `crates/` is deleted at M4, after desktop parity (`future-architecture.md` section 5).
- The final Rust release (ADR 0067) is still due before the port's first C-only feature.
