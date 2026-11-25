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

## 6. Setting the georeference (ticket GEO1, ADR 0045)

**What.** Rust sets a model's `georef` only on IFC import; nothing edits it. The C core adds the
command `set_georef` (`{"op": "set_georef", "georef": {…}}`: the whole georeference, `{}` clears
it; its inverse is the old one) and the verb `georef_set` (a C addition, `ccore/src/verb_table.c`:
`rotation_deg`, `crs` ("" for none), `east_m`, `north_m`, `height_m`, `map_rotation_deg`, `clear`;
missing ones are kept; angles within 360 degrees, east and north within 1e8 m, height within
1e5 m; the same values give no command). Two queries are new (`ccore/src/query.h`): `north`
(project north and the map's turn, for the viewport and the view cube) and `coordinates` (a world
point as the status bar says it, as Rust's `coords::text`, with its survey and WGS84 values). The
serde error text listing the command tags names `set_georef` too.

Two refusals Rust has not: a non-finite value in the georeference (a binary file can hold a NaN;
Rust has no check and its canonical form would panic): "number out of range" on load, "georef:
non-finite value" from a command; an IFC import whose map conversion holds one keeps no
georeference. And, one family at a time (ADR 0045, Swiss first): coordinates of a model in another
system of Rust's table (`crs_table.rs`), or in a target other than the survey system and WGS84
(UTM, LV03, a State Plane zone), are refused with "… are not converted by the C core" where Rust
says them through proj4rs.

**What Rust sees.** The same `georef` key and bytes (v1 and v2), the same IFC: a model edited
with `set_georef` is a model Rust could have imported (`georef_test`: the frame with `georef_set`
saves to `golden/geo/geo.json` byte for byte).

**Tolerance.** WGS84 is computed as proj4rs does it (somerc inverse on Bessel, the
three-parameter shift, geocentric to geodetic), with the C core's own exp, log, tan and asin
(`ccore/src/georef.c`, no libm): within 2e-9 degrees of Rust's (0.2 mm; measured 4.4e-10), the
status bar's six-decimal text byte for byte on the 36 points of `golden/geo/*.coords.txt`.

**Where.** `ccore/src/georef.{h,c}`, `ccore/src/command.{h,c}`, `ccore/src/query.{h,c}`,
`ccore/src/verb_table.c`, test `ccore/test/georef_test.c`.

## 7. The measure tool's pointer (ticket T1, ADR 0068 rule 2)

**What.** Rust's measure tool (`crates/bimble-viewer/src/measure.rs`, `lib.rs` `snap_at`) runs in
the egui viewer: it projects corners and edge points with the camera and compares screen pixels.
The C core has no camera, so the page sends the pointer ray and `ray_tol`, the ray `tol_px` (12)
pixels beside it (`tool measure_pointer`, `measure_click`, `measure_cancel`, `measure_clear`, a C
addition, as the place and box tools): a point's pixel distance is its distance to the ray over the
gap between the two rays at its depth. Rust's limits stay (10 px for a corner, 8 for an edge,
corners and edges behind the surface under the cursor are not snapped). Two departures: the
corners and edges snapped to are those of every element the ray meets within that visible depth,
not only the nearest one's (a column standing on a footing meets the ray at the same depth: either
could be "the" hit, and the corner under the cursor was lost); and a corner or edge point outside
the section box (or a plan's range) is not snapped (Rust snaps it when it lies in front of the
hit, a point in the empty space the box removed). Reference plans (DXF, PDF) are not in the C
core: nothing snaps on them.

