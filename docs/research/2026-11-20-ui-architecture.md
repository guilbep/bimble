# Research: fast UI iteration and one API for the UI, the CLI and agents

_20 Nov 2026. Code study (Opus), searches (Sonnet), measurements and options by the orchestrator._

Asked (Pierre): "fast UX/UI iteration the next few weeks". The proposal: two web interfaces (egui,
and Vite + Vue). Both use one API, which egui, a native app and an MCP server also use. "Is that the
correct architecture?"

## Answer

One API: yes. It is principle 7 of the ROADMAP, and the AI goal needs it whatever the UI is.
Two complete UIs over it: no, not now. Two UIs drift. ADR 0020 rejected a JavaScript viewer
because it writes plans, styles and editing twice. A Vue UI over the same wasm core duplicates
only the UI, which is smaller but still a second UI to keep in step. The API comes first, because every option below needs it.

## Facts from the code (file:line checked by the orchestrator where marked)

- `Command` is serde JSON, tagged `op` (`crates/bimble-core/src/command.rs:8-10`, checked). 18
  variants (counted). `apply` returns the inverse. `History` with undo and redo is in core.
- No registry, no schema, no query API in core. `Model` exposes public maps.
- The "place a slab from A to B" verb is in the viewer: `edit::new_element`
  (`crates/bimble-viewer/src/edit.rs:135`), with `move_by`, `copy_by`, `delete`.
- Gap: `concrete()` takes the first concrete material of the model. If the model has none, it
  creates C30/37 (`edit.rs:98-121`, checked). "A slab in C35/45" is not possible through it today.
- `bimble-render` does not depend on egui (checked). It draws into its own texture. Camera, picking
  and highlight are already in the renderer.
- Viewer, 19 068 lines. Counts are estimates (files by lines with `ui.` or `egui::`):
  - about 2.5k lines of logic without UI (big, views, measure, snap, state, cutlines, share)
  - about 2.8k mixed (edit, schedules, paper, collab)
  - about 13.7k egui (lib 5 040, viewing 1 969, ribbon 1 594, layout 1 389, others).
- wasm: one export, `start()` (`crates/bimble-web/src/lib.rs:19`, checked). No MCP, JSON-RPC or
  script entry point anywhere.

## Measured: the egui edit loop (20 Nov, Apple M4 Pro, 14 cores, warm cache, 2 runs each)

One string changed in `ribbon.rs`, then built:

| Build | Time |
|---|---|
| native dev (`cargo build -p bimble-cli`) | 3 s, 2 s |
| web dev (`PROFILE=dev scripts/build-web.sh`) | 5 s, 7 s |
| native release | 12 s, 12 s |
| web release, mtime touch only | 7 s, 7 s |

- Web release wasm: 21 844 110 bytes, 6 813 780 gzipped, without wasm-opt (not installed here).
  ADR 0020 recorded 14 MB and 4.3 MB gzipped on 6 Nov, also without wasm-opt. So the binary grew
  by about 56 % in two weeks. The cause is not studied.
- Not measured: the time to restart the app and get back to the state under test, and the same loop
  on an older laptop. Vite's hot reload was not measured either (no Vue page exists).
- Reading: compile time is not the bottleneck on this machine. The loss is the restart (state is
  lost) and the cost of layout in egui. This is an inference from the numbers above.

## The shared step (every option needs it): `bimble-api`

- Verbs moved out of `edit.rs`: place column, beam, wall, slab, footing, move, copy, delete, set
  material by name (fixes the C30/37 gap). Each verb makes sure that its input is valid, then returns `Command`s.
- Queries: storeys, materials, elements by class or storey, properties, schedules.
- JSON schemas from the types (`schemars`), so verbs describe themselves.
- Budgets at the boundary: an agent is untrusted input (point counts, sizes, element counts).
- `bimble mcp` (stdio) with few tools, as in Archicad's Tapir MCP: `list_verbs`, `describe_verb`,
  `apply`, `query`, `undo`, and `render_view` (a PNG, so a model sees what it did). No code execution.
