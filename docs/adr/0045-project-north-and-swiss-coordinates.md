# ADR 0045 — Project north and Swiss coordinates (LV95, WGS84)

- Status: Accepted (11 Nov 2026); amended the same day (export, view cube, reference plans)
- Decided by: Pierre ("do like Revit! it's for a Swiss team, so coordinates should be Swiss based,
  with an option for the most used one in the world"); details by Claude, reviewed by the advisor
- Reversal cost: medium (an optional field on models, views and view files; plans and sections of
  turned buildings are drawn differently)

## Context / Problem

The office's Revit exports draw in Swiss survey coordinates (LV95, metres ~2 600 000 /
1 200 000) with the building turned about −27° in its site, every storey sharing the building's
frame. Bimble drew world axes: plans came out turned against the building's grid, and a section
looking "north" cut across the grid. Revit draws plans in **project north** (along the building)
unless a view's Orientation says **true north**, and reads coordinates in the survey system.

Which coordinates: Switzerland surveys in **LV95** (CH1903+, EPSG:2056). The rest of Europe
draws in ETRS89 / UTM (metres per 6° zone), the US in State Plane (NAD83, often feet). What
everyone can read is **WGS84** latitude and longitude (GPS, EPSG:4326).

## Decision

1. **`Georef`** on the model (`.bimble`, its hash and history, merges take the side that changed
   it), the scene, and `.bimbleview` (a `GEOR` section; older readers skip it): the building's
   turn (`rotation_deg`, project north from true north, counter-clockwise), the survey system
   (`crs`), the survey offset and the map's turn when the model is in its own coordinates.
   Read from the first `IfcBuilding`'s frame; an IFC4 `IfcMapConversion` gives the offset and
   map turn, its `IfcProjectedCRS` the system's name; otherwise a building within LV95's range
   (E 2 480–2 840 km, N 1 070–1 300 km) is LV95.
2. **Project north by default** for every plan, ceiling plan and section; a view's
   **Orientation** (view properties) can say **true north** (`View.true_north`, stored only when
   set). On screen the camera turns with the building (same picture as an unturned building);
   on paper the scene is turned before drawing (`Turn::Plan(a)`, `Reflect(a)`), grid axes with
   it; section facings are the building's (a "north" section looks along project north), and
   their box on screen turns with them (`ClipBox.yaw`, the shader clips in the box's frame).
   The T key and View → Top look down in project north.
3. **Coordinates in the status bar** for the point under the pointer (on an element, else on a
   plan's cut) or the measured point: **LV95** the Swiss way (E 2'600'123.45 N 1'200'456.78 ·
   432.10 m), or **WGS84** (Settings → Coordinates), worked out from LV95 with swisstopo's
   approximate formulas (about 1 m; tested on swisstopo's example point). A model with no known
   system shows model metres; `bimble info` prints the system and the turn.

## Consequences

- The office's plans and sections are along their grid, as in Revit. Models whose building is
  not turned (`rotation_deg` 0) are drawn exactly as before.
- Sections saved on a turned model before this cut along the building's axes now, no longer
  the world's.
- Older versions (0.13.2 and before) can't read a model with a place (every model imported from
  the office's files), its history, or a view set to true north (strict formats); everyone
  upgrades together. A `.bimbleview` still opens there, unturned.
- Paper plans of a turned model draw from a turned copy of the scene: on a 32 766-element model
  a 1:100 plan took 2.58 s instead of 2.00 s, peak memory 2.15 instead of 1.72 GB. Leaving out
  elements outside the plan's height range before turning would recover most of it.
- The IfcMapConversion branch (survey offset, map turn) is read but no file has exercised it
  yet: check one coordinate against IfcOpenShell on the first IFC4 export that has one.
- Found while doing it: looking straight down, the camera ignored its yaw ("up" fixed to +Y)
  and panning was reversed in 2D views (its "right" came from a rounding error's sign). Up now
  comes from the yaw, panning uses the view's own axes (tests).
- Not done at first (see the amendment): the turn on export, the view cube, reference plans in
  a turned plan.

## Amendment (Pierre: "fix the not done part, and align DXF/PDF reference plans")

- **Export keeps the place and project north**: the building is placed turned by the building's
  angle at a whole metre near the model, elements in its frame; IFC4 also gets IfcProjectedCRS and
  IfcMapConversion. IfcOpenShell reads the elements at the same world positions; re-import gives
  the same turn and survey system (IFC4 and IFC2x3, ADR 0047). A model with no place exports as
  before.
- **The view cube is the building's**: its faces and compass turn with project north (FRONT is the
  building's south), as Revit's; the first 3D view and Home look from the building's south-west.
- **Reference plans line up**: a DXF in the model's coordinates (an LV95 DXF on a model drawn in
  LV95, the office's case) stays where it is, as before; on a model in its own coordinates (IFC4 map
  conversion) it is placed by its survey coordinates; a plan whose coordinates are elsewhere (a PDF,
  a local DXF) is centred on the model and turned upright in the project-north plan. The message
  says which.

## Alternatives considered

- Turning the model itself on import: GlobalIds stay, but every coordinate changes, links and
  survey coordinates no longer match the architect's.
- WGS84 by a library (proj): exact, but a C dependency for a status-bar readout; swisstopo's
  formulas are a few lines and within a metre.

## Revisit when

- Offices outside Switzerland use it: ETRS89/UTM, State Plane.
- Exports must round-trip the turn (write `IfcMapConversion`).
