# ADR 0075 — Setting out: base point, survey point, project zero and north as project data

- Status: Accepted, 26 Nov 2026 (decisions 9-12: ticket WAVE1B, the same day, same delegation)
- Decided by: Pierre delegated the decision ("don't wait for me to validate the ADR, just do your
  best, I'll review once it's implemented"); the choices below by Claude on ticket SETOUT, to be
  reviewed by Pierre on the implementation.
- Reversal cost: low (one optional georeference key, three session actions, one query)
- Extends: ADR 0045 (project north, Swiss coordinates), ADR 0056 (levels, project zero, grids)

## Context / Problem

Pierre opened the published lab's Setting out tab: "Survey Point, project base point, true/project
north, project zero, levels, grid, and reference planes is not implemented!". Before this ticket
five of the tab's eight controls were drawn "not yet" (Survey point, Project base point, True /
project north, the Project zero box, Reference planes) and the side panel's "Move the base
point…" was a disabled button. Rust v0.19.0 had no survey point, base-point marker, project zero
readout or reference plane either (its tab was the same placeholder).

What existed: the model's georeference (`Georef`, ADR 0045: `rotation_deg` = project north from
true north, `crs`, `east_m` / `north_m` / `height_m` = the survey coordinates of the world origin,
`map_rotation_deg`), its verb `georef_set`, its IFC4 export as `IfcMapConversion` +
`IfcProjectedCRS` and its import; levels and grids as model data with verbs (ADR 0056, G1, G2,
SLAB1).

## Decision

1. **The model's coordinates stay as they are** (mm, f64). Everything here is data on the
   georeference; no element moves. No reprojection, no geodesy beyond ADR 0045's.
