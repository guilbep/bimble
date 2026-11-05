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
- **Validation on load:** format and version header, size limit, references (storey, material),
  finite numbers, positive dimensions, counter-clockwise slab outlines.
- **Saves are atomic:** write to a temporary file, then rename.

## Mapping to IFC4

| `.bimble` | IFC4 |
|---|---|
| element | IfcColumn / IfcBeam / IfcSlab / IfcWall / IfcFooting, same GlobalId |
| geometry | IfcExtrudedAreaSolid of IfcRectangleProfileDef or IfcArbitraryClosedProfileDef |
| generator | Pset `Bimble_Generator`: `Generator` (label), `Version` (integer), `Parameters` (JSON text) |
| storey | IfcBuildingStorey, same GlobalId, elements contained via IfcRelContainedInSpatialStructure |
| material | IfcMaterial (name, category), via IfcRelAssociatesMaterial |

## Next

Content-addressed object store (one object per element, keyed by hash), commits and branches.
