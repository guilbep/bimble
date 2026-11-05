# ADR 0003 — Scope and data model: structural first, IFC as the model, `.bimble` as a superset

- Status: Accepted (5 Nov 2026)
- Decided by: Pierre (scope, IFC as model, own format), from the two source documents below
- Reversal cost: high (everything is typed with IFC classes)

## Context
Two documents fed the project and disagreed:
- the long **PRD/RFC** ("compete with Revit"): general BIM, SQLite native project, custom `.b3d` geometry
  container, IFC 4.3 as an *exchange* format;
- the French **Feuille de route** (v0.1, 21 Oct 2026): structural BIM for a bureau d'études (concrete,
  steel, timber), formwork and rebar drawings, IFC file *as* the native file, Python first.

## Decision
- **Scope:** structural, concrete first, then steel and timber. The drawing (formwork, then rebar) is the
  product, not the 3D model.
- **IFC is the data model and the vocabulary.** Elements are IFC classes (`IfcColumn`, `IfcBeam`,
  `IfcSlab`…) with IFC GlobalIds; the UI uses IFC names.
- **The IFC file is not our storage.** Pierre: "the docx was wrong… we should be able to go farther and have
  our own format". `.bimble` stores everything IFC can, plus what it can't: generators and parameters,
  later constraints, history and branches.
- **IFC export always works.** Each element is exported with explicit geometry (readable by any tool) plus a
  `Bimble_Generator` property set (generator, version, parameters as JSON) so it can be regenerated.
- Production IFC version: **IFC4 ADD2 TC1** (widest support in Revit, Tekla, FreeCAD). The reader also
  accepts IFC2x3 and IFC4x3.
- `.b3d` (a name Pierre likes) is kept for a future **derived** binary geometry cache, never a source of truth.

## Alternatives considered
- IFC file as the native format (Feuille de route): simplest, but no room for history, branches or
  generator data beyond property sets.
- SQLite project + `.b3d` now (PRD): weeks of format work before the first drawing. Parked until a benchmark
  shows JSON + content-addressed objects are not enough.
- Reverse-engineering `.rvt`: rejected (legal risk, version churn).

## Consequences
- `.bimble` v1 is canonical JSON (`docs/format/bimble-format.md`); a content-addressed store comes with
  versioning (ADR 0004).
- Anything we can't export must be reported, never silently dropped.
