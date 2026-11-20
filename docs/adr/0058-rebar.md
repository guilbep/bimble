# ADR 0058 — Rebar: bars, stirrups and meshes hosted by a concrete element, a bar schedule

- Status: Proposed
- Decided by: not yet: proposed from the wireframes v01, for Pierre
- Reversal cost: medium (the bar data becomes part of `.bimble` and the IFC export)
- Extends: ADR 0003 (scope and data model), ADR 0005 (Commands), ADR 0029 (schedules), ADR 0031 (materials)

## Context / Problem

The ROADMAP puts rebar after the formwork MVP ("concrete formwork MVP + sub-models/locking → rebar",
ROADMAP.md:193). The drawing is the product: an engineer needs a bar schedule and a rebar drawing
from the same model as the formwork plan. The wireframe B3 marks the Rebar tab "new". Bimble has no
bar today: `Generator` knows columns, beams, slabs, walls, footings, polygons and meshes only.

The design code is open. SIA 262 or EC2 decides covers, minimum laps, bend diameters and shape
codes. That choice belongs to the engineer (ROADMAP.md:203, "Rebar code (SIA 262 / EC2) —
engineer"). This ADR keeps it open.

## Decision

- A bar set is an element of class `IfcReinforcingBar`, with a host: the concrete element it sits
  in (wireframe: "Host IfcBeam B12"). A stirrup is an `IfcReinforcingBar` with role `STIRRUP`. A mesh
  is an `IfcReinforcingMesh`.
- One bar set holds: position number, diameter, count, steel grade (a material, ADR 0031: B500B),
  cover, shape code and the shape's dimensions. Bimble computes unit length, mass and the host's
  ratio in kg/m³. Computed values are not stored.
- The shape codes and the cover and lap rules come from a **rebar code** setting in the office
  standards. The setting has no default value until Pierre and the engineer choose SIA 262, EC2
  or both. Shape codes in the wireframe ("00 · straight", "51") stand for that table.
- Placing: the user clicks a face of the host, the bar runs along it at the cover distance. Esc
  stops (wireframe status line).
- The Rebar tab has the groups Bars (Bar, Stirrup, Mesh, Rebar area), Shapes (Shape codes, Lap),
  Lists (Bar schedule, Rebar drawing). The bar schedule is a schedule (ADR 0029) filtered to one
  host or to a selection.
- Rebar is hidden by default in formwork plans (ROADMAP.md:93 already says so).

## Alternatives considered

- Rebar as a property set on the host only (bar count and area per m): fast, but no bar schedule
  and no rebar drawing. Rejected: the drawing is the product.
- One element per physical bar: exact, but thousands of elements per slab. A bar set with a count
  matches the schedule rows and `IfcReinforcingBar` with a mapped shape.
- Choose SIA 262 now: the office is Swiss. Rejected for now: the code is the engineer's decision.

## Evidence

- `docs/design/wireframes-v01/en/Rebar.html`: properties "Bars pos. 12, Ø16, Count 4, Steel B500B,
  Cover, Shape code 00 · straight, Host IfcBeam B12, Class IfcReinforcingBar", computed "Unit length
  7 340 mm, Mass 46.3 kg, Beam ratio 118 kg/m³", a bar schedule with Pos., Ø, Count, Shape, Length,
  Mass.
- `crates/bimble-core/src/model.rs:256`: `Generator` has no bar variant.
- `crates/bimble-core/src/command.rs:10`: `Command` has `AddElement` and `SetGenerator`, enough to
  add and edit a bar set once the generator exists.
- ADR 0031: B500B is in the material library (SIA 262, Es = 205 GPa), so bar mass needs no new data.

## API needed

- Core: a new `Generator::RebarSet { host, position, diameter, count, shape_code, dims, cover }`.
  Added with `AddElement`, edited with `SetGenerator`. Validation refuses a host that is not
  concrete and a cover larger than the host.
- Command: `SetHost { id, host }` to move a bar set to another host (the GlobalId stays).
- Verb `place` with `{"class":"IfcReinforcingBar","host":"<GlobalId>","face":n,"diameter":16,
  "count":4,"shape_code":"00","material":"<GlobalId>"}`.
- Queries: `rebar` `{"host":"<GlobalId>"}` → rows (position, diameter, count, shape, length, mass),
  `rebar_codes` `{}` → the shape codes of the chosen rebar code (empty until chosen).
- IFC: `IfcReinforcingBar` (NominalDiameter, BarLength, PredefinedType MAIN/SHEAR/STIRRUP),
  `IfcReinforcingBarType` (BendingShapeCode, BendingParameters), `IfcReinforcingMesh`. IFC has no
  plain "host" attribute for bars: Bimble keeps `host` in its superset (ADR 0003) and writes
  `IfcRelAggregates` host → bars in the export. IFC has no standard place for the cover either:
  it is a Bimble property, `Bimble_Rebar.Cover`.

## Revisit when

Pierre and the engineer choose the rebar code, or the formwork MVP is done and rebar starts.