**What Rust sees.** Nothing: measurements are session state, never in a file or the history. The
label and the list are Rust's: `format_mm` and `measurements_json` were moved from the viewer to
`bimble_api::measure` (the viewer calls them there), and `ccore_golden` writes
`golden/measure.txt` from them; the distance is f32 as the viewer's `Dimension::distance`.
`query measure` (the tool's state for the page) is a C addition.

**Where.** `ccore/session/measure.{h,c}`, `ccore/session/calls.c` (`measure_tool`, the action
`measure`, `query measurements` and `measure`), tests `ccore/test/measure_test.c`,
`ccore/test/lab_measure.mjs`; the page: `web-ui/src/gl/measure.ts`, `web-ui/src/glviewport.ts`.

## 8. A plan on paper draws the model's grids (ticket D3)

**What.** Rust's `plan()` takes its grid axes from `Scene::grids`, which only the IFC reader fills
(`read.rs` `grid_axes`); `Scene::from_model` has none, so Rust draws a `.bimble` model's plan with
axes through its columns. The C plan (`cc_plan_axes`, `ccore/src/draw_annot.c`) takes the model's
own grids (deviation 4) when it has some, in the order Rust's reader gives the same grids from
their IFC export: grids by id, each grid's U axes then its V axes, an axis repeated with the same
tag and ends once, scene mm (world less the scene origin). `PlanInfo.model_axes` is then true.

**Why.** ADR 0056: a model's grids are its axes; the drawing must show them. With them, the C plan
of a model draws what Rust draws for that model's IFC export (checked by hand: the 13 axes of
`golden/grids/model.json` against the `IfcGridAxis` polylines of `golden/grids/grid.ifc`, same tags,
same order, same ends).

**What Rust sees.** A model without grids (every Rust golden) draws the same bytes
(`draw_annot_test`: `plan/<model>/*.svg`). A model with grids differs from Rust's plan of the same
`.bimble` (Rust ignores the `grids` key), not from Rust's plan of its IFC.

**Where.** `ccore/src/draw_annot.c` (`cc_plan_axes`), one call in `ccore/src/draw_plan.c`, test
`ccore/test/draw_annot_test.c` (`test_grid_axes`).

## 9. A view turns the model's grids with it (ticket D4)

**What.** Rust's `viewdraw::map_scene` turns `Scene::grids` with a plan's scene (a building turned
in its site, drawn in project north; a ceiling plan, reflected) and drops them for a section. A
`.bimble` scene has no grids in Rust (deviation 8), so Rust draws such a view with axes through
its columns. The C drawing takes the model's own grids (deviation 8) and turns them the same way
(`cc_view_turn_axes`, `ccore/src/draw_view.c`: each end turned, plus the scene origin, less the
origin, as `map_scene` computes it); a section has no axes, as in Rust.

**Why.** ADR 0056 and deviation 8: a model's grids are its axes, on every plan of it. Turned with
the plan, they stay on the columns they were set out on, as Rust's plan of the model's IFC export
draws them.

**What Rust sees.** A model without grids (every Rust golden, `view/*.svg`) draws the same bytes
(`draw_view_test`). A model with grids differs from Rust's plan of the same `.bimble`, not from
Rust's plan of its IFC.

**Where.** `ccore/src/draw_view.c` (`cc_view_turn_axes`), one call in `ccore/src/draw_plan.c`.

## 11. A sheet drawn by the session (ticket D5)

**What.** The query `sheet_svg {id}` draws the sheet as Rust's (`calls.rs` `sheet_svg`,
`sheetdraw::draw`), with three differences in its options. The frame's note says the C core's
version (`Bimble 0.1.0-ccore`, `CC_IFC_VERSION`), not Rust's `CARGO_PKG_VERSION`; the note is
placed by its width, so its `x` differs too. The graphic style and the plan template are the
defaults (with the model's project information): the C session keeps no style or template of its
own, where Rust takes the viewer's (the defaults until the user edits them). The viewer-only
wasm has no drawing code: `sheet_svg` answers "this build is view-only".

**Why.** The version names the program that drew the sheet (as the IFC header, deviation 3's
`serverInfo.version`). The C core has no style or template editor yet.

**What Rust sees.** `ccore/test/draw_sheet_test.c` draws every sheet of `golden/drawing/model.json`
through the library with Rust's version: `sheet/*.svg` and `.info.txt` byte for byte. Through the
session (`sheet_svg`, natively and in the wasm module, `wasm_run.mjs`), `S-101.svg` is the golden
byte for byte once both version notes are cut out.

**Where.** `ccore/session/sheet_svg.c`; the library is `ccore/src/draw_sheet.c`.
## 10. The IFC display reader past Rust's (ticket IFC2)

**What.** The C display reader (`cc_ifc_read`, and the import through the shared
`ccore/src/ifc_geom.h`) reads IFC content that Rust's `read_str` (frozen at v0.19.0) skips:

- A product whose `Representation` is a plain `IfcProductRepresentation` (IFC2X3 allows the
  supertype of `IfcProductDefinitionShape`) is read like an `IfcProductDefinitionShape` (same
  attributes). Rust skips such a product without a warning.
- Parametric profiles of `IfcExtrudedAreaSolid` (and of every swept area that takes a profile):
  `IfcLShapeProfileDef`, `IfcTShapeProfileDef`, `IfcUShapeProfileDef`, `IfcCShapeProfileDef`,
  `IfcZShapeProfileDef`, `IfcAsymmetricIShapeProfileDef`, `IfcTrapeziumProfileDef`,
  `IfcEllipseProfileDef`, `IfcRoundedRectangleProfileDef`. Centred on the bounding box, as the
  specification and IfcOpenShell 0.9 place them. Fillet and edge radii are drawn as arcs with the
  reader's arc segmentation (as the I-shape's fillets are); C-shape outer corners get the
  internal radius plus the wall. Slopes (flange, web, leg) are not drawn (the I-shape ignores its
  slope too). The Z-shape's FlangeWidth runs from the web's centre line, as IfcOpenShell draws it.
  Invalid dimensions (not a number, not > 0, a thickness past the depth): warning
  `invalid profile <TYPE> (skipped)`. Rust: `unsupported profile <TYPE> (skipped)`.
