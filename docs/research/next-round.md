# C core: the next round of tickets

- Date: 25 Nov 2026. Author: Fable 5.2 (PLAN station). Branch: `rewrite/c-core` at `14c3832`.
- Follows `docs/research/core-architecture.md` (tickets A1–C6) and ADR 0067–0069.
- Done: A1–A7, B3, B4, B5, B6, C1, C6, E1, J1, P1, R1, V1. Running: R2 (`cc/r2`), C4 (`cc/c4`).
- Open from the old plan: B1, B2, B7, C2, C3, C5. This round takes B7 and C5. B1, B2 and C2 are
  performance or paper work: they wait (ADR 0069 rule 6).

Twelve tickets, each for one builder session (60–120 min, Opus, high effort). The order follows
Pierre's priorities: (1) IFC in and out, (2) editing and 3D, (3) the 2D view; then Mission 3.

## Order and waves

| # | Ticket | Priority | Wave | Proves |
|---|---|---|---|---|
| 1 | K1 CI for the C core | all | 1 | every suite on a clean Linux machine |
| 2 | U1 Flaky pixel test, status hint, read-only line | 2 | 1 | lab tests |
| 3 | X1 Relations in the model | 1 | 1 | new behaviour, own tests, DEVIATIONS |
| 4 | Q1 Quantities in the core | Mission 3 | 1 | golden vs Rust |
| 5 | M1 MCP over stdio (B7) | Mission 3 | 1 | golden vs Rust (transcript) |
| 6 | S1 Storeys and plan views of an opened IFC | 1, 3 | 2 | golden vs Rust or DEVIATIONS |
| 7 | G1 Levels and grids in the model (C5a) | 3 | 2 | new behaviour, own goldens, DEVIATIONS |
| 8 | T1 Measure tool | 2 | 2 | query golden vs Rust; tool is new |
| 9 | W1 Viewer-only wasm | 1 | 2 | size check, byte-equal answers |
| 10 | X2 Assemblies and connections: verbs, IFC export and import | 1 | 3 | IfcOpenShell, own goldens, DEVIATIONS |
| 11 | G2 Grids and levels in the plan view and the page (C5b) | 3 | 3 | lab test |
| 12 | Q2 Quantities in the session and the page | Mission 3 | 3 | golden vs Rust, lab test |

- **Wave 1 starts now**, beside R2 and C4. No wave 1 ticket touches a file that R2 or C4 changed.
- **Wave 2** starts when C4 and R2 are merged, and X1 for G1.
- **Wave 3** starts when its dependencies are merged (X1, G1, Q1, T1).
- Tickets of one wave run in parallel, each in its own worktree with `CCORE_PORT_BASE=auto`.

## Hotspots: who owns them in each wave

Files that many tickets need. One owner per wave edits freely. The others add at most one hook
(under 10 lines, add-only, at the place named in the ticket) and put their code in their own file.
Merge the owner first, then rebase the others.

| File | Now (C4, R2) | Wave 1 | Wave 2 | Wave 3 |
|---|---|---|---|---|
| `ccore/session/calls.c` | C4 | nobody | S1 owns; G1, T1 hook | Q2 owns; X2 hook |
| `web-ui/src/glviewport.ts` | C4 | nobody | T1 owns | G2 owns |
| `web-ui/src/bimble.ts` | C4 | nobody | T1 hook | G2, Q2 hook (types and one function each) |
| `ccore/src/model.h`, `model.c`, `command.c`, `file.c` | — | X1 owns | G1 owns | nobody |
| `ccore/src/verbs.c`, `verb_table.c` | C4 | nobody | G1 hook (table rows) | X2 hook (table rows) |
| `ccore/src/ifc_in.c`, `ifc_read.c`, `step.c`, `scene.c` | R2 | nobody | S1 (`ifc_read` read only) | X2 hook in `ifc_in.c` |
| `ccore/src/ifc_out.c` | — | nobody | G1 hook | X2 hook |
| `ccore/build.sh` | R2 | nobody | W1 owns | nobody |
| `ccore/test/ports.mjs` | C4 | nobody | S1, T1: one row each | G2, Q2: one row each |
| `crates/bimble-api/examples/ccore_golden.rs` | — | Q1 owns | S1, T1: one mode each | Q2: one mode |
| `ccore/golden/DEVIATIONS.md` | C4 creates it | X1 appends | G1, S1 append | X2 appends |
| `ccore/golden/README.md` | — | Q1, M1: one section each | one section each | one section each |

New verbs go in a new file (`src/verbs_<topic>.c`) and get rows in `cc_verb_c_additions`, never
in `cc_verb_table`: the 14 Rust schemas and the `verbs` list stay byte-equal.

## Rules for every ticket

1. Read first: `CLAUDE.md`, `ccore/README.md` (layers), ADR 0068, and the files named in the ticket.
2. Layers: L0–L3 have no files, clock, threads or globals. A file includes only its layer and lower.
3. Mutations go through Commands; writers go through verbs. GlobalIds are never regenerated.
4. Serialization stays canonical. A new model field is **left out when empty**, so every existing
   file keeps its bytes and its hash. Each ticket that adds a field tests this.
5. Untrusted input: budgets, bounded recursion, no abort. Out of memory is an error value.
6. A golden comes from Rust (`ccore_golden` example, `--check` stays green). Where Rust has no
   such behaviour, the ticket writes its own golden and one entry in `ccore/golden/DEVIATIONS.md`
   (what differs from the oracle, why, which ADR).
