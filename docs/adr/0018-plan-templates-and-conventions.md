# ADR 0018 — Plans like the office draws them: templates, seen fills, axes, dimensions

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre (I4: formwork plans like the office's; "good, you can start"); conventions read
  from the office's own plans (kept out of this repo, ADR 0008); implementation by Claude
- Reversal cost: low (all of it is in `bimble-drawing`; the template file is optional)

## Problem
The first plans (ADR 0015) were exact cuts with a generic title block. The office's formwork plans
(A2, 1:100) show more, and in their words: grid axes with bubbles and axis-to-axis dimension chains
(in cm: 362, 421), concrete *cut* darker than concrete *seen* below, a legend with those two swatches
per material, slab levels, diagonal crosses in openings, the title "Plan …, éch 1 : 100" underlined
under the drawing, a "level zero = … msm" note, and a title block with their columns (project,
phase in red, client, architect, revision, drawn/checked, format/scale, date/plan number).

## Decision
- **A plan template (JSON) says what goes around the model and in which words**: title, level note,
  dimension unit (mm, cm, m), hidden IFC classes (rebar, spaces, openings, annotations, terrain by
  default), switches (seen fills, axes, dimensions, slab levels, opening crosses), legend words and
  family names, title-block columns with stacked entries (label, value, colour, size), and free
  `fields` used as `{placeholders}`. Missing keys fall back to the built-in English template;
  unknown keys are errors (a typo must not be silently ignored). `docs/templates/plan-fr.json` is a
  generic French one in the office's layout, with no office data in it.
- **Seen below the cut**: the top of every element entirely below the cut (down to the storey level)
  is filled with its family colour lightened (`paper.seen_lighten`, 0.6), painted from the lowest top
  to the highest, each element's visible edges first: higher tops hide what is under them — hidden-line
  removal by painting, exact for horizontal tops, cheap.
- **Grid axes**: IfcGrid axes are read from IFC (IfcPolyline and IfcIndexedPolyCurve; a grid repeated
  per storey counted once). Without grids, axes are made through the columns cut by the plane (centres
  shared by at least two columns within 50 mm), lettered A, B, … and numbered 1, 2, …. Bubbles on top
  and left, long-dash thin lines.
- **Dimensions**: top and left, first chain from the outer face through the axes, second overall;
  architect's 45° ticks; texts that don't fit their span move outside it.
- **Text on sheets**: anchor (start, middle, end), rotation and bold, placed from Helvetica widths so
  SVG, PDF and the raster preview agree.

## Alternatives considered
- Hard-coding the office's layout: every office has its own title block; a template keeps client
  and office names out of the code and lets them change without a release.
- True hidden-line removal (polygon clipping of every projected edge): much more work; the painter's
  order already gives the expected look for slabs, beams and walls. Revisit with sloped or stepped tops.

## Evidence
- Real file #1 (the office's own model, 776 elements, 192 grid axes in 12 grids): 16 unique axes,
  chains 362 / 362 / 362 / 420.9 / 362 … as on the office's plan; whole command (load IFC +
  plan + PDF) 0.05–0.06 s, 75 MB peak (3 runs, `/usr/bin/time`).
- Tests: model grid axes give bubbles and the 600/630/15 cm dimensions (and none with axes off);
  columns give axes A–D, 1–3 on the demo; seen tops painted low to high, a 1 × 2 m opening crossed
  (20 × 40 mm at 1:50), hidden classes counted; template texts in the title block; rotated and
  centred text identical in SVG and PDF.

## Revisit when
- Sections and elevations (same sheet, same template).
- The office asks for dash-dot axes, half-centimetre superscripts or revision tables.
