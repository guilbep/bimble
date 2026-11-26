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
  for ages; C stops and adds the note "stopped expanding blocks (too many copies)"); at most
  4,000,000 points made, drawn or dropped (twice the output's cap: a file of a million arcs of NaN
  radius, or a block of them copied, is bounded; C stops and adds the note "stopped drawing (more
  than 4000000 points made)"); an arc or ellipse whose `while sweep <= 0 { sweep += TAU }` would
  take more than 2^20 turns is not drawn (Rust does not end); an arc or ellipse sweeping more than
  two turns is drawn as one turn, 65 points (the same curve; Rust makes every turn: one ARC to
  11,200,000 degrees was 1,991,113 points and 8 s, a 4 KB file of 100 of them minutes), up to two
  turns the points are Rust's. Without that work, the same answer: an arc of non-finite centre or
  radius is dropped before its points are made (Rust's Out::push drops it), and once the output is
  truncated an arc that cannot fit is not made. The truncated case is Rust's: "stopped at 2000000
  points", and a later polyline that fits is still pushed. 102 groups and xdata control groups are
  read without recursion (Rust recurses).
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
  `$BIMBLE_FONTS`, else the repository's `ccore/fonts/` in a dev build, else
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

## 18. Files from the page: the dialog, the recent files, a dropped file (ticket FS1)

**What.** Rust asks where with the system's dialog (`rfd`, `files.rs`); a browser page cannot show
one for the native host, so the page draws its own (`web-ui/src/dialogs/FileBrowser.vue`) over the
host's queries `fs_roots`, `fs_home`, `fs_list` (`ccore/host/fs.h`, the path rule). Rust has no
such queries. The recent files keep Rust's rule and file (`settings.rs` `recent`, `RECENT` 12,
`opened`; the same `settings.json`), with these differences:
- The C core writes the field `recent` only: the file is read, that field replaced, the whole
  written back with sorted keys and each number as its text in the file (Rust writes every field
  from its struct: what it does not know is dropped there, kept here). A missing file is created
  with `recent` only; Rust reads it with its defaults for the rest. A file that is not a JSON
  object is left alone (Rust would overwrite it with its defaults on its next save).
- The list is read again from the file before each change, so the two apps running at once do not
  drop each other's entries (Rust reads it once at start).
- `query recent_files` adds `kind` (by extension) and `mtime` (the file's, seconds; null when
  missing) to Rust's `{exists, folder, name, opened_unix, path, schema}`; `opened_unix` stays null
  as in Rust. A hidden action `recent_remove {path}` (Home's remove) has no Rust counterpart.
- The host writes the settings file only when it knows the folder: `--config-dir`,
  `BIMBLE_CONFIG_DIR`, or Rust's folder for `bimble-ccore open` (the desktop). The plain host
  (`bimble-ccore <port>`: tests, `dev.sh`) keeps the list in memory.
- A file dropped on the page: Rust gets its path from the window; a browser gives bytes only, so
  the native page sends them to `POST /stage` (the host's own temp folder, 256 MB at most, removed
  when the host stops) and opens that copy with `{dropped: true}`: under its name, no recent file,
  Save asks where (as a new model's). A dropped PDF or DXF is refused there (a reference plan keeps
  its file's path; Rust links the dropped file where it is).

**Where.** `ccore/host/fs.c`, `ccore/session/settings_file.c`, `ccore/session/calls.c` (FS1
blocks), `web-ui/src/files.ts`; tests `ccore/test/fs_test.mjs`, `ccore/test/lab_files.mjs`.

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
  IFC. The C core reads no side file and answers `project` (and `storeys`, `materials`) "this file
  is read only", so it keeps both off for a read-only file (the 2 actions `parity.md` still shows as
  differing in `ifc_readonly`); the page does not ask those queries of a read-only file.
  Every other enable rule ACT1 changed is Rust's own (`results/parity.md`).

## 20. Views of a read-only scene (ticket S1, ADR 0021)

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

## 21. View verbs, sections in the viewport, what a view hides (ticket SEC1, ADR 0021, 0046)

- **Verbs `view_add`, `view_remove`, `view_edit`** (`src/verbs_views.h`): C additions. Rust has no
  such verbs in `dispatch.rs`: its viewer makes the `View` (`views.rs new_plan`, `new_section`, the
  3D view and Duplicate of `viewing.rs:781-840`) and stores it with `Command::AddView`, `SetView`,
  `RemoveView` (`store_view`, `remove_view`). The verbs make the same `View`, named the same way, and
  the same one command (one undo step); their transcript is our own golden (`golden/views/verbs.txt`,
  `golden/views/model.json`, `test/verbs_views_test.c`). What is Rust's byte for byte: the model
  Rust wrote with sections, a crop and a plan hiding `IfcSlab` (`golden/drawing/model.json`) is read
  and written back unchanged, removing a view and undoing gives the same bytes, and `view_add` +
  `view_edit` make Rust's "Section B-B" field for field (but its id).
- **A section's `origin` when `view_add` gives none** is computed by the session (`calls.c`
  `sec1_section_origin`): the middle of the scene's bounds, rounded, as `new_section`; the verb
  itself needs it (a verb sees the model, not the scene).
- **`view_edit` lists:** each list given replaces the view's, sorted and unique (Rust's form keeps
  `BTreeSet`s, so its lists are sorted too). The view range's levels are `view_edit`'s; their
  offsets stay Rust's `set_view`'s.
