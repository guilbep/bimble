# ADR 0059 — Steel and timber members from a section catalogue

- Status: Proposed
- Decided by: not yet: proposed from the wireframes v01, for Pierre
- Reversal cost: medium (catalogue names become data in `.bimble` files)
- Extends: ADR 0003 (scope), ADR 0005 (Commands), ADR 0031 (material library)

## Context / Problem

After concrete, the ROADMAP comes to steel, then timber (ROADMAP.md:21). Today a beam is a
rectangle (`Generator::RectBeam`). A steel beam is an HEA 200 or an IPE 300: a named section with
known dimensions and kg/m. The wireframe B4 shows a Steel tab and a Timber tab with columns, beams
and bracing placed from a catalogue. The material library (ADR 0031) already has steel S235–S460,
timber C24 and glulam GL24h–GL32h.

## Decision

- A **section catalogue** ships with Bimble: EN 10365 for HEA, HEB, IPE, UPN, and EN 10219 / 10210
  for RHS and CHS, with h, b, tw, tf and kg/m (wireframe). Timber: glulam and solid timber
  rectangles in mm (wireframe: "same catalogue, glulam GL24h, GL28h and solid timber C24").
  The catalogue is data, read-only, in the office standards. An office adds its own sections there.
- A member stores the **section name** and the section's dimensions. A file opens the same on a
  machine with another catalogue. A member keeps its GlobalId when its section changes.
- Placing a beam: storey, section, material, justification ("Top at level"), rotation,
  sub-project, then two clicks. A click near a column's centre snaps to it, so a beam from column
  to column goes exactly centre to centre (wireframe "How" text).
- Steel and Timber tabs share the tools Column, Beam, Bracing (Structure), Base plate and
  Connection (Connections), Catalogue (Sections). Base plates and connections are later. The
  tabs show them so the place is fixed.
- Mass in schedules uses the catalogue kg/m for steel, volume × density for timber (ADR 0031).

## Alternatives considered

- Extrude a drawn polygon (`ExtrudedPolygon` exists): no new code, but no section name, no kg/m,
  and an IFC export without a profile name. Rejected.
- Store only the section name and look it up: smaller, but a file then depends on the catalogue.
  Rejected: files are untrusted and need to stand alone.
- One "Steel and timber" tab: the wireframe has two trade tabs (ADR 0051), with the same tools are the same.

## Evidence

- `docs/design/wireframes-v01/en/Steel.html`: "Place: steel beam, Storey N02 (+6.20), Section HEA 200,
  Material Steel S355, Justification Top at level", the table "Steel sections · EN 10365" with HEA
  160–260 (h, b, tw, tf, kg/m), the timber note.
- `crates/bimble-core/src/model.rs:256-293`: `Generator` has `RectColumn`, `RectBeam`,
  `ExtrudedPolygon`, no profile.
- `crates/bimble-core/src/model.rs:37`: `MaterialCategory` has `Steel` and `Timber`.
- ADR 0031: grade values for S235–S460, C24, GL24h–GL32h, and `lookup` that maps "Steel S355 J2".

## API needed

- Core: `Generator::ProfileMember { profile: Profile, start, end, rotation, justification }`.
  `Profile { name, kind: I|U|RHS|CHS|Rect, h, b, tw, tf, r }` in mm.
- Commands: `AddElement` and `SetGenerator` (exist). No new Command.
- Verb `place` with `{"class":"IfcBeam","section":"HEA 200","material":"<GlobalId>",
  "storey":"<GlobalId>","justification":"top","rotation":0,"points":[[x,y,z],[x,y,z]]}`,
  also `IfcColumn` and `IfcMember` (bracing).
- Queries: `sections` `{"family":"HEA"}` → catalogue rows, and `section` `{"name":"HEA 200"}`.
- IFC: `IfcBeam` / `IfcColumn` / `IfcMember` with `IfcExtrudedAreaSolid` of an
  `IfcIShapeProfileDef`, `IfcUShapeProfileDef`, `IfcRectangleHollowProfileDef`,
  `IfcCircleHollowProfileDef` or `IfcRectangleProfileDef`, with `ProfileName` = "HEA 200".
  `IfcMaterialProfileSet` links material and profile. Standard IFC places, no superset needed.

## Revisit when

Concrete and rebar are done, or an office asks for connections and base plates.