7. A regression test must fail on the old code. Say in the report how you checked.
8. Memory: the dev machine has 16 GB. One Rust build or one wasm build at a time on the machine;
   `cargo … -j 2`. Q1, M1 (wave 1) and S1, T1 (wave 2) each need a Rust build: do not start two at
   once.
9. Synthetic fixtures only. No client file, no client name.
10. Do not commit `ccore/out/` or `web-ui/node_modules`. No performance claim without a number.

**The gate** (every ticket; exit 0 and no `FAIL` line). After K1 is merged it is `ccore/ci/run.sh`.

```sh
cd ccore && ./build.sh tcc > /dev/null && test/run.sh \
 && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh \
 && ./build.sh wasm > /dev/null && CCORE_PORT_BASE=auto node test/wasm_run.mjs \
 && CCORE_PORT_BASE=auto test/host.sh
```

Each ticket's done check is the gate **and** its own command below. A `.mjs` test prints
`N passed, 0 failed` and exits 0.

---

## Wave 1

### K1 — CI for the C core

**Goal and reason.** A GitHub Actions job runs the C suites on each push and pull request. Today
no machine but the dev laptop runs them: a builder can break a suite and nobody sees it. One
script holds the gate, so CI and builders run the same thing. Linux first. macOS and Windows have
never built the core: this ticket measures what is missing, it does not port.

**Do.**
- `ccore/ci/run.sh [fast|full]`: `fast` is the gate above. `full` adds the Chromium lab tests
  (`lab_boot`, `lab_gl`, `lab_tool`, `lab_place`, `lab_section`, `lab_plan`, `lab_wasm`,
  `lab_links`) and the other `.mjs` suites, one after the other, each with `CCORE_PORT_BASE=auto`.
  It prints one line per suite and a last line `ci: N suites, 0 failed`.
- `.github/workflows/ccore.yml`, job `ccore-linux` on `ubuntu-latest`: apt `tcc clang lld cmake`
  through `scripts/apt-install.sh`; the manifold build of `ccore/deps.sh` behind `actions/cache`
  (key: `MANIFOLD_TAG` and the hash of `deps.sh`); Node 24 and `npm ci` in `web-ui/`; then
  `ccore/ci/run.sh full`. The lab tests name `/usr/bin/chromium`: accept a `CHROMIUM` variable in
  one shared place if there is one, else link the image's Chrome to that path in the workflow.
- A second job `ccore-macos` with `continue-on-error: true`: clang only, `CC_NO_KERNEL=1`,
  `test/run.sh`. It may fail: its log is the feasibility evidence.
- `ccore/results/ci.md`: the times per step on the dev machine; for macOS and Windows a table of
  what blocks (`host/platform_native.c` and `host/server.c` use POSIX; `deps.sh` uses `stat -c`,
  `sha1sum`; tcc on macOS arm64), with the cost of each in words, not days.

**Owns.** `.github/workflows/ccore.yml`, `ccore/ci/`, `ccore/results/ci.md`, one "CI" paragraph in
`ccore/README.md`. **Do not touch.** `.github/workflows/ci.yml` (Rust), `ccore/build.sh`,
`ccore/test/*.mjs` (U1 edits `lab_links.mjs`), any source.

**Read first.** `.github/workflows/ci.yml`, `ccore/test/run.sh`, `ccore/test/host.sh`,
`ccore/deps.sh`, `ccore/test/ports.mjs`, `scripts/apt-install.sh`.

**Done check.**
```sh
ccore/ci/run.sh full && python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ccore.yml'))"
```
The workflow itself is proven on the first push: the orchestrator reads that run. If `lab_links`
fails here before U1 is merged, list it as known and keep it in the `full` list.

**Proves.** The suites pass outside a builder's worktree. **Depends on.** Nothing.
**Parallel with.** All of wave 1.

### U1 — The flaky pixel test, the status-bar hint in plan, the read-only line

**Goal and reason.** Three small faults that cost trust. `lab_links.mjs` failed once: its pick
loop reads pixels (line 219) without waiting for a drawn frame, while `lab_section.mjs` waits for
`window.__bbGl.frames` to grow. In a plan view the status bar says "Drag: orbit", but a plan has
no orbit. The properties panel shows a linked or read-only element like an editable one: the user
types a name and gets an error.

**Do.**
- `lab_links.mjs`: every pixel read waits for a frame counted after the change it tests (the
  `settle` pattern of `lab_section.mjs:209`). Then run it 20 times.
- `StatusBar.vue`: when the active view is a plan (`openViews`), the hint is a new key
  `status.hintPlan` ("Drag: pan · Scroll: zoom"), in fr and en; regenerate `keys.gen.ts` with the
  repo's script.