- **`query view_clip` of a section** (C addition, ticket C1's query): the box Rust's `view_clip`
  gives (scene bounds in the section's turned frame, cut at its line, as deep as its depth, cropped)
  plus `dir`, `facing`, `origin`, `depth_mm`, `yaw` for the page's camera. `cut` stays a plan's (null).
- **What a view hides is the view's** (Rust `apply_visibility`, `save_visibility`,
  `show_all_in_view`): the session's "in view" sets are read from the active view on a view change
  and after every edit, undo and redo; hide, isolate and unhide in view and Show all in view store
  them with `view_edit` (one undo step; Rust: one `SetView`). The view's class, family, storey and
  material lists hide their elements on screen (`visibility_sets` `by_view`, a C addition). Rust
  also keeps them while the view is shown unsaved in a read-only IFC (its side file); the C core
  has none, so a read-only scene keeps "in view" session-wide as before (DEVIATIONS 20).

## 22. On paper, Export view and several sheets as one PDF (ticket PAPER1)

- **`query view_svg`** (`session/sheet_svg.h`): a C addition next to `sheet_svg`. Without `bare` it
  is the drawing of `view_pdf` (lib.rs `plan_options`) as `Sheet::to_svg()`, equal byte for byte to
  D4's `golden/drawing/view/<nn>.svg` (`test/paper_view_test.c`). With `bare: true` it is the
  drawing paper.rs `refresh_paper` shows On paper; Rust shows it as a raster of the prepared sheet
  (egui texture), the lab as the SVG itself (pan and zoom of the picture). The answer adds the
  paper's name, orientation, size and the view's scale.
- **On paper of a read-only scene:** removed by ticket LNK1. Rust draws the views of any loaded
  scene, an IFC opened as it is too (`ribbon.rs:597 two_pictures`, `:611 _ => loaded` for Export
  view); the C core now does the same: the drawing takes a model made from what the read-only scene
  read (`session/paper_scene.h`: storeys, materials, grids, georeference, one element per scene
  element) and `paper_view`, `on_paper`, `open_views.paper.offered`, `view_svg` and `view_pdf` work
  there. Proof (`test/paper_view_test.c` `test_lnk1`): `golden/ifc/frame.ifc` opened as it is
  draws its 6 plans and ceiling plans (bare or not) and the PDF of a plan byte for byte as
  `golden/frame.json` opened for editing draws them. What may still differ on other files: the
  title block's project fields (a read-only scene has no project information: Rust's comes from
  its side views file, which the C core has not, DEVIATIONS 20) and an element's storey or material
  when the IFC names two of them alike (the made model keys them by name).
- **Placing and measuring switch to Model:** Rust does it when it paints the paper (paper.rs:217,
  each frame); the C core when it is next asked (`actions`, `open_views`, the change JSON) after
  measuring or placing started. The note Rust shows goes to the log.
- **The Export view window** (`on_paper`, pressed while open: ribbon.rs:722) is the lab's Export
  dialog on "This view". The lab opens it without running the action (as Project information and
  History), so the core's pressed state stays false in the lab; the action toggles it for any other
  caller. The dialog draws the active 2D view (else the first) and lists the 2D views; Rust's
  "Storey plan" choice (a storey and a cut height without a view), its paper choice and its
  template check boxes are not offered. File names are Rust's: `plan-<view>-1-<scale>.pdf` / `.svg`.
