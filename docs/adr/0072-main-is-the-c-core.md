# ADR 0072 — `main` is the C core; the Rust sources live on tag `v0.19.0` and branch `oracle/rust`

- Status: Accepted, 25 Nov 2026
- Decided by: Pierre, 25 Nov 2026: "Goal for tomorrow morning: not having anymore rust in the
  'main' branch and having a super nice 'dev' setup so we can debug/iterate new feature super
  fast". It carries out ADR 0067 ("go full in the C port … do a final RUST release before we move
  completely to C"): the final Rust release is v0.19.0. Prepared by Claude (ticket MAIN1); the
  merge is the orchestrator's after its own checks.
- Extends: ADR 0067 (the frozen oracle), ADR 0069 (port order, three builds from one C codebase).
- Reversal cost: low. The removal is one commit (182 files deleted, two folders moved); reverting
  it brings every Rust file back and keeps the C core. Nothing was rewritten: the history keeps
  every Rust commit.

## Context

On 25 Nov the C core (`ccore/`) reached the lab's needs on the branch `rewrite/c-core`: the verbs,
IFC in and out, the 2D views, sheets and PDF, the IFC display reader (617 public files read, C ==
Rust on 616: `ccore/results/ifc_corpus.md`), the lab on a native host and in wasm, release archives
for Linux and macOS (`ccore-v0.1.0-preview.2`). Rust v0.19.0 shipped the same day as the last Rust
release. Two cores in one tree cost twice: the Rust CI (`ci.yml`, `perf.yml`, `release.yml`), the
`cargo` commands in every guide, the Rust paths in the C tests, and an editor that indexes both.

## Decision

1. **`main` holds no Rust.** Deleted: `crates/`, `Cargo.toml`, `Cargo.lock`, the Rust web shell
   `web/`, the C slab spike `spikes/` (the port replaced it), the Rust-only scripts, the Rust CI
   (`ci.yml`, `perf.yml`, `release.yml`), the Rust paths of `web-ui` (`npm run wasm`, `wasm:dev`,
   `twin:dev`, the twin's e2e scripts, the Rust Lighthouse budgets). `Formula/bimble.rb` stays: it
   installs the v0.19.0 release.
2. **The data the C tests read moves out of `crates/`.** The six synthetic IFC fixtures go to
   `ccore/test/fixtures/` and the Roboto files (the optional PDF font, ADR 0071) to `ccore/fonts/`;
   the tests and `host/fonts.c` (the dev build's font folder) read them there.
3. **The oracle is frozen and committed.** The C suites check the references under `ccore/golden/`
   without Rust. The Rust sources with the golden dumpers (`crates/bimble-api/examples/
   ccore_golden.rs`, `ccore/tools/parity.mjs --rust`, `ccore/tools/mcp_golden.sh`) stay reachable
   on the branch **`oracle/rust`**, made from the C work branch's head before this removal, and the
   release on the tag **`v0.19.0`**. A reference that must change is regenerated there and copied
   back in a commit that says why (ADR 0067 rule 3). On `main` the Rust modes of the tools stop
   with a message that names the branch; they never fail the gate.
4. **CI on `main` is `ccore.yml`**: every push to `main` and every pull request to `main`, no path
   filter. Its jobs `ccore-linux`, `ccore-san`, `ccore-lab` and `ccore-macos` are the checks to
   require; `ccore-windows` stays allowed to fail until it is green.
5. **GitHub Pages**: the site root stays the Rust v0.19.0 lab, built from that release's assets
   and that tag's `web-ui` (nothing compiled); `/c/` is the C lab built from `main`, on every push
   to `main` that changes it.
6. **The developer setup is `DEV.md`**: the edit loop (`make dev`), one suite, the gate, the
   sanitizers, how to add a verb, query, action, capability, page, C file and lab test, debugging,
   the tools, the usual failures; `ccore/compile_flags.txt` gives editors the include paths.

## Consequences

- `npm run dev` is the C loop (`ccore/dev.sh`); `npm run build` is the lab for the native host
  (what the release archive serves), `npm run build:ccore` the lab on the C wasm (GitHub Pages).
- `web-ui/src` still holds the Rust wasm client paths (the `bimble_web.js` branch of `start()`,
  the `/frame` stream viewport, the "Rust viewer: everything" capability default). They are dead on
  `main` and are left for a follow-up ticket: this change touches no file under `web-ui/src`.
- Comments and documents that cite a Rust file and line (`ribbon.rs:612`, `crates/…`) stay: they
  are the port's provenance. They point into `oracle/rust`.
- `ccore/tools/parity.mjs` reads the Rust names from the committed `golden/parity/names.json`
  when `crates/` is absent; `--rust` needs `oracle/rust`.
- The Rust release workflow is gone from `main`: a Rust release can no longer be made from `main`.
  A fix to v0.19.0, if ever needed, branches from the tag.

## Correction (26 Nov 2026, ticket FIXDOC)

The context above says "617 public files read, C == Rust on 616". Two parts of that were
overstated, and the decision does not depend on them:

- "Read" meant not refused, not crashed, no sanitizer report; 173 git-lfs pointer files and 38
  ifcZIP and ifcXML files were not read, and 97.7 % (now 97.9 %) of the products with a body were
  drawn, not all.
- "C == Rust on 616" held for the C reader of ticket IFC1. Tickets IFC2 and IFC3 then made the C
  reader draw types Rust does not, on purpose (26 and then 17 files changed:
  `ccore/results/ifc_corpus.md`, `ccore/golden/DEVIATIONS.md` section 10). The Rust side of that
  comparison was a release-mode build of the golden dumper from the development sources of
  25 Nov, not the v0.19.0 release binary.

The current numbers are in the README's Status block, written from the reports by
`ccore/tools/status.mjs`.

## How to revert

`git revert <the removal commit>` ("Remove Rust from the tree: …", ticket MAIN1) on `main`: Rust,
its CI and its scripts come back beside the C core. Reverting the merge of the whole C work branch
(`git revert -m 1 <merge>`) would also take the C core out of `main`. To work on Rust without
reverting: `git worktree add ../bimble-rust oracle/rust`.
