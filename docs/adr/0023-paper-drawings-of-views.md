# ADR 0023 — Paper drawings of any view; what is seen from above

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre (#32 before sheets; #22 "sloped slabs, ramps, stairs, stepped footings:
  quite often"); approach by Claude
- Reversal cost: low (one module, `viewdraw`, in front of the plan drawing; two rules in it)

## Context / Problem
The paper drawing (ADR 0015, 0018) only knew a storey plan cut at one height. Views (ADR 0021)
add a view range, ceiling plans looking up and sections; sheets (#33) need all of them on paper.
And what is seen below the cut was filled only from a slice just under each element's highest
point, so sloped slabs, ramps, stairs and stepped footings were barely filled and lines that should
be hidden showed through (#22).

## Decision
- **Turn the scene, keep one drawing.** A view is drawn by turning the scene so it looks straight
  down, then running the plan drawing with the view's heights:
  - plan looking down: as it is; seen from the view depth to the cut, dashed above up to the top;
  - ceiling plan: reflected (z → −z, triangles re-wound), so north stays up as on screen;
  - section looking N/E/S/W: rotated, x along the section, y up; seen from the cut to its depth.
  Grid axes, slab levels and the "above" lines stay with plans; sections get none of them yet.
  The view's visibility hides the same elements on paper as on screen.
- **What is seen from above is the union of the faces turned upwards**: the outline of all
  upward triangles of an element (edges used once), not a slice near its top. A flat top gives
  the same outline as before; a ramp, treads and steps are filled over their whole area. Elements
  are still painted from the lowest top to the highest.
- **An edge is drawn only if one of its two faces turns upwards** (edge normals, ADR 0014):
  a downstand under its slab, a stair's underside, a beam's bottom are hidden by their own solid.
- `bimble views FILE` lists views; `bimble plan --view NAME` draws one; the viewer's Plan window
  draws the active 2D view. Templates get `{view}` and `{drawing}`; the default title block shows
  `{drawing}`; ceiling plans and sections are titled by the view's name.

## Alternatives considered
- **Full hidden-line removal** (split every edge at every crossing, test each piece against all
  faces): exact for any arrangement of elements, at a cost of O(edges × faces) with a spatial
  index. The two rules above fix what #22 reported (an element's own hidden parts, sloped and
  stepped tops); between elements the painter's order stays. Revisit if interpenetrating or
  overhanging elements show wrong lines on real plans.
- **A second drawing for sections**: duplicate dimension, title and sheet logic. Rejected.

## Evidence
- Tests that fail on the previous code: a ramp seen below the cut is filled over 40 × 20 mm
  (before: 0.07 × 20 mm); a downstand under its slab draws no line over the slab (before: 6).
- A plan view with the default range gives the same SVG as the storey plan it replaces.
- Ceiling plan and section of the demo frame drawn (cut columns and beams, titles); per-view
  visibility on paper.
- A 15,071-element generated frame, storey plan at 1:500: 0.16–0.18 s before and after
  (3 runs each, release build, this container); memory within 95–99 MB both.

## Revisit when
- Real plans show wrong lines between elements (then: exact hidden-line removal).
- Sections need grid axes and level marks (with section markers, #24).
