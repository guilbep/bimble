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

<!-- status:begin -->
<!-- Written by `node ccore/tools/status.mjs --write` from ccore/results/. Do not edit between the markers: `ci/run.sh unit` checks them. -->

## Status (version `0.1.0-preview.8`)

A preview. Each number is copied from a generated report under `ccore/results/`; the sources and
the conditions of each number are listed under the list.

- **IFC reading** [1]: 617 public STEP files (IFC2X3 283, IFC4 83, IFC4X3 249, other 2;
  1.24 GB) read. "Read" means not refused, no crash, no timeout, and no report under ASan and
  UBSan; it does not mean every product is drawn: 125,832 of 128,591 products with a body
  get a mesh (97.9 %). Not counted: 38 non-STEP files (ifcZIP, ifcXML: refused, not
  read yet) and 173 git-lfs pointer files that were not fetched.
- **IFC against Rust** [1]: before the C reader learnt new types, C and Rust gave the same scene
  and meshes on 616 of 617 files. Since then the C reader draws types Rust does not, on purpose:
  26 STEP files changed with IFC2 (591 kept their scene byte for byte), 17 more files with IFC3,
  each holding a newly supported type. The Rust side was a release-mode build of the golden
  dumper from the development sources of 25 Nov (13:14), not the v0.19.0 release binary.
- **IFC against IfcOpenShell 0.9.0** [1]: on a 40-file sample, C gives a non-empty mesh to
  15,416 products where IfcOpenShell triangulates 15,679; the same count on 36 of the 40 files.
- **Against the Rust app** [2]: the C core serves 54 of the 54 Rust action names (the name
  only, not what the action does); 44 of them have the same `enabled` and `pressed` flags in all
  13 scenarios (synthetic fixtures, mainly a 72-element frame). 21 of the 29 Rust queries
  answer with the same JSON key names and types (not checked: the values). 16 of the 16 Rust
  shortcuts have a row in the lab's key table; 1 of them cannot run in a browser tab
  (Ctrl N: the browser keeps it). The Rust answers were dumped from a development build that
  reports `bimble 0.18.1`, not from v0.19.0.
  6 controls still shown are not served by the C core; the report lists them.
- **Size** [3]: 89,938 lines of C; the viewer wasm 643,169 bytes (300,063 gzip, 64.3 % of
  its 1,000,000-byte limit), the editor wasm 1,406,131 bytes (576,447 gzip), the native program
  3,608,016 bytes (stripped), the Linux aarch64 release archive 5,892,227 bytes.
  That archive was built on the dev machine with a stale `web/ccore/` copy inside (2,098,769 bytes
  before compression) that the CI archive does not have.
- **Speed** [4]: Lighthouse performance (desktop / mobile, median of 3 runs): 80 / 75 for the
  current build served locally on the dev machine under load (load average 17.89 at the start);
  100 / 94 for the page then deployed on GitHub Pages, over the network, an older and smaller
  build (wasm 637,715 bytes against 945,885). Both measured on 2026-11-25.
- **Edit loop** [5]: a C edit written → new pixels in the lab in 0.23 s (median of 10) on the
  72-element frame, host built by tcc; 2.1 s on a 20,000-element model. Conditions below.
- **Checks** [6]: `ccore/ci/run.sh full`, 57 suites, 0 failed, 836 s on the dev machine
  (one run).

Sources and conditions:

1. `ccore/results/ifc_corpus.md` (`ccore/bench/corpus.sh`): the public corpora at pinned commits;
   the sanitizer run covered 655 files (617 STEP, 38 not STEP). "Drawn" counts products
   with a non-empty mesh; the IfcOpenShell count is products its iterator triangulates to at
   least one face.
2. `ccore/results/parity.md` (`ccore/tools/parity.mjs`): its header says what it supports and what it
   does not.
3. `ccore/results/weight.md` (`ccore/tools/weight.mjs`): the wasm files built by `build.sh wasm` and
   `wasm-view`, the native program and the archive by `build.sh release` on the dev machine
   (aarch64 Linux).
