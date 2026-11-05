# ADR 0015 — Drawings on paper: one sheet model, SVG and PDF; plans as exact horizontal sections

- Status: Accepted (5 Nov 2026)
- Decided by: Pierre (priority 4: "storey plan cut to SVG at scale, 1:1 to 1:5000, printable to PDF
  on A4 to A0"); design and evidence by Claude
- Reversal cost: low for the writers; medium for the plan definition (office conventions will
  refine it)

## Problem
Engineers deliver drawings, not screenshots. A plan must be vector, at an exact scale, on a
standard sheet, with line weights and hatches that follow office conventions, and print the same
from PDF or SVG.

## Decision
- **New crate `bimble-drawing`** (no GPU): `Sheet` in paper mm (areas with even-odd fill, hatch
  and outline; lines with pens and dashes; text) written by `to_svg` and `to_pdf`. PDF through
  `pdf-writer` (pure Rust, small): the page works in mm with y down (one transform), hatches are
  clipped line sets (same in SVG), text uses the built-in Helvetica (WinAnsi; other characters
  become "?").
- **Plan** = horizontal section at `cut_above_mm` (default 1 200, as Revit) over the storey level:
  cut areas (exact mesh × plane section, f64, loops chained from bit-identical edge points; open
  meshes give lines), edges between level and cut as thin lines, the storey's own edges above the
  cut (to the next storey) dashed. Fills and hatches of all cut elements first, then all outlines,
  so touching elements keep both outlines.
- **Scale 1:N, N in 1..5000; paper A4–A0**, landscape or portrait; automatic = the smallest that
  fits; when nothing fits, the error gives the standard scale that would.
- **Style:** colours and hatches come from the graphic style (ADR 0013); new `paper` section with
  pens (cut 0.5, projection 0.18, above 0.13 dashed, hatch 0.13, frame 0.5 mm), hatch spacing per
  screen pixel, text height. The style data moved to `bimble-core::style` (the renderer keeps the
  GPU table) so drawings don't depend on the GPU stack.
- **Storey levels** in `Scene::storeys`: IFC from the storey placement (Elevation attribute as
  fallback), `.bimble` from the model.
- **Title block** (project, plan and cut height, scale, paper, date, version) and a graphic scale
  bar. Configurable title block: next step.
- CLI `bimble plan`, viewer "Plan…".

## Alternatives considered
- **SVG only, PDF by an external converter** (Inkscape, rsvg): an install on every office machine;
  rejected. **svg2pdf**: heavier (usvg); the sheet model makes direct PDF simple.
- **Raster plans from the GPU renderer:** not to scale on paper, not editable in CAD; rejected.
- **Hidden-line removal now:** needed for clean projections under overhangs; later (cut areas drawn
  over projections already hide most).

## Evidence
- Unit tests: a wall cut halfway gives its exact rectangle; a hollow section two loops of the exact
  areas; an open sheet a 1 000 mm chain; z-clipping exact; a 300 × 300 mm column is a 3 × 3 mm
  square at 1:100; automatic paper; errors with scale suggestions; SVG/PDF structure (A4 media box
  841.8898 × 595.2756 pt); calendar dates.
- Storey levels equal IfcOpenShell's on a real file with 14 storeys.
- Real file at 1:100: A1 portrait chosen, 311 cut elements; the PDF page is 594.0 × 841.0 mm;
  at 1:50 the error says 1:75 fits on A0 (checked by hand).
- 51 301-element model: plan in 0.59 s.

## Revisit when
- Office drawing conventions arrive (I4 formwork plans): line weights per class, which classes to
  show (rebar is cut too), annotations, dimensions on paper.
- Large models make plans slow: index elements by height.
