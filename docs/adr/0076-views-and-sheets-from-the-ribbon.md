# ADR 0076 — Views and sheets from the ribbon: New view…, Section, Place view…, Revision…, annotation types

- Status: Accepted, 26 Nov 2026
- Decided by: Pierre delegated the decision ("don't wait for me to validate the ADR, just do your
  best, I'll review once it's implemented"); the choices below by Claude on ticket VIEWSHEET, to
  be reviewed by Pierre on the implementation.
- Reversal cost: low (four session actions, two optional `view_add` arguments, one query, one
  hidden action, four dialogs and one viewport tool in the page; no file field)

## Context / Problem

Pierre found dead controls in the published lab and asked to "implement all of them", with the
goal "for each feature we want to target < 200ms response time". In the View, Sheets and Annotate
tabs six ribbon controls had `action: null` and no screen: New view…, Section, Place view,
Revision, Text type, Dimension type. The C core already had the verbs behind them: `view_add`
(SEC1), `sheet_place` (SH1), `set_sheet {revision}` (Rust's), the standards' text and dimension
types (`draw_style.c`) and the annotate tool.

## Decision

Each choice with the alternative not taken.

1. **New view… is a dialog over the verb `view_add`**: kind (floor plan, ceiling plan, section,
   3D), level for plans, facing for a section, name, scale; one `view_add` with `scale` and `name`
   in it (one undo step), then the view opens. Elevation is listed "not available yet" (the core has
   no elevation view; a section looking at the facade stands in). *Alternative:* the navigator's
   menu (one item per level and facing) in the ribbon too: no name or scale, and the list grows
   with the levels.
2. **`view_add` takes `scale` and `depth_mm`** (C additions). *Alternative:* `view_add` then
   `set_view`: two undo steps for one gesture.
3. **Section is a two-click tool in a floor plan** (action `draw_section`, session state): the line
   from the first to the second click; the view looks to the line's right, snapped to the four
   facings the core's sections have (north, east, south, west); the cut goes through the line's
   middle; depth 0 (all), set afterwards in Properties; one `view_add`; the plan stays shown with
   the new mark (no jump into the section) and the tool stops; Esc drops the first point, then the
   tool. In 3D the tool is off with its reason ("open a floor plan"). The navigator's "+" keeps its
   menu (sections through the model's middle): one verb, two entry points. *Alternatives:* any
   angle (the core's sections are axis-aligned, `view.h cc_facing`: a turned section needs a new
   field); a third click for the depth (one more step for a value most sections leave at "all");
   drawing in 3D on the work plane (left for later: a plan is where sections are drawn).
4. **Revision… sets the sheet's one revision index** (Rust's `set_sheet {revision}`, printed in the
   title block's `{revision}` in SVG and PDF), from a small dialog; one undo step. *Alternative:*
   a revision table (index, date, text, author) in the title block: Rust v0.19.0 has none, it needs
   a new `.bimble` field, a date from the carrier and a title-block layout; recorded as the next
   step, said in the dialog.
5. **Place view… is a list of the model's views on the sheet shown**; a click places the view at
   the paper's centre (one `sheet_place`) and selects it, to be moved with the Select tool (as the
   Sheets page). A view already on a sheet is placed again (Rust's layout does the same on a drop);
   the list says on how many sheets each view already is. Off with its reason without a sheet
   (action `sheet_place_view`). *Alternatives:* click on the paper to place (a second mode on the
   sheet page for little gain: the drop at the centre is one click and moving is the Select tool's
   job); refusing a second placement (Rust allows it; a detail and its overview may share a view).
6. **Text type and Dimension type are two selects** of the standards' types (25 text types, the
   dimension types of the default standards), "Default" first, the choice session state (action
   `annotation_type`, query `annotation_types`), given to the next text or dimension the annotate
   tool places (a text gets the type's size, as Rust's sheet tool). *Alternatives:* a readout of
   one type (the core holds 25: a select is honest); also changing the selected annotation (Rust
   does not; it needs a verb that changes an annotation's kind: left for later).
7. **A dialog tool with an action is as enabled as its action** (`query actions`), with the reason
   in its tooltip and on a click (`offTip`); the page never guesses. *Alternative:* page-side
   checks (a sheet open?): the rule of the lab is that the core says.

## Evidence

- `ccore/test/view_sheet_test.c` (43 checks): enablement, the switch, `view_add` scale and depth
  (one step, refusals), `annotation_types`, hostile `annotation_type` bodies, the annotate tool's
  kind and size.
- `ccore/test/lab_view_sheet.mjs` (39 checks): each control through its whole gesture on the
  native host (tcc build, 72-element frame, headless Chromium with SwiftShader), with the time from
  the click to the first frame after the effect. Last run (26 Nov, machine load 4.9-6, never under
  4 that morning: other tickets' browsers): New view… dialog 29 ms, create and open 138 ms, Section
  on 11 ms, second click to frame 54 ms, Place view… dialog 31 ms, pick 198 ms, Revision… dialog
  33 ms, apply 35 ms, Text type 62 ms, Dimension type 22 ms (over five runs: pick 71-198 ms,
  apply 35-229 ms, create 106-221 ms once the session has shown a plan).
- Two causes found and one fixed: (1) any change of the action list redrew the viewport (a whole
  SwiftShader frame before a dialog's first paint): New view… dialog 318-504 ms before the fix,
  23-88 ms after (the viewport now redraws for orthographic, edges and the section tool only);
  (2) the session's first plan costs about 500 ms more once (create 700-900 ms when it is the first
  plan shown): the host answers in under 11 ms (`curl`: tab open 0.8 ms, mesh 10 ms, view_clip
  0.8 ms), a CPU profile of the page shows 570 of 681 ms idle (the GPU process: SwiftShader
  compiling the plan's shaders), and the next plans cost 60-190 ms. Not fixed: a headless
  software-GL cost, not the verb's.

## Revisit when

- the core gains turned sections or elevations (decision 3, the dialog's "not available yet");
- a revision table is asked for (decision 4);
- the sheet tools (select, text, dimension on a sheet) are served: the types then apply there.
