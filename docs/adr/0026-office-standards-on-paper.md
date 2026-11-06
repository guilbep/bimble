# ADR 0026 — Office standards on paper: pens, line, text, dimension and hatch types

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre (16 pens per scale as in Revit; line types colour × pattern × pen; text and
  dimension types; hatch types per material cut and seen; "everything should be configurable!
  hatching also"); data model and defaults by Claude
- Reversal cost: low (all in the graphic style file, every new key optional with a default)

## Context / Problem
Office drawings follow a standard: pen widths per scale, named line types, text and dimension
types, hatch patterns per material. Ours had a handful of fixed `*_pen_mm` fields and hatches
derived from the screen pattern. The office's own names carry a prefix that stays out of this repo.

## Decision
All of it lives in `PaperStyle` (the graphic style JSON), each list with built-in defaults:
- **Pens** (`pens`): Model, Perspective and Annotation tables, 16 rows × scales (Revit's defaults,
  1:5 to 1:500). A drawing at a scale between two columns takes the nearest in log ratio (1:75 →
  1:100 column; to confirm with Pierre).
- **Line types** (`line_types`): name, colour, dash pattern in paper mm, pen number. Defaults: 9
  colours × continu/dash 1/2/3/5, axes 1/2, axes_detail_1. **Lines on drawings** (`lines`) gives the
  line type of each kind of line: cut, seen, above, hatch, axis, dimension, frame. Model pens for
  model lines, annotation pens for the rest.
- **Text types** and **dimension types** (#43): Arial 5/4/3.5/2.5/2 mm in five colours;
  dimensions in cm without unit, m with two decimals, or mm, in four colours, each with a line
  type. View texts, view dimensions and sheet texts carry an optional `kind`; without one they
  draw as before.
- **Hatch types** (#44): optional solid fill, line families (angle, spacing and offset in paper
  mm, dashes), colour, model pen. A drafting pattern: the same spacing at every scale.
  **Hatches by material** picks one for each material family, cut and seen; a family without one
  keeps its screen-derived pattern, so existing drawings do not change. No fill means white paper.
- The Graphic style window edits all of it; renaming a line or hatch type updates what uses it.

## Alternatives considered
- Separate standards files per kind: more files to keep in step; one style file is what the viewer
  already loads, saves and shares.
- Model patterns (spacing in model mm, scaling with the drawing): the office lists are drafting
  patterns; model patterns can be added as a flag on a hatch type later.
- Hatch types on screen too: the GPU draws two line families and stipple per family; screen
  hatches stay as they are for now.

## Evidence
Tests: pen lookup and validation, default types valid, a typed dimension written "6.00 m" in red
with red lines, a typed sheet text in blue at 4 mm, a material hatch type drawn at 2 mm / 45° in
its colour (none without it), hatch offsets, hatch text round trip in the editor.

## Revisit when
Pierre answers on in-between scales; the office review (#26) finds a pattern we can't express
(curves, symbols such as insulation zigzags).
