# ADR 0057 — Site: a surveyed terrain surface, excavations and earthwork volumes

- Status: Proposed
- Decided by: not yet: proposed from the wireframes v01, for Pierre
- Reversal cost: high (a new kind of geometry: a triangulated surface and Boolean volumes)
- Extends: ADR 0045 (LV95 coordinates, project north), ADR 0054 (temporary phase)

## Context / Problem

Foundations and retaining walls sit in the ground. The engineer needs the terrain, the
excavation and the cut and fill volumes. Bimble writes one `IfcSite` with no shape
(`crates/bimble-ifc/src/lib.rs:363`). It knows where the model is (LV95, `Georef`,
`georef.rs:14`), but holds no terrain. All element generators are extrusions and solids.

## Decision

1. **Terrain surface**: one per model, `Model.site.terrain`, a triangulated surface from survey
   points in LV95 metres, stored relative to the project base point in mm (ADR 0045). A bounded
   count (budget: 200 000 points, files are untrusted).
2. **Import**: points from CSV (E, N, H) or DXF points, contours from DXF polylines. The import
   triangulates (Delaunay) once and stores triangles, so the result is exact and canonical.
   The source stays a link ("Linked: survey.dxf").
3. **Pads and excavations** are elements: `IfcExcavation` (IFC4.3) / `IfcEarthworksCut`, with
   a generator `{ outline, bottom_m, slope (ratio), working_space_mm }`. An excavation is
   `temporary` by default (ADR 0054).
4. **Volumes**: cut and fill are computed between the terrain and the excavations or pads,
   shown in the properties panel (Cut 1 842 m³, Fill 312 m³) and in schedules (ADR 0029).
5. **UI**: a Site ribbon tab with Survey (Import points, Import contours), Surface (Topo
   surface, Pad), Earthworks (Excavation, Slope, Volumes) and Measure. Handles on the selected
   excavation change it. The status bar shows the pointer in LV95. Changes apply at once.

## API needed

- Commands: `SetTerrain { points, triangles }` (whole surface, one undo), `AddElement` for
  excavations and pads (exists) with a new `Generator::Excavation`.
- Verbs: `terrain_import {path, kind: "points"|"contours", crs?}`, `terrain_clear {}`,
  `excavation_add {outline: [[x,y]..], bottom_m, slope, working_space_mm}`,
  `excavation_set {id, …}`.
- Queries: `terrain` → `{points, triangles, bbox, source}`, `earthworks` → `[{id, cut_m3,
  fill_m3}]`, and `pointer` gains LV95 E/N/H (it gives world mm today).
- IFC: the surface exports as `IfcGeographicElement` (PredefinedType `TERRAIN`) with an
  `IfcTriangulatedFaceSet`, contained in `IfcSite`. Excavations export as `IfcEarthworksCut`
  in IFC4.3 and as `IfcBuildingElementProxy` with `ObjectType "Excavation"` in IFC4 and
  IFC2x3 (ADR 0047). Volumes go in a Bimble property set `Bimble_Earthworks` (superset,
  ADR 0003).

## Alternatives considered

- Terrain as a linked reference only (DXF, ADR 0021): it draws, but no volume and no section cut.
- A grid of heights (DEM raster): simple, but loses breaklines from the surveyor's contours.
- Leave earthworks to civil tools: then the excavation drawing and the foundation never match.

## Evidence

- `docs/design/wireframes-v01/en/Terrain.html`: ribbon groups Survey / Surface / Earthworks,
  excavation F1 (bottom, slope 2:1 (63.4°), working space, IfcExcavation, Phase Temporary),
  volumes Cut 1 842 m³ / Fill 312 m³, terrain 1 284 points, LV95 pointer, linked survey.dxf.
- `crates/bimble-ifc/src/lib.rs:363`: `IfcSite` written without a shape.
- `crates/bimble-core/src/georef.rs:14-30`: LV95 base point and height.

## Questions for Pierre

- IFC4.3 export needed now? The wireframe's `IfcExcavation` is not an IFC class: IFC4.3 names
  it `IfcEarthworksCut`.
- Is the site in scope before the structural tools (rebar, steel) are done?

## Revisit when

An office links a terrain from swisstopo (swissALTI3D) instead of a survey file.
