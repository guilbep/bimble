# `.bimble` format — version 1 (POC)

Status: draft. It will change; readers must reject versions they don't know.

## Encoding

UTF-8 JSON:

```json
{ "format": "bimble", "version": 1, "model": { ... } }
```

`model` contains `project_id`, `project_name`, and three maps keyed by IFC GlobalId (22 characters,
IFC base64 alphabet): `storeys`, `materials`, `elements`.

## Rules

- **Units:** millimetres. Numbers are IEEE f64, written in shortest round-trip form.
- **Canonical form:** maps are sorted by GlobalId and struct fields have a fixed order, so the same model
  always gives the same bytes. Hashes and diffs rely on this.
- **GlobalIds are stable:** they never change when an element is edited.
- **Elements** carry an IFC class (`IfcColumn`, `IfcBeam`, `IfcSlab`, `IfcWall`, `IfcFooting`), a storey,
  a material, a placement relative to the storey (`origin`, `z_axis` = extrusion direction, `x_axis`)
  and a `generator`.
- **Generators** (`kind` + parameters) are data only. They are never executed.
- **Openings** (optional, ADR 0032): each element may list openings — a placement in element-local
  coordinates plus a generator, normally a `Mesh` for an imported `IfcOpeningElement`. They are
  subtracted from the element's mesh when it is built; absent when there are none. An opening may
  carry an optional `id`, the `IfcOpeningElement` GlobalId kept from import and written back on
  export; without one (older files, copies), export derives it from the element and the index.
- **Properties** (optional, ADR 0032): property sets beyond the material, as
  `"Pset_Name/Property" → value` with the value as text. Exported grouped by pset as `IFCLABEL`
  values; absent when empty.
- **Validation on load:** format and version header, size limit, references (storey, material),
  finite numbers, positive dimensions, counter-clockwise slab outlines.
- **Saves are atomic:** write to a temporary file, then rename.

## Mapping to IFC4

| `.bimble` | IFC4 |
|---|---|
| element | IfcColumn / IfcBeam / IfcSlab / IfcWall / IfcFooting, same GlobalId |
| geometry | IfcExtrudedAreaSolid of IfcRectangleProfileDef or IfcArbitraryClosedProfileDef |
| openings | IfcOpeningElement per opening (geometry relative to the element), via IfcRelVoidsElement |
| properties | IfcPropertySet per `Pset_Name`, values as IfcLabel, via IfcRelDefinesByProperties |
| generator | Pset `Bimble_Generator`: `Generator` (label), `Version` (integer), `Parameters` (JSON text) |
| storey | IfcBuildingStorey, same GlobalId, elements contained via IfcRelContainedInSpatialStructure |
| material | IfcMaterial (name, category), via IfcRelAssociatesMaterial |

## Next

Content-addressed object store (one object per element, keyed by hash), commits and branches.