4. `ccore/results/perf.md` (`ccore/tools/perf.mjs`, mode full, commit be45506): headless
   Chrome, Lighthouse's simulated throttling; the local build is served by a static server that
   answers as GitHub Pages.
5. `ccore/results/bench.md` (`ccore/bench/lab_loop.sh`): 72 elements (73 after the placed column),
   `build.sh host` (tcc, 73 ms of the total), headless Chromium drawing with SwiftShader (CPU),
   timed from the edit written to the first new canvas hash; `dev.sh`'s file watcher delay is not
   in it.
   `ccore/results/large.md` (`ccore/bench/large.mjs`): the same loop, median of 5: 0.39 s at 2,000,
   2.1 s at 20,000 and 6.0 s at 60,000 elements (synthetic models).
6. `ccore/results/ci.md`: the run after ticket WIN2, 16 GB aarch64 dev machine; CI runs the same
   script in parallel jobs (`.github/workflows/ccore.yml`).
<!-- status:end -->

Not yet: the boolean kernel in wasm (the browser build cuts box and convex prism openings itself
and draws the others uncut), Windows (its CI job and release zip are new and still allowed to
fail), collaboration, rebar, steel. See [ROADMAP.md](ROADMAP.md).

## Try it

- **In a browser:** <https://guilbep.github.io/bimble/> (the C lab, built from `main`). Files
  you open stay on your computer. A link opens a model with nothing to install:
  <https://guilbep.github.io/bimble/?open=samples/frame.ifc> (the demo frame, synthetic); any
  `https://` file whose server allows other sites (CORS) opens with `#open=<url>`, and the page asks
  before it fetches from another site (ADR 0086).
- **Desktop (Linux x86_64 and aarch64, macOS arm64 and x86_64):** the archives of the latest
  `ccore-v*` [prerelease](https://github.com/guilbep/bimble/releases). Unpack, then
  `./bimble-ccore open model.ifc`: the lab opens in your browser.
- **Native Linux app (preview, ADR 0084):** the Linux archive also holds `bimble`, the same core
  with a native 3D view and the lab's ribbon in one window: `./bimble project.bimble` (no file: the
  Home screen). It needs SDL 3.4 or newer and a Vulkan driver; for large linked projects. Build it
  from source with `make native` (SDL3 and FreeType installed).
- **Native macOS app (preview, ADR 0088):** `bimble-<version>-macos-arm64.dmg` (Apple Silicon) or
  `-macos-x86_64.dmg` (Intel; checked on CI only) on the same prerelease; drag `bimble.app` to Applications. It is
  signed ad hoc, not notarised: the first time, on macOS 15 and newer, open it, then System Settings → Privacy &
  Security → **Open Anyway** ([Apple](https://support.apple.com/en-us/102445)); on macOS 11–14, right-click → Open;
  or once in Terminal `xattr -dr com.apple.quarantine /Applications/bimble.app`. macOS 11 or newer.
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
make native                    # the native Linux app, ccore/out/app/bimble (DEV.md section 2b)
```

Rules for contributors and agents: [CLAUDE.md](CLAUDE.md). Decisions: [docs/adr](docs/adr). Work
log: [docs/journal](docs/journal). The C core's internals: [ccore/README.md](ccore/README.md).
Releases: [docs/RELEASING.md](docs/RELEASING.md).

## Where the Rust version went

Bimble was written in Rust until 25 Nov 2026. The last Rust release, **v0.19.0**, is the frozen
reference the C core is checked against (ADR 0067): its binaries stay on the
[releases page](https://github.com/guilbep/bimble/releases/tag/v0.19.0), `brew install bimble`
installs it, and its lab is under <https://guilbep.github.io/bimble/rust/>. The Rust
sources are on the tag `v0.19.0` and on the branch `oracle/rust` (with the dumpers that wrote
`ccore/golden/`), not on `main` (ADR 0072).

## Licence

MIT, see [LICENSE](LICENSE).
