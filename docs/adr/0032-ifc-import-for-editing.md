# ADR 0032 — IFC import for editing: extend the own reader, convert to generators

- Status: Accepted (8 Nov 2026)
- Decided by: Pierre (starts I3, CLI before the viewer); implementation by Claude
- Reversal cost: medium

## Context / Problem

IFC files open for viewing, plans and comparison, but only `.bimble` models are editable
(ADR 0019). I3 converts an IFC into an editable `.bimble` model, so an office can take an
architect's or another engineer's IFC and work on it in Bimble.

Measurements on the first real fixture (a Revit 2025 structural export, IFC4, 881 elements;
see ROADMAP I3) say what the importer must handle:

- ~90 % of bodies are `IfcExtrudedAreaSolid` with `IfcArbitraryClosedProfileDef` — Revit writes
  even rectangular beams as polylines. Rectangles must be detected to map back to the parametric
  generators (`rect_column`, `rect_beam`, `wall`, `slab`, `pad_footing`).
- The rest is `IfcPolygonalFaceSet` and similar explicit meshes (parts, some walls and footings,
  roofs): non-parametric geometry that still needs to be selectable and editable.
- Openings arrive as `IfcOpeningElement` with boolean relationships; units are usually metres.

The roadmap left an open decision: IfcOpenShell via C++ bindings, web-ifc, or extend our reader.

## Decision

1. **Extend the own pure-Rust reader** (ADR 0002) for editing import. One code path serves display
   and import; it already parses every needed entity (extruded solids with arbitrary profiles and
   voids, polygonal face sets, faceted breps, placement chains, units).
2. **Geometry becomes generators** (`.bimble` format v2):
   - Rectangle detection maps straight to the existing parametric generators, by IFC class and
     extrusion direction.
   - A non-rectangular extrusion becomes a new `ExtrudedPolygon` generator (outline, voids, height).
   - Explicit meshes (`IfcPolygonalFaceSet`, faceted breps, triangulated face sets) become a new
     `Mesh` generator (vertices + triangle indices). Such elements are editable as properties and
     placement only, not parametrically.
3. **Scope of the first version:** the five structural classes (`IfcColumn`, `IfcBeam`, `IfcSlab`,
   `IfcWall`, `IfcFooting`), storeys, materials (name and category guessed from the grade),
   units converted to mm, GlobalIds kept. Openings arrive as `IfcOpeningElement` voids, are kept
   on the element as `Mesh` generators in element-local coordinates, and are cut from the mesh at
   draw time with manifold3d (ADR 0010); an opening whose geometry isn't a closed solid is drawn
   uncut and reported. Property sets beyond the material are read as text: each
   `IfcPropertySingleValue` becomes `"Pset_Name/Property" → value` on the element, exported again
   grouped by pset (labels, so the round trip is byte-stable); Bimble's own psets
   (`Bimble_Generator`, `Bimble_Properties`, `Bimble_ProjectInformation`) are re-derived on
   export, not copied. Anything unsupported is skipped and reported in an import report — never
   silently dropped.
4. **CLI first:** `bimble import-ifc model.ifc model.bimble` prints the report (counts by class,
   parametric vs mesh, openings, property values, warnings). The viewer entry ("Import IFC…")
   imports an IFC as an editable .bimble copy saved next to the file with the same stem; Ctrl S
   writes that copy. The copy starts unsaved (Save enabled, closing asks), and when a model with
   that name already exists it becomes `<stem> (imported).bimble`, `<stem> (imported 2).bimble`…
   so Save never overwrites one.
5. **Round-trip is the test:** `frame.bimble → export-ifc → import-ifc` must give back the same
   elements (GlobalIds, classes, generators, geometry) — checked in CI, with IfcOpenShell (ADR 0009)
   reading the intermediate IFC as today.

## Alternatives considered

- **IfcOpenShell via C++ bindings:** most complete IFC coverage, but adds a C++/Python toolchain to
  a from-source build and breaks the wasm story (ADR 0020). Kept as the test oracle only.
- **web-ifc:** battle-tested and wasm-friendly, but a second parser to maintain beside our reader,
  and its geometry output is tessellated, which loses the profiles the parametric generators need.

## Evidence

- Fixture analysis in ROADMAP I3: geometry is light (809 products → ~21k triangles), so conversion
  cost is in element count, not triangles; rectangle detection is the main mapping work.
- The display reader (ADR 0002) is checked element by element against IfcOpenShell on real files;
  the import reuses its parsing, so the same evidence carries over.
- Export already writes `IfcExtrudedAreaSolid` of rectangle or arbitrary-closed profiles plus a
  `Bimble_Generator` pset, so the frame round-trip exercises exactly the inverse path.

## Revisit when

- More classes are imported (`IfcRoof`, `IfcBuildingElementPart`, `IfcPile`…).
- Property values richer than text are needed (measures with units, lists, per-type editing).
- The round trip is checked against other tools' files (Bonsai, FreeCAD) on a machine that has
  them — the Revit-style fixture in `crates/bimble-ifc/tests/revit_style.rs` stands in for now.
- A real project file maps worse than 95 % of elements without warnings.
