# ADR 0021 — Views (3D, plans looking down or up, sections) and reference plans

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre (the five needs, the wireframe layout, "PDF + DXF; DWG via a converter",
  "views in .bimble, a side file for IFC", "plans and sections now"); data model and code by Claude
- Reversal cost: medium (a `views` map in the model and its file format; the viewer's left panel)

## Context / Problem
Engineers work in 2D: a storey plan cut at a height, a ceiling plan looking up at the slab
above, sections. They also check their model against another office's drawing (the architect's
plan, a DWG or a PDF): overlay it, move it into place, measure, but never change it.
Until now Bimble had one 3D view, a section box, and a plan export window.

## Decision
- **Views are model data.** `Model.views: BTreeMap<GlobalId, View>`, serialized only when not
  empty (old files and their hashes are unchanged). A view is `ThreeD`, `Plan{storey, look:
  Down|Up, range}` or `Section{origin, facing N/E/S/W, depth}`, plus what it hides (families,
  IFC classes, storeys) and its reference plans. Changed through `AddView`, `SetView`,
  `RemoveView` commands (undo, history, merges); `RemoveStorey` is refused while a plan uses the
  storey.
- **Default views are not stored.** "3D", "Plan <storey>" and "Ceiling <storey>" are derived
  from the storeys with ids from `GlobalId::derived`, so they are the same every time; a changed
  default is stored under the same id and "Reset to default" removes it.
- **View range like Revit's:** top, cut plane, bottom and view depth, each a level (this storey,
  the one above, the one below, unlimited) plus an offset in mm. Looking down shows
  [depth, cut]; looking up shows [cut, depth]. The order is validated (down: depth ≤ bottom ≤ cut
  ≤ top; up: bottom ≤ cut ≤ top ≤ depth); an invalid range is refused with a message, never
  drawn half-right. Default cut: 1200 mm (down), 2300 mm (up), lowered to half the storey height
  on low storeys.
- **IFC models are not written.** Their views go in `<file>.ifc.bimble-views.json` next to the
  IFC (format `bimble-views`, version 1), saved with Ctrl+S or "Save views"; in a browser it is
  downloaded (it can't be read back there yet).
- **2D views reuse the 3D renderer.** An orthographic camera square to the view, the range as the
  clip box (cut faces filled and hatched as with the section box), so selecting, editing and
  measuring work exactly as in 3D. Plans looking up mirror the picture left–right so north stays
  up and east right, as on a reflected ceiling plan (the clip passes use the other winding when
  mirrored). In 2D, dragging pans; the view cube, Home, the section box and perspective are off.
  Each view keeps its own camera while the viewer is open.
- **Reference plans are dead geometry.** DXF is read (lines, arcs, circles, ellipses, polylines
  with bulges, splines, solids, block inserts; model space; units from `$INSUNITS`); vector PDF
  paths of the first page at a scale 1:N given at import; DWG is refused with the converters to
  use (ODA File Converter, LibreDWG's `dwg2dxf`). The result is polylines in mm stored in the view
  (at most 2,000,000 points), with an offset and a rotation: the only things that can change. It
  is drawn over the view in the reference colour, snapped to when measuring, moved by dragging
  ("Drag it in the view") or by typing the offset. It is kept where its coordinates put it when
  that overlaps the model, else centred on it.
- **Layout from the wireframe:** the left panel is the navigator (Views by kind with New,
  Duplicate, Delete/Reset; Quantities; Layouts; Objects by IFC class; Links: linked model and
  reference plans; Measurements; Warnings) above the properties of the selected element, else of
  the view (name, range, section, visibility, reference plans).

## Alternatives considered
- **Views in a separate file for .bimble too:** they would not follow the model's history and
  merges. Rejected.
- **Drawing 2D views with the plan exporter (vector, CPU):** better line work, but a second path
  for picking and editing. Kept for paper output (the plan window), not for the screen.
- **Reading DWG directly:** LibreDWG is GPL and C; the ODA SDK is not open source. A converter
  step is honest and keeps the licence MIT/Apache.
- **Referencing the reference file by path instead of storing its geometry:** the plan would
  disappear when the file moves or the model is sent to someone. Stored geometry is larger but
  self-contained; the 2,000,000-point limit bounds it.

## Evidence
- `bimble-core` tests: range order, defaults, low storeys, views in commands, history and merges.
- `bimble-render` test `mirrored_camera_flips_the_picture_with_cut_surfaces` fails without the
  winding fix (1728 of 2269 pixels differ).
- `bimble-drawing::reference` tests: DXF entities, blocks, recursion limit, units; PDF paths.
- Driven in headless Chrome on the demo model with a synthetic DXF (journal 2026-11-06).

## Revisit when
- Sheets with several views (the "Layouts" section only opens the plan window today).
- Raster PDFs or images as references; several pages.
- Reading the side file back in a browser.