2. **Project base point = the world origin.** Its survey coordinates are the georeference's
   east, north, height; the side panel and Project information edit them (`georef_set`).
   *Alternative:* a base point anywhere in the model (Revit's unclipped point): two origins to
   keep apart in every export; rejected for now (the "Move the base point" button stays "not yet").
3. **Survey point = a benchmark of the site in world mm** (`Georef.survey_point_mm`, a C
   addition, file key after Rust's six, left out when unset), at the base point while unset (a
   new Revit project has both at the origin); its survey coordinates follow from the
   georeference. *Alternative:* the survey point as the map's origin (E 0, N 0): kilometres off
   the model, never on screen.
4. **Project zero = `Georef.height_m`** (ADR 0056 decision 1 kept): the ribbon's Project zero box
   reads and sets it. *Alternative:* `IfcSite.RefElevation` as its own field: a second height that
   can disagree with the map conversion's.
5. **True north = `Georef.rotation_deg`**, one source for the View tab's orientation, the section
   box (ISS71) and the tab's "True / project north" button (action `view_true_north`: the box in
   3D, the stored view's `true_north` in a plan or a section, one undoable step there).
   `georef_set` brings angles into (-180, 180], no -0.0. *Alternative:* also write
   `IfcGeometricRepresentationContext.TrueNorth` on export: it would change every Rust-made IFC
   reference of a turned model, so the building placement keeps carrying the turn (as Rust).
6. **Markers are session state, drawn by the page** (actions `show_base_point`,
   `show_survey_point`, pressed when shown, hidden at start as in Revit; `query setting_out` gives
   positions, survey coordinates and the north directions): flat strokes in palette colours, a
   fixed size on screen, on the overlay of the 3D view and of plans. *Alternative:* per view in
   the model (Revit's visibility per view): a model change for a look.
7. **Survey point on IFC export: none.** `IfcMapConversion` is the standard tie to the map;
   other tools read no survey point. *Alternative:* a `Bimble_SettingOut` property set on
   `IfcSite`: kept for when a round trip asks for it.
8. **Reference planes and moving the base point by clicks are not served**: `query capabilities`
   says `reference_planes` and `base_point_move` false, and the lab draws them "not yet".
   *Planned shape for reference planes:* `Model.reference_planes` (`{id, name, origin, normal,
   x_axis, extent}`), exported as `IfcVirtualElement` with a planar `IfcAxis2Placement3D`
   (a virtual boundary is what IFC4 offers for a non-physical plane; an `IfcAnnotation` would do in
   IFC4x3 only); levels and grid axes count as implicit planes for the work-plane choice.

9. **Reference planes are elements of class `IfcVirtualElement`** (ticket WAVE1B, under the same
   delegation): no generator (as an `IfcElementAssembly`, ADR 0070), the placement is the plane's
   frame (z axis = unit normal), the outline's size the properties `Bimble/PlaneWidth`,
   `Bimble/PlaneHeight` (mm, default 10 m). So create (verb `reference_plane_add`: origin + normal,
   or three points), rename (`rename`), move, delete, undo, history, merge and save are the element
   paths that exist, and a model without one keeps its bytes. At most 1,000 in a model.
   *Alternative:* decision 8's planned `Model.reference_planes` section: its own commands, file
   section, repo tree, merge and diff code for the same data an element carries; rejected.
10. **IFC: `IfcVirtualElement`, ObjectType `REFERENCE_PLANE`**, its placement the frame, a
    `'Reference'`/`'Curve3D'` representation (the closed outline in its own z = 0), the sizes in
    the `Bimble` property set, no material; read back as a plane (IFC4 and IFC2x3; IfcOpenShell
    validates both). *Alternative:* `IfcAnnotation` (IFC4x3 only) or an `IfcGrid`'s plane: neither
    is a bounded plane of its own in IFC4.
11. **The work plane** (session state, `tool work_plane`): the active storey's level by default; a
    stored plane, a level, or a grid axis' vertical plane (the implicit ones, listed by `query
    reference_planes`, never stored) can be chosen. The place tools land the pointer on a
    horizontal work plane and raise what they make by its height above the level; any other work
    plane moves the point onto its trace on the level. Measure ends a ray that meets no element
    on the work plane. A chosen plane that is deleted stops being the work plane. *Alternative:*
    elements placed in the plane's own frame (a column normal to an oblique plane): no generator
    of the core is meant for it yet.
12. **Three points by clicks** (the ribbon's Reference planes button): the Measure tool's snapping
    (corner, edge, face, else the work plane) gives each point; the third makes the plane (origin
    the first point, x towards the second), one history step; Esc takes the last point back, then
    stops. *Alternative:* a dialog of nine numbers only (kept as "origin + normal" in the side
    panel): no pick of what is on screen.

13. **The two-click grid axis tool** (WAVE1B A2): a place kind `grid_axis` (action
    `place_grid_axis`, the ribbon's Grid axis), so the place tool's snapping (grid axes, their
    crossings, then the elements), its rubber line and Esc are the same; Shift locks the axis
    along x or y (Ctrl does for the other kinds; Shift is their "no snap"). One `grid_axis_add`
    step into the first grid with an axis parallel to it (else the first grid); its tag follows
    its family: the next number after the highest, or the next letter (A..Z, AA..). Off without a
    grid (a grid needs axes in two directions: the Levels & grids page makes the first). A work
    plane does not pull a grid axis onto its trace. *Alternative:* a page-side tool as the
    section line's: no snapping to the axes without porting snap.rs to the page.

## Consequences

- The tab has no dead control: 7 of its 8 controls act (Survey point, Project base point, True /
  project north, Project zero, Levels, Grids, Measure), Reference planes says "not yet"; the side
  panel's fields all act, "Move the base point…" says "not yet" (`ccore/test/lab_setting_out.mjs`).
- A file with a survey point does not open in Rust v0.19.0 (`deny_unknown_fields`), as a file with
  grids. Every other file keeps its bytes and hash (`ccore/golden/DEVIATIONS.md` 32).
- `query actions` has 61 rows (Rust's 54, ISS72's 2, ISS71's 2, these 3).

## Not done

- ~~Reference planes (decision 8)~~: served by decisions 9-12 (WAVE1B). ~~A two-click grid axis tool in a plan~~ (decision 13; the Levels & grids page
  adds axes by numbers), grids drawn in 3D (Rust draws them on paper only; the lab in plans).
- Survey point in IFC (decision 7), moving the base point by clicks (decision 2).