- The same functions through `wasm-bindgen` in the web build, and called by the egui viewer.
- Size (estimate from the code study): about 1.2k lines moved, 600 to 900 new.

## Options for fast UI iteration

Scores 1 (bad) to 5 (good). "Drift" 5 = one UI only. "Old machines" = ROADMAP principle 1.

| | Option | First iteration | Iteration speed | Drift | Old machines | AI fit | Cost | Total /30 |
|---|---|---|---|---|---|---|---|---|
| A | Stay on egui, design first on a canvas, "open in this state" for the dev loop | 5 | 3 | 5 | 5 | 3 | 5 | 26 |
| B | Vue shell around the existing egui canvas, web only | 3 | 4 | 2 | 4 | 5 | 3 | 21 |
| C | Vue everywhere: new viewport crate without egui, Tauri on the desktop, egui retired | 1 | 5 | 4 | 2 | 5 | 1 | 18 |
| D | Two full UIs (egui and Vue) side by side (the proposal as asked) | 2 | 4 | 1 | 4 | 4 | 1 | 16 |
| E | Chat first: MCP plus a chat panel in egui, Claude as the second UI | 4 | 2 | 5 | 5 | 5 | 4 | 25 |

- The total weighs the six columns the same. On iteration speed alone (the question asked), the
  order is C (5), then B and D (4), then A (3), then E (2).
- Estimates, not measured: "Iteration speed" for B, C and D (no Vite loop exists) and "Old
  machines" for B and C (Tauri's web view and a DOM shell on an old laptop are not measured).
- E serves the AI goal, not the UI goal. It is in the table because it competes for the same weeks.

- A. Screens are first drawn on a design canvas (as for ADR 0027), then ported to egui. A flag such
  as `bimble view model.bimble --open-tab sheets --select <GlobalId>` brings the app back to the state
  under test after each 2-3 s build. The cheapest option. Iteration on looks stays slower than in CSS.
- B. Vite + Vue owns the panels, properties, schedules, chat and Home in the DOM. The eframe canvas
  stays the viewport with all its tools (view cube, measure, section handles, sheet tools: about
  3.7k lines in `viewing`, `layout`, `viewcube`). One wasm instance, one model, the API between them.
  Desktop stays egui, so panels exist twice until a decision on C.
- C. One UI in the end. The cost: about 13.7k lines of egui UI and the in-viewport tools rewritten,
  a new wasm-bindgen viewport (500 to 800 lines for the renderer wrapper alone), and Tauri's web view
  on old Windows machines unmeasured. ROADMAP open decision 3 (native vs Tauri) belongs here.
- D. Read as asked: both UIs kept at parity. Every feature is built twice, or one UI lags. If egui
  is frozen while Vue moves on, the proposal becomes B (web) or C (everywhere). The worst for a team of one or two.
- E. The fastest route to the AI goal: an agent (Claude Desktop, Claude Code) drives Bimble through
  MCP, the user sees the result in the viewer. Does nothing for visual UX.

## Recommendation (orchestrator, for Pierre to decide)

1. Now: the shared step (`bimble-api` + `bimble mcp`). It is on the path of every option and of
   goals 1 and 2 (`print` becomes one more verb, see `2026-11-20-3d-printing.md`).
2. The next weeks: A + E. Design screens on a canvas, port them to egui, add the "open in this state"
   flag. Use MCP for "create a slab from here to there".
3. In parallel, a 3-day spike of B on one panel (properties + chat), web only, behind its own page.
   Measure Vite's loop against the egui numbers above. Then decide C or not in an ADR.

The pivot question: which UX do you want to iterate on? Panels, forms, chat, Home: Vue helps.
Placing, snapping, sections, sheets: these stay in Rust in every option, and Vue does not help.
