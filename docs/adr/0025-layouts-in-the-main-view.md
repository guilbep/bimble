# ADR 0025 — Layouts in the main view

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre (layouts open in the main view like the plan preview, static pages that are
  dynamic: select, dimension, text, drag views in; edit the model through a placed view);
  interaction details by Claude
- Reversal cost: low (one viewer module; the sheet drawing is unchanged)

## Context / Problem
Sheets (ADR 0024) were only made from the command line. Pierre wants to open them in the main
view and work on them: place views by dragging, move them, add dimensions and texts.

## Decision
- **The sheet on screen is its PDF.** The sheet's vector drawing (the same items as PDF/SVG) is
  parsed once (`Sheet::prepare`) and rendered by resvg for exactly the part of the paper in view,
  at the screen's resolution, whenever the camera or the sheet changes. It stays sharp at any
  zoom; what is on screen is what prints. Fonts are loaded once per process.
- **Interaction is in paper millimetres.** Each placed view's box, lines (for snapping) and its
  frame (world ↔ paper, `ViewFrame`) come with the drawing, so picking, snapping and turning a
  click into a world point need no second geometry path.
- **Tools:** Select (click; drag a selected view to move it; double-click opens the view, where the
  model is edited; Delete removes), Text (inside a placed view: the view's text at the world
  point under the click; elsewhere: the sheet's), Dimension (two clicks snapped to the drawn lines'
  ends, else to the lines; the third click places the dimension line and chooses how it measures:
  above/below both points → across, left/right of both → up, between → along the two points).
- **Views are dragged from the navigator** (2D views only) and placed at 1:100 under the pointer.
- **Every change** is a Command (`SetSheet`, `SetView`) in a .bimble model (undo, history), or a
  change of the side file for an IFC.
- A view's dimensions and texts are also drawn over the view itself on screen.

## Alternatives considered
- **Drawing the sheet with egui shapes**: egui fills convex polygons only; hatches, holes and text
  would need their own tessellation and would differ from the PDF.
- **One big texture of the whole sheet**: blurry when zooming in, or huge (A0 at 200 px/mm).

## Evidence
- Unit tests: snapping, picking texts, choosing a dimension's axis and its line.
- On the virtual display with the demo frame: open S-101, drag Plan Niveau 2 from the navigator,
  move it, zoom at the pointer, place a horizontal dimension A2–B2 ("600"), place a sheet text,
  double-click the plan to open it: the dimension shows over the view.
- Sheet redrawn in 84–96 ms on llvmpipe, debug build (2 views, A1, window 1280 × 750).

## Revisit when
- Dimensions follow the elements they were taken on (needs element ids on the drawn lines).
- Annotation types (#43), line weights (#42) and hatch types (#44) change what the tools offer.
