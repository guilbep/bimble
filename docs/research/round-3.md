# C core: round 3 of tickets

- Date: 25 Nov 2026. Author: Fable 5.2 (PLAN station). Branch: `rewrite/c-core` at `942f316`.
- Follows `docs/research/next-round.md` (round 2). The ticket format, the **"Rules for every
  ticket"** (rules 1–10) and the hotspot rule (one owner per wave, others add one hook of at most
  10 lines in their own file) are the same: read them there. They are not repeated here.
- **The gate** for every ticket is `ccore/ci/run.sh fast` (exit 0, last line `ci: N suites, 0
  failed`; 518 s on the dev machine, `ccore/results/ci.md`). A done check below is the gate
  **and** the ticket's own command.
- Done, not planned again: round 2 wave 1 (K1, U1, X1, Q1, M1), G1, W1, C4. Running: VC1 (view
  cube, page side), GEO1 (georeference). Queued from round 2, still valid as written: S1, X2, G2,
  Q2. T1 is refreshed below.

## Pierre's order (25 Nov) and what this round holds

Georeference, view cube (both running), measure, sheets, drawings, PDF links, DXF links, ports to
Windows and macOS, history and collaboration, the rest. And a first **preview release**: the web
lab on GitHub Pages at its own path (the Rust viewer keeps the site root), a Linux binary, then
macOS with brew, then Windows.

The brief asked for 10 to 14 tickets. The list of things that must be in it is 15 sessions, and
the drawings need one more step than the brief names (axes, dimension chains, labels and the title
block are 860 lines of `plan.rs`: they are their own ticket, D3). So there are **16 tickets**:
**1–14 are the round**, 15 and 16 (history read, collaboration) are written and ready, and run
when a builder is free. H1 touches no hotspot, so it can start early.

## Order and waves

| # | Ticket | Pierre's item | Wave | Proves |
|---|---|---|---|---|
| 1 | T1 Measure tool (refreshed) | measure | B | label text golden vs Rust; tool by own tests |
| 2 | SH1 Sheets in the session, the Sheets page | sheets | A | Rust verbs' goldens; new verbs by own golden + DEVIATIONS |
| 3 | D1 Drawing spike: plan cut to SVG, and every drawing golden | drawings | A | SVG byte-equal with Rust |
| 4 | D2 Style, pens, hatches: the minimal plan | drawings | B | SVG byte-equal with Rust |
| 5 | D3 Plan annotations: axes, chains, labels, legend, title block | drawings | C | SVG byte-equal with Rust |
| 6 | D4 Views on paper: view range, ceiling plan, sections, crop | drawings | C | SVG byte-equal with Rust |
| 7 | D5 Sheet composition, `sheet_svg`, SVG export | drawings | D | SVG byte-equal with Rust; lab test |
| 8 | D6 PDF writer | drawings | D | content stream byte-equal with Rust; file by own check + DEVIATIONS |
| 9 | L1 PDF links (reference plans) | PDF links | B | polylines golden vs Rust; lab test |
| 10 | L2 DXF links, and managing a reference | DXF links | C | polylines golden vs Rust; lab test |
| 11 | PG1 Pages preview at `c/` | preview: web | A | local sub-path test; the first workflow run |
| 12 | R1 Linux release artifact of `bimble-ccore` | preview: Linux | A | the unpacked archive serves the lab |
| 13 | MAC1 macOS port and brew | ports, preview | B | the suites on a macOS runner |
| 14 | WIN1 Windows port (time-boxed) | ports, preview | C | the suites on a Windows runner |
| 15 | H1 History store: read Rust-written folders | history | C | golden folder written by Rust |
| 16 | H2 Commit and synchronize (collaboration) | collaboration | D | same object hashes as Rust |

- **Wave A starts now**, beside VC1 and GEO1: PG1, R1, D1, SH1. None owns a file VC1 or GEO1 edits.
- **Wave B** when VC1, GEO1, D1 and R1 are merged: T1, D2, L1, MAC1.
- **Wave C** when T1, D2, L1 and MAC1 are merged: D3, D4, L2, WIN1, H1.
- **Wave D**: D5 (after D3, D4, SH1), D6 (after D2; its export hook after D5), H2 (after H1).
- The queued round 2 tickets fit in like this: S1 and X2 with wave A or B (S1 owns `calls.c`:
  merge it first, the others rebase their hooks); G2 after T1 (`glviewport.ts`); Q2 with wave C.

## Hotspots

No round 3 ticket owns `ccore/session/calls.c`: each one adds hooks (one `if` per op, one row per
action, one name out of a stub list) and keeps its code in its own `session/<topic>.c`.

| File | Wave A | Wave B | Wave C | Wave D |
|---|---|---|---|---|
| `ccore/session/calls.c` | SH1 hook (S1 owns if it runs) | T1, L1 hooks | L2, H1 hooks (Q2 owns if it runs) | D5, D6, H2 hooks |
| `web-ui/src/glviewport.ts` | VC1 (running) | T1 owns; L1 one draw call | L2 nothing (G2 owns if it runs) | nobody |
| `web-ui/src/bimble.ts` | PG1: 3 lines at `:511`; SH1 types | T1, L1: types, one function each | L2, H1 | D5, H2 |
| `ccore/src/model.*`, `command.*`, `file.c` | GEO1 (running) | **nobody** | **nobody** | **nobody** |
| `ccore/src/verb_table.c` | SH1 rows (rebase on GEO1) | nobody | nobody | nobody |
| `crates/bimble-api/examples/ccore_golden.rs` | D1 owns (mode `drawing`) | T1 mode `measure`, L1 mode `reference` | H1 mode `history` | nobody |
| The Rust build slot (one at a time, `-j 2`) | D1 | T1, then L1 | H1 | nobody |
| `ccore/build.sh` | R1 owns; D1 one `VIEW_DENY` line | MAC1 owns; D2, L1 one line | WIN1 owns; D3, D4, L2 one line | D5, D6 one line |
| `ccore/host/*` , `session/platform.h` | R1 owns | MAC1 owns | WIN1 owns; H1 adds `list_dir` (one function, each platform file) | H2 |
| `.github/workflows/ccore.yml` | R1 owns (job `ccore-release`) | MAC1 | WIN1 | nobody |
| `.github/workflows/pages.yml` (on `main`) | PG1 | nobody | nobody | nobody |
| `ccore/test/ports.mjs` | SH1, R1: one row each | T1, L1 | L2, H1 | D5, H2 |
| `ccore/golden/DEVIATIONS.md`, `golden/README.md` | append only, one section each | | | |

Round 3 needs **no model change**: sheets, views, references and annotations are in `view.h`
since A6a. A ticket that finds it needs `model.h` stops and reports.

**Four Rust builds in the round** (D1, T1, L1, H1). D1 writes every drawing golden in one build,
so D2–D6 need no Rust build and do not edit the dumper. If a golden is missing or wrong, the
ticket reports it; the orchestrator gives one builder the slot.

## What to cut first if time is short

1. H2 (collaboration). `calls.c:748` keeps answering "no shared folder". Nothing else needs it.
2. WIN1: keep only its first step (the C suites on a Windows runner) and the blocker table.
3. D6's Roboto embedding: ship PDF with Helvetica (not embedded), which Rust supports too.
4. L2's manage operations (move, rotate, reload): keep the DXF reader and "add".
5. D3's opening dimensions and section marks (two template switches stay off; DEVIATIONS line).
6. T1 in plan views (3D only).
7. H1's page; keep the reader and the queries.

Never cut: D1 (it unblocks five tickets), PG1 and R1 (the preview), the gate.

---

## Wave A

### PG1 — The C lab on GitHub Pages at `c/`

**Goal and reason.** Pierre wants a preview he can open on any machine: the Vue lab on the C wasm,
at its own path, while the Rust viewer keeps the site root. `pages.yml` builds the whole site in
one artifact per deployment, so the C lab must be built in **every** deployment or it disappears
at the next release (as `preview/pr-N` does). The workflow runs from the default branch, so its
change lands on `main`, not on `rewrite/c-core`.

**Do.**
- On `rewrite/c-core`: the lab starts on the C wasm without URL parameters when built with
  `VITE_CCORE=1` (3 lines at `web-ui/src/bimble.ts:511-512`: treat it as `ccore=1&gl=1`).
  `?view=1` loads `ccore/ccore-view.wasm` (the `ccore_wasm` path of `ccore.ts` already exists).
  A script `build:ccore` in `web-ui/package.json`: both wasm builds, `copy-ccore.mjs` twice,
  `vite build --base ./`. A small "Preview, C core" line in the status bar with the commit, from
  `VITE_CCORE_REV`.
