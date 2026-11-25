# Deviations from the Rust oracle

The Rust crates are the oracle: the C core matches them byte for byte (README). A few things are
deliberate departures, decided in an ADR. Each one is listed here: what differs, why, what a Rust
reader sees, and where the C code is. A change to Rust's behaviour that is not in this list is a
bug.

## 1. Dimension anchors name a feature (ticket C4, ADR 0068 rule 9)

**What.** Rust's `Anchor` (crates/bimble-core/src/view.rs) is `{at, element?}`: a world point
and the element it was taken on. The C core adds an optional third key, `feature`:

    {"at": [-150.0, -150.0, 0.0], "element": "2P97ZHH3v3XutfaphHA_Tu", "feature": "corner:0"}

`feature` is `"origin"`, `"end"` or `"corner:N"` of the element's generator (`ccore/src/annot.h`
lists them per generator). When the element and the feature exist, the end of the dimension is
computed from them each time it is asked (`query view_annotations`, the page's overlay), so a
dimension follows its element when it moves or changes size, and undo brings it back. Without the
element (deleted) or the feature, `at` is used. `at` keeps the point at creation; it is never
written back.

**Why.** Pierre's priority 3 (ADR 0069): a 2D view where dimensions stay right when the model
changes. Rust stores only `at` and `element` and does not move `at` when the element moves.

**What Rust sees.** A file without feature anchors is unchanged: same keys, same bytes. A file
with one is refused by Rust's reader (`deny_unknown_fields` on `Anchor`: "unknown field
`feature`"), in v1 (JSON) and v2 (CBOR). Rust needs the same optional field to read them.

**Commands.** The C verbs `add_dimension`, `add_text`, `move_annotation`, `delete_annotation`
(C additions, `ccore/src/verb_table.c`) each make one `set_view` command (`add_view` the first
time a default plan gets an annotation), one history step, as Rust's desktop does with
`update_view`. ADR 0068 rule 9 asks for own Commands (`add_annotation`, `set_annotation`,
`remove_annotation`); they are not done yet, so the history stays readable by Rust. The cost: one
edit records the whole view.

**Where.** `ccore/src/annot.{h,c}` (features, the drawing geometry, the query), `ccore/src/view.c`
(the `feature` key: read, write, copy, validation), `ccore/src/verbs.c` (the verbs),
`ccore/session/calls.c` (`query view_annotations`, the annotate tool), tests
`ccore/test/annotation_test.c`, `annotations_test.mjs`, `lab_dims.mjs`.

## 2. Relations and assemblies (ticket X1, ADR 0070)

**What.** The C model has a relation table that Rust's `Model` does not have: key `relations`
in the model, after `schedules`, a map GlobalId → `{id, kind, relating, related, realizing?,
properties?}`, in v1 (JSON) and v2 (CBOR). `kind` is an IFC class name from a table
(`IfcRelAggregates`, `IfcRelConnectsElements`, `IfcRelConnectsWithRealizingElements`;
`IfcRelDefinesByType` is reserved and refused until the model holds type objects). The element
class `IfcElementAssembly` is new too: such an element has no `generator` key (its shape is its
parts). Commands `add_relation`, `remove_relation`, `set_relation` are the C core's own, and
`remove_element` of an element that a relation names is refused ("`<id>` is still used by the
relation `<relation id>` (`<kind>`)"); Rust has no such case. The serde error texts that list the
classes and the command tags name the new ones too.

**Why.** ADR 0068 rule 10 (full IFC compatibility): assemblies and typed connections as objects
with GlobalIds, cheap before files hold them.

**What Rust sees.** A model without relations and without assemblies is unchanged: same keys,
same bytes, same hash (`relation_test` reloads `golden/frame*.json` and `golden/bin/frame*.bimble`).
A model with relations loads in Rust without them (`Model` ignores unknown keys) and Rust's save
drops them. A model with an `IfcElementAssembly` is refused by Rust ("unknown variant
`IfcElementAssembly`", and "missing field `generator`").

**Goldens.** `golden/relations/assembly.json` and `assembly.bimble`: `golden/frame2.json` plus one
assembly of three parts and one connection with a realizing element and properties, built by
`ccore/test/relation_test.c` through Commands (`CCORE_WRITE_GOLDEN=1` writes them again).
`ccore_golden --check` skips `relations/` and this file (`C_OWN`): Rust does not produce them.

**Until X2.** No verb makes a relation, the scene draws nothing for an assembly and warns once
("could not mesh"), and the IFC export refuses a model with an assembly (an error, never a silent
drop).

**Where.** `ccore/src/relation.{h,c}` (table, rules, read, write), `ccore/src/model.{h,c}`,
`ccore/src/command.{h,c}`, `ccore/src/file.c`, test `ccore/test/relation_test.c`.

## 3. The MCP server's additions (ticket M1, ADR 0068 rules 3 and 5)

**What.** `bimble-ccore mcp <model>` (`ccore/host/mcp.c`) answers Rust's transcript byte for byte
(`golden/mcp/responses.jsonl`, the server version masked). Beyond it:

- `run_verb` and `describe_verb` know the C additions of the verb table (`rename`,
  `add_dimension`, `add_text`, `move_annotation`, `delete_annotation`); Rust answers
  `unknown verb`. `list_verbs` and the `unknown verb` text stay Rust's 14, as the session's
  `verbs` query does.
- `run_verb`'s `args` may carry `"dry_run": true` and `"if_rev": N`, taken out before the verb
  reads its arguments, as the session's `verb` call does (`session/calls.h`). A dry run answers
  `{"commands":[…],"rev":N}` (the commands as serde JSON, and the server's rev) and changes
  nothing. `if_rev` runs the verb only when N is the server's rev, else the session's text
  "conflict: the model changed (rev R, expected N)". The rev is 1 after the load and + 1 for each
  step applied, undone or redone. Rust refuses both keys ("unknown field `dry_run`").
- `--read-only`: `run_verb` (but a dry run), `undo`, `redo` and `save` answer an error ("read
  only: the server was started with --read-only"); Rust has no such mode. `propose` is a later
  ticket.
- `serverInfo.version` is the C core's own (`CC_IFC_VERSION`, as the host's `state.version`).
- JSON that does not parse: the code (-32700) and serde's texts and columns are Rust's (the
  reader's errors are mapped to serde's, `parse_error_text`), except two cases where the text
  differs: nesting deeper than 64 levels (the C reader's budget; serde allows 128, so Rust
  answers such a line and C refuses it) and bad UTF-8 (the C reader checks the whole line first:
  "invalid UTF-8", where serde names the first syntax error or "invalid unicode code point").

**Why.** ADR 0068 rule 5: agents reach the model through the same verbs as the UI, with the
same dry run and conflict check (rule 3).

**What Rust sees.** Nothing: the server writes the model only on `save`, in Rust's form (the v1
text for `.json`, else v2), and Rust's transcript is unchanged.

**Goldens.** `golden/mcp/additions_requests.jsonl` and `additions_responses.jsonl`, written by
`tools/mcp_golden.sh additions` from the C server and checked by `test/mcp_test.mjs`.
`ccore_golden --check` skips `mcp/` (`C_OWN`): `tools/mcp_golden.sh` makes it, not the dumper.

**Where.** `ccore/host/mcp.{h,c}`, `ccore/host/main.c` (the sub-command), test
`ccore/test/mcp_test.mjs`.

## 4. Levels and grids (ticket G1, ADR 0056)

**What.** Two model keys Rust's `Model` does not have, both left out when unused:
- a storey's `"is_storey": false` (after `elevation`): a datum level (top of parapet, bottom of
  foundation). No element belongs to it (a file load, `add_element` and `set_storey` refuse one),
  and the IFC export leaves it out (it is not an `IfcBuildingStorey`).
- `grids` (after `relations`): a map GlobalId → `{id, name, axes: [{id, tag, start_mm, end_mm}]}`,
  plan, world mm, in v1 (JSON) and v2 (CBOR). Rules in `ccore/src/grid.h`: a name, 1 to 1000
  axes, tags unique in a grid, ends within 1e7 mm, length ≥ 1 mm, ids unique across the model,
  axes in at least two directions (IfcGrid needs UAxes and VAxes).

Commands `set_storey`, `add_grid`, `set_grid`, `remove_grid` and the verbs `level_add`,
`level_set`, `level_remove`, `grid_add`, `grid_axis_set`, `grid_remove` (C additions,
`ccore/src/verb_table.c`) are the C core's own. The query `grids` is new; `storeys` gains
`is_storey` and `height_mm` (to the next storey up, null for the top one and for a datum level)
only in a model with a datum level or a grid. The IFC export writes each grid as an `IfcGrid`
(UAxes: the axes parallel to its first one; VAxes: the others) of `IfcGridAxis` on two-point
`IfcPolyline`s, with a `FootPrint` `GeometricCurveSet` representation in a new
`IfcGeometricRepresentationSubContext`, contained in the storey nearest elevation 0 (the building
when there is none); IFC2x3 drops `PredefinedType` and adds the owner history. The serde error
text listing the command tags names the new ones too.

**Why.** ADR 0056 (accepted under Pierre's delegation): a new model needs axes and levels
before columns. Rust never built it: there is no oracle.

**What Rust sees.** A model without grids and without datum levels is unchanged: same keys, same
bytes, same hash, same IFC (`grid_test` reloads `golden/frame*.json` and `golden/bin/frame*.bimble`,
undoes every step back to frame's bytes and IFC, and the suites `file`, `file2`, `query`,
`ifc_out` and `verbs_all` still match Rust's goldens). A model with grids or datum levels loads in
Rust without them (`Model` and `Storey` ignore unknown keys): Rust's save drops the grids and makes
every datum level a storey again.

**Goldens.** `golden/grids/verbs.txt` (47 steps: each verb, refusals, undo and redo, the queries
`storeys` and `grids`, on a copy of `frame.json` with seeded ids), `model.json` (the model at the
end), `grid.ifc` and `grid.ifc2x3.ifc` (its export), all written by `ccore/test/grid_test.c`
(`CCORE_WRITE_GOLDEN=1` writes them again). IfcOpenShell 0.9 validates both IFC files with 0
issues (`scripts/validate_ifc.py`). `ccore_golden --check` skips `grids/` (`C_OWN`).

**Until G2 and S1.** A datum level still gets the default plan views a storey gets (`view.c`,
`session/views.c`), the scene and the 2D view draw no grid of the model, and an IFC's grids are
not imported into `grids` (the reader keeps reading them for drawing, as before).

**Where.** `ccore/src/grid.{h,c}` (rules, read, write), `ccore/src/verbs_levels.c` (the verbs),
`ccore/src/ifc_out_grid.{h,c}` (the export, one call in `ifc_out.c`), `ccore/src/model.{h,c}`,
`ccore/src/command.{h,c}`, `ccore/src/file.c`, `ccore/src/query.c`, test `ccore/test/grid_test.c`.

## 5. Views placed on sheets (ticket SH1)

**What.** Four verbs Rust's dispatch has not (C additions, `ccore/src/verb_table.c`), each one
`set_sheet` command (`sheet_remove`: one `remove_sheet`), one history step:
- `sheet_place {sheet, view, at_mm, scale?}`: a new placed view (new GlobalId) at `at_mm`, paper mm
  from the sheet's top-left corner. Its scale is the view's (ADR 0046). `scale` may differ only on
  the first placement of a view whose scale is not said (100): that placement then gives the view
  its scale, as `sheet.rs` `adopted_scales` does when Rust opens the file. Otherwise another scale
  is refused ("the view … is drawn at 1:N: a view has one scale on every sheet").
- `sheet_move {sheet, placed, at_mm}` (the same point: no command), `sheet_unplace {sheet, placed}`,
  `sheet_remove {id}`.
The sheet is checked as `Command::SetSheet` checks it (`sheet.rs` `validate`: at most 200 placed,
on the paper, 1:1 to 1:5000, the view in the model), with Rust's texts after the verb's name.
Rust's own UI places a view by a drag from the navigator, at the view's scale (`layout.rs:449`).

The query `sheet_layout {id}` (`ccore/session/sheets.h`) is new: the paper's width and height in
its orientation, and per placed view its id, view, name, kind, the scale it is drawn at (the
view's own, or the one its placements share: `adopted_scales` applied when read), `at_mm` and a
`box_mm`: the view's crop, else the model's box (a plan: x and y; a section: its width and
height), divided by the scale, rounded to 0.001 mm, with `"box": "estimate"`. Rust answers
`sheet_svg` instead (the whole drawing); the C core still refuses `sheet_svg` until D5. A sheet
tab (`tab`, type `sheet`) answers 200 for a sheet of the model ("no such sheet" otherwise) and
keeps no tab: the page opens on that sheet (`open_views` lists view tabs only, views.h).

**Why.** Pierre's priority after measure: sheets usable before the drawings exist (round 3, SH1).
Rust never had these verbs: there is no oracle.

**What Rust sees.** Nothing new: the file a sheet makes is a plain `Sheet` with `Placed` entries
(`id`, `view`, `scale`, `at_mm`), which Rust reads and writes. A model without sheets keeps its
bytes (`sheets_test` undoes every step back to `frame.json`'s bytes).

**Goldens.** `golden/sheets/verbs.txt` (56 steps: each verb, each refusal, undo and redo, the
history labels, the queries `sheet_layout` and `sheets`, on `frame.json` opened in a session, with
seeded ids) and `model.json` (the model at its end), written by `ccore/test/sheets_test.c`
(`CCORE_WRITE_GOLDEN=1` writes them again). `ccore_golden --check` skips `sheets/` (`C_OWN`).

**Where.** `ccore/src/verbs_sheets.c` (the verbs, in `VIEW_DENY`), `ccore/src/verb_table.c` (rows),
`ccore/session/sheets.{h,c}` (the query, the tab check), two hooks in `ccore/session/calls.c`,
tests `ccore/test/sheets_test.c` and `ccore/test/lab_sheets.mjs`.