- `Properties.vue`: one line above the fields, "Linked model, read-only" (with the link's name) or
  "Read-only file: import it to edit", and the fields disabled. Use what the page already has
  (`state`, `links`, the selection's `link`). If the C side lacks a field, stop and report: do
  not edit `calls.c`.
- Extend `lab_plan.mjs` (hint text in plan and back in 3D) and `lab_links.mjs` (the line shows for
  the linked element; the name field is disabled).

**Owns.** `ccore/test/lab_links.mjs`, `ccore/test/lab_plan.mjs`, `web-ui/src/shell/StatusBar.vue`,
`web-ui/src/Properties.vue`, `web-ui/src/i18n/parts/statusbar.ts`, a new
`web-ui/src/i18n/parts/` entry if needed, `web-ui/src/i18n/keys.gen.ts`.
**Do not touch.** `glviewport.ts`, `bimble.ts`, `ccore/session/`, `ccore/src/`.

**Read first.** `ccore/test/lab_links.mjs:150-255`, `ccore/test/lab_section.mjs:140-215`,
`web-ui/src/shell/StatusBar.vue`, `web-ui/src/Properties.vue`, `ccore/session/links.h`,
`anthropic-skills:bimble-design` for the line's style.

**Done check.**
```sh
cd ccore && for i in $(seq 20); do CCORE_PORT_BASE=auto node test/lab_links.mjs | tail -1 | grep -q ' 0 failed' || exit 1; done \
 && CCORE_PORT_BASE=auto node test/lab_plan.mjs && (cd ../web-ui && npm run typecheck)
```

**Proves.** Lab tests; the flake fix by 20 green runs (state in the report how many of 20 failed
before, if you can reproduce it). **Depends on.** Nothing. **Parallel with.** All of wave 1.

### X1 — Relations as objects in the model

**Goal and reason.** ADR 0068 rule 10: full IFC compatibility needs assemblies and typed
connections, as objects with GlobalIds. It is cheap now and costly once files exist
(`core-architecture.md` §8). This ticket gives the model its relation table and the two Commands.
Verbs and IFC come in X2. There is no Rust oracle for this.

**Do.**
- `cc_relation {gid, kind, relating, related[], properties[]}` and `cc_model.relations` (sorted by
  id). `kind` is the IFC class name as text, checked against a table: `IfcRelAggregates`,
  `IfcRelConnectsElements`, `IfcRelConnectsWithRealizingElements`, `IfcRelDefinesByType`. Openings
  stay embedded in their element (`IfcRelVoidsElement` is not stored here).
- `IfcElementAssembly` as an element class with no generator of its own (its shape is its parts).
  Follow "open classes" only as far as needed: keep the enum, add the class; note in the ADR
  what a full class-name store would change.
- Validation: every id exists; no relation to itself; `IfcRelAggregates` has no cycle (bounded
  walk) and one whole per part; `related` is sorted, without duplicates, at most 10,000.
- Commands `AddRelation`, `RemoveRelation`, `SetRelation`, each with its inverse. `RemoveElement`
  of an element in a relation is refused with a clear text (X2 decides what `delete` does).
- File v1 (JSON) and v2 (CBOR): key `relations`, left out when empty. Canonical order.
- ADR 0070 "Relations as model objects" (Accepted under Pierre's delegation, as ADR 0068), a row
  in `docs/adr/README.md`, an entry in `DEVIATIONS.md`.

**Owns.** `ccore/src/model.h`, `model.c`, `command.c`, `command.h`, `file.c`, new
`ccore/src/relation.c/.h` (most of the code), `ccore/test/relation_test.c`,
`ccore/golden/relations/` (a model with one assembly and one connection, v1 and v2),
`docs/adr/0070-*.md`, `docs/adr/README.md`.
**Do not touch.** `verbs.c`, `verb_table.c`, `view.c`, `ifc_in.c`, `ifc_out.c`, `session/`.

**Read first.** `core-architecture.md` §8, `ccore/src/model.h`, `command.h`, `file.c` (how views
and sheets were added in `6cbf2ca`), `crates/bimble-core/src/model.rs:500-520`, ADR 0003, ADR 0042.

**Done check.**
```sh
cd ccore && test/run.sh relation file file2 verbs_all two_sessions \
 && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh relation file file2 \
 && cargo run -j 2 -p bimble-api --example ccore_golden -- --check
```
`relation_test` covers: round trip v1 and v2 with equal model hash; apply, undo, redo; each
validation rule; and that `golden/frame.json` and `golden/bin/frame.bimble` reload and save to the
same bytes (no `relations` key).

**Proves.** New behaviour with its own tests and a DEVIATIONS entry. **Depends on.** Nothing.
**Parallel with.** All of wave 1. **Blocks.** G1 (same files), X2.

### Q1 — Quantities in the core

**Goal and reason.** Mission 3 quick win: volume, area, length and mass per element, and the
schedule tables grouped by material or class. Rust has all of it (`bimble-drawing/src/schedule.rs`:
`measure`, `density`, `schedule`, `to_csv`, `table_json`; fields in `bimble-core/src/schedule.rs`).
The C model already stores the schedule definitions (A6a), but the session answers
`schedule_rows` and `schedule_csv` with a stub (`calls.c:896`). This ticket ports the computation
as a pure L3 module. Q2 wires it.

**Do.**
- `src/quantity.c/.h`: `cc_measure(scene, element)` (generator formulas where Rust uses them, mesh
  volume by the divergence theorem elsewhere, openings as Rust treats them), density from the
  material's grade, `cc_schedule_table(scene, def)` with groups, subtotals and total,
  `cc_schedule_csv`, `cc_schedule_json(table, max)`.
- The cell text and rounding must match Rust byte for byte: sum in the same order.
- Goldens from Rust: a new mode in `ccore_golden.rs` writes, for `frame.json` and `frame2.json`
  and each built-in schedule, `golden/schedule/<model>/<name>.json` (`table_json` with max 50),
  `.csv`, and `measures.txt` (one line per element: id, volume, area, length, mass, as `{:?}`).

**Owns.** `ccore/src/quantity.c/.h`, `ccore/test/quantity_test.c`, `ccore/golden/schedule/`,
`crates/bimble-api/examples/ccore_golden.rs`, one section in `ccore/golden/README.md`.
**Do not touch.** `session/`, `model.h` (X1), `web-ui/`.

**Read first.** `crates/bimble-drawing/src/schedule.rs`, `crates/bimble-core/src/schedule.rs`,
`crates/bimble-geom/src/lib.rs:56-70`, `ccore/src/scene.h`, `ccore/src/view.h` (`cc_schedule`),
`ccore/src/library.h`, ADR 0029, ADR 0031, `ccore/golden/README.md` "Regenerate and check".

**Done check.**
```sh
cd ccore && test/run.sh quantity && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh quantity \
 && cargo run -j 2 -p bimble-api --example ccore_golden -- --check
```
`quantity_test` compares every file of `golden/schedule/` byte for byte.

**Proves.** Golden vs Rust. If a float differs in the last digit, find the operation order; do not
add a tolerance without a DEVIATIONS entry. **Depends on.** Nothing. **Parallel with.** All of
wave 1 (one Rust build at a time with M1). **Blocks.** Q2.

### M1 — MCP over stdio from the verb table (B7)

**Goal and reason.** ADR 0068 rule 5: agents reach the model through the same verbs as the UI.
Rust has `bimble mcp model.bimble` (228 lines, JSON-RPC 2.0, one message per line). The C verb
table already gives names, summaries and schemas. This is a new carrier (L6) on `cc_call`: no
change to the session.

**Do.**
- `host/mcp.c`: `bimble-ccore mcp <model>` reads lines from stdin (at most 1 MiB; longer lines
  are refused without parsing), answers on stdout, logs on stderr. `initialize`, `tools/list`,
  `tools/call`, the same tool names, texts and error codes as `crates/bimble-cli/src/mcp.rs`.
  Tools come from the verb table and the model queries; nothing is written until `save`.
- A transcript golden: a script feeds the same request lines to the Rust server and stores the
  answers in `ccore/golden/mcp/` (`requests.jsonl`, `responses.jsonl`; new GlobalIds masked as
  `golden/README.md` describes). Cover: initialize, list, each query, three verbs, an unknown
  tool, bad JSON, a line over the limit, undo, save to a temp path.
- C additions (the `rename` verb, `dry_run`, `if_rev`) are offered too, tested in a second
  transcript of their own, with a DEVIATIONS entry. Modes read-only / propose / edit: only a
  `--read-only` flag now (verbs refused); `propose` is a later ticket.

**Owns.** `ccore/host/mcp.c`, `ccore/host/mcp.h`, `ccore/host/main.c` (the sub-command),
`ccore/test/mcp_test.mjs`, `ccore/golden/mcp/`, `ccore/tools/mcp_golden.sh`, one section in
`ccore/golden/README.md`, one paragraph in `ccore/README.md`.
**Do not touch.** `session/calls.c`, `verb_table.c`, `verbs.c`, `host/server.c`, `build.sh`
(`host/*.c` is already a glob), `ports.mjs` (stdio needs no port).

**Read first.** `crates/bimble-cli/src/mcp.rs`, `crates/bimble-api/src/dispatch.rs` (list,
describe), `ccore/src/verb_table.h`, `ccore/session/calls.h`, `ccore/host/main.c`,
`ccore/test/verbs_session_test.mjs`, ADR 0048.

**Done check.**
```sh
cd ccore && ./build.sh tcc > /dev/null && node test/mcp_test.mjs \
 && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh verbs_all
```
`mcp_test.mjs` pipes `requests.jsonl` into `out/tcc/bimble-ccore mcp` and compares each answer
line with the golden.

**Proves.** Golden vs Rust (transcript); the C additions by their own transcript.
**Depends on.** Nothing. **Parallel with.** All of wave 1.

---

## Wave 2 (after C4 and R2 are merged; G1 after X1)

### S1 — Storeys and plan views of an opened IFC

**Goal and reason.** Pierre's first use is to open an IFC and look at a floor plan (Mission 1).
Today a read-only scene (an opened IFC or `.bimbleview`) keeps no storeys in the session, so
`open_views` offers no plan. The reader already returns storeys and grid axes
(`ifc_read.h:57-60`) and the view file holds them (`viewfile.h:71-74`): the session drops them.

**Do.**
- The session keeps the storeys (name, elevation) and grid axes of a read-only scene, and of each
  linked model.
- `open_views`, `view_clip` and the plan tabs work on a read-only scene: one plan down and one up
  per storey with the default range of `views.c`. Views of a read-only scene are derived, never
  stored; their ids come from the storey name and elevation (the IFC storey's GlobalId when the
  reader keeps it).
- Check what Rust does first (`crates/bimble-viewer/src/views.rs`, `calls.rs` `open_views`). If
  Rust offers plans for an opened IFC, add a `ccore_golden` mode and match it. If not, write an
  own golden and a DEVIATIONS entry.
- Grid axes of the read-only scene are answered by a query `grid_axes` (G2 draws them).
- Lab test: open `golden/ifc/fixtures/same_level.ifc` and one multi-storey fixture, open a plan
  tab, the pixel count changes with the storey, pick names an element.

**Owns.** `ccore/session/session.c/.h`, `ccore/session/views.c/.h`, `ccore/session/links.c`,
`ccore/session/calls.c` (wave owner), `ccore/test/views_test.mjs`, new
`ccore/test/lab_ifc_plan.mjs`, one row in `ports.mjs`, a synthetic two-storey fixture in
`golden/ifc/fixtures/` if none fits.
**Do not touch.** `glviewport.ts` (T1 owns it; C1's plan camera needs no change. If it does, stop
and report), `ifc_read.c`, `model.h`.

**Read first.** `ccore/session/views.c`, `calls.c` `q_open_views` and `q_view_clip` (590–700),
`session.c` (how the read-only scene is held, commit `40430a4`), `ccore/src/ifc_read.h`,
`viewfile.h`, `ccore/test/lab_plan.mjs`, ADR 0021.

**Done check.**
```sh
cd ccore && CCORE_PORT_BASE=auto node test/views_test.mjs && CCORE_PORT_BASE=auto node test/lab_ifc_plan.mjs \
 && CCORE_PORT_BASE=auto node test/lab_plan.mjs && CCORE_PORT_BASE=auto node test/links_test.mjs
```

**Proves.** Golden vs Rust if Rust has it; else new behaviour, own tests, DEVIATIONS.
**Depends on.** C4 merged (same `calls.c`), R2 merged (reader types). **Parallel with.** G1, T1,
W1. **Not in this ticket.** Georeference in the C model: a later ticket (`model.h`, after G1).

### G1 — Levels and grids in the model (C5a)

**Goal and reason.** A new model needs axes and levels before columns: "2D plans with grid,
levels, dimensions" is in Pierre's creation mission. ADR 0056 defines it, and Rust never built
it: no oracle. This ticket is the model half. G2 draws and edits.

**Do.**
- ADR 0056: set to Accepted under Pierre's delegation (as ADR 0068 words it); update the index.
- `cc_storey.is_storey` (default true, left out of the file when true). `cc_model.grids`:
  GlobalId → `{name, axes: [{id, tag, start_mm[2], end_mm[2]}]}`, left out when empty.
- Commands `SetStorey`, `AddGrid`, `SetGrid`, `RemoveGrid` with inverses. Changing a storey's
  elevation moves its elements because placements are relative: test it on the scene.
- Verbs in `src/verbs_levels.c`: `level_add`, `level_set`, `level_remove` (refused while elements
  are on it), `grid_add`, `grid_axis_set`, `grid_remove`; rows in `cc_verb_c_additions` with
  schemas. A convenience `grid_add` form: `{name, x: [0, 6000, …], y: […], tags?}` makes a
  rectangular grid.
- Queries: `storeys` gains `is_storey` and `height_mm` only when the model uses the new fields
  (the Rust `query.txt` golden must stay equal); new `grids`.
- IFC export: `IfcGrid` with `IfcGridAxis` and `IfcPolyline`, in a new `src/ifc_out_grid.c`, one
  hook in `ifc_out.c`. A model without grids exports the same bytes as today.
- Hooks: `calls.c` only if `grids` does not pass through `model_query` (then one line).

**Owns.** `ccore/src/model.h`, `model.c`, `command.c/.h`, `file.c`, `query.c`, new
`src/grid.c/.h`, `src/verbs_levels.c`, `src/ifc_out_grid.c`, `ccore/test/grid_test.c`,
`ccore/golden/grids/` (model, verb transcript, exported IFC), `docs/adr/0056-*.md`,
`docs/adr/README.md`. **Hooks.** `verb_table.c` (rows), `ifc_out.c` (one call), `calls.c` (one
line at most). **Do not touch.** `glviewport.ts`, `session/views.c` (S1), `web-ui/`.

**Read first.** ADR 0056, ADR 0018, X1's `relation.c` (the pattern for a new table), `verbs.c`
and `verb_table.c` after C4, `ccore/src/ifc_out.c`, `ccore/golden/README.md` "verbs_all",
`scripts/check_ifc.py`, `scripts/validate_ifc.py`.

**Done check.**
```sh
cd ccore && test/run.sh grid file file2 verbs_all query ifc_out \
 && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh grid \
 && cargo run -j 2 -p bimble-api --example ccore_golden -- --check \
 && python3 -I ../scripts/validate_ifc.py golden/grids/grid.ifc
```
(`validate_ifc.py` needs `pip install ifcopenshell`; if it is missing, say so and show the
`IfcGrid` lines instead. Do not skip silently.)

**Proves.** New behaviour, own goldens, DEVIATIONS entry; the old goldens prove nothing moved.
**Depends on.** X1 merged (same model files), C4 merged (`verb_table.c`). **Parallel with.** S1,
T1, W1. **Blocks.** G2.

### T1 — The measure tool

**Goal and reason.** Measuring is in Pierre's viewing mission and works on a read-only IFC too.
Rust has it (`bimble-viewer/src/measure.rs`: snap to corner, edge, face; orthogonal lock;
`measurements` query `[{id, label, value_mm}]`). In C the action `measure` exists in the ribbon
table but does nothing, and `measurements` answers `[]` (`calls.c:889`). ADR 0068 rule 2 puts the
tool state in C: the page forwards the pointer.

**Do.**
- `session/measure.c/.h`: tool names `measure_pointer`, `measure_click`, `measure_cancel`,
  `measure_clear` under op `tool`; body as the place tool (`ray`, `ray_tol`, or `at` in plan).
  Snap on the scene's triangles and edges (reuse the snap of `tool.c`; a linear scan is fine, no
  box tree: ADR 0069 rule 6). It honours the section box and hidden elements. Two clicks make one
  measurement; they are session state (no history step, no rev change), kept across a restart in
  the session file if that is one field, else not.
- Query `measurements` as Rust's `state::measurements_json`; `format_mm` as `measure.rs:169`. A
  `ccore_golden` mode writes `golden/measure.txt` for fixed point pairs (label and value text).
- Action `measure` toggles the tool (pressed state in `actions`), Esc cancels.
- Page: `web-ui/src/gl/measure.ts` draws the hover snap marker, the rubber line and the kept
  measurements with their labels, in 3D and in plan. One hook in `glviewport.ts` for the pointer
  and one draw call.
- Works in a read-only scene and on linked models.

**Owns.** `ccore/session/measure.c/.h`, `web-ui/src/gl/measure.ts`, `web-ui/src/glviewport.ts`
(wave owner; keep the change small), `ccore/test/measure_test.c`, `ccore/test/lab_measure.mjs`,
`ccore/golden/measure.txt`, one row in `ports.mjs`. **Hooks.** `calls.c`: one `if` in `call_tool`,
one in `call_query`, the action row, and `measurements` out of the `EMPTY` list (line 889);
`bimble.ts`: the tool call; `ccore_golden.rs`: one mode.
**Do not touch.** `session/tool.c` beyond making its snap function callable, `session/views.c`.

**Read first.** `crates/bimble-viewer/src/measure.rs`, `state.rs:218-240`, `ribbon.rs:685-695`
and `:1596`, `ccore/session/tool.h`, `calls.h` (the tool protocol), `visibility.h`,
`glviewport.ts` (pointer handling near 1020–1045), `ccore/test/lab_tool.mjs`.

**Done check.**
```sh
cd ccore && test/run.sh measure tool visibility && CCORE_PORT_BASE=auto node test/lab_measure.mjs \
 && CCORE_PORT_BASE=auto node test/lab_tool.mjs && CCORE_PORT_BASE=auto node test/lab_section.mjs
```
`lab_measure.mjs`: measure the frame's bay between two column corners in 3D and in plan, the
label is the known distance; the navigator's count is 1; clear gives 0; `rev` did not change.

**Proves.** The query and the label text: golden vs Rust. The tool protocol: new behaviour with
its own tests (C addition, already covered by ADR 0068 rule 2; one DEVIATIONS line).
**Depends on.** C4 merged (`glviewport.ts`, `tool.c`, `calls.c`). **Parallel with.** S1, G1, W1
(rebase the `calls.c` hooks on S1). **Blocks.** G2 (`glviewport.ts`).

### W1 — A viewer-only wasm (time-boxed)

**Goal and reason.** ADR 0069 rule 3: three builds selected by files, and a viewer wasm of at
most 400 KB. The one wasm is 504 KB (211 KB gzip) and grows with each ticket. The viewer needs to
read `.bimbleview`, IFC and `.bimble`, draw, pick, clip, measure and query. It does not need
verbs, Commands, the IFC writer, the place tool or the journal. Do it only if it is cheap: stop
at 90 minutes.

**Do.**
- First measure: a table of bytes per object file in the wasm (`ccore/results/wasm-size.md`).
- `build.sh wasm-view` → `out/wasm/ccore-view.wasm`: a file list in `build.sh` (deny-list of
  editor-only files), and `-DCC_VIEW_ONLY` only at the few call sites that reference them (the
  router answers "this build is view-only" for `verb`, `apply`, `undo`, `redo`, placing, save,
  export). Prefer weak dispatch through a table over `#ifdef` in many places.
- `build.sh wasm` is unchanged (the editor build).
- A size check: the build fails over 400,000 bytes for the view build.
- `test/wasm_view_run.mjs`: on the goldens, every read query, `/mesh`, select, visibility and plan
  answer is byte-equal to the full wasm's; a verb answers the view-only text.
- If the budget is not met in the time box: commit the measurement and the list of the largest
  files, keep `wasm-view` without the hard limit, and say what the next cut is.

**Owns.** `ccore/build.sh`, `ccore/src/wasm_api.c`, `wasm_stub.c`, `ccore/test/wasm_view_run.mjs`,
`ccore/results/wasm-size.md`, `web-ui/scripts/copy-ccore.mjs`, `web-ui/package.json` (one
script), a paragraph in `ccore/README.md`.
**Do not touch.** `session/calls.c` beyond a guard of at most 10 lines at the top of `cc_call`
(coordinate with S1: put the guard in a new `session/view_only.c` and call it from one line).

**Read first.** `ccore/build.sh` (after R2), `docs/research/2026-11-20-wasm-size.md`, ADR 0050,
ADR 0064, ADR 0069, `ccore/test/wasm_run.mjs`, `ccore/src/wasm_api.h`.

**Done check.**
```sh
cd ccore && ./build.sh wasm > /dev/null && ./build.sh wasm-view && node test/wasm_view_run.mjs \
 && CCORE_PORT_BASE=auto node test/wasm_run.mjs && test "$(wc -c < out/wasm/ccore-view.wasm)" -le 400000
```

**Proves.** A measured size (the benchmark is the byte count) and byte-equal answers.
**Depends on.** R2 merged (`build.sh`). **Parallel with.** S1, G1, T1.

---

## Wave 3

### X2 — Assemblies and typed connections: verbs, IFC export and import

**Goal and reason.** X1 stored relations. Without verbs nobody can make one, and without IFC they
do not leave Bimble. This ticket closes the loop that "full IFC compatibility" asks for: an
assembly and a connection survive export, IfcOpenShell reads them, and import brings them back
with the same GlobalIds.

**Do.**
- Verbs in `src/verbs_rel.c`, rows in `cc_verb_c_additions`: `assemble {name, parts[]}` (one
  `IfcElementAssembly` and one `IfcRelAggregates`), `disassemble {id}`, `connect {a, b, kind?,
  realizing?[]}`, `disconnect {id}`. `delete` of a part updates or removes its relations in the
  same step (one undo).
- Query `relations` (`{element?}` filter) and the element detail lists its relations, only when
  the model has some (the Rust query goldens stay equal).
- Export in `src/ifc_out_rel.c`: `IfcElementAssembly`, `IfcRelAggregates`,
  `IfcRelConnectsElements`, `IfcRelConnectsWithRealizingElements`, IFC4 and IFC2x3, relation
  GlobalIds kept. One hook in `ifc_out.c`. A model without relations exports today's bytes.
- Import in `src/ifc_in_rel.c`, one hook in `ifc_in.c`: the same entities read back; unknown
  relation kinds are counted in the import report, not dropped silently.
- `scripts/check_rel.py` (IfcOpenShell): the exported file has the assembly with its parts and the
  connection with both ends, GlobalIds equal to the model's.

**Owns.** `ccore/src/verbs_rel.c`, `ifc_out_rel.c`, `ifc_in_rel.c`, `ccore/test/rel_ifc_test.c`,
`ccore/golden/relations/` (transcript, IFC4, IFC2x3, re-import JSON), `scripts/check_rel.py`.
**Hooks.** `verb_table.c` (rows), `ifc_out.c`, `ifc_in.c` (one call each), `query.c` (one query),
`calls.c` (one line at most; Q2 owns the file).
**Do not touch.** `model.h`, `command.c`, `web-ui/`.

**Read first.** X1's ADR 0070 and `relation.h`, `ccore/src/ifc_out.c`, `ifc_in.c` (after R2),
`crates/bimble-ifc/src/import.rs` (how aggregates are skipped today), `scripts/check_ifc.py`,
`golden/README.md` "ifc/import".

**Done check.**
```sh
cd ccore && test/run.sh rel_ifc relation ifc_out ifc_in verbs_all \
 && CC=clang CFLAGS="-fsanitize=address,undefined" test/run.sh rel_ifc \
 && python3 -I ../scripts/check_rel.py golden/relations/assembly.json golden/relations/assembly.ifc \
 && python3 -I ../scripts/validate_ifc.py golden/relations/assembly.ifc golden/relations/assembly.ifc2x3.ifc
```

**Proves.** IfcOpenShell as the reference; own goldens; DEVIATIONS entry. The old IFC goldens
prove nothing moved. **Depends on.** X1, G1 (`verb_table.c`, `ifc_out.c` hooks), R2.
**Parallel with.** G2, Q2.

### G2 — Grids and levels in the plan view and the Levels page (C5b)

**Goal and reason.** G1 put grids and levels in the model. The engineer must see the axes in a
plan, with their tags, and edit levels in the page that already exists in the lab
(`LevelsGrids.vue`, wireframe A7). The place tool already snaps to "grid axis" and "axis
crossing": it must use the model's grid when there is one. With C4's dimensions, this completes
priority 3: a plan with grid, levels, dimensions and text.

**Do.**
- `session/grids.c`: query `view_grid {view}` answers the axes to draw in view mm (from the
  model's grids, else the read-only scene's axes from S1, else the column-based fallback of
  ADR 0018) with tag positions, and the level lines for a section or elevation later (not now).
- The place tool's grid snap reads the same list (one call in `tool.c`).
- Page: `web-ui/src/gl/grid.ts` draws axes (dash-dot, the palette's grid colour) and tag bubbles
  on the 2D layer C4 made for dimensions; one hook in `glviewport.ts`. Shown in plans, off in 3D.
- `LevelsGrids.vue` and its side panel call `level_add`, `level_set`, `level_remove`, `grid_add`,
  `grid_remove`; changes apply at once (ADR 0052); errors show the verb's text.
- Lab test: add a 3 × 2 grid by verb, open the ground plan: 5 axis lines and 5 tags are drawn;
  place a column with the tool at an axis crossing: its origin is exactly the crossing; set a
  level 300 mm higher: the plan's elements moved; undo twice restores.

**Owns.** `ccore/session/grids.c/.h`, `web-ui/src/gl/grid.ts`, `web-ui/src/glviewport.ts` (wave
owner), `web-ui/src/pages/LevelsGrids.vue`, `LevelsGridsSide.vue`, `levelsPick.ts`,
`ccore/test/lab_grid.mjs`, `ccore/test/grids_test.mjs`, one row in `ports.mjs`.
**Hooks.** `calls.c` (one `if` in `call_query`), `tool.c` (one call), `bimble.ts` (types, one
function per verb).
**Do not touch.** `model.h`, `src/grid.c` (report a fault to the orchestrator), `Quantities.vue`.

**Read first.** G1's `grid.h` and golden, ADR 0018, ADR 0056, C4's 2D overlay code in
`glviewport.ts` and `annot.h`, `ccore/session/tool.c` (snap labels), `web-ui/src/pages/LevelsGrids.vue`,
`anthropic-skills:bimble-design`, `ccore/test/lab_plan.mjs`.

**Done check.**
```sh
cd ccore && CCORE_PORT_BASE=auto node test/grids_test.mjs && CCORE_PORT_BASE=auto node test/lab_grid.mjs \
 && CCORE_PORT_BASE=auto node test/lab_plan.mjs && CCORE_PORT_BASE=auto node test/lab_tool.mjs \
 && (cd ../web-ui && npm run typecheck)
```

**Proves.** New behaviour by lab tests (no oracle; covered by G1's DEVIATIONS entry).
**Depends on.** G1, C4, T1 merged (`glviewport.ts`), S1 (the `grid_axes` of a read-only scene).
**Parallel with.** X2, Q2.

### Q2 — Quantities in the session and the Quantities page

**Goal and reason.** Q1 computes the tables. The lab's Quantities page (`Quantities.vue`) already
calls `schedule_rows`, `schedule_csv` and `schedule_list`, and the C session answers stubs. This
ticket wires them, so Pierre sees volumes, areas and kilograms per element and per material, and
downloads the CSV. It also works on an opened IFC, where the numbers come from meshes.

**Do.**
- `session/schedules.c`: `schedule_list`, `schedule_rows {name}`, `schedule_csv {name}` as
  `crates/bimble-viewer/src/calls.rs:399-490` answers them; remove them from the `EMPTY` and `CUT`
  stub lists in `calls.c`. Cache the table per rev.
- A new query `quantities {ids?}`: per element `{id, volume_m3, area_m2, length_m, mass_kg}` and
  the totals per material; for agents and MCP. C addition: DEVIATIONS entry.
- Goldens: a `ccore_golden` mode records the three Rust answers for `frame.json` and
  `frame2.json` through the viewer's call path (if that path needs a window, take the text from
  Q1's goldens and say so).
- The schedule tab (`call_tab`, type `schedule`) opens the page; `set_schedule` (group, filter)
  updates the table in the same call round.
- Lab test: open Quantities, the total row equals the golden's, change the grouping to material,
  the subtotals change, Export CSV gives the golden bytes, a click on a row selects the element.

**Owns.** `ccore/session/schedules.c/.h`, `ccore/session/calls.c` (wave owner),
`ccore/test/schedules_test.mjs`, `ccore/test/lab_quantities.mjs`, `ccore/golden/schedule/session/`,
`web-ui/src/pages/Quantities.vue` (only if the page needs a fix), one row in `ports.mjs`.
**Hooks.** `bimble.ts` (the `quantities` type and function), `ccore_golden.rs` (one mode).
**Do not touch.** `src/quantity.c` (report a fault), `glviewport.ts`, `LevelsGrids.vue`.

**Read first.** Q1's `quantity.h`, `crates/bimble-viewer/src/calls.rs:395-490`,
`crates/bimble-viewer/src/schedules.rs`, `web-ui/src/pages/Quantities.vue`,
`web-ui/src/bimble.ts:150-160` and `:850-865`, `calls.c:885-920` and `call_tab`.

**Done check.**
```sh
cd ccore && CCORE_PORT_BASE=auto node test/schedules_test.mjs && CCORE_PORT_BASE=auto node test/lab_quantities.mjs \
 && CCORE_PORT_BASE=auto node test/wasm_run.mjs && cargo run -j 2 -p bimble-api --example ccore_golden -- --check
```

**Proves.** Golden vs Rust for the three queries; `quantities` by its own test and a DEVIATIONS
entry. **Depends on.** Q1, S1 merged (`calls.c`), T1 merged (line 889 of `calls.c`).
**Parallel with.** X2, G2.

---

## Not in this round

| Item | Why it waits | Next step |
|---|---|---|
| Georeference in the C model | needs `model.h` after G1; no user-visible gap in the lab yet | one ticket: `georef` in model, file, IFC in and out; goldens exist in Rust |
| Polygon slab in the place tool | `tool.c` belongs to C4, then G2 | small ticket after G2 |
| C2 exact cuts, C3 pick and snap in 2D | paper and precision; C4 shows what the screen still lacks | plan after C4's report |
| B1 scene per element, B2 box tree | performance (ADR 0069 rule 6) | when a model Pierre uses is slow, with a number |
| MCP modes `propose` / `edit`, `/mcp` on the HTTP host | M1 first | after M1 |
| macOS and Windows builds | K1 writes what blocks | a port ticket per platform from `results/ci.md` |
| LCA (ADR 0063), rebar, steel and timber members | need quantities and relations first | after Q2 and X2 |
| The final Rust release tag | Pierre's decision (ADR 0067) | ask Pierre |

## Risks

- **`calls.c` merges.** Three waves, one owner each. If a hook grows over 10 lines, move the code
  to the ticket's own file. A later ticket "router table" (ops and queries registered by each
  `session/*.c`) removes the hotspot; it is worth one session once this round is merged.
- **Model shape (X1, G1).** Two new tables in one round. Rule 4 (left out when empty) and the
  old goldens protect existing files. Both write DEVIATIONS entries, because `.bimble` files with
  these keys are no longer readable by the Rust oracle.
- **No oracle** for X1, X2, G1, G2. Their checks are IfcOpenShell (`validate_ifc.py`,
  `check_rel.py`) and their own goldens. A reviewer should read those goldens once by hand.
- **Rust builds for goldens** (Q1, M1, S1, T1, Q2) on a 16 GB machine: one at a time.
