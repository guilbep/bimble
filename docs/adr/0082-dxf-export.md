# ADR 0082 — DXF export of views and sheets

- Status: Accepted, 26 Nov 2026. Delegated by Pierre (owner): "don't wait for me to validate the
  ADR, just do your best, I'll review once it's implemented" (ticket DXF1, the last line of
  Mission 1: "Export views to DXF").
- Concerns: `ccore/src/dxf_out.{h,c}` (the writer), `ccore/src/paper.h` (`cc_item_tag`),
  `ccore/src/draw_plan.c` and `draw_annot.c` (the tags), `ccore/session/sheet_svg.c`
  (`query view_dxf`, `sheet_dxf`), `ccore/session/paper_scene.c` (the links' names),
  `web-ui/src/dialogs/Export.vue`, `FileMenu.vue`; suite `ccore/test/dxf_out_test.c`, goldens
  `ccore/golden/dxf_c/`, lab check `ccore/test/lab_export.mjs`.
- Reversal cost: low (a new export; the drawing's bytes for SVG and PDF do not change).

## Context

Rust v0.19.0 read DXF (reference plans, `reference.rs` over the `dxf` crate) and never wrote one:
the only DXF it built was in its tests. So there is no oracle: the goldens are C-own, and the
core's reader (`src/dxf_in.c`, the port of that crate's reader) and ezdxf are the checks.

## Decisions (each with the alternative not taken)

1. **Write what the paper pipeline draws.** The writer takes the same items as SVG and PDF
   (`cc_paper`), so a DXF shows exactly the PDF's drawing.
   Alternative: a second drawing from the scene for CAD; two drawings would drift apart.
2. **Tags on the items, not a new intermediate.** `cc_item` gains `tag` (IFC class, role, element,
   material family) and per-ring tags for the gathered projection and above lines; `draw_plan.c`
   sets them where it already draws. SVG and PDF ignore them: every golden keeps its bytes.
   Alternative: a parallel "CAD drawing" list; more code, and the drawing order would be lost.
3. **AC1015 (AutoCAD 2000), ASCII.** The version every CAD program of the last 25 years reads,
   with LWPOLYLINE, HATCH, true colour readers, lineweights and the OBJECTS section.
   Alternative: R12 (AC1009), read by even older tools, but no LWPOLYLINE, no lineweights, no
   HATCH: cut fills and weights would be lost. Binary DXF: smaller, not diffable.
4. **A view: model space in real millimetres at 1:1**, the view's own coordinates (a plan: world
   X, Y; a section: along it and the height above project zero), taken back from paper through the
   drawing's frame and rounded to 0.001 mm (the meshes are f32: the digits beyond are noise).
   Texts, dimension offsets and hatch spacings are the paper's times N; line types in paper mm with
   `$LTSCALE` = N. Alternative: paper mm (a plotted sheet); unusable to dimension or trace on.
5. **A sheet: the sheet as on paper, in paper mm, in model space** (frame, title block, views at
   their scales). Alternative: paper space with VIEWPORTs onto a 1:1 model: needs every view's
   model twice and LAYOUT/VIEWPORT objects per sheet; left for later. "All views into a folder"
   gives each view at 1:1 instead.
6. **Layers by IFC class and role**: `IfcColumn-cut`, `-view`, `-above`; `Hatch-<family>` (cut
   fills), `Hatch-<family>-view` (tops seen); `IfcGrid`; `Annotation-dimension`, `-text`,
   `-symbol`; `Sheet`; a linked model's elements prefixed `<link name>-`. Colour: the ACI nearest
   to the pen's RGB (dark ink: 7) and the RGB as group 420; lineweight: the nearest standard
   weight to the pen's width. Alternative: AIA/ISO 13567 codes (`S-COLS-CUT`); offices map
   either, and the IFC names are the app's vocabulary (CLAUDE.md). `-above` is the graphic
   standards' role name; the ticket's example `-hidden` is the same thing.
7. **Dimensions exploded** (their lines and value text on `Annotation-dimension`). A DIMENSION
   entity needs its anonymous block (`*D<n>`) of lines, arrows and text, a DIMSTYLE matching the
   paper's, and the measured points: next step. Alternative: DIMENSION now with AutoCAD
   regenerating the block; readers that do not regenerate would show nothing.
8. **Circles back from polygons; no arcs yet.** The paper has polylines only: a ring of 16 points
   or more on one circle (grid bubbles) is written as CIRCLE, everything else as LINE or
   LWPOLYLINE (no bulges: the paper has no arcs). Alternative: fit arcs to any polyline; risky.
9. **The paper's white fills are left out** (a bubble's white disc): a CAD background is often
   dark, and nothing in the drawing needs masking (the axis stops at the bubble).
10. **Texts**: TEXT (left, centre, right), style Standard on `arial.ttf` (Helvetica's metrics, the
    PDF's default); `$DWGCODEPAGE` ANSI_1252 and every non-ASCII character `\U+XXXX`.
    Alternative: MTEXT; needed for multi-line notes only, the paper has one item per line.
11. **Deterministic**: handles counted from 1 (tables, blocks, objects, then the entities), layers
    sorted by bytes, numbers in the core's shortest round-trip text (no `-0`, no exponent),
    `\n` line ends, no time stamp: a golden diffs line by line.
12. **Streamed**: the writer sends chunks of 64 KB to a sink and keeps only its layers and line
    types; two passes over the items (layers and counts, then the file). The session still
    gathers the file (the carriers write whole files). A million lines: 117 MB in 1,793 chunks.
13. **Not in the viewer-only wasm.** The writer needs the drawing (`draw_*`, `paper`), which the
    viewer leaves out (it has no PDF export either): `view_dxf` answers "this build is
    view-only" there, capability `export_dxf` false. Alternative: put the drawing in the viewer
    (its PDF export too); a size decision of its own (ADR 0069 budget).
14. **The page**: the Export dialog's DXF choice (This view, All 2D views: one file each named
    after the view; Sheets: one file per sheet), File › "Export this view to DXF…" opens it on
    DXF; the click shows "Exporting…" in its first frame, then "Exported: N file(s)".

## Evidence

- `ccore/test/dxf_out_test.c`: the fixture's plan and section read back by the core's reader
  match the hand-computed values (cut columns, slabs, footings, hatch area 90,000 mm²).
- ezdxf 1.4.5 (`ezdxf.readfile`, `recover`, `audit`): 0 errors, 0 fixes on both goldens, on the
  plan with two linked models and on a sheet; `recover` turns the `\U+XXXX` texts back into
  "épicéa", "Maître d'ouvrage — Ville".
- Sizes: the plan of the 72-element fixture 58,901 bytes, its section 43,291 bytes; the writer
  21,917 bytes of `ccore.wasm` (`build.sh wasm-size`); `ccore-view.wasm` 498,397 bytes, unchanged.
- `ccore/test/lab_export.mjs`: the dialog exports a plan; linked back as a DXF reference it has
  the file's extents.
