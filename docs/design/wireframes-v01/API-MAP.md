# Wireframes v01: what each tool calls (20 Nov 2026)

STUDY station (Opus), read only, partial at its time limit. "Inferred" means only names and
comments were read. Verdicts: A = callable from Vue today (bridge export, query or Command),
E = only an egui `Action` (crates/bimble-viewer/src/ribbon.rs), M = no Rust logic.

## What Vue can call today

- Bridge: `query`, `apply`, `undo`, `redo`, `select`, `open`, `on_change`.
- Queries: `state`, `storeys`, `materials`, `elements`, `element`.
- Commands: every `bimble_core::Command` variant (Rename, SetGenerator, SetPlacement, AddView,
  SetView, AddSheet, SetSheet, SetSchedules, SetProject...).
- Not bridged yet: the verbs `place`, `move`, `copy`, `delete` (`bimble_api::dispatch::run`).

## Per screen

| Screen | A | E | M |
|---|---|---|---|
| Quick access | Open, Undo, Redo, file name, state | Home, Save, Synchronize, Palette, Settings | Print and export |
| D1 3D view | object counts, materials, classes, New view (write) | Edges, Hatching, Orthographic, Section box, Fit, Top, Hide, Isolate, Reveal, Links, Measure | phase filter, sub-projects, detail, views list (no read) |
| B2 Modify | name, read-only fields, sizes, Z, delete, select by class | hide, isolate, box to selection | material change, rotate, mirror, align, offset, trim, split; move and copy need the verb bridge |
| B1 Place slab | storeys, materials | Place (interactive), Measure | footing, raft, stair, ramp, opening, reservation, sub-project, shape, slope |
| D2 On paper | scale, orientation, crop, range (write only) | sheet tools, section labels, openings, marks, levels, On paper switch | text and dimension types (inferred), phase filter |
| D6 Quantities | schedule definitions (write only) | by material, CSV | rows (computed in egui), CFC |
| A3 Project info | fields (write only) | Project, links | phases, global parameters, team members |
| A1 Home, A2 File menu | Open | New, Import IFC, Save, Share, exports, Settings, History | Print and export, recent files on the web |
| D8 Palette | | Palette (ALL_ACTIONS cannot be read) | |
| C1, C2, C6 | | Shared folder, Synchronize, borrow, History | sub-projects, users, variants, compare, merge |
| D3 Sheets | AddSheet, SetSheet (write only) | New sheet, Close sheet, sheet tools | revision, print and export, drawn/checked |
| D5 Standards | materials | every standards table, revert, reset, save | |

## Smallest Rust additions, largest gain first

1. Bridge `action(name)` and query `actions` (label, tab, enabled, pressed) from `ALL_ACTIONS`:
   about 40 E tools callable, with their real states.
2. Bridge `verb(name, args)` over `dispatch::run`: move, copy, place parameters (B2, B1).
3. Queries `views`, `sheets`, `project`, `schedules` (with computed rows): D1, D2, D3, D6, A3.
4. Query `history` (undo labels, then versions): C6, the "Annuler : déplacer B12" tooltip.
5. Query `visibility` (hidden, isolated, section box): status chips, "Masqués ici".

## Reference selectors (for `web-ui/design-check/landmarks.json`)

`header.top`, `.top > .qb`, `.fname > span:first-child`, `.fname > .chip`, `.top > .cmdk`,
`nav.tabs`, `.ribbon`, `.work > aside.left`, `.left > nav.nav`, `.left > .props`, `.main > .vtabs`,
`.vtabs > .seg` (Model/On paper, PlanOnPaper only), `.main > .vp`, `.vp + div` (Modifier, B2),
`footer.status`, `.status > .chip` (a set), `.status > .seg` (FR/EN). Scope `.grp`, `.tool`, `.ti`
(`.ribbon >`, `.nav >`): they also appear in the Modifier panel and the context menu.
