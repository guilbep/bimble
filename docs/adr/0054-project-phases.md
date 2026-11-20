# ADR 0054 — Project phases: Existing, New, Temporary fixed, phase filters free

- Status: Proposed
- Decided by: not yet: proposed from the wireframes v01, for Pierre
- Reversal cost: medium (two fields per element, one per view, a filter table in the model)
- Extends: ADR 0021 (views), ADR 0024 (sheets), ADR 0026 (graphic standards)

## Context / Problem

Structural drawings show what exists, what is new and what is temporary (shoring, excavation).
Revit lets each project define its own phases. The wireframes v01 fix the list (decision 5 of
`DESIGN-SPEC.md`): "The three phases Existant / Projet / Temporaire are fixed, view filters are
free". Bimble has no phase today. The SIA phase in project information (`phase`, ADR 0030) is
the project's stage on the title block, not an element's phase.

## Decision

1. **Three fixed phases**: `existing`, `new`, `temporary` (FR: Existant, Projet, Temporaire).
   They are an enum in code, not data. No one adds, renames or orders them.
2. **Elements** carry `phase_created` (default `new`) and an optional `phase_demolished`.
   A model without phases keeps its hash (fields absent at the default).
3. **Phase filters are data**: `Model.phase_filters`, an ordered list of `{ id, name,
   existing, new, temporary }`, each value one of `normal`, `halftone`, `hidden`. The wireframe
   shows five, with free names. "Add a filter" appends one.
4. **Views** pick one filter (`View.phase_filter`, a GlobalId) and one phase (`View.phase`,
   default `new`). An element demolished at or before the view's phase is hidden.
5. **Graphic overrides** per state (halftone pen, demolished line type) live in the standards
   (ADR 0026), not in each filter.
6. **UI**: Project → Project phases opens a page with three tabs: Phase definitions (read-only
   list of the three), View filters (the table), Graphic overrides. The View3D properties show
   "Phase filter" and "Phase". The SelectModify properties show "Phase created" and
   "Phase demolished". Changes apply at once (ADR 0052).

## API needed

- Commands: `SetElementPhase { ids, created, demolished }`, `SetPhaseFilters { filters }`
  (the whole list, like `SetSchedules`). Views keep `SetView`.
- Verbs: `phase_set {ids: [..], created: "existing|new|temporary", demolished?: ..|null}`,
  `phase_filter_add {name}`, `phase_filter_set {id, name?, existing?, new?, temporary?}`,
  `phase_filter_remove {id}` (refused while a view uses it), `view_phase {view, filter, phase}`.
- Queries: `phases` → `{phases: [..], filters: [{id, name, existing, new, temporary,
  used_by_views}]}`. `elements` gains a `phase` filter. `element` returns both phase fields.
- IFC: IFC4 has no phase entity. Export the state in the standard `Status` property of each
  class's common set (`Pset_WallCommon.Status`…: NEW, EXISTING, TEMPORARY, DEMOLISH), which
  other tools read. The exact phases go in a Bimble property set `Bimble_Phase` with `Created`
  and `Demolished` (superset, ADR 0003). Filters stay Bimble data.

## Alternatives considered

- Free phases like Revit: more power, but every office then defines its own and exchange breaks.
  The three cover a structural engineer's drawings.
- Phases as sub-projects (ADR 0053): a sub-project hides, it cannot halftone, and an element is
  existing and in "Concrete" at once.
- Only the IFC `Status` property: it has no "demolished in phase" and no view filter.

## Evidence

- `docs/design/wireframes-v01/en/Phases.html`: the filter table (N°, name, Existing, New,
  Temporary with Normal / Halftone / Hidden), "Filters are free: as many as needed".
- `docs/design/wireframes-v01/en/View3D.html`: "Phase filter" in the view properties.
- `docs/design/wireframes-v01/en/Terrain.html`: the excavation shows "Phase: Temporary".
- `crates/bimble-core/src/model.rs:502-528`: `Element` has no phase.
- `crates/bimble-core/src/project.rs:20`: `phase` is the SIA number for title blocks only.

## Questions for Pierre

- Is "demolished" needed now, or only "created"? Structural renovation work says yes.
- Is the IFC `Status` property enough for exchange, or must `Bimble_Phase` go out too?

## Revisit when

An office needs a fourth phase (for example, two construction stages).
