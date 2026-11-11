# ADR 0046 — One view, two pictures: Model and On paper; scale is the view's

- Status: Accepted (11 Nov 2026); amended the same day (background drawing, crop, sections; all in 0.15)
- Decided by: Pierre ("we should do like Revit, 2D views editable, zoomable… it's weird to have a
  preview of the PDF in a dialog instead of a full tab"; "be better than Revit, UX-wise and
  philosophically"; agreed to the plan and its recommendations: "do everything"). Review, PRD and
  plan by Fable; built by Claude.
- Reversal cost: medium (a view field; the paper drawing moves from a window into the view's tab)
- Amends ADR 0021 ("kept for paper (the plan window)") and ADR 0024 ("crop regions: not yet").

## Context / Problem

A plan or section tab showed the GPU scene (instant, editable); the office drawing of the same
view (pens by scale, hatches, dashed lines above the cut, grids, chains) lived in a floating "On
paper" window as a whole-sheet raster. Scale was not the view's: it sat in that window and on each
sheet placement, and texts on screen were sized "as at 1:100". Drawing a 1:100 plan of the
32 766-element model took 2.58 s on the UI thread.

Revit has one picture, the drawing, every line tied to its element; it is regenerated on the UI
thread, which is why it is slow on big models.

## Decision

1. **A view is a question, not a container of graphics**: where (kind, range, crop), what
   (visibility), how big (scale), and its own annotations. How things look comes from the office
   standards, the same in every view and on every sheet (no per-view overrides).
2. **One view, two named pictures, in the view's tab**: *Model* (the GPU picture, depth-exact,
   instant, where geometry is edited) and *On paper* (the exact items the PDF and sheets are made
   of). A switch on the tab and the `P` key; nothing hidden: a tool that needs the model switches
   to Model visibly, at the same world point. The "On paper" window becomes "Export view…".
3. **Selecting in On paper is done on the model**: the clicked paper point becomes a world point
   (`ViewFrame::to_world`), a ray along the view's look through the box tree, clipped by the view's
   range, picks the element whose cut or top is drawn there — the same selection and properties as
   in Model, also for linked models. Hover names the element; the selected one is outlined on the
   paper. Moving, copying, deleting and placing switch to Model. Drawn lines carry no element ids.
4. **Scale is the view's** (`View.scale`, 1:100 when not said, so older files and hashes don't
   change): its pens, texts and dimensions on paper and on screen, its size on every sheet. The same
   view at two scales is two views. A placement keeps the scale written (older versions print the
   file the same); changing it on a sheet changes the view. A file from before, whose sheets show a
   view at another scale, has the view take it on opening, as an edit to save, said in the window.
5. **Honest on old machines**: the paper picture is a still drawn once per change, leaving out what
   lies outside the view's range (when the scene is copied: a turned building or something hidden;
   measured on the office's turned 32 766-element model, 1.35 → 0.27 s; sections not yet culled); the 0.15 gate is measured: if that brings the large-model redraw
   under about a second, drawing in the background waits for 0.16; else it comes first.
6. Crop regions per view (on screen and on paper) follow.

## Amendment (background drawing, crop, sections)

- **Drawn in the background** on the desktop: the scene is shared (`Arc<Scene>`; a change while a
  drawing runs makes its own copy), On paper views and sheets are drawn on a thread; the last
  picture stays, greyed, with "Drawing… n s"; a drawing made for what has changed since is dropped
  (test). In a browser, at once (no threads there).
- **Crop regions**: `View.crop`, a rectangle in the view's own plane (a plan's x, y turned to its
  orientation, a section's along the cut and height; world mm), optional. Set in the view's
  properties: on (the model's extent), *To the screen* (what the screen shows), or typed in
  metres. On screen through the clip box turned with the view; on paper by clipping the drawing's
  geometry before it is laid out (loops ring by ring, lines, beam labels; grid axes clipped, those
  found from columns counted before cropping), so the page and the chains follow.
- **Sections leave out** what lies behind their cut or beyond their depth before the copy;
  drawings byte-identical on two office models. Little gain: a section through the 32 766-element
  model still takes 6–10 s to draw — the cost is in the section drawing itself, which the
  background drawing now keeps off the window.
- A scale change is one undo step with its sheets; On paper selection skips classes the template
  leaves off paper; a bare drawing's page holds its title.
- Not measured: the window's frame times while a drawing runs (needs a real desktop window).

## Consequences

- One truth per view: what is checked On paper is what prints.
- Two hidden-line rules remain visible (depth on screen, painter's order on paper, ADR 0023); the
  hover name says what a click would select.
- `bimble plan --view` draws a view at its scale; `--scale` stays for storey plans.

## Alternatives considered

- The paper drawing as the only picture (Revit): every edit waits for the drawing (seconds on big
  models); editing would need a second picking path on drawn lines.
- The GPU view styled as paper: line weights, hatches with holes, chains and legends redone on
  the GPU, never exactly the PDF.
- Element ids on drawn items: exact line hit-testing, at the cost of all of `bimble-drawing` and
  heavier drawings; the ray already selects what a cut shows, and works for linked models.

## Revisit when

- Redraws are under a second on the office's largest files: On paper could be the default
  picture for reading and annotating.
- The office asks for per-view graphic overrides.
