# ADR 0029 — Schedules: element tables measured from the geometry

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre (proposed schedules like the office's Revit nomenclatures: concrete columns,
  steel beams, walls…); design by Claude
- Reversal cost: low (a data type, a function, a view)

## Context / Problem
The office keeps one schedule per kind of element and material (concrete columns, timber beams,
steel beams, slabs, walls…): level, location on the grid, type or section, material, length,
area, volume, with totals. The viewer only had a count and volume per IFC class.

## Decision
- A **schedule** is data (`bimble_core::schedule::ScheduleDef`): the IFC classes and the material
  family it lists, its columns (`Field`: level, location, name, class, material, section, length,
  width, height, area, volume, GlobalId), grouped by level or not, with totals or not. Built-in
  ones: concrete/steel/timber columns and beams, slabs, walls, footings, all elements.
- **Values are measured from the geometry** (`bimble_drawing::schedule`), so schedules work the
  same on a `.bimble` model and on any IFC: the element's direction in plan is its longest level
  edge; length, width and height are its extents along it, across it and in z; a standing column's
  length is its height and its section its plan extents ("30/30", "Ø30", timber "GL24h 24x46", the
  same words as on plans); volume from the closed mesh; area is a wall's face, else plan area.
- **Location** is the grid crossing nearest the element's centre in plan, within 1.5 m ("B-2"),
  from the model's grid or, without one, from the columns, as plans do.
- Rows are sorted by level (lowest first), then location and name in natural order (P9 before
  P10); a subtotal per level, a total at the end. Any column can sort the rows.
- In the viewer: **Schedules** in the project browser (with counts; empty ones greyed) opens one
  in the main view, with its own tab. A row selects its element; **Show in the view** frames it;
  **Export CSV…** writes rows, subtotals and total. On the command line: `bimble schedules`,
  `bimble schedule FILE NAME [--csv OUT]`.

## Alternatives considered
- Values from IFC property sets (Qto_*BaseQuantities): absent or inconsistent across exporters;
  measuring is the same for every file. Property sets can be added as fields later.
- Schedules only built in: done first, then (same day) schedules of one's own.

## Update (6 Nov 2026, evening): editing schedules
Schedules are now kept with the model: `Model.schedules` changed through `Command::SetSchedules`
(undo, history, hash, diff, merge by name), or the views file next to an IFC. A stored schedule
with a built-in name replaces that one; `effective()` gives the list shown. In the viewer the
properties panel edits name, IFC classes, family, grouping, totals and columns (tick, move
earlier); **Duplicate**, **New schedule**, **Back to built-in**, **Delete**. A Mass column
(volume × the library density, ADR 0031) was added; steel schedules show it.

## Evidence
Tests on the demo frame: 24 concrete columns "30/30" located A-1…D-3, 2.78 m, total volume =
sum of rows; 34 concrete beams "25/50", 25 × 50 cm; empty schedules total 0.000 (not -0.000:
an empty f64 sum is -0.0); natural order. Checked on the virtual display: Concrete beams with
subtotals per level, a row selecting its element.

## Revisit when
Users need filters on values or parameters from the IFC property sets.