- **`query sheets_pdf` and `cc_paper_pdf_pages`:** Rust has no multi-page PDF (`Sheet::to_pdf` writes
  one page; the egui app exports one file per sheet). C-own golden
  `golden/paper_c/S-101+S-102-helvetica.pdf` (checked with `qpdf --check` and `pdftotext`: 2 pages,
  both sheet numbers). The file is the one-page file's layout: page 1 keeps ids 3 and 4, each further
  page takes two ids after the fonts (page, content), the two fonts are shared (Roboto: one subset of
  all pages' glyphs), /Title is the first sheet's. One page is `cc_paper_pdf`'s file byte for byte
  (the `pdf_out` and `sheet_pdf` suites unchanged). The lab names it
  `<first number>-<last number>.pdf` (a C choice: Rust has no such file).

## 23. The user's settings and About (ticket SET1)

**Oracle.** `crates/bimble-viewer/src/settings.rs` (`Settings`, `from_json`, `to_json`, the file's
folder), `ui_theme.rs` (themes, fonts, text sizes), `lib.rs:4023-4190` (Settings, About),
`calls.rs:253` (`state.user`), `ribbon.rs:591` (`settings`, `about` always enabled).

**Same.** One file for both apps, the same eleven fields, names, defaults and reading rules
(clamps, ids, `short_line`, 0.14's `wgs84`, a broken file reads as the defaults); written as Rust's
`to_json` writes it (a sorted map, compact, `text_size` an f32 as a JSON f64: 12.3 is
`12.300000190734863`); `state.user` `{email, name, trigram}`, null with no name.

**Differs.**
- **Unknown members are kept** on write (Rust's `to_json` drops them): a file written by a later
  version keeps what this one does not know. Rust reads such a file unchanged (it ignores them).
- **`set_settings` refuses** a value out of range, an unknown id or field, a line over 120
  characters or with a control character (Rust's reader clamps or falls back; its dialog cannot
  produce them). A refused call changes nothing.
- **A file that is not a JSON object is left alone** (FS1's rule): the change is kept for the
  session and `query settings` says so in `note`; Rust overwrites it with the defaults and the
  change.
- **Applies**: `memory_share`, `moving_fps`, `moving_proxies` and `gpu_cull` are stored and round
  tripped but used by nothing (the lab's viewport is the page's WebGL one, natively too): `query
  settings` reports `applies: false` and what each would need. They are not shown in the lab's
  dialog. `recent` does not apply in the browser (no paths).
- **The look**: the lab maps Rust's theme ids onto its own design tokens (`web-ui/src/shell/theme.ts`).
  `grey`, Rust's default, is the lab's own default look (the Bimble design), not Rust's grey
  surfaces; the five others follow Rust's colours. `roboto`, Rust's default font, is the lab's own
  text font (Source Sans 3; the lab ships no Roboto); `ubuntu` asks the system for Ubuntu.
- **Trigram**: upper case for Latin, Greek and Cyrillic letters only (`char::to_uppercase` knows
  every script).
- **About**: `query about` adds the C core's build (`ccore/VERSION`) and the page's commit to Rust's
  three lines; "Graphics" is the page's WebGL renderer string, not a wgpu adapter.
- **The goldens** `golden/settings/*.json` are written by hand in Rust's form (this ticket had no
  Rust build): `ccore_golden` mode `settings` is still to write them from Rust.

**Where.** `ccore/session/settings.c`, `settings_file.c` (the writer, shared with FS1),
`calls.c` and `capabilities.c` (SET1 blocks); `web-ui/src/dialogs/Settings.vue`, `About.vue`,
`web-ui/src/shell/theme.ts`, `web-ui/src/ccore.ts` (SET1 block); tests `ccore/test/settings_test.c`,
`ccore/test/lab_settings.mjs`.

## 24. The materials browser: verbs, queries and the page (ticket MAT1, ADR 0031)

**Oracle.** `crates/bimble-core/src/command.rs:183-198,317-341,399-458` (AddMaterial,
RemoveMaterial, SetMaterial: checks, error and describe texts), `library.rs` (the table, `lookup`,
`to_material`), `model.rs:48-85,173-185` (MaterialCategory, Material),
`crates/bimble-viewer/src/materials_page.rs` (the page), `ribbon.rs:611,791` (the action).
Golden: `golden/materials/` from `ccore_golden` `materials_files` (branch `oracle/mat1`).

**Same.** The command `set_material` (it was refused before MAT1): JSON, checks in Rust's order,
error texts, describe and undo texts, canonical writing; the 26 commands of
`golden/materials/transcript.txt`, the model after them (`after.json`) and its IFC4 export
(`after.ifc`) byte for byte; undoing them gives `frame2.json` back. AddMaterial does not check the
name (Rust's): only SetMaterial refuses a name taken (trimmed, ASCII case ignored). The library is
Rust's (16 grades, values and sources). The action `materials` is enabled as Rust's (`_ => loaded`).

**Differs.**
- **Verbs.** Rust's page applies the commands itself; its dispatch has no material verb. The C
  core's writers are verbs (ADR 0068 rule 1): `material_add`, `material_set`, `material_remove`
  are C additions (`src/verbs_materials.h`), one history step each. `material_add` refuses a name
  already there (Rust's page offers no Add for a grade in the model, `materials_page.rs:180`, but
  its command would add it), and adds a blank material by name (Rust's page has no blank add).
  `material_remove` refuses a material in use with the element count
  ("material_remove: <name> is the material of N elements: give them another material first"), where
  the command's text names the id only; Rust's page has no remove at all.
- **Queries.** `materials` stays Rust's summary; `materials_full` (every field) and
  `material_library` (the table) are C additions, and `materials_page` (the page's rows: the
  values or the grade's, the family's screen colour and pattern, the paper hatch names) is the
  session's. Rows sort by the lowercase name as Rust; lowercase covers ASCII and Latin-1 only.
- **The page** shows the family's on-screen pattern (lines, stipple) as a swatch; Rust paints the
  paper hatch of the standards (`paint_hatch`). The default standards set no paper hatch per
  family, so both say "the family's own pattern". On-screen hatching of the viewport is not drawn
  by the lab yet: the colour changes on screen at once, the hatch shows on paper drawings.
- **Assign** (the selected elements to the material): Rust does it in the Properties panel; the C
  page has a button too, through Rust's verb `set_material`.

## 25. Show all and Show previous (ticket ISS72, GitHub issue #72)

**Oracle.** None: Rust v0.19.0 has neither action. Its nearest are `clear_temporary` ("Show all
hidden for now") and `show_all_in_view` (`viewing.rs:490`), each of which leaves the other part
hidden; `ribbon.rs:130` lists 54 actions. The office's engineer asked for both buttons (issue #72).

**Differs (C additions).**
- **`query actions` has 56 rows**: Rust's 54 in Rust's order, then `show_all` ("Show all") and
  `show_previous` ("Show previous"), tab View, no shortcut, never pressed. Rust's rows are unchanged.
- **`show_all`**: `clear_temporary` then `show_all_in_view`: everything shown in the current view,
  whatever hid it (hidden or isolated for now, in the view, by the view's class, family, storey,
  link or material lists). One undo step when the view stored something (as `show_all_in_view`),
  none otherwise. Enabled when something is hidden for now or in the view.
- **`show_previous`**: the visibility state before the last visibility change of the current view
  (state = the temporary set + the view's stored lists; Reveal hidden and the section box are not
  part of it). Every action that changes the state (hide, isolate, unhide, for now and in the view,
  `clear_temporary`, `show_all_in_view`, `show_all`, `show_previous`) pushes the state it left on
  the view's stack; `show_previous` restores the top one and pushes the state it leaves, so pressed
  twice it goes back and forth. The stored part is one undo step (as any in-view change). One stack
  per view, 32 states each, the oldest dropped; session state (not in the file nor the session
  file), cleared when a model is opened, made or closed. Undo and redo push nothing. Enabled when
  the current view's stack has a state.
- **No shortcut**: Rust's H (`lib.rs:2983`, hatching) ignores the modifiers, so Shift H is taken.
- Tests: `test/visibility_test.mjs` (ISS72 checks), `test/lab_visibility.mjs` (the ribbon buttons,
  native and wasm), `test/wasm_run.mjs` and `test/wasm_view_run.mjs` (native = wasm).

## 26. The section box: Box to selection with 1 m, a right-click menu, arrowheads, turned to project or true north (ticket ISS71, issue #71)

**Rust.** `box_to_selection` (lib.rs `fit_section_box`) fits the box around the one primary
selected element, or around everything visible, with `ClipBox::around`'s margin (1 mm + 1e-5 of
the size); it is enabled only while the box is on (ribbon.rs:593). The viewport has no context menu.

**Differs.** The office's structural engineer asked for it (GitHub issue #71).
- **Box to selection** = the bounds of every selected element (the primary, the others, and a
  linked model's selected element moved by its origin - ours, lib.rs:1499) grown by
  `CC_BOX_SELECTION_MARGIN_MM` = 1000 mm on every side (`session/visibility.h`). Nothing selected:
  Rust's fit (everything visible, Rust's margin). It **turns the box on**, so it is enabled with a
  model in 3D, the box on or off (`loaded && !in_2d`; Rust: `&& section_on`).
- **The viewport's right-click menu** (`web-ui/src/shell/ContextMenu.vue`, items in
  `shell/contextItems.ts`): a right click without a drag selects the element under the pointer
  when it is not selected, then shows Box to selection, Hide, Isolate, Show all, Show previous (the
  actions `box_to_selection`, `hide_now`, `isolate_now` and section 25's `show_all`,
  `show_previous`, greyed out as `query actions` says). French: "Boîte sur la sélection" (the
  issue's words) in the ribbon, the palette, the menu and the small screen.

- **The handles** are one arrowhead per face (Rust's `section.rs` overlay: six square handles),
  pointing out along the face's normal, 18 px on screen, the press radius 16 px about the face centre.
- **The box turns** (Rust's `ClipBox::yaw` is always 0 for the section box): two C actions after
  Rust's 54 and section 25's two (`query actions`: 58 rows), `box_north_project` (the default) and `box_north_true` (pressed: the choice the session
  keeps; enabled in 3D when the two turns differ), give the box `yaw` = `plan_turn(true_north)`
  (`georef.h`), as a section's (views.rs): project north is the building's axes (`rotation_deg`, 0
  for every model without a georeference: the box as before), true north the map's
  (`-map_rotation_deg`). A change keeps what the box held: the box before the last change when the
  box is still the one that change made (a round trip gives the same box), else around the
  selection + 1 m in the new frame, else around the box's corners in the new frame. The face drag
  takes its rays into the box's frame; the measure tool's clip, the page's shader, picks, edges and
  handles turn with it. `query visibility` prints the yaw (f32 as serde).

**Where.** `ccore/session/visibility.c` and `.h`, `calls.c`, `measure.c` and `.h` (ISS71 blocks);
`web-ui/src/shell/ContextMenu.vue`, `contextItems.ts`, `BoxNorth.vue`, `Ribbon.vue`, `RibbonTool.vue`
(tooltip), `web-ui/src/glviewport.ts`, `bimble.ts` (ISS71 blocks); tests
`ccore/test/visibility_test.mjs`, `lab_section_box.mjs`, `host_test.mjs`, `palette_test.mjs`.

## 27. Writing the history: commit, branch, switch (ticket H2a, ADR 0017)

**What.** The C history store writes as Rust's `bimble_core::repo` (`ccore/src/repo_write.c`): for
the same model, author, time and message, every object, tree and commit has Rust's name (BLAKE3 of
the plain JSON, byte for byte `serde_json::to_vec`); `golden/history/`'s 4+1 commits replayed in C
give every commit and tree hash of Rust's folder (suite `repo_write`). It differs from Rust here:
- **Relations and grids.** The C model holds two sections Rust's has not (`relations`, ticket X1;
  `grids`, ticket G1). A tree of a model with them carries them as two more maps of objects,
  `relations` and `grids`, after `meshes`; left out when empty, so the tree of every model Rust can
  hold is Rust's, byte for byte. The C reader (`repo.c`) reads them back. Rust refuses such a tree
  (`deny_unknown_fields`: `unknown field "relations"`).
- **The gzip bytes** of an object are the C core's deflate, not flate2's (`Compression::fast`):
  the object's name is the hash of the plain bytes, so names are equal; any gzip reader inflates
  both (H1 already reads either).
- **Errors of the file system** read `io: cannot write <path>` and `io: cannot create the folder
  <path>`, where Rust prints the OS error; a platform without folders (the wasm module) cannot
  write a history: `io: no history folder here` (the session says `no history folder in the
  browser` first).
- **The author.** Rust's `Settings::author()` (settings.rs:158) is `name <e-mail>`, the name alone,
  the e-mail alone, or else `$BIMBLE_AUTHOR`, `$USER`, `$USERNAME`, `unknown`. The C session (L5)
  has no environment: without a name (`state.user` is null) `action commit` is refused with
  `commit: set your name in Settings first (the author of each version)` and the History page links
  to Settings. With a name, the author is Rust's.
- **The session's actions** `commit {message}`, `branch_create {name}`, `branch_switch {name,
  discard?}` (`ccore/session/versions.h`) have no Rust twin: the egui History panel calls
  `Repo` directly (lib.rs:1903-2097). They do what the panel does: save first, start the history
  on the first commit, create then switch to a new branch, refuse a switch while changes are not
  committed (Rust disables the button; `discard: true` is the C core's way through, there is no
  such button in Rust), write the switched model to the file and open it again.
- **Crash safety** is Rust's order (objects first, refs last, each file through a temporary file
  and a rename); the native `write_file` does not fsync, as Rust's `write_atomic` does not.

**Why.** CLAUDE.md: the C model's extra sections must not be lost by a commit, and leaving them out
when empty keeps every Rust-written history and hash unchanged (round-4 rule 4). L5 has no
environment by design (core-architecture.md §5). The actions are what the lab's History page needs
(ADR 0038).

**What Rust sees.** A folder written by C for a model without relations and grids: Rust's own
bytes after inflating, the same log, branches and models. With relations or grids: Rust refuses
the trees that hold them (the commits before still read).

**Where.** `ccore/src/repo_write.c/.h`, `ccore/src/file.c` (`cc_model_object_json`),
`ccore/src/repo.c` (reads `relations`, `grids`), `ccore/session/versions.c`,
`web-ui/src/pages/History.vue`; tests `ccore/test/repo_write_test.c`,
`ccore/test/versions_test.mjs`, `ccore/test/lab_history.mjs`.

## 28. Merging history branches (ticket H2b, ADR 0017)

**What.** The C history store merges as Rust's `Repo::merge` and `merge_models`
(`ccore/src/merge.c`, `ccore/src/repo_merge.c`): `golden/history/`'s scenario replayed in C, then
`variant` merged into `main`, gives Rust's merge commit `9d1b67cbf963…` and its tree, byte for
byte `model_at_6.json`, every object name of Rust's folder (suite `repo_merge`); 26 merge cases ×
3 Prefer give Rust's conflicts, validation texts and merged models byte for byte
(`golden/history/merge/`, a new Rust golden from `ccore_golden` on the oracle). It differs from
Rust here:
- **Relations and grids** (the C sections, section 27) merge by Rust's rules, per object id, after
  Rust's sections: a conflict's kind is `relation` (its name the relation's IFC class,
  `IfcRelConnectsWithRealizingElements`) or `IfcGrid` (its name the grid's), the path as Rust's
  (`properties.Bimble_Connection/Fastener`, `name`). Rust's model has neither section.
- **Equality** is the objects' serde bytes (`cc_model_object_json`), where Rust compares the typed
  values (`PartialEq`): the writer gives one text per value, so they agree on every value a model
  can hold.
- **The walk** queues each hash once (Rust queues duplicates and skips them when reached: the same
  order, so the same merge base); a parent that is not a hash fails when it is queued, where Rust
  fails when the walk reaches it (they differ only past the 1,000,000-commit bound).
- **A fast-forward** reads the target's model before it moves the ref (Rust moves, then reads): a
  damaged target leaves the ref where it was.
- **The session's action** `merge {name, prefer?}` has no Rust twin: the egui History panel calls
  `Repo::merge` with `Prefer::Neither` (lib.rs:2015-2033) and, on conflicts, prints the first 8
  and "merge from the command line with --prefer". The C page lists the first 8, then "and N
  more", and offers keep mine / take theirs, which merge again with `prefer: "ours" | "theirs"`.
  With a `prefer` the answer still lists the conflicts it resolved (Rust's `merge` drops them). The
  merge needs the author's name in Settings, as commit does (section 27). Up to date leaves the
  working file as it is (Rust writes HEAD's model, the same since nothing is pending).

**Where.** `ccore/src/merge.c/.h`, `ccore/src/repo_merge.c/.h`, `ccore/src/repo_write.c`
(`cc_repo_put_commit`, `cc_repo_move_head`), `ccore/src/file.c` (`cc_model_load_unchecked`),
`ccore/session/versions.c`, `web-ui/src/pages/History.vue`; tests `ccore/test/repo_merge_test.c`,
`ccore/test/versions_test.mjs`, `ccore/test/lab_history.mjs`.

## 29. Links: the panel, drawn on paper, kept in the .bimble (ticket LNK1, GitHub issue #69)

- **Linked models on paper (a C addition).** Rust's paper drawing draws the host scene only
  (`paper.rs:119 l.scene`): a linked model is never on a plan, a sheet or a PDF. Issue #69 asks for
  a coordination plan of linked models ("sortir un plan de coordination"), and a `.bimble` whose
  geometry is only links has an empty model, so the C core draws every visible link with the host:
  one scene, the host's elements then each link's (its meshes moved by link origin - host origin,
  `lib.rs:1499 offset`), a link's elements under the host storey of the same name (else none) and
  their own materials (`session/paper_scene.h`). A link hidden in the session (`manage_links hide`)
  or in the drawn view's `hidden_links` is left out. Without visible links the drawing is the
  model's as before: the D4 goldens keep their bytes (`draw_plan`, `draw_view`, `draw_sheet`,
  `paper_view`). Test: `paper_view_test.c` `test_lnk1` (a new model draws nothing; with
  `frame.ifc` linked its plan draws the cut; hidden, nothing again).
- **`query links_panel`** (`session/links_panel.h`, a C addition): the Links panel's rows. `query
  links` stays Rust's `links_json`; the panel also needs each link's path, a reference plan's
  view, placement (move and rotate by numbers) and source.
- **The panel** (`web-ui/src/shell/LinksPanel.vue`, in the Navigator's Links group as Rust's side
  panel; Manage links… opens it): per row show / hide, Reload, Reload from…, remove; a reference
  also ΔX, ΔY (mm) and its angle. Rust's Manage links window has Reload, Reload from… and Delete,
  its side panel Solid / Edges / Hidden per model: the C panel has an eye (Hidden or shown; no
  Edges mode) and no "Show differences" (compare is LNK1b).
- **Reload in the browser:** the wasm module keeps no path, so Reload asks for the file again (the
  browser's picker): a model is linked again under its name (in its place), a reference reloaded
  with the picked bytes staged (`host_action_file`, `wasm_api.h`). Rust's wasm build disables
  Reload there ("Where it was read from isn't known here") and keeps Reload from….
- **Linked models kept in the `.bimble` (a C addition, issue #69: "on link tous les liens qui nous
  intéressent, et ensuite on l'enregistre en .bimble").** Rust v0.19.0 saves no linked model (its
  `Linked` list lives in the viewer; only reference plans, with their lines, are stored in views,
  `source` the absolute path they were read from). The C core writes the session's links into the
  file as the model section `links` (`model.h cc_model_link`), the C core's own like `relations`
  and `grids`, left out when empty (every existing file keeps its bytes: `file2_test.c`
  `links_section`, and the `file`/`file2` goldens): `[{"hidden"?: true, "name", "path",
  "relative"?}]` in link order, `path` as linked (absolute on the desktop, as Rust's `source`),
  `relative` the same file from the saved file's folder. A Rust viewer ignores the section (it
  drops it on save). Opening the `.bimble` links each one again: its `relative` first, then its
  `path`; a file found at neither is kept as a **missing** link (`query links_panel` "missing":
  true, nothing drawn, written again on save) with **Locate…** in the panel
  (`manage_links {"op": "locate", "name", "path"}`, a C addition). Linking, unlinking, hiding or
  showing a model marks the model unsaved (Rust: no change to save); none of it is an undo step
  (as Rust: a link is not in the history). The wasm module has no files: every saved link opens
  missing there and Locate… takes the picked file's bytes. Test: `links_test.mjs` (three IFC and a
  DXF saved, reopened, the folder moved, one file moved and located, Box to selection on a linked
  element).

## 30. Placing: the polygon slab by clicks, single grid axes, snapping to the grid, openings in wasm (ticket SLAB1)

**Polygon slab (Rust's own, golden).** The clicks, the close (a click within 100 mm of the first
corner once there are 3, or Enter), `verbs::new_slab` (at least 3 points, no crossing, Rust's
texts; a clockwise outline stored counter-clockwise from its lower-left corner) and the top offset
are Rust's (edit.rs:318-385): `golden/place/polygon.txt` comes from `ccore_golden` on the branch
`oracle/slab1` (block `slab1_files`), replayed through the session's tool calls by
`test/place_polygon_test.c`. Rust has no verb that takes an outline (the editor calls `new_slab`
directly): the C core exports `cc_verb_new_slab` (verbs.h) for the tool, not a new verb.

**What differs.**
- **Esc** takes back the last corner of a polygon slab first, then stops placing (Rust's Esc stops
  placing at once, lib.rs:3031). The ticket asked for it: a mis-click costs one key, not the outline.
- **Enter** is not a row of the shortcut table (it is not in Rust's `handle_keys` either): the
  viewport's placing branch takes it (lib.rs:2772), `shell/keys.ts` routes it to the viewport
  before the table (`viewportKeys.enterTool`) while a polygon slab is clicked.
- **The ghost.** Rust draws only the snap marker while placing. The C tool answers the slab the
  next click would close (the corners and the pointer, or the corners when the pointer is on the
  first one) as its ghost from 2 corners on, and the page draws the outline so far and the closing
  edge. A crossing outline has no ghost; `ghost_error` says why.
- **At most 63 corners** per outline (`CC_TOOL_MAX_POINTS`; Rust's `Vec` has no bound): a 64th
  click is refused with `a slab outline has at most 63 corners`.

**Single grid axes (C additions, no Rust oracle).** Rust edits no grid (it reads them from IFC).
`grid_axis_add {grid, tag, start, end}` appends one axis with a new GlobalId; `grid_axis_remove
{grid, axis}` drops one; both answer one `set_grid` (one history step), every other GlobalId kept,
and run the grid rules of `grid_add` (tags unique in the grid, at least 1 mm long, ends within
1e7 mm, at most 1000 axes). IFC4 `IfcGrid` has `UAxes` and `VAxes`, each `LIST [1:?] OF UNIQUE
IfcGridAxis` (`WAxes` optional): the export puts the axes parallel to the first in U and the others
in V, so the grid must keep axes in two directions. What is enforced: removing the last axis of a
direction is refused with `grid_axis_remove: <tag> is the last axis of its direction; an IfcGrid
needs axes in two (UAxes and VAxes)`. Removing the first axis is allowed (U is then the axes
parallel to the new first one). Tested by `test/grid_axis_test.c` (the IFC4 and IFC2x3 exports
carry the new axis), not in `golden/grids/verbs.txt` (that transcript keeps its bytes).

**Snapping to the grid (Rust's rule, plus the links).** The place tool's axes are the model's
grid axes longer than 1 mm, their crossings (the strongest target) and lines, as snap.rs
`targets` takes `scene.grids`; the axes through the columns only when there is no grid; the
tolerance stays 12 screen pixels. A C addition: the axes of the visible links (model, IFC and
viewer-file links, as `query grid_axes` lists them) count too; Rust's placing snaps to its own
scene only. The measure tool does not share this code (measure.rs snaps to what is under the
cursor) and is unchanged. Tested by `test/snap_grid_test.c` (snap.rs's own grid test, and a column
clicked 52 mm from a crossing placed on it exactly).

**Openings in the wasm build (no kernel).** Rust always cuts with manifold3d; the wasm build has no
kernel and drew every opening uncut. Now `src/prism_cut.c` cuts the exact special cases when
`cc_boolean_kernel()` is NULL (wasm, `CC_NO_KERNEL=1`), the native build keeps the kernel (every
golden unchanged): (a) a host whose profile is an axis-aligned rectangle (a rectangular column,
beam, wall or slab) with openings that are axis-aligned boxes in its frame, at any depth (an
imported window or door: an 8-corner mesh): the solid is split along every distinct coordinate and
the faces between solid and empty cells are kept; (b) any profile (with voids) with openings that
are convex prisms along its extrusion through the whole depth, strictly inside the outline and
apart from the voids and each other: each becomes one more void of the extrusion. The mesh is not
the kernel's (more triangles on a split face; the same crease edges): the volume, area and box are
the kernel's within 1e-6 (`test/prism_cut_test.c` with and without the kernel in one native run;
`test/wasm_run.mjs` suite `openings`: wasm against the native host). Anything else (a recess in an
L slab, a shaft touching the outline, a mesh host) stays uncut with the warning `<class> <name>:
N opening(s) not cut (no boolean kernel in the C core)`. In the goldens, the one opening
(`ifc/import/opening.json`: a wall and a box window) is case (a); `frame2.json` has no opening.

## 31. Unsaved work and the host's guards (ticket FIXSES)

- **The session file is set aside, never overwritten (a C addition: Rust has no session file).**
  A session file the host cannot keep whole (written for another model path, a model file changed
  since, a line that does not replay) and that holds steps not saved is renamed
  `<session>.orphan-<unix time>` (its model side files with it) before a new snapshot is written.
  `query state` then has the C-only field `session_set_aside` (the path; left out when none, so
  every other state answer keeps its bytes). A model path spelt otherwise (`m.json`, `./m.json`)
  is the same model once the carrier resolved both (`session/session.c` `same_model_path`,
  `set_aside`).
- **Switch and merge refuse a link not saved.** Rust's linked models live in the viewer and
  survive a branch switch; the C core keeps them in the `.bimble` (section 29) and a switch or a
  merge reopens the working file, so `branch_switch` answers 400 `links changed and not saved:
  save them first, or send {"discard": true}` and `merge` 400 `links changed and not saved: save
  them first` (`session/versions.c`).
- **A switch or merge whose working file cannot be written changes nothing.** Rust (`lib.rs:2082`)
  moves HEAD, keeps the new model in memory and shows the save error (the model then unsaved). The
  C session's model is its file: the call answers 400 `<path>: cannot write` and HEAD (switch) or
  HEAD's ref (merge; the merge commit stays unreferenced in the store) is put back.
- **Export and share of a new model need a path.** Rust asks where with a dialog. The C host has
  none: `export_ifc4`, `export_ifc2x3` and `share_for_viewing` of an untitled model without
  `path` answer 400 (before, `Untitled.<ext>` was written in the host's working folder). The wasm
  module, without files, still writes under the current name. The "never over the open file"
  check compares the resolved paths too (`session/calls.c` `is_open_file`).
- **A second `Host` or `Origin` header is malformed (400).** Rust's `read_head` (`serve.rs:164`)
  keeps the last one; which one a proxy or a browser checked is not known, so the C host trusts
  neither (`host/server.c` `parse_head`).

## 32. Setting out: the survey point, the markers, north from the tab (ticket SETOUT, ADR 0075)

Rust v0.19.0 has none of this: its Setting out tab was a placeholder (no survey point, no
base-point marker, no project zero readout, no reference planes; `git grep -i "survey_point\|base
point\|reference plane" oracle/rust -- crates/` finds only a DXF block's base point). The C core's
own references are hand-computed in `test/setting_out_test.c`; no golden changes.

- **Georef gains `survey_point_mm` (C only).** `cc_georef.has_survey_point`, `survey_point_mm`
  (world mm `[x, y, z]`): file key `"survey_point_mm"` after Rust's six, left out when unset, so
  every model without one keeps its bytes and hash (tested: `frame.json` saved before and after a
  set-then-cleared georeference is the same bytes). Rust's `Georef` is `deny_unknown_fields`: Rust
  v0.19.0 refuses a file that holds one (as it refuses a C file with `grids`). On load: finite,
  `|x| <= 1e7` mm, exactly three numbers, no duplicate key; the unknown-field message still names
  Rust's six fields. The IFC export does not change for it: `IfcMapConversion` stays the tie to the
  map, and the building's place is decided by the six (`cc_georef_places`).
- **`georef_set` brings angles into (-180, 180]** (`rotation_deg`, `map_rotation_deg`; 350 -> -10,
  -180 -> 180, -0.0 -> 0.0). Before, any value up to ±360 was kept as typed. A C verb: no Rust
  reference. New argument `survey_point_mm` (`[x, y, z]` sets, `[]` removes).
- **Three actions after ISS71's** (`query actions`: 61 rows): `show_base_point`,
  `show_survey_point` (session state, pressed when the marker is shown, enabled with an editable
  model) and `view_true_north` (enabled when true north differs from project north; pressed and
  run as the section box's orientation in 3D, as the active stored view's `true_north` in a plan or
  a section: one `set_view` Command there).
- **`query setting_out`** (C only, `session/setting_out.h`): the markers and the values the page
  draws; null without an editable model. **`query capabilities`** gains `setting_out` (true),
  `reference_planes` and `base_point_move` (false: not served yet).

## 33. Views and sheets from the ribbon: New view…, the section tool, Place view…, Revision…, annotation types (ticket VIEWSHEET, ADR 0076)

Rust v0.19.0 made views from the navigator's "New view…" menu (viewing.rs:806-840), placed views
on sheets by dragging them from the navigator (layout.rs:449), kept one `revision` per sheet
(`set_sheet`, dispatch.rs:105; the title block's `{revision}`, template.rs:189) and chose the text
and dimension types of new annotations in its ribbon (ribbon.rs:1285 `annotation_types`, kept in
the layout: `text_kind`, `dim_kind`). The C core adds no file field and changes no golden; its
references are hand-computed in `test/view_sheet_test.c`.

- **Four actions after SETOUT's** (`query actions`: 65 rows): `new_view` (enabled with an editable
  model), `draw_section` (a switch, session state: enabled and pressed only while a floor plan of
  an editable model is shown; it stops the place, annotate and measure tools and they stop it),
  `sheet_place_view` and `sheet_revision` (enabled with an editable model and a sheet shown). The
  three dialog actions answer ok and change nothing: the page opens its dialog, which runs the
  verbs. `sheet_place_view` is not named `place_view`: the `place_` prefix is the place tool's.
- **`view_add` takes `scale` (1 to 5000, a whole number) and a section's `depth_mm` (0 or more)**,
  so a view made with its scale is one Command and one undo step. Rust's viewer set the scale
  afterwards (a second `set_view`). Refused: a fraction, 0, 5001, a string, a depth on a plan, a
  negative or out-of-range depth.
- **`query annotation_types`** (C only): the standards' text and dimension types (the default
  standards, `cc_style_default`: the C core keeps no other) and the ones in use, null for Default.
  **Action `annotation_type`** (not listed, as `set_settings`): `{"text": name | null}` or
  `{"dimension": name | null}`; an unknown name, a non-string, an empty or over-200-byte name, two
  keys or a body that is not JSON is refused and the state kept. The annotate tool (`tool
  annotate`) gives a new text the type's name as `kind` and its size (layout.rs:540-545, as Rust's
  sheet text tool), a new dimension the type's name; Rust applied them to sheet annotations only
  (the C core's sheet tools are not served yet, capability `sheet_tools` false).
- **Revision**: Rust's one `revision` per sheet, nothing new (no revision table, no date, no
  author: Rust had none; ADR 0076 decision 4).

## 34. Concrete: footings, raft, openings and sleeves from the ribbon (ticket CONC1, ADR 0077)

Rust v0.19.0 has the `pad_footing` generator and openings as data (`model.rs` `Opening`, exported
by `bimble-ifc/src/lib.rs:498-524`), and no verb, command or tool that makes a footing, a raft, an
opening or a sleeve (`git grep -i "opening\|footing\|raft\|sleeve" oracle/rust -- crates/`). The
C core's references are hand-computed in `test/concrete_test.c` (volumes, positions, counts) and
`test/lab_concrete.mjs` (the gestures in the lab). No golden changes.

- **Verbs (C additions):** `footing_add`, `raft_add`, `opening_add`, `opening_set`,
  `opening_remove` (`src/verbs_concrete.h`). Five actions after VIEWSHEET's (`query actions`: 70
  rows): `place_footing`, `place_strip_footing`, `place_raft`, `place_opening`, `place_sleeve`,
  enabled with an editable model, pressed while that kind is placed. `set_placing` takes
  `"concrete": {sizes}`; `query placing` answers it while one of these kinds is chosen (Rust's
  bytes otherwise).
- **Command `set_openings {id, openings}`** (the C core's own, after `set_material`): an element's
  whole opening list, the old one its inverse; each opening's placement and generator validated,
  GlobalIds unique in the list, at most 10,000. `add_element` now writes and reads `openings` in
  the command JSON (the session journal); before, the C parser refused them ("unsupported by the
  C core: element openings") and the writer left them out.
- **IFC export of openings** (Rust's, now ported): the C writer left every opening out ("No
  openings in the C model"); it now writes `IfcOpeningElement` + `IfcRelVoidsElement` as Rust,
  GlobalIds from the opening's own id or `opening/<element>/<i>`. A sleeve (a 32-gon opening) is
  written with ObjectType `'SLEEVE'` and `.USERDEFINED.` (Rust has no sleeve).
- **PredefinedType from the property `Bimble/PredefinedType`** when it is one of the class's
  enumeration (slab: FLOOR, ROOF, LANDING, BASESLAB; footing: FOOTING_BEAM, PAD_FOOTING, PILE_CAP,
  STRIP_FOOTING); otherwise Rust's (FLOOR, PAD_FOOTING…). No file without the property changes.
- **`query element` `openings`**: the element's count (Rust's `e.openings.len()`, query.rs:146);
  the C core answered 0.
## 34. What the core refuses, told before the click (ticket DEAD1, ADR 0074)

- **Query `level_refusals`** (C only, ticket DEAD1): what the Levels page's Remove and Building
  storey would answer, told before the click (the verbs `level_remove` and `level_set` run dry):
  the page disables the control with the core's text. Rust's page showed the refusal after the
  click. Test: `test/dead1_test.c`.
