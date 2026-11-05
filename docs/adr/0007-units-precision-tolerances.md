# ADR 0007 — Units, precision and tolerances

- Status: Accepted (5 Nov 2026)
- Decided by: Claude (proposed), following the Feuille de route's "lengths in mm"
- Reversal cost: medium

## Decision
| Quantity | Rule |
|---|---|
| Model lengths | millimetres, `f64` |
| IFC import | converted to mm from the file's length unit (SI prefixes and conversion-based units, e.g. feet) |
| GPU meshes | `f32`, **relative to the scene origin** (centre of the bounding box, rounded to 1 mm) so Swiss site coordinates (~2.5 M m) keep sub-mm precision |
| Arc tessellation | chord deviation ≤ **0.5 mm**, 8–1024 segments per arc |
| Placement axes | unit length and perpendicular within **1e-6** |
| Edge welding for outlines | 0.01 mm; crease angle 20° |
| IFC export geometric context | precision 1e-5 |

## Evidence
- Fixed 7.5° arc steps left chords 2.7 mm inside a 1.25 m-radius wall; IfcOpenShell is finer. With the
  0.5 mm tolerance, the Revit sample matches IfcOpenShell within 0.002 mm (bounding boxes).