- `IfcRevolvedAreaSolid`: the profile turned about its axis (right-handed) by the angle, in
  rings at the reader's arc segmentation for the profile's largest radius, with end caps unless
  the turn is whole; oriented outward by its signed volume. Rust: `unsupported geometry
  IFCREVOLVEDAREASOLID (skipped)`.
- `IfcExtrudedAreaSolidTapered`: the start profile at 0, the end profile at Depth, joined point to
  point; an end profile of another point count is drawn untapered, with the warning `tapered
  extrusion: the end profile does not match the start (drawn untapered)`.
- `IfcCsgSolid` (its tree root drawn as any item, booleans included) and the primitives
  `IfcBlock`, `IfcRectangularPyramid`, `IfcRightCircularCylinder`, `IfcRightCircularCone`,
  `IfcSphere` (latitude and longitude rings at the reader's arc segmentation), as items and as
  boolean operands. Invalid dimensions: `invalid <TYPE> (skipped)`.
- A file that is not STEP but an ifcZIP archive (`PK\3\4`), ifcXML (`<`) or ifcJSON (`{`, `[`)
  is refused with `unsupported format: ifcZIP (...)` (ifcXML, ifcJSON) instead of `parse: line 1:
  not a STEP file (missing ISO-10303-21 header)`.

**Why.** ADR 0068 rule 10 (full IFC compatibility); the public corpus run (ticket IFC1,
`ccore/results/ifc_corpus.md`) ranked these gaps. The reference is IfcOpenShell and the IFC
specification, not Rust.

**What Rust sees.** A file without these types gives the same scene, byte for byte, as before
(the corpus run before and after: `ccore/results/ifc_corpus.md`, "After IFC2"). A file with them
gives more elements, or more triangles, than Rust, and no "unsupported" warning for them.

**Goldens.** `golden/ifc_c/fixtures/*.ifc` (hand-written, synthetic) and
`golden/ifc_c/read/<name>/` (the C reader's output, `CCORE_WRITE_GOLDEN=1 test/run.sh ifc_read`
writes it again). `ccore_golden --check` skips `ifc_c/` (`C_OWN`). The suite also checks each
scene's volume and bounding box against numbers computed by hand from the fixture.

**Where.** `ccore/src/ifc_read.c`, `ccore/src/ifc_geom.h`, test `ccore/test/ifc_read_test.c`
(`own_goldens`).

**Ticket IFC3 (the importer, and more types).**

- The importer (`cc_ifc_import`, `ccore/src/ifc_in.c`) takes a product whose `Representation` is
  a plain `IfcProductRepresentation` too, for every class (before: only the five classes it
  parametrises, IfcColumn, IfcBeam, IfcSlab, IfcWall, IfcFooting; the others, IfcMember and
  IfcBuildingElementProxy among them, were dropped without a word, as Rust's `import_str` does).
- The importer parametrises a product's `IfcExtrudedAreaSolid` (wall, column, beam, slab,
  footing, extruded polygon) only when that extrusion is all its Body holds (one item, through
  mapped items of one item each; a mapped item that maps a map already on the way down is a
  cycle, drawn as nothing, and does not count). Rust (`import.rs extruded_solid`) takes the first
  extrusion it meets and drops the other items without a word; C imports such a product as a mesh
  of its whole Body (every item the display reader draws).
- Profile slopes are drawn: `FlangeSlope` of `IfcIShapeProfileDef` (IFC4) and
  `IfcUShapeProfileDef`, `WebSlope` and `FlangeSlope` of `IfcTShapeProfileDef`, `LegSlope` of
  `IfcLShapeProfileDef` (in the file's plane angle unit; a slope not > 0 or not under 45 degrees
  is ignored). The inner face is turned by the slope about a reference point where the nominal
  thickness holds, taken from IfcOpenShell's source (v0.8.0, `src/ifcgeom/mapping/I/U/T/
  LShapeProfileDef.cpp`; the spec's figures do not dimension it): I at the flange's edge, U at
  half FlangeWidth from the web's back, L at the box's centre lines, T's web at mid-depth and
  T's flange at a quarter FlangeWidth from the centre. Corners where two sloped faces meet are
  their intersection; fillets and edge radii at those corners are arcs tangent to both faces
  (any angle). One difference from IfcOpenShell: its T flange line ends at x = WebThickness/2
  with x/2 - WebThickness (not x/2 - WebThickness/2) as the run, so its underside is a little
  less steep than FlangeSlope; C keeps the slope exact (`tshape_slope`: C 3998.77 mm^2, IfcOpenShell
  3965.28, 0.84 %; the corpus file `files/IfcTShapeProfileDef.ifc`: 0.21-0.22 %). Not drawn:
  the slopes of `IfcAsymmetricIShapeProfileDef` (IFC4 BottomFlangeSlope, TopFlangeSlope) and the
  I-shape's FlangeEdgeRadius (as before).
- `IfcCenterLineProfileDef`: the curve offset by Thickness/2 to both sides, mitred joins (a join
  sharper than a miter of 4 half-thicknesses is bevelled), square ends; a closed curve gives a
  ring. Area = Thickness x length on a polyline. IfcOpenShell 0.8's source maps it to nothing
  (`return nullptr`); 0.9 fills the area the curve encloses (`centreline`: 194,444 mm^2 where
  the profile is 30,000; `files/10336--…`: 194,244 mm^3 for a half-ellipse strip C draws as
  120,811, Thickness x arc length x depth = 121,100 by hand).
- `IfcCompositeProfileDef`: each of its profiles swept on its own (extruded or revolved), a
  union of regions as IfcOpenShell maps it (a collection); one profile: that profile. A tapered
  extrusion of a composite keeps the warning `unsupported profile IFCCOMPOSITEPROFILEDEF
  (skipped)`. The importer makes a mesh of an extrusion of several regions.
- `IfcMirroredProfileDef`: the parent profile, as placed by its Position, mirrored about the y
  axis (the derived Operator, applied as `IfcDerivedProfileDef`'s operator is: after the
  parent's Position, Rust's order). IfcOpenShell 0.9 mirrors inside the parent's Position (a
  parent placed at x 500 stays at x 400..600 there; C draws it at -600..-400).
- `IfcGeometricSet` and `IfcGeometricCurveSet` in a body: their elements are read as items (a
  surface or solid drawn, or warned under its own type). Curves, points and texts in a body
  (`IfcPolyline`, `IfcIndexedPolyCurve`, `IfcCompositeCurve`, `IfcTrimmedCurve`, conics,
  B-splines, offset curves, `IfcCartesianPoint`, point lists, text literals) bound no volume:
  the scene has no edge-only element, so they are not drawn, and the warning is
  `no body by design: <TYPE> (a curve, point or text in a body: nothing to draw)` instead of
  Rust's `unsupported geometry <TYPE> (skipped)`. IfcOpenShell 0.9's iterator draws no faces for
  them either (`geometric_set`; `files/452--line-segment-straight.ifc`: 0 products).
- Representation items nest 64 levels deep (boolean operands, CSG roots, mapped items,
  geometric sets), Rust 32: `representation nesting too deep` past 64. The deepest public file
  needs 49 (`files/588--…`; `files/599--…` 39, `S_Office_Integrated Design Archi.ifc` 42).
  Work stays bounded by the budget's visits and triangles; each level costs 0.2 KB of stack on
  wasm32 (item 128 + boolean_result 80 bytes, `-fstack-usage`; 1.1 KB before the leaves and the
  boolean arithmetic were made NOINLINE), 13 KB at the bound against the module's 64 KB stack.
- Import goldens of the C's own: `golden/ifc_c/import/<name>.json` and `<name>.report.txt` for
  every `golden/ifc_c/fixtures/*.ifc` (ids masked as in `golden/ifc/import/`; the report with its
  C-only `unsupported: ` lines), `CCORE_WRITE_GOLDEN=1 test/run.sh ifc_in` writes them again;
  `ccore/test/ifc_in_test.c` (`own_imports`) also checks each model's volume against the hand
  value.

## 12. The PDF's font files are packed by the C core's own deflate (ticket D6)

**What.** Rust's `Sheet::to_pdf` (pdf-writer) embeds each Roboto file it uses as a FontFile2
stream packed with flate2 (zlib, level 6, miniz_oxide). The C writer (`ccore/src/pdf_out.c`)
packs the same file with the C core's deflate (`cc_gzip` of `inflate.h`: LZ77 and fixed Huffman
codes in one block) in a zlib wrapper (head `78 01`, the same Adler-32). So in a Roboto PDF these
two streams differ, with their `/Length` and every offset after them (the cross-reference table,
`startxref`). Every other byte is Rust's: the content stream, the font dictionaries, the widths,
the ToUnicode maps, the information dictionary, the order of the objects and the free list. A
Helvetica PDF has no packed stream: it is Rust's file byte for byte.

**Why.** Byte equality would need a port of miniz_oxide's compressor (its hash chains, lazy
matching and dynamic Huffman trees), for no gain to a reader: any inflater gives the same font
file. Sizes (`pdf_out` suite): Roboto-Regular 349 400 bytes packs to 232 017 (Rust 203 917),
Roboto-Bold 351 108 to 235 223 (Rust 206 258); a Roboto sheet using both weights is about 57 KB
larger than Rust's.

**What Rust sees.** `pdf/*-helvetica.pdf` and every `.content` are the same bytes
(`pdf_out_test`). `pdf/sample-roboto.pdf`: the same objects in the same order, each the same
bytes but the two FontFile2 streams, whose dictionaries match but `/Length` and which both
inflate (Rust's and the C core's) to the shipped font file.

**Also.** The writer takes the font files from its caller (`cc_pdf_options.roboto`, 350 KB each:
not in the wasm) and refuses any other file (`CC_PDF_ERR_FONT`): its glyph ids and metrics are
the table of `roboto_widths.inc`, so a character outside it (U+0020–U+024F and 20 punctuation
marks) is glyph 0, where Rust reads the whole `cmap`. A pen of negative or NaN width is
`CC_PDF_ERR_PEN` where Rust's pdf-writer asserts (a panic).

**Where.** `ccore/src/pdf_out.c` (`zlib`, `embed_font`), `ccore/test/pdf_out_test.c`.
## 15. The history store's texts and queries (ticket H1, ADR 0017)

**What.** The C history store (`ccore/src/repo.c`, `diff.c`) reads a Rust-written history folder
and gives Rust's texts byte for byte where `golden/history/` holds them (log, branches, revisions,
`model_at`, `version::diff`, status, the damaged, missing, unknown-revision, format and no-history
errors). It differs from Rust in these places:
- A history object that does not parse (a commit or tree with a missing, unknown or mistyped
  field, broken JSON, a broken gzip stream) gives `damaged history: object <hash> ` then the C
  reader's own words, not serde's or flate2's (`missing field `tree``, `unknown field `x``,
  `gzip: …`). The prefix, the hash and the refusal are Rust's.
- An I/O failure Rust prints from the OS (`io: … (os error N)`) prints `io: No such file or
  directory (os error 2)` for a missing file and `io: input/output error` for the others.
- A history without `refs/heads/` (Rust's `init` always makes it) has no branches; Rust's
  `read_dir` fails there.
- A commit whose first parent is not 64 bytes long is reported at once (`… is not a hash`), where
  Rust carries it one step further before the same refusal.
- The session queries `versions`, `branches`, `version_changes` and `pending`
  (`ccore/session/versions.h`) have no Rust twin: the egui History panel reads `bimble_core::repo`
  directly. Their JSON is the C core's; the values in it are Rust's (`Repo::log`, `branches`,
  `version::diff` in serde's form, `Repo::status`).

**Why.** Files are untrusted input (CLAUDE.md): the C reader refuses the same objects, with the
same first words; matching serde's and flate2's error texts word for word would mean porting
them. The queries are what the lab's History page needs (ADR 0038).

**What Rust sees.** Nothing: the store is read only (writing is ticket H2), and every intact
folder Rust writes reads the same.

**Where.** `ccore/src/repo.c`, `ccore/session/versions.c`; tests `ccore/test/repo_test.c`,
`ccore/test/versions_test.mjs`.
## 13. Reference plans read from PDFs (ticket L1, ADR 0021)

The C reader (`src/pdf_in.c`) gives Rust's polylines and notes byte for byte on every golden of
`reference/` (`bimble_drawing::reference::read` over lopdf 0.47: the hand-written fixtures and D1's
plan PDFs, compressed or not, 30,000 segments included), with one exception and these differences
on files the goldens do not hold:

- **Encrypted PDFs are refused** with "the PDF is encrypted: …". lopdf decrypts a PDF whose user
  password is empty; `encrypted.pdf` (a fake standard security handler) gives "the PDF has no page"
  in Rust. Decryption (RC4, AES) is not in the C core: a plan from an architect is rarely
  encrypted, and the text says what to do. `test/ref_test.c` checks both texts.
- **Filters**: only FlateDecode (with PNG predictors) is decoded. lopdf also decodes LZW,
  ASCII85, ASCIIHex and RunLength: such a page content is read raw in C (no lines, or fewer).
- **Inline images** end at the first white space + `EI` + white space; lopdf counts the bytes from
  the image's size first. Only data holding " EI " differs.
- **Error texts** of damaged files: "not a readable PDF: couldn't parse input" (lopdf's most common,
  the one of every bad golden) for any file whose header, cross-reference data and rebuilt table
  all fail; lopdf has other texts for rarer cases. "page 1: the page's content passes 256 MB" for
  lopdf's MemoryLimitExceeded text.
- **Reals** are parsed to the nearest double, then rounded to f32 (lopdf: `f32::from_str`). The two
  differ only for a decimal text within 2^-53 of a halfway point between two f32 (more than about
  15 significant digits); no golden has one.

In the session (`session/links_ref.c`): `link_pdf` takes `offset_mm` and `rotation_deg` beside
`path` and `scale` (a C addition, so agents and tests can place a plan exactly; without them the
placement is Rust's, viewing.rs place_reference). A reference in a section view is placed as it is
(Rust also centres it on the model along the section). The default scale is the view's (Rust: the
"Add reference plan" form's 1:N, default 100).
## 14. DXF reference plans (ticket L2, ADR 0021)

The reader (`src/dxf_in.c`) prints Rust's polylines and notes byte for byte on every file of
`golden/dxf/fixtures/` (`golden/dxf/*.txt`, from `ccore_golden`; suite `dxf_in`). Where it differs
from `reference::read_dxf` over the `dxf` crate 0.6.2:

- **Additions, not in Rust's output:** each polyline's layer (an entity of layer `0` in a block takes
  the INSERT's, as AutoCAD), the LAYER table's on / frozen, and TEXT / MTEXT anchors (placed, with
  their height and text). Rust reads no layer and no text. Nothing is filtered by layer: the
  polylines stay Rust's.
- **Budgets Rust has not:** at most 2^24 entity visits through blocks (a block bomb: Rust loops
  for ages; C stops and adds the note "stopped expanding blocks (too many copies)"); an arc or
  ellipse whose `while sweep <= 0 { sweep += TAU }` would take more than 2^20 turns is not drawn
  (Rust does not end); an arc of 2,000,000 points or more is not made, only noted as truncated
  (Rust allocates it and may abort). 102 groups and xdata control groups are read without
  recursion (Rust recurses).
- **Refused, Rust reads them:** binary DXF and DXB ("not a readable DXF: binary DXF is not read
  by the C core; save it as a text (ASCII) DXF").
- **Malformed files only** (a well-formed file reads the same): the CLASSES, TABLES, OBJECTS and
  THUMBNAILIMAGE sections are skipped to their ENDSEC, not parsed (Rust can fail inside them);
  header variables other than `$INSUNITS` and `$ACADVER` are not checked for their codes; handles
  (codes 5, 330…) are not parsed as hex; the `\U+XXXX` and `^` escapes in strings are not decoded
  (block and layer names compare as raw bytes); a double in an error text is printed as `{:?}`, not
  as the crate's `{:.12}`; a code line starting 0xEF ?? 0xBF keeps the 0xBF.
- **Floats:** sin, cos and atan2 are the C core's correctly rounded ones (`ifc_math.h`); glibc's
  differ in the last bit for about 0.1 % of inputs. All 14 fixtures match byte for byte today; the
  suite accepts a number within 4 ulp and prints how many it needed (0).
- **Session:** `link_dxf` is L1's path (`session/links_ref.c` `cc_refs_link(s, "dxf", …)`, placed
  as section 13 says; `ref_read.c` hands the DXF to `cc_dxf_read` and its polylines to
  `cc_refread_push`). `manage_links` by id (`session/links_dxf.c`: move, rotate, show, hide,
  reload, remove) is a C addition for agents and tests: Rust's Manage links window has Reload,
  Reload from… and Delete, and moves a reference by dragging it (`views.rs:203`), turns it in the
  view form.
## 16. The PDF export in the app (ticket D7)

**What.** The queries `sheet_pdf {sheet, path?}` and `view_pdf {view, path?}`
(`ccore/session/sheet_svg.h`) are the C core's carriers of Rust's `export_sheet`
(`viewing.rs`, the sheet as `Sheet::to_pdf`) and of the plan panel's "Export PDF…" (`lib.rs`,
`plan_options` with the view: its scale, the smallest paper, not bare). They differ from Rust in
three places:
- Rust compiles the Roboto files into the program (`include_bytes!`), so its PDF always embeds
  Roboto. The C core's PDF is **Helvetica by default** (ticket SIZE1, the owner's decision of 25 Nov
  2026: "Default to Helvetica (0 bytes, PDF viewers have it built in)! Roboto as optional"): the
  drawing is laid out with Helvetica's metrics (the style's other font, `SheetFont::Helvetica`), so
  the file is Rust's Helvetica file, with nothing embedded. No Roboto file is shipped (not in the
  program, the wasm, the release archive nor the web build: 0.7 MB saved). Roboto is optional: the
  body asks `"font": "roboto"`, and the carrier must have the two shipped files (the native host:
  `$BIMBLE_FONTS`, else the repository's `crates/bimble-drawing/fonts/` in a dev build, else
  `<config dir>/bimble/fonts`; the page hands none over). `query pdf_fonts` says
  `{"helvetica":true,"roboto":bool}`; the Export dialog offers Roboto only when it is true. Asked
  for without the files, or with other files, the sheet is drawn in Helvetica and a note goes to
  the log: the export does not fail.
- Rust asks where to save (`files::save`). Without `path`, and always in the wasm module, the call
  answers the PDF's bytes themselves (status 200), the one query whose answer is not JSON; the page
  downloads them as `"<number> <name>.pdf"` (a view: `"plan-<name>-1-<scale>.pdf"`), Rust's names.
- The version in the frame's note and the producer is the C core's (deviation 11).

**What Rust sees.** By default (with or without the Roboto files at hand), with the test clock
(2026-01-01), the file named `frame.json` and Rust's version (`cc_paper_set_version`, tests only),
`sheet_pdf` of S-101 is `golden/drawing/pdf/S-101-helvetica.pdf` byte for byte, the producer
masked (`test/sheet_pdf_test.c`). With `"font": "roboto"` and the files, the file is D6's Roboto
file (deviation 12).

**Where.** `ccore/session/sheet_svg.c` (`cc_paper_pdf_query`), `ccore/host/fonts.c`,
`ccore/src/wasm_api.c` (`host_fonts`); tests `ccore/test/sheet_pdf_test.c`, `ccore/test/lab_export.mjs`.

## 16. Views of a read-only scene (ticket S1, ADR 0021)

An IFC opened as it is, or a `.bimbleview`, now offers Rust's views: the 3D view, then a plan and
a ceiling plan per storey (viewing.rs `refresh_views` on `Scene::levels`: key the storey's
GlobalId text, or its name when it has none). `open_views`, the plan tabs and `view_clip` match
Rust's `default_views`, `auto_name` and `resolve` byte for byte (`golden/ifc/views/`, from
`ccore_golden`; `ccore/test/views_test.mjs`). What differs:

- **No side views file.** Rust keeps the views of an IFC in a side file (`open_side_file`,
  `ViewsFile`): stored views and sheets over the defaults. The C core has none: the views of a
  read-only scene are derived from its storeys, never stored; `set_view` on one is refused as
  every edit of a read-only scene ("this file is read only: import it to edit").
- **The query `grid_axes` is new** (`session/views.h`, `cc_sv_grid_axes_json`), for G2 to draw:
  `{"axes": [{"end_mm", "start_mm", "tag"}], "levels": [{"elevation_mm", "key", "name"}],
  "links": [{"axes", "levels", "name", "visible"}]}`, null with nothing open. A read-only scene's
  axes are `Scene::grids` as read and its levels `Scene::levels` (the golden's `grid_axes` line);
  an editable model's axes are its grids' (ADR 0056, G1; id order, axes in order) and its levels
  its storeys (id order); then each linked model's, in link order. Rust has no such query.
- **`view_clip`** (already a C addition, ticket C1) works on a read-only scene: `cut` from
  `resolve` and `elevation` the plan's level as `resolve` finds it (the lowest with that key).
- Code: the levels are `cc_level` (`src/view_effective.h`): `cc_views_default_lv`,
  `cc_views_effective_lv`, `cc_view_resolve_lv` (`src/view_range.h`); the model functions are
  these on the model's storeys, so every model golden keeps its bytes. The session takes the
  levels of what is open (`session/views.c` `cc_sv_levels`).

**What Rust sees.** Nothing new on a file: no model field, no stored view. A Rust viewer opening
the same IFC offers the same views with the same ids.

## 17. The schedule list of a read-only file, and the palette's folding (ticket Q2)

- **`schedule_list` on an IFC or a .bimbleview opened as it is:** Rust's stored schedules are the
  side file's next to the IFC (`schedules.rs` `stored_schedules`, `side_views`); the C core reads
  no side file, so it answers the built-in ten (`session/schedules.h`). Without a side file the two
  answer the same text. Everything else of `schedule_list`, `schedule_rows` and `schedule_csv` is
  Rust's byte for byte (`golden/schedule/`, `test/schedules_test.mjs`).
- **`palette_match`:** Rust's `fold` is `str::to_lowercase` (all of Unicode, final sigma as `ς`)
  and its words split on `!char::is_alphanumeric` (all of Unicode). The C core (`session/palette.c`)
  lowers the Latin, Greek and Cyrillic blocks (the table of `src/quantity.c`'s natural sort), and
  splits words on ASCII non-alphanumerics, Latin-1 punctuation and symbols, General Punctuation
  (`…`), U+2190..U+2BFF (arrows, maths, shapes) and the White_Space characters. Every ribbon label
  and Rust's unit tests (`ribbon.rs:1802`, `:1887`) give the same indices (`test/palette_test.mjs`);
  a label with an upper-case letter of another script, a final `Σ`, or a separator of another block
  may match differently.

## 19. `query capabilities` and the actions a read-only file keeps off (ticket FAKE1)

- **`query capabilities`** (`session/capabilities.h`): a C addition, Rust has no such query. One
  object of booleans, keys sorted: what this build of the core serves that is not an action (a page,
  a panel, a data set, an export). The page reads it once (`web-ui/src/bimble.ts`) and shows the one
  "Not available yet" state for a false row; the Rust viewer and its native twin are asked nothing
  and count as "everything" (null in the page). The rows follow the dispatcher, not a list in the
  page: the viewer-only wasm answers false for quantities, schedules and the exports (VIEW_DENY),
  the wasm modules false for versions (no folder); `test/capabilities_test.c` ties each false row
  to a stub answer or an action never enabled, and each true row to an answer.
- **`project` and `new_sheet` on an IFC opened as it is:** Rust enables both with any file loaded
  (`ribbon.rs:611 _ => loaded`): its project data and its sheets live in the side file next to the
  IFC. The C core reads no side file and answers `project` "this file is read only", so it keeps both
  off for a read-only file (the 2 actions `parity.md` still shows as differing in `ifc_readonly`).
  Every other enable rule ACT1 changed is Rust's own (`results/parity.md`).
