# ADR 0024 — Sheets: views placed at their own scales; annotations belong to views

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre ("several views, one title block", like Revit; dimensions in the view; text in
  the view or on the sheet; a placed view can be edited from the sheet; numbers like S-101);
  data model and drawing by Claude
- Reversal cost: medium (model and side-file format; the drawing reuses the plan drawing)

## Context / Problem
Office drawings are sheets: a plan, sections and details, each at its own scale, with one title
block. Layouts must later be edited in the main view (#34). This ADR fixes what a sheet is and
where dimensions and texts live.

## Decision
- **`Model.sheets`** (and `sheets` in the IFC side file, which becomes version 2 when it has
  some; version 1 is still read and still written without sheets). A sheet has a number (unique,
  e.g. S-101; the next one is proposed), a name, a paper (A4–A0, portrait or not), title-block
  fields of its own, placed views (view, scale 1:N, centre on the paper in mm) and texts of its
  own. `AddSheet` / `SetSheet` / `RemoveSheet` commands: undo, history, merges (field by field,
  like everything else); the merged model is validated.
- **A view can be placed on several sheets.** A stored view placed on a sheet can't be removed;
  a default view placed on a sheet can be reset (its id doesn't change).
- **Annotations belong to views** (`View.annotations`): dimensions (two anchors: a world point
  and the element it was taken on, plus an offset) and texts (a world point, a size on paper).
  They are drawn wherever the view is drawn: on its own plan and on every sheet. Following the
  element when it moves comes with placing dimensions (#34).
- **On paper**, each placed view is the plan drawing of ADR 0023 in a "bare" form (drawing,
  axes, dimension chains, annotations, title; no frame, title block, legend or scale bar), moved
  to its place. The sheet adds the frame and the template's title block once, its texts, and one
  legend made of all its views' entries. Title-block placeholders on a sheet: `{number}`,
  `{drawing_no}`, `{sheet}`, `{drawing}` (the sheet's name), `{scale}` (all its scales).
- A view that can't be drawn (a 3D view) leaves a note on the sheet; the sheet is still drawn.
- `bimble sheets`, `bimble sheet FILE NUMBER out.pdf`, and `bimble sheet-new` (views laid out
  left to right) until sheets are edited in the viewer (#34); the viewer lists sheets under
  Layouts with a PDF export.

## Alternatives considered
- **Annotations on the sheet only**: simpler, but they wouldn't follow the model or show in the
  view itself (Pierre chose views).
- **Sheets in their own file**: they would miss the history and merges, as views would (ADR 0021).
- **Viewports cropping their view**: not yet; a placed view shows its whole drawing. Crop regions
  come with the layout editor if needed.

## Evidence
- Tests: sheets through commands (unique numbers, missing views refused, placed view kept, undo,
  file round trip); history commit and field-by-field merge; side file v1/v2; a sheet with a plan
  at 1:100 and a section at 1:50 (both placed, one legend, one title block, everything on the
  paper); a view dimension of 6 m drawn as "600" (cm); a 3D view leaves a note.
- The demo frame: `sheet-new` with a plan and a ceiling plan on A1, drawn to PNG and PDF.

## Revisit when
- Layouts are edited in the main view (#34): crops, view titles placement, sheet lists.
