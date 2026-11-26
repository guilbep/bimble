# ADR 0075 — Setting out: base point, survey point, project zero and north as project data

- Status: Accepted, 26 Nov 2026
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

## Consequences

- The tab has no dead control: 7 of its 8 controls act (Survey point, Project base point, True /
  project north, Project zero, Levels, Grids, Measure), Reference planes says "not yet"; the side
  panel's fields all act, "Move the base point…" says "not yet" (`ccore/test/lab_setting_out.mjs`).
- A file with a survey point does not open in Rust v0.19.0 (`deny_unknown_fields`), as a file with
  grids. Every other file keeps its bytes and hash (`ccore/golden/DEVIATIONS.md` 32).
- `query actions` has 61 rows (Rust's 54, ISS72's 2, ISS71's 2, these 3).

## Not done

- Reference planes (decision 8), a two-click grid axis tool in a plan (the Levels & grids page
  adds axes by numbers), grids drawn in 3D (Rust draws them on paper only; the lab in plans).
- Survey point in IFC (decision 7), moving the base point by clicks (decision 2).
