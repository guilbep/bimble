# ADR 0014 — Section box: GPU clipping, back faces as cut surfaces, back edges hidden

- Status: Accepted (5 Nov 2026)
- Decided by: Pierre (priority 1: "the clipping box"); design and measurements by Claude
- Reversal cost: low (one option in `DrawOptions`; the viewer overlay is a separate module)

## Problem
Engineers need to look inside a model: cut it with a box (Revit's section box), see cut solids
as filled sections, and move the box faces directly in the view. Plans (priority 4) are the same
operation seen from above.

## Decision
- **`DrawOptions::clip: Option<ClipBox>`**, axis-aligned, scene coordinates. The fragment shaders
  discard faces and edges outside it; picking ignores hits outside it.
- **Cut surfaces from back faces.** Meshes are closed and wound outward (measured on three real
  files: 99.4–99.9 % of elements closed, none inside-out). Where the box removes a solid's front, its
  back faces become visible; they are drawn unlit at `style.cut_shade` (new, default 0.72) and
  hatched like the material.
- **Two passes only when the box is on**: outward faces, then inward faces pushed slightly back,
  so silhouette corners (where an outward and an inward face meet at equal depth) don't speckle
  with cut colour. With the box off, rendering is unchanged (one pass, both sides).
- **Back edges hidden in sections.** Each edge now carries the normals of its two faces (snorm8,
  8 bytes per edge vertex: 12 → 20 bytes). With the box on, an edge whose two faces both face away
  from the viewer is not drawn: such an edge is only ever visible through a cut, where it made cut
  solids look hollow.
- **Viewer:** "Section box (B)" fits the box to the selection or to what is visible; six handles
  (one per face) move faces along their axis, with the drag projected on the axis as seen on screen;
  the box is reset when another model is opened.
- **CLI:** `bimble render --clip x0,y0,z0,x1,y1,z1` (model mm) frames the part inside the box.

## Alternatives considered
- **Stencil capping** (parity of front/back faces along the ray, cap polygons on each box plane):
  exact flat caps at the plane, also for non-closed meshes; more passes and stencil state. Revisit if
  open meshes or the oblique look of caps become a problem.
- **Writing the cap depth at the box plane from the back-face pass:** wrong for solids that are not
  cut (their back faces would be moved in front of their own front faces).
- **CPU mesh clipping:** exact, but re-meshes on every handle drag; too slow for large models.

## Evidence
- `clip_box_cuts_solids_and_shows_the_cut`: one box per family cut at half height; cut surfaces at
  exactly the configured brightness; nothing above the cut; a box enclosing everything changes < 50
  pixels (it changed 158, at silhouette corners, before the two-pass fix).
- `picking_ignores_what_the_clip_box_removes`: hits the top face, then the bottom face from inside
  once the top is cut, then nothing when the box is beside the element.
- Geometry budgets: edge counts unchanged; GPU bytes +15 % on the 51 301-element frame
  (66.5 → 76.3 MB), from the edge normals.
- Viewer on a virtual display with a real model: B shows the box, dragging the top handle cuts the
  upper storeys and does not orbit; 0 CPU ticks in 5 s idle with the box on.

## Revisit when
- Section boxes at an angle (rotated) are needed: the box becomes a transform plus extents.
- Open meshes (terrain sheets, some imports) show wrong caps in practice: stencil capping.