- `ccore/test/lab_pages.mjs`: build to a temp folder as `site/c/`, serve `site/` with a static
  Node server, open `/c/`: a frame is drawn, `place` works and undo works, `/c/?view=1` answers
  "this build is view-only" to a verb, no request leaves `/c/` (relative URLs only).
- On a branch from `main` (second pull request): `pages.yml` gets a job `ccore_lab` shaped like
  `preview` (no right to deploy; `contents: read`): checkout `inputs.ccore_ref` (default
  `rewrite/c-core`), apt `clang lld`, Node 24, `npm ci`, `npm run build:ccore`, upload artifact
  `ccore-lab`. `deploy` downloads it to `site/c/` when the job succeeded; a failed `ccore_lab`
  does **not** stop the Rust site (unlike `preview`). The `test -f site/index.html …` line of the
  Rust lab stays as it is.
- One paragraph in `ccore/README.md`; the URL in the report.

**Owns.** `web-ui/package.json` (one script), `web-ui/scripts/copy-ccore.mjs`,
`ccore/test/lab_pages.mjs`, one row in `ports.mjs`; on `main`: `.github/workflows/pages.yml`.
**Hooks.** `bimble.ts` (3 lines), `StatusBar.vue` (one line). **Do not touch.** The `deploy` steps
that build the Rust lab and `egui/`, `release.yml`, `web-ui/scripts/copy-wasm.mjs`, `ccore/build.sh`.

**Read first.** `.github/workflows/pages.yml` (all; the `preview` job at 38–86 is the pattern, the
deploy condition at 96–103), `web-ui/src/ccore.ts:1-25`, `web-ui/src/bimble.ts:505-530`,
`web-ui/scripts/copy-ccore.mjs`, `ccore/test/lab_wasm.mjs`, ADR 0020, ADR 0069 rule 3.

**Done check.**
```sh
cd ccore && CCORE_PORT_BASE=auto node test/lab_pages.mjs && CCORE_PORT_BASE=auto node test/lab_wasm.mjs \
 && (cd ../web-ui && npm run typecheck) \
 && python3 -c "import yaml; yaml.safe_load(open('../.github/workflows/pages.yml'))"
```
(the last line in the worktree of the `main` branch change). The deployment itself is proven by
one manual run of `pages` after Pierre merges it: **stop there and ask** (a deploy is his step).

**Proves.** The built lab works under a sub-path with relative URLs only. **Depends on.** Nothing.
**Parallel with.** All of wave A. **Note.** The wasm has no boolean kernel (`build.sh:203`):
openings are drawn uncut in the preview. Say it on the preview line.

### R1 — The Linux release artifact of `bimble-ccore`

**Goal and reason.** ADR 0069 rule 2: the desktop is the user's browser plus a local C server.
Today the host serves only `/change`, `/frame`, `/mesh` and the calls (`host/server.c:517-546`);
the page comes from Vite. A release needs one archive: unpack, run `bimble-ccore open model.ifc`,
the browser opens the lab. The dev machine is aarch64, so both architectures.

**Do.**
- `host/static.c`: the host serves the built lab from `<exe dir>/web/` (or `--web <dir>`): a
  list of files read once at start; a request path is matched against that list (never joined to
  a path: no `..`, no symlink walk); content types by extension; at most 64 MiB in all.
- `bimble-ccore open [--port N] [--no-browser] [model]`: a free port on 127.0.0.1, a random
  token, prints `http://127.0.0.1:<port>/?native=1&gl=1&port=<port>&token=<t>` and starts
  `xdg-open` on it. The old form (`bimble-ccore <port> [model]`) is unchanged: every test uses it.
  `--version` prints `ccore/VERSION` (new file, `0.1.0-preview.1`; do not change `CC_IFC_VERSION`,
  the IFC goldens hold it).
- `build.sh release`: clang `-O2`, manifold **static** (`out/deps/manifold/lib/*.a`, `deps.sh:26`),
  the web-ui build, then `out/release/bimble-ccore-<version>-<arch>-linux.tar.gz` with
  `bimble-ccore`, `web/`, `LICENSE`, a ten-line `README.txt`.
- `ccore.yml`: job `ccore-release` on `workflow_dispatch` and on tags `ccore-v*`, matrix
  `ubuntu-22.04` (x86_64, low glibc as `release.yml:92`) and `ubuntu-22.04-arm`; uploads the
  archives as artifacts. On a tag it attaches them to a **prerelease**. The tag prefix must not be
  `v`: `v*` starts the Rust release, the Pages deployment and the brew formula
  (`release.yml:11`, `pages.yml:101`).
- `test/release_test.mjs`: unpack the archive in a temp folder, run `open --no-browser
  golden/frame.json`, GET `/` is the lab's `index.html`, `/../etc/passwd` and an unknown path are
  404, a query answers, and `ldd` names no `libmanifold`.

**Owns.** `ccore/host/static.c/.h`, `host/main.c`, `ccore/build.sh` (mode `release`),
`ccore/VERSION`, `ccore/test/release_test.mjs`, `.github/workflows/ccore.yml`, a "Release"
paragraph in `ccore/README.md`. **Hooks.** `host/server.c`: one branch for GET of a static path.
**Do not touch.** `release.yml`, `Formula/`, `session/`, `src/`.

**Read first.** `ccore/host/main.c`, `host/server.c:480-560` and `:137-180` (`respond_x`),
`ccore/build.sh` (`lib_and_tests`, the kernel setup at 23–41), `ccore/deps.sh`,
`.github/workflows/release.yml:88-170`, `crates/bimble-viewer/src/serve.rs` (the origin and token
rules the host copied), ADR 0043, ADR 0069.

**Done check.**
```sh
cd ccore && ./build.sh release && CCORE_PORT_BASE=auto node test/release_test.mjs \
 && CCORE_PORT_BASE=auto test/host.sh
```
A tag and a published prerelease are Pierre's step: **stop and ask**.

**Proves.** The archive alone runs the lab on a machine with no repo. **Depends on.** Nothing.
**Parallel with.** All of wave A. **Blocks.** MAC1, WIN1 (same files).

### D1 — Drawing spike: one plan's cut as SVG, and every drawing golden

**Goal and reason.** "The drawing is the product." Rust draws a storey plan, a section and a
sheet as a list of paper items (`Item`: area, lines, text) and writes them as SVG or PDF. This
ticket ports the two ends, cut geometry and the SVG writer, and proves them byte for byte on one
plan. It also writes **all** goldens of D2–D6 in its one Rust build, so the next five tickets
need no Rust.

**Do, part 1: the dumper (first, then commit the goldens).** Mode `drawing` in
`ccore_golden.rs` writes `ccore/golden/drawing/`. Fixed inputs everywhere: project `"frame"`,
date `"2026-01-01"`. Use the template's switches (`template.rs:20-34`) to make stages: Rust's
`plan()` itself gives each stage's SVG.

| Files | What | For |
|---|---|---|
| `cut/<model>.txt` | per element at the ground plan's cut: `cut::section` loops and open chains, `facing_up_region`, `{:?}` floats | D1 |
| `svg/sample.svg` | `to_svg()` of a sheet built in the dumper: each item kind, dashes, hatches, holes, the three anchors, turned and bold text, text to escape, both fonts | D1 |
| `roboto.txt` | advance of each character U+0020–U+024F, U+2009, U+2013, U+00D7 (regular and bold, as `fonts.rs:76-88` gives them, f32 `{:?}`), ascent, descent, cap height, box | D1, D6 |
| `plan/<model>/cut.svg`, `frame.txt` | a sheet made in the dumper: one `Area` per cut element (loops through `PlanInfo.frame.to_paper`, one fixed pen, no fill, no hatch); the `ViewFrame` fields | D1 |
| `style/default.json`, `pens.txt`, `template.json` | `GraphicStyle::default()`, `role_pen` for each role and each of `STANDARD_SCALES`, `PlanTemplate::default()` | D2 |
| `plan/<model>/min.svg`, `min.info.txt` | `plan()`, `bare`, axes `off`, every annotation switch off, legend off; the `PlanInfo` as `{:#?}` | D2 |
| `plan/<model>/full.svg`, `bare.svg`, `a1_50.svg`, `errors.txt` | default template: auto paper 1:100, bare, A1 at 1:50; the error texts (scale 0, unknown storey, paper too small) | D3 |
| `model.json` | `frame2.json` plus a plan view with a range, a ceiling plan, two sections (north, east), a cropped plan, a plan that hides a class, one dimension and one text in a view, and two sheets (plan 1:100 with section 1:50 and a sheet text; one placing a 3D view, which gives a note) | D4, D5 |
| `view/<name>.min.svg`, `.svg` | each view of `model.json` through `PlanOptions.view`, reduced and default template | D4 |
| `sheet/<number>.svg`, `.info.txt` | `sheetdraw::draw` of each sheet, with `SheetInfo` | D5 |
| `pdf/<name>.pdf`, `.content` | `to_pdf()` of `sample`, `plan/frame/full`, one sheet, in Roboto and in Helvetica; the page's content stream inflated (`flate2` is a dev-dependency) | D6, L1 |

