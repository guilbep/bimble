<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="packaging/logo-dark.svg">
    <img src="packaging/logo.svg" alt="Bimble" width="220">
  </picture>
</p>

# bimble

Open-source structural BIM (Building Information Modeling), built to stay fast on old machines.
IFC is the data model; `.bimble` is our own format, a superset of IFC with history and branches.
Concrete first: model → IFC → formwork and rebar drawings.

Bimble is a small C core (`ccore/`) behind a Vue lab (`web-ui/`). The core reads and writes the
model, IFC, drawings and PDF; the lab is the user interface, in the browser. The same C code runs
as a local server on the desktop (your browser is the window), as a wasm module in a web page,
and as an MCP server for agents (ADR 0067–0069, ADR 0072).

## Status (25 Nov 2026, version `0.1.0-preview.2`)

A preview. The numbers come from the generated reports in `ccore/results/`:

- **IFC reading:** 617 public STEP files (IFC2X3, IFC4, IFC4X3; 1.24 GB): all read, no report
  under ASan and UBSan, the same scene and meshes as the last Rust release on 616 of them; 97.7 %
  of the products with a body are drawn. Against IfcOpenShell 0.9.0 on 40 files: 15,409 products
  with geometry for its 15,679 (`ifc_corpus.md`, with the short to-do list for full IFC).
- **Editing:** the 14 verbs of the Rust release plus C additions (rename, levels, grids,
  dimensions, texts), undo and redo, `.bimble` text and binary, IFC4 and IFC2x3 export, plans,
  sheets, PDF and SVG, quantities and schedules, linked models, PDF and DXF reference plans.
- **Against the Rust app** (`parity.md`): 54 of 54 actions present, 40 with the same state in
  every scenario; 18 of 29 queries the same shape; 16 of 16 shortcuts handled. What is still
  missing or a stub is listed there, and the lab labels it "not available yet".
- **Size** (`weight.md`): 52,910 lines of C; the viewer wasm 407,538 bytes (192,859 gzip), the
  editor wasm 953,165 bytes, the native program 3.1 MB, the Linux release archive 2.4 MB.
- **Speed** (`perf.md`, `bench.md`): the C lab on GitHub Pages scores 100 / 94 (Lighthouse
  performance, desktop / mobile); edit a C file → new pixels in the lab in 0.23 s (median, dev
  loop).

Not yet: the boolean kernel in wasm (openings are drawn uncut in the browser build), Windows
(a CI probe), collaboration, rebar, steel. See [ROADMAP.md](ROADMAP.md).

## Try it

- **In a browser:** <https://guilbep.github.io/bimble/c/> (the C lab, built from `main`). Files
  you open stay on your computer.
- **Desktop (Linux x86_64 and aarch64, macOS arm64 and x86_64):** the archives of the latest
  `ccore-v*` [prerelease](https://github.com/guilbep/bimble/releases). Unpack, then
  `./bimble-ccore open model.ifc`: the lab opens in your browser.
- **Homebrew:** `brew tap guilbep/bimble https://github.com/guilbep/bimble && brew install
  bimble-ccore`.
- **For agents:** `bimble-ccore mcp model.bimble` is an MCP server over stdio (`ccore/README.md`).

## Develop

[DEV.md](DEV.md) is the guide: prerequisites, `make dev` (edit C, see pixels), one suite, the
gate, sanitizers, debugging, how to add a verb, query, action, page or test. In short, on Linux
with clang, lld, tcc, node and Chromium:

```bash
ccore/deps.sh                  # once: the boolean kernel (manifold)
(cd web-ui && npm ci)          # once
make dev                       # the lab on http://localhost:5173, rebuilt on every save
ccore/test/run.sh json         # one suite
ccore/ci/run.sh fast           # the gate of every change (CI runs `full`)
```

Rules for contributors and agents: [CLAUDE.md](CLAUDE.md). Decisions: [docs/adr](docs/adr). Work
log: [docs/journal](docs/journal). The C core's internals: [ccore/README.md](ccore/README.md).
Releases: [docs/RELEASING.md](docs/RELEASING.md).

## Where the Rust version went

Bimble was written in Rust until 25 Nov 2026. The last Rust release, **v0.19.0**, is the frozen
reference the C core is checked against (ADR 0067): its binaries stay on the
[releases page](https://github.com/guilbep/bimble/releases/tag/v0.19.0), `brew install bimble`
installs it, and its lab is still the root of <https://guilbep.github.io/bimble/>. The Rust
sources are on the tag `v0.19.0` and on the branch `oracle/rust` (with the dumpers that wrote
`ccore/golden/`), not on `main` (ADR 0072).

## Licence

MIT, see [LICENSE](LICENSE).