`<model>` is `frame` and `frame2`. Add `drawing/` to the `--check` set (it is Rust's output, not
`C_OWN`). One section in `golden/README.md`.

**Do, part 2: the C code.**
- `src/draw_cut.c/.h`: `cut.rs` (`section` with its weld of 1e-4 mm, `clip_z`,
  `facing_up_region`, `edge_seen_from_above`) and `crop.rs` (`Rect` polygon, segment, polyline).
- `src/paper.c/.h`: the paper sheet (`cc_paper`: size, title, font, items), `Item`, `Pen`,
  `Hatch`, the paper sizes, `hatch_lines` (`sheet.rs:221-270`), text width (Helvetica's table at
  `sheet.rs:119`, Roboto's from a table generated from `roboto.txt`; sum in f32, in order) and
  `to_svg` (`sheet.rs:274-373`).
- Rust writes paper numbers with `{:.3}` and others with `{}`. The wasm build has no `printf`:
  write an exact fixed formatter in `fmt.c` (correct rounding from the exact value) and test it
  against Ryu on a million values.
- `test/draw_test.c`: `cut/*.txt`, `svg/sample.svg` and `plan/*/cut.svg` byte for byte (the frame
  is read from `frame.txt`: D2 computes it).
- One line in `build.sh` `VIEW_DENY` for the new files (the viewer wasm stays under 400,000 bytes).

**Owns.** `ccore/src/draw_cut.c/.h`, `src/paper.c/.h`, `ccore/test/draw_test.c`,
`ccore/golden/drawing/`, `crates/bimble-api/examples/ccore_golden.rs` (wave owner), the Rust slot.
**Hooks.** `src/fmt.c/.h` (one function), `build.sh` (one line). **Do not touch.** Any file in
`crates/*/src/` (the oracle is not edited to make a golden), `session/`, `web-ui/`.

**Read first.** `crates/bimble-drawing/src/cut.rs:20-232`, `crop.rs:14-97`, `sheet.rs:1-375`,
`fonts.rs`, `plan.rs:40-150` (options, `PlanInfo`) and `:752-800`, `viewdraw.rs:57-98`
(`ViewFrame`), `template.rs:20-34`, `ccore/src/scene.h` (element meshes are f32, as Rust's),
`ccore/golden/README.md` "Regenerate and check", `ccore_golden.rs:46-170`.

**Done check.**
```sh
cd ccore && test/run.sh draw fmt && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh draw \
 && ./build.sh wasm-view > /dev/null \
 && cargo run -j 2 -p bimble-api --example ccore_golden -- --check
```

**Proves.** Golden vs Rust: the cut of every element and the SVG text, byte for byte.
**Depends on.** Nothing. **Parallel with.** All of wave A. **Blocks.** D2–D6, L1 (its PDF fixtures).
**If time is short.** The goldens come first: commit them even if the C half is not finished.

### SH1 — Sheets in the session and the Sheets page

**Goal and reason.** The C model holds sheets since A6a (`view.h:95-118`), and the verbs
`add_sheet` and `set_sheet` match Rust. But nobody can place a view on a sheet, and the Sheets
page shows an error: it asks `sheet_svg`, which the session refuses (`calls.c:953`). This ticket
makes sheets usable before the drawings exist: list, add, rename, paper, place and move views.
D5 then puts the real drawing on the paper.

**Do.**
- Verbs in `src/verbs_sheets.c`, rows in `cc_verb_c_additions`: `sheet_place {sheet, view,
  at_mm, scale?}` (the first placement of a view sets its scale, as `sheet.rs:63` `adopted_scales`),
  `sheet_move {sheet, placed, at_mm}`, `sheet_unplace {sheet, placed}`, `sheet_remove {id}`. Each
  makes one `set_sheet` or `remove_sheet` Command (they exist, `command.h:49-50`); the limits and
  texts of `crates/bimble-core/src/sheet.rs:146-195` (`validate`, at most 200 placed).
- `session/sheets.c`: query `sheet_layout {id}`: paper width and height in mm
  (`sheet.rs:133`), and for each placed view its id, view name, scale, `at_mm` and a box. Until
  D5 the box is the view's crop, else the model's plan box, divided by the scale; say `"box":
  "estimate"` in the answer so the page and D5 can tell.
- Page: `Sheets.vue` draws the paper from `sheet_layout` (white sheet, right proportion, each
  placed view as a labelled outline) when `sheet_svg` answers an error, and under handles when it
  answers. Drag a box: `sheet_move`. `SheetsSide.vue`: add, rename, number, paper, portrait,
  delete, "place view" from a list of views; fill the `data-missing` rows at 87–88 (the placed
  view selected, its scale).
- A sheet tab (`call_tab`, type `sheet`, `calls.c:1485`) opens the page on that sheet.
- Golden `golden/sheets/verbs.txt` (each verb, each refusal, undo, redo, on `drawing/model.json`
  or `verbs_all_after.json`), written by the C test; DEVIATIONS entry (C additions; the file a
  sheet makes is plain `Sheet`, so Rust reads it).

**Owns.** `ccore/src/verbs_sheets.c`, `ccore/session/sheets.c/.h`, `ccore/test/sheets_test.c`,
`ccore/test/lab_sheets.mjs`, `ccore/golden/sheets/`, `web-ui/src/pages/Sheets.vue`,
`SheetsSide.vue`, i18n part for sheets. **Hooks.** `verb_table.c` (rows), `calls.c` (one `if` in
`call_query`), `bimble.ts` (types, one function per verb), `ports.mjs` (one row), `build.sh`
(`VIEW_DENY`: `verbs_sheets.c`). **Do not touch.** `model.h`, `view.h`, `command.c`, `glviewport.ts`.

**Read first.** `crates/bimble-core/src/sheet.rs`, `crates/bimble-api/src/dispatch.rs:73-110` and
`query.rs:180-240`, `ccore/src/view.h`, `ccore/src/verbs_levels.c` (the pattern for added verbs),
`ccore/test/viewsheet_test.c`, `web-ui/src/pages/Sheets.vue`, `SheetsSide.vue`, wireframe notes in
them, `anthropic-skills:bimble-design`.

**Done check.**
```sh
cd ccore && test/run.sh sheets viewsheet verbs_all && CCORE_PORT_BASE=auto node test/lab_sheets.mjs \
 && (cd ../web-ui && npm run typecheck)
```
`lab_sheets.mjs`: add a sheet, place the ground plan, the paper shows one box with the view's
name; drag it, `sheet_layout` gives the new `at_mm`; undo twice; the Rust verb goldens still match.

**Proves.** `add_sheet`, `set_sheet`: the existing Rust goldens. New verbs: own golden,
DEVIATIONS. **Depends on.** Nothing (rebase the `verb_table.c` rows on GEO1 if it adds rows).
**Parallel with.** All of wave A. **Blocks.** D5.

---

## Wave B

### T1 — The measure tool (round 2 ticket, refreshed)

**Goal and reason.** As in `next-round.md` "T1": measuring is in Pierre's viewing mission, Rust
has it, and in C the action exists (`calls.c:82`) but does nothing and `measurements` answers
`[]` (`calls.c:946`). What changed since round 2, and replaces the old text where they differ:

- **Line numbers.** `calls.c`: the stub list is at 946, `call_query` 878, `call_tool` 2143, the
  action row 82. `glviewport.ts`: `toolAt` 1105, `annotAt` 1071, the pointer listeners 1226–1320,
  the move dispatch 1394.
- **Draw on C4's overlay.** C4 added a 2D canvas over the viewport (`glviewport.ts:377-383`,
  drawn at 1024–1057). The snap marker, the rubber line and the kept measurements with their
  labels go there, through `web-ui/src/gl/measure.ts` (new folder) and one call in the overlay's
  draw. No new GL layer.
- **The viewer wasm must measure** (W1, ADR 0069 rule 3): `session/measure.c` is **not** in
  `VIEW_DENY`, `session/view_only.c` lets `tool measure_*` pass, and `wasm_view_run.mjs` gets one
  case. Check the size stays under 400,000 bytes.
- **VC1** edits the viewport's camera code now: start after it is merged.
- The rest stands: `session/measure.c/.h` with `measure_pointer`, `measure_click`,
  `measure_cancel`, `measure_clear` under op `tool`; snap to corner, then edge, then face on the
  scene's triangles and crease edges with Rust's pixel limits (the page sends `ray` and
  `ray_tol`); the orthogonal lock; the section box and hidden elements honoured; two clicks make
  one measurement; session state, no history step, no `rev` change; read-only scenes and linked
  models; query `measurements` as `[{id, label, value_mm}]`; Esc cancels.
- Golden: a `ccore_golden` mode `measure` writes `golden/measure.txt`: `format_mm` for a fixed
  list of values (0, 0.4, 999.5, 2500, 12 345.6, 1e6 …) and `measurements_json` for three fixed
  pairs. Rust's value is f32: compute the distance in f32 too.

**Owns.** `ccore/session/measure.c/.h`, `web-ui/src/gl/measure.ts`, `web-ui/src/glviewport.ts`
(wave owner; keep it small), `ccore/test/measure_test.c`, `ccore/test/lab_measure.mjs`,
`ccore/golden/measure.txt`. **Hooks.** `calls.c` (one `if` in `call_tool`, one in `call_query`,
the pressed state of the action, `measurements` out of the list at 946), `view_only.c`,
`bimble.ts`, `ccore_golden.rs` (one mode), `ports.mjs`.
**Do not touch.** `session/tool.c` beyond making a function callable, `session/views.c`.

**Read first.** `crates/bimble-viewer/src/measure.rs:22-190` (`snap` :60, `orthogonal` :129,
`Dimension` :153, `format_mm` :169), `state.rs:218-233`, `calls.rs:424-425`,
`ccore/session/tool.h` (`cc_snap` :54, `cc_ray_plane` :63), `calls.h` (the tool protocol),
`visibility.h`, `view_only.c`, `ccore/test/lab_tool.mjs`, `lab_dims.mjs` (how the overlay is tested).

**Done check.**
```sh
cd ccore && test/run.sh measure tool && CCORE_PORT_BASE=auto node test/lab_measure.mjs \
 && CCORE_PORT_BASE=auto node test/lab_tool.mjs && CCORE_PORT_BASE=auto node test/lab_dims.mjs \
 && ./build.sh wasm-view > /dev/null && node test/wasm_view_run.mjs \
 && cargo run -j 2 -p bimble-api --example ccore_golden -- --check
```
`lab_measure.mjs`: measure one bay of the frame between two column corners in 3D and in a plan;
the label is the known distance; `measurements` has 1 row; clear gives 0; `rev` did not change.

**Proves.** The label and the query: golden vs Rust. The tool protocol: own tests, one DEVIATIONS
line (ADR 0068 rule 2). **Depends on.** VC1 merged. **Parallel with.** D2, L1, MAC1 (L1 takes
the Rust slot after T1). **Blocks.** G2, L1's draw call (rebase).

### D2 — Style, pens and hatches: the minimal plan, byte-equal

**Goal and reason.** D1 proved the cut and the writer. A real plan also needs the office's look:
which pen at which scale, which hatch for which material family, what is seen below the cut. This
ticket ports the style tables and the body of `plan()` without annotations, and matches Rust's
`plan()` output for the reduced template.

**Do.**
- `src/draw_style.c/.h`: the defaults of `style.rs:328-417` (fills, families, paper style), the
  pen table and line roles (`pens.rs:32-95`, `:200-234`, `model_mm` picks the nearest scale), the
  hatch types (`hatches.rs:86-228`), and `plan.rs:180-325` (`role_pen`, `hatches_for`,
  `seen_fill_for`, `hatch_type_lines`, `cut_look`, `seen_look`). Built-in defaults only: a style
  file is a later ticket. Test: the tables written as JSON equal `style/default.json`; `pens.txt`.
- `src/draw_plan.c/.h`: `plan()` at `plan.rs:752` for the reduced template: the checks, the
  storey setup (`:690-750`), the model geometry (`:775-912`: cut, seen tops low to high,
  projection lines, lines above), the paper and its frame (`:997-1107`, auto paper and the
  "suggest a scale" error), the items back to front for cuts and seen (`:1108-1206`,
  `:1260-1290`), the title of a bare page. Leave a call site, empty for now, where each
  annotation stage goes (D3) and where the setup is chosen (D4).
- `test/draw_plan_test.c`: `plan/<model>/min.svg` and `min.info.txt` byte for byte, and now
  `cut.svg` with the frame **computed**, not read.

**Owns.** `ccore/src/draw_style.c/.h`, `src/draw_plan.c/.h`, `ccore/test/draw_plan_test.c`.
**Hooks.** `build.sh` (`VIEW_DENY`). **Do not touch.** `paper.c`, `draw_cut.c` (report a fault to
the orchestrator), the dumper, `session/`.

**Read first.** The Rust lines above, `crates/bimble-drawing/src/template.rs:201-232` (defaults)
and `:296-312` (`hides`), D1's `paper.h` and `draw_cut.h`, `ccore/src/scene.h`,
`ccore/src/library.h` (material families), ADR 0021, ADR 0046.

**Done check.**
```sh
cd ccore && test/run.sh draw_plan draw && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh draw_plan
```

**Proves.** Golden vs Rust: a whole SVG of `plan()`. If a float differs in the last digit, find
the operation order (f32 mesh, f64 paper); no tolerance. **Depends on.** D1. **Parallel with.**
T1, L1, MAC1. **Blocks.** D3, D4, D6. **If time is short.** Land the style tables with their
goldens first; the plan body is then a second session.

### L1 — PDF links: a PDF as a reference plan

**Goal and reason.** Pierre starts a model on the architect's plan. Rust reads the paths of a
vector PDF's first page and places them in a 2D view (`reference.rs:326`, ADR 0021). The C model
already stores references in a view (`view.h:56-66`), and the actions `link_pdf` and `link_dxf`
exist in the table (`calls.c:101-102`) and do nothing. This ticket adds the PDF reader and the
whole path from a file to lines on the screen. L2 adds DXF on the same path.

**Do.**
- Dumper mode `reference` (one Rust build, for L1 and L2): for each file of
  `golden/reference/fixtures/` writes `<name>.txt`: the polylines (`{:?}`) and the notes of
  `reference::read`, and the error texts for bad input. Fixtures, all synthetic: D1's
  `drawing/pdf/*.pdf` (Bimble's own plans, compressed streams), one hand-written PDF without
  compression (lines, a rectangle, a Bézier curve, a transform, a form XObject), a truncated one,
  one with a cycle in the page tree; for L2 three DXF texts (lines, arcs, an LWPOLYLINE with
  bulges, a block inserted twice with scale and rotation, `$INSUNITS` in metres; an R12 file; a
  block that inserts itself).
- `src/ref_pdf.c/.h`: `read_pdf` (`reference.rs:326-558`): the cross-reference table and
  streams, object streams, FlateDecode (zlib over `inflate.c`), the first page, the operators
  `m l c v y h re cm q Q Do`, curves as 64 segments per circle (`:21`), scale 1:N. Budgets as
  Rust: 256 MB (`:18`), depth 8 (`:19`), plus a cap on objects and on points; every loop bounded;
  an encrypted file is refused with a text.
- `session/refs.c/.h`: `link_pdf {path | bytes, scale?}` adds a `Reference` to the active plan
  or section through one `set_view` Command (one undo step, as `viewing.rs:1470`); the
  references appear in query `links` (`state.rs:199-216`, `calls.rs:414-421`); query
  `view_references {view}` gives the segments in view mm (`views.rs:157-200`, capped, with a
  count). In wasm the page hands over the picked file's bytes, as `link_model` does.
- Page: `web-ui/src/gl/reference.ts` draws the segments on the overlay canvas in the palette's
  reference colour, under the annotations; one draw call in `glviewport.ts`. Measure the frame
  time with the 30,000-segment fixture and write the number in the report (no optimisation).

**Owns.** `ccore/src/ref_pdf.c/.h`, `ccore/session/refs.c/.h`, `web-ui/src/gl/reference.ts`,
`ccore/test/ref_test.c`, `ccore/test/lab_refs.mjs`, `ccore/golden/reference/`.
**Hooks.** `calls.c` (the action, one `if` in `call_query`, the rows in `links`), `glviewport.ts`
(one call; T1 owns the file: rebase on it), `bimble.ts`, `ccore.ts` (the file picker for the two
actions), `ccore_golden.rs` (one mode), `build.sh` (`VIEW_DENY`: no, the viewer shows references
of a file; `refs.c`'s `set_view` path is behind `view_only`), `ports.mjs`.
**Do not touch.** `view.h`, `view.c`, `session/links.c` (linked models).

**Read first.** `crates/bimble-drawing/src/reference.rs` (all), `crates/bimble-core/src/view.rs:479-510`
(`Reference::place`), `crates/bimble-viewer/src/viewing.rs:1470-1530`, `views.rs:150-275`,
`links.rs:38-200`, `ccore/src/inflate.h`, `ccore/session/links.c` (how a picked file reaches the
session), `web-ui/src/bimble.ts:980-1020`, ADR 0021, ADR 0050.

**Done check.**
```sh
cd ccore && test/run.sh ref && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh ref \
 && CCORE_PORT_BASE=auto node test/lab_refs.mjs && CCORE_PORT_BASE=auto node test/lab_links.mjs \
 && cargo run -j 2 -p bimble-api --example ccore_golden -- --check
```
`lab_refs.mjs`: link the plan PDF in the ground plan at 1:100, lines are drawn, a known corner of
the PDF lies on its column corner (within one pixel), undo removes the reference.

**Proves.** The polylines: golden vs Rust (state any float difference; lopdf and the C reader
must agree on the same file, and if a fixture cannot match, write why in DEVIATIONS). The bad
files: no crash under ASan. **Depends on.** D1 (PDF fixtures), T1 merged (`glviewport.ts`).
**Parallel with.** T1 (Rust slot after it), D2, MAC1. **Blocks.** L2.

### MAC1 — macOS port and brew

**Goal and reason.** Second step of the preview: `brew install` on a Mac. `ccore-macos` is only a
probe that may fail. Read its log first (`gh run list --workflow ccore.yml`): it is the evidence;
`results/ci.md` holds estimates.

**The blockers, from `ccore/results/ci.md` "macOS: what blocks".**

| Blocker | Where | Fix |
|---|---|---|
| `getrandom()` is Linux only | `src/guid.c:26`, `:293` | `getentropy()` under `__APPLE__` |
| bash 3.2: an empty array under `set -u` | `test/run.sh`, `build.sh` (`"${KFLAGS[@]}"`) | guard the expansions (`${a[@]+"${a[@]}"}`); no Homebrew bash needed |
| `stat -c`, `sha1sum` | `deps.sh` (`deps_import`) | `shasum`, `stat -f` behind one helper |
| `-Wl,--whole-archive`, `.so`, `-Wl,-rpath=` | `deps.sh:35`, `kernel_setup` | the release links the static archives with clang; the shared library is only for tcc: skip it on macOS |
| `date +%s%N`, `llvm-ar`, `sha1sum` | `build.sh` (step times, the host cache key) | a portable clock helper, `ar` |
| `MSG_NOSIGNAL` | `host/server.c:112`, `:700` | `SO_NOSIGPIPE` on the socket (SIGPIPE is already ignored) |
| GNU `timeout`, `date +%s%N` | `ci/run.sh` | a shell fallback |
| `/usr/bin/chromium` in 18 test files | `test/*.mjs` | one `CHROMIUM` variable in `ports.mjs` (also asked by K1) |
| `xdg-open` | R1's `open` | `open` on macOS |
| no `wasm-ld` in Apple clang | `build.sh wasm` | not ported: the wasm is built on Linux |
| `inotifywait`, `setsid` | `dev.sh` | not ported now (one line in the README) |
| tcc on macOS arm64 | the edit loop | not ported now: clang only |

**Do.**
- Fix the rows above. `ccore-macos` loses `continue-on-error` and runs `test/run.sh` with clang
  **with the kernel** (manifold through `deps.sh`, cached) and `test/host.sh`.
- `build.sh release` on macOS: arm64 and x86_64 archives (`CMAKE_OSX_ARCHITECTURES` for manifold,
  as `release.yml:126-133`), `bimble-ccore-<version>-<arch>-macos.tar.gz`, in `ccore-release`.
- Brew: `ccore/ci/formula.sh` writes `Formula/bimble-ccore.rb` from the archives' SHA-256
  (pattern: `scripts/homebrew-formula.sh`), macOS and Linux. The formula is on `main` (the tap is
  the repo): a second pull request, **never** `Formula/bimble.rb`.

**Owns.** `ccore/build.sh`, `deps.sh`, `test/run.sh`, `ci/run.sh`, `ci/formula.sh`,
`host/server.c`, `host/main.c`, `.github/workflows/ccore.yml`, `ccore/results/ci.md` (measured
rows replace the estimates); on `main`: `Formula/bimble-ccore.rb`. **Hooks.** `src/guid.c` (one
`#ifdef`), `ports.mjs` and the 18 lab tests (the `CHROMIUM` name only).
**Do not touch.** `release.yml`, `Formula/bimble.rb`, `scripts/homebrew-formula.sh`, `dev.sh`.

**Read first.** `ccore/results/ci.md`, the probe's log, `.github/workflows/ccore.yml`,
`release.yml:88-170` and the "Homebrew formula" step at 257, `scripts/homebrew-formula.sh`, ADR 0043,
R1's `build.sh release`.

**Done check.** On the dev machine: `ccore/ci/run.sh fast` (nothing broke on Linux) and
`bash --posix -n` of each script. On GitHub (the proof): the run of `ccore.yml` on the ticket's
branch shows `ccore-macos` green and two macOS archives in `ccore-release`; give the run URL.
```sh
ccore/ci/run.sh fast && gh run watch --exit-status "$(gh run list --workflow ccore.yml --branch "$(git branch --show-current)" --limit 1 --json databaseId --jq '.[0].databaseId')"
```

**Proves.** The C suites and the host on macOS, measured. **Depends on.** R1.
**Parallel with.** T1, D2, L1. **Blocks.** WIN1 (same files). The formula's merge and the tag are
Pierre's step.

---

## Wave C

### D3 — Plan annotations: axes, dimension chains, labels, legend, title block

**Goal and reason.** A plan without axes, dimensions and a title block is a picture, not a
drawing. This is the rest of `plan()`: about 860 lines of Rust. After it, the default template's
plan is byte-equal with Rust.

**Do.** In `src/draw_annot.c`, called from D2's call sites, each stage behind its template switch:
- grid axes from the model's grids, else from the columns (`plan.rs:913-968`, `clusters` :661,
  `letter_tag` :647), drawn dash-dot with bubbles (`:1207-1258`, `:1482-1500`);
- slab levels and opening crosses (`:1139-1206`), opening sizes and positions (`:1291-1357`);
- section labels of columns and beams (`:511-608`, `:1358-1402`; `cm` :514, `section_text` :524);
- section marks (`:78-104`, `:1403-1481`);
- dimension chains (`:449-510`, `:1501-1588`), the view's own dimensions and texts
  (`:1589-1664`, `viewdraw.rs:409-433`; a C feature anchor resolves to its point first);
- texts, scale bar and the legend (`:1665-1830`, `draw_legend` :326-404), frame and title block
  (`:1833-1935`), placeholders and the dimension unit (`template.rs:58-78`, `:234-312`).
- Where the C model has grids (G1) and Rust's scene has its own grid axes, feed the same axes:
  check with `golden/grids/model.json` by hand and say what you saw.

**Owns.** `ccore/src/draw_annot.c/.h`, `src/draw_template.c/.h`, `ccore/test/draw_annot_test.c`.
**Hooks.** `draw_plan.c` (the calls at D2's sites), `build.sh` (`VIEW_DENY`).
**Do not touch.** `draw_style.c`, `paper.c`, `draw_view.c` (D4), `src/annot.c` (C4's screen
geometry), the dumper.

**Read first.** The Rust lines above, `template.rs` (all), `crates/bimble-core/src/pens.rs:352-366`
(`DimType::format`) and `:370-466`, D2's `draw_plan.h`, `ccore/src/annot.h`, `grid.h`, ADR 0018.

**Done check.**
```sh
cd ccore && test/run.sh draw_annot draw_plan && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh draw_annot
```
`draw_annot_test`: `plan/<model>/full.svg`, `bare.svg`, `a1_50.svg` and `errors.txt`, byte for byte.

**Proves.** Golden vs Rust: the default plan. **Depends on.** D2. **Parallel with.** D4 (both add
calls in `draw_plan.c`: D3 the annotation sites, D4 the setup site), L2, WIN1, H1.
**Blocks.** D5.

### D4 — Views on paper: view range, ceiling plans, sections, crop, hidden classes

**Goal and reason.** D2 draws a storey cut at one height. The model's views say more: a plan
with a view range, a ceiling plan looking up, a section looking north or east, a crop, hidden
classes and elements, true north. Rust turns the scene into "seen from above" for each case
(`viewdraw.rs`), then `plan()` draws it. This ticket ports that turn.

**Do.**
- `src/draw_view.c/.h`: `setup` (`viewdraw.rs:143-278`), `Turn` and `ViewFrame` with their
  inverses (`:57-141`), `within_heights` :280, what a view keeps (`:290-322`), `map_scene`
  (`:324-407`: the scene turned, reflected for a ceiling plan, laid down for a section), the
  resolved view range (`crates/bimble-core/src/view.rs:296-436`, already in `session/views.c`
  for the screen: share one function, do not copy it), the crop (`crop.rs`, D1).
- `PlanOptions.view` in `draw_plan.c`: one call at D2's setup site.
- Georeference: if GEO1 is merged, `plan_turn` and `facing_dir` (`view.rs:63-72`) use it and the
  true-north golden is checked; if not, turn 0 and one DEVIATIONS line.
- `test/draw_view_test.c`: every `view/<name>.min.svg` byte for byte; `view/<name>.svg` too once
  D3 is merged (the test skips them with a printed line until `draw_annot.h` exists, never
  silently).

**Owns.** `ccore/src/draw_view.c/.h`, `ccore/test/draw_view_test.c`. **Hooks.** `draw_plan.c`
(one call), `session/views.c` (only to call the shared range function), `build.sh` (`VIEW_DENY`).
**Do not touch.** `draw_annot.c` (D3), `view.h`, the dumper.

**Read first.** `crates/bimble-drawing/src/viewdraw.rs` (all), `crates/bimble-core/src/view.rs:200-436`,
`crates/bimble-viewer/src/views.rs:16-150`, `ccore/session/views.c`, `golden/drawing/model.json`,
ADR 0046.

**Done check.**
```sh
cd ccore && test/run.sh draw_view draw_plan viewsheet && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh draw_view \
 && CCORE_PORT_BASE=auto node test/views_test.mjs
```

**Proves.** Golden vs Rust for six views. **Depends on.** D2. **Parallel with.** D3, L2, WIN1, H1.
**Blocks.** D5.

### L2 — DXF links, and managing a reference

**Goal and reason.** Most architects send DXF. L1 made the path from a file to the screen; this
ticket adds the DXF reader on it, and what a user does next with a reference: move it onto the
grid, turn it, hide it, reload it when the architect sends a new version, remove it.

**Do.**
- `src/ref_dxf.c/.h`: `read_dxf` (`reference.rs:112-156`), `dxf_entity` (`:158-281`: LINE,
  LWPOLYLINE and POLYLINE with bulges through `bulged` :283, ARC, CIRCLE, ELLIPSE, SPLINE as its
  control polygon if Rust does that, INSERT with scale and rotation, depth 8), `arc` :92, units
  from `$INSUNITS` to mm, model space only. A text reader with a line budget; binary DXF and DWG
  are refused with Rust's texts (`:36-44`).
- `link_dxf` in `session/refs.c`, on L1's path.
- `manage_links` for a reference: `{op: move|rotate|show|hide|reload|remove, id, …}`, each one
  `set_view` Command; reload keeps the place (`views.rs:263-272`); the rows and buttons of the
  Links panel work for references as for linked models.
- Drag a reference in a plan (`views.rs:203-215`) only if `glviewport.ts` needs no change beyond
  one hook; else leave it and say so.

**Owns.** `ccore/src/ref_dxf.c/.h`, `ccore/session/refs.c` (after L1), `ccore/test/ref_dxf_test.c`,
`ccore/test/lab_refs.mjs`, the Links panel component. **Hooks.** `calls.c` (the action; the
`manage_links` branch), `bimble.ts`. **Do not touch.** `ref_pdf.c`, the dumper (L1 wrote the DXF
goldens), `glviewport.ts` beyond the one hook.

**Read first.** `crates/bimble-drawing/src/reference.rs:46-325`, `crates/bimble-viewer/src/links.rs`,
`views.rs:203-275`, L1's `refs.h`, `golden/reference/fixtures/*.dxf`.

**Done check.**
```sh
cd ccore && test/run.sh ref_dxf ref && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh ref_dxf \
 && CCORE_PORT_BASE=auto node test/lab_refs.mjs && CCORE_PORT_BASE=auto node test/lab_links.mjs
```

**Proves.** Golden vs Rust (polylines of each DXF fixture); the manage operations by the lab
test. **Depends on.** L1. **Parallel with.** D3, D4, WIN1, H1.

### WIN1 — Windows port (time-boxed, 150 min)

**Goal and reason.** Last step of the preview. Nothing has been tried on Windows; the web lab
already runs there in a browser, so this is the local server and the files. Take the short way:
clang from llvm-mingw under MSYS2 bash, so pthreads, the atomics and the scripts work as they are.
No MSVC.

**The blockers, from `ccore/results/ci.md` "Windows: what blocks".**

| Blocker | Where | Fix |
|---|---|---|
| BSD sockets, `poll`, `sigaction`, `MSG_NOSIGNAL` | `host/server.c:6-18`, `:112`, `:622-627`, `:677` | `host/net.h`: six functions over POSIX or Winsock (`WSAStartup`, `closesocket`, `WSAPoll`, `SetConsoleCtrlHandler`) |
| `rename` does not replace; `/dev/urandom`; UTF-8 paths | `host/platform_native.c:55`, `:72`, `host/main.c:34` | `host/platform_win.c`: `MoveFileExW`, `BCryptGenRandom`, wide-character paths |
| POSIX file calls in the session layer | `session/session.c:423-426`, `:602`, `:643` | move them behind `cc_platform` (a layer leak today: L5 must not call the system) |
| `getrandom()` | `src/guid.c:293` | `BCryptGenRandom` |
| pthreads, `sysconf` | `src/jobs.c:7-9` | winpthreads (llvm-mingw); `GetSystemInfo` for the core count |
| bash scripts with GNU tools | `build.sh`, `deps.sh`, `test/*.sh`, `ci/run.sh` | MSYS2 on the runner |
| the kernel's `.so` | `deps.sh` | static archives only, as MAC1 |
| `xdg-open` | R1's `open` | `ShellExecuteW` |

**Do, in this order; stop where the time box ends and write what is left.**
1. Job `ccore-windows` (`windows-latest`, MSYS2 CLANG64, `continue-on-error` until green):
   `CC_NO_KERNEL=1 test/run.sh`. Fix `guid.c`, `jobs.c`, the session's file calls.
2. `host/net.h`, `platform_win.c`: `test/host.sh` (host_test and lab_boot with the runner's Chrome).
3. manifold and `build.sh release`: `bimble-ccore-<version>-x86_64-windows.zip` in `ccore-release`.
4. `results/ci.md`: measured rows replace the estimates.

**Owns.** `ccore/host/net.h`, `host/net_posix.c`, `host/net_win.c`, `host/platform_win.c`,
`host/server.c`, `host/main.c`, `session/session.c` (only the file calls), `session/platform.h`
(coordinate with H1's `list_dir`: merge H1 first if both are open), `ccore/build.sh`, `deps.sh`,
`.github/workflows/ccore.yml`, `ccore/results/ci.md`. **Hooks.** `src/guid.c`, `src/jobs.c` (one
`#ifdef` each). **Do not touch.** `release.yml`, `session/calls.c`, `dev.sh`.

**Read first.** `ccore/results/ci.md`, MAC1's changes, `ccore/host/server.c` (all),
`host/platform_native.c`, `session/platform.h`, `session/session.c:420-650`,
`.github/workflows/release.yml:92-96`.

**Done check.** `ccore/ci/run.sh fast` on the dev machine (Linux and the POSIX socket file still
pass), then the run of `ccore.yml` on the ticket's branch: give the URL and which of steps 1–3
are green.

**Proves.** What runs on Windows, measured on a runner. **Depends on.** MAC1.
**Parallel with.** D3, D4, L2, H1.

### H1 — The history store: read Rust-written folders

**Goal and reason.** A model's history lives in `<model>.bimble-history/` (ADR 0017): objects
named by the BLAKE3 hash of their canonical JSON, trees, commits, branches. Offices that used the
Rust app have such folders. The C core must read them before it writes one: versions, who, when,
what changed, and any old state of the model. The C core has BLAKE3 and canonical JSON already.

**Do.**
- Dumper mode `history`: with `bimble_core::repo`, from `frame.json`, fixed times and authors
  (`alice@example.org`, `bob@example.org`): four commits on `main` (rename a beam, move a column,
  add a sheet), a branch `variant` with one commit, one merge. Writes the folder as
  `golden/history/frame.bimble-history/` (a fixture: small JSON objects) and `log.txt`,
  `branches.txt`, `diff_<a>_<b>.txt` (`version::diff`), `status.txt`, `model_at_<n>.json`, and
  the error texts for a damaged object, an unknown revision and a bad format line.
- `src/repo.c/.h` (L4, the first file of the store layer; all files through `cc_platform`):
  open (`repo.rs:171-194`, format line :26), read an object and **check its hash**
  (`:251-285`, budget 256 MB :28), `commit_info` :286, `tree` :290, `read_tree` → model
  (`:328-356`), `head` :357, `branch_commit` :379, `branches` :411, `resolve` (branch, hash,
  prefix, `HEAD~n`; `:470-525`), `model_at` :526, `log` (walk at most 1,000,000; `:532-546`),
  `status` :547. `src/diff.c`: `version.rs:127-334` (`Change`, `FieldChange`, `diff`).
- `cc_platform.list_dir` (one function; `platform_native.c`, and a stub that answers "no
  folders" in wasm).
- Session: queries `versions {rev?, limit?}`, `branches`, `version_changes {a, b}`, `pending`
  (the status against HEAD). Read only: no commit, no switch. `History.vue` fills its
  `data-missing` rows (22–45) from them; the wasm lab shows "no history folder in the browser".

**Owns.** `ccore/src/repo.c/.h`, `src/diff.c/.h`, `ccore/session/versions.c/.h`,
`ccore/test/repo_test.c`, `ccore/test/versions_test.mjs`, `ccore/golden/history/`,
`web-ui/src/pages/History.vue`, `HistorySide.vue`. **Hooks.** `session/platform.h` and each
platform file (`list_dir`), `calls.c` (one `if` in `call_query`), `bimble.ts`, `ccore_golden.rs`
(one mode), `build.sh` (`VIEW_DENY`), `ports.mjs`, the layer table in `ccore/README.md`.
**Do not touch.** `src/history.c` (the undo history: another thing), `model.h`, `file.c`.

**Read first.** `crates/bimble-core/src/repo.rs:1-590`, `version.rs`, `file.rs` (`shared`: how a
tree resolves shared meshes, `repo.rs:243`), `ccore/src/guid.h:31-33` (`cc_blake3`), `json_r.h`,
`session/platform.h`, `web-ui/src/pages/History.vue`, ADR 0017, ADR 0038.

**Done check.**
```sh
cd ccore && test/run.sh repo && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh repo \
 && CCORE_PORT_BASE=auto node test/versions_test.mjs \
 && cargo run -j 2 -p bimble-api --example ccore_golden -- --check
```
`repo_test`: every golden text byte for byte; each `model_at` saves to the golden's bytes; one
flipped byte in an object gives Rust's "damaged history" text; a GEO1 model's georeference
survives (`repo.rs:1080`), or one DEVIATIONS line if GEO1 is not merged.

**Proves.** Golden vs Rust on a folder Rust wrote. **Depends on.** Nothing in round 3 (it can
start in wave A if a builder and the Rust slot are free). **Parallel with.** D3, D4, L2, WIN1.
**Blocks.** H2.

---

## Wave D

### D5 — Sheet composition: views on a sheet, `sheet_svg`, SVG export

**Goal and reason.** The sheet is what the engineer sends. Rust places each view's bare drawing
at its scale and position, adds the sheet's texts, one legend, the frame and the title block
(`sheetdraw.rs`). With this ticket the Sheets page shows the real paper and the lab exports SVG.

**Do.**
- `src/draw_layout.c/.h`: `sheetdraw::draw` (`sheetdraw.rs:60-226`: each placed view through
  `plan()` bare at its scale, `shift` :47, the note for a view that cannot be drawn, the merged
  legend, the sheet's texts, `frame_and_title_block`), `scales` :228, `SheetInfo` with each
  placed view's box, frame and segments (`:23-45`).
- Session (`session/paper.c`): `sheet_svg {id}` as `calls.rs:487-512` (same options: the
  template with the project information, the project name from the file name, today's date from
  the platform clock) and out of the stub list (`calls.c:953`); `view_svg {view}` for one view
  (`paper.rs:16-22`); the answer is cached per `rev`. `sheet_layout` (SH1) now gives the true
  boxes from `SheetInfo` and drops `"box": "estimate"`.
- The actions `on_paper` ("Export view… (PDF, SVG)", `calls.c:97`) and a sheet export: SVG now
  (the native host writes the file, the wasm lab downloads it, as `save` does); PDF answers "not
  yet" until D6.
- Page: `Sheets.vue` shows the SVG as an `<image>` (it already does: the SVG holds user text, so
  never in the DOM) under SH1's handles.

**Owns.** `ccore/src/draw_layout.c/.h`, `ccore/session/paper.c/.h`, `ccore/test/draw_layout_test.c`,
`ccore/test/paper_test.mjs`, `ccore/test/lab_sheets.mjs` (extend). **Hooks.** `calls.c` (the stub
list, one `if` in `call_query`, two action rows), `session/sheets.c` (the boxes), `bimble.ts`,
`ccore.ts` (the download), `build.sh`. **Do not touch.** `draw_plan.c`, `draw_annot.c`,
`draw_view.c` (report a fault), the dumper.

**Read first.** `crates/bimble-drawing/src/sheetdraw.rs` (all), `crates/bimble-viewer/src/calls.rs:487-512`,
`viewing.rs:599-660` (`export_sheet`), `paper.rs:16-60`, `crates/bimble-core/src/sheet.rs:56-97`,
SH1's `sheets.h`, `web-ui/src/pages/Sheets.vue`.

**Done check.**
```sh
cd ccore && test/run.sh draw_layout draw_annot draw_view && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh draw_layout \
 && CCORE_PORT_BASE=auto node test/paper_test.mjs && CCORE_PORT_BASE=auto node test/lab_sheets.mjs \
 && ./build.sh wasm > /dev/null && CCORE_PORT_BASE=auto node test/wasm_run.mjs
```
`draw_layout_test`: each `sheet/<number>.svg` and `.info.txt` byte for byte. `paper_test.mjs`:
`sheet_svg` of `drawing/model.json` equals the golden when the date and project are the golden's
(a test clock); the native and the wasm answers are equal. `lab_sheets.mjs`: the paper image is
drawn; moving a view changes it.

**Proves.** Golden vs Rust; the page by the lab test. **Depends on.** D3, D4, SH1.
**Parallel with.** D6, H2. **Note.** State in the report the size of `ccore.wasm` before and
after D1–D5 (`build.sh wasm-size`).

### D6 — The PDF writer

**Goal and reason.** The last step to paper: the same items as a one-page vector PDF with real
text (`sheet.rs:375-541`). Rust uses `pdf-writer` and deflate; a byte-equal file would need the
same deflate. So the oracle is the **page's content stream** (operators and numbers, byte for
byte, from `drawing/pdf/*.content`), and the container is the C core's own, with a DEVIATIONS
entry.

**Do.**
- `src/pdf_w.c/.h`: the catalog, one page of the paper's size, the content stream of
  `sheet.rs:400-535` (areas even-odd, hatches clipped, pens and dashes, text with its anchor and
  angle; `text_start` :148, `win_ansi` :612), the information dictionary, a cross-reference
  table. Streams: stored deflate blocks first; a fixed-Huffman deflate in `src/deflate.c` if time
  allows (then give the file sizes with and without).
- Fonts. Helvetica first (not embedded; `SheetFont::Helvetica`, `style.rs:232-239`). Then Roboto
  embedded as Rust does (`embed_font`, `sheet.rs:543-610`: Type 0, 2-byte glyph ids, widths and
  a ToUnicode map of the glyphs used; this needs the font's `cmap` and `hmtx`, a small reader in
  `src/ttf.c`). The two files are 350 KB each: they do **not** go into the wasm. The carrier
  hands them over (the native host reads `web/fonts/`, the page fetches them on the first
  export). Write this as a new ADR (the next free number), Accepted under Pierre's delegation.
- Export: `export_pdf {sheet | view}` on D5's export path; the "not yet" answer goes away.

**Owns.** `ccore/src/pdf_w.c/.h`, `src/ttf.c/.h`, `src/deflate.c/.h`, `ccore/test/pdf_test.c`,
`ccore/test/pdf_check.py`, the ADR and its index row. **Hooks.** `session/paper.c` (one export
function), `calls.c` (nothing if D5's action row is enough), `ccore.ts` (fetch the fonts),
`build.sh`, R1's archive list (`web/fonts/`). **Do not touch.** `paper.c` (`to_svg`), the dumper.

**Read first.** `crates/bimble-drawing/src/sheet.rs:375-740`, `fonts.rs`, D1's `paper.h` and
`golden/drawing/roboto.txt`, `ccore/src/inflate.h` (the gzip writer's stored blocks), L1's
`ref_pdf.h`, ADR 0021.

**Done check.**
```sh
cd ccore && test/run.sh pdf && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh pdf \
 && python3 -I test/pdf_check.py out/tcc/pdf/ && CCORE_PORT_BASE=auto node test/lab_sheets.mjs
```
`pdf_test`: the content stream of each case equals `drawing/pdf/<name>.content`; the C reader
of L1 reads each C-written PDF back to the same polylines as it reads from Rust's file.
`pdf_check.py`: `qpdf --check` and `pdftotext` find the title block's texts (if a tool is
missing, print it and fail: do not skip silently).

**Proves.** Content: golden vs Rust. Container and fonts: own checks, DEVIATIONS.
**Depends on.** D2 (items and style); the export hook on D5; L1 for the read-back check (without
L1, `pdf_check.py` alone). **Parallel with.** D5, H2.

### H2 — Commit and synchronize: collaboration, last

**Goal and reason.** Two engineers on one model through a shared folder (ADR 0038): commit,
fetch, merge by GlobalId, push. Rust has it in `repo.rs` and `remote.rs`; the lab's Synchronize
page waits for it. Objects are canonical JSON named by their hash, so the proof is strong: **the
C core must write the same object names as Rust** for the same model, message, author and time.

**Do.**
- Extend H1's dumper output only if needed (then it takes the Rust slot): the commit hashes of
  the golden folder are already the oracle.
- `src/repo.c`: `init_dir` :156, `put_bytes` :212 (write to a temp name, rename), `write_tree`
  :295, `commit` :442 ("nothing to commit" :39), `create_branch` :560, `switch` :572.
- `src/merge.c`: `merge_models` (`repo.rs:628-893`: object by object, then field by field;
  `Conflict`, `Prefer`), `merge_base` :914, `merge` :922-995.
- `src/remote.c`: `remote_of`, `set_remote` (`remote.rs:32-52`), `open_or_init` :54,
  `copy_commits` :79, `fetch` :119, the lock (`:128-165`), `push` (fast-forward only, :167),
  `sync` :365 and `sync_working` :440 (three attempts, :29). **Not now:** borrowing
  (`:266-345`) and `clone`/`join`; write them in "left for later".
- Session and page: the actions `commit {message}` and `synchronize` (`calls.c:111`), query
  `collab` no longer the stub (`calls.c:748`; shape of `collab.rs:264-300`), query `sync` for
  conflicts; a conflict stops the sync and pushes nothing (`remote.rs:671`); choosing a side is a
  later ticket. Native only; the wasm lab says so.

**Owns.** `ccore/src/merge.c/.h`, `src/remote.c/.h`, `src/repo.c` (after H1),
`ccore/session/collab.c/.h`, `ccore/test/remote_test.c`, `ccore/test/collab_test.mjs`,
`web-ui/src/pages/Synchronize.vue`. **Hooks.** `calls.c` (two actions, two queries),
`session/platform.h` (`make_dir`, `remove_file` if missing), `bimble.ts`.
**Do not touch.** `src/history.c`, `model.h`, `host/server.c`.

**Read first.** `crates/bimble-core/src/repo.rs:140-470` and `:589-995`, `remote.rs` (all, with
its tests at 536–800: they are the cases to port), `crates/bimble-viewer/src/collab.rs`,
`web-ui/src/pages/Synchronize.vue`, H1's `repo.h`, ADR 0017, ADR 0038, ADR 0068 rule 7.

**Done check.**
```sh
cd ccore && test/run.sh remote repo && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh remote \
 && CCORE_PORT_BASE=auto node test/collab_test.mjs
```
`remote_test`: replaying the golden's four commits from `frame.json` gives the golden's commit
hashes and a folder equal file by file; the two-person cases of `remote.rs:536-700` (sync, a
push that is not a fast-forward, a conflict that pushes nothing); a damaged remote object is
refused and not copied (`:758`).

**Proves.** Same hashes as Rust (the strongest golden of the round); the flows by own tests.
**Depends on.** H1. **Parallel with.** D5, D6. **Cut first** if time is short.

---

## Not in this round

| Item | Why it waits | Next step |
|---|---|---|
| A `calls.c` router table (ops and queries registered by each `session/*.c`) | this round only hooks the file; 16 tickets add about 30 hooks | one session after wave D; it ends the hotspot |
| Style files and plan templates from a file (`style.rs:429`, `template.rs:234`), the style editor | the defaults draw Pierre's plans | after D5 |
| Sheet texts and dimensions on the sheet, placing by drag from the navigator | SH1 gives place and move | after D5 |
| Borrowing elements, clone and join of a shared folder, choosing a side in a conflict | H2 is the flow without conflicts | after H2 |
| A webview window on macOS and Windows (ADR 0069 rule 2, second step) | the browser plus the local server is the preview | after WIN1 |
| Signed installers (`.dmg`, `.msi`) | ADR 0043: unsigned for now, brew is enough | Pierre's decision |
| The boolean kernel in the wasm lab | openings are drawn uncut in the preview | measure manifold in wasm (size) first |
| IFC grids into `grids`, polygon slab, C2, C3, B1, B2, LCA | as in round 2's table | unchanged |
| The final Rust release tag, and merging `rewrite/c-core` to `main` | Pierre's decisions (ADR 0067) | ask Pierre; PG1 and the brew formula already need small pull requests to `main` |

## Risks

- **Two tickets need `main`** (PG1's `pages.yml`, MAC1's formula) while the port lives on
  `rewrite/c-core`. Each is a small pull request of one file. A tag for the preview must not
  start with `v`.
- **Byte-equal SVG** depends on float order (f32 meshes, f64 paper) and on `{:.3}`. D1 settles
  the formatter; D2 is where a last-digit difference will show first. No tolerance without a
  DEVIATIONS entry.
- **D1 carries the goldens of five tickets.** If a later ticket finds a golden missing, it stops
  and reports; one builder regenerates with the Rust slot. Do not edit `crates/*/src` to help.
- **PDF and DXF readers are untrusted input** at its worst: every loop and recursion bounded,
  ASan on bad fixtures, sizes capped before allocation.
- **Fonts and wasm size.** Drawing code goes into the editor wasm, not the viewer (each ticket
  adds its files to `VIEW_DENY`); the fonts stay outside both. D5 reports the size.
- **CI time.** The gate is 518 s, 91 % of it ASan. Sixteen tickets add suites: if the gate passes
  12 minutes, split ASan into its own job (K1's owner), do not drop it.
- **Ports are proven only on runners.** MAC1 and WIN1 done checks are a run URL; a builder cannot
  prove them on the dev machine.
