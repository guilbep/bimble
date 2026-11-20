# ADR 0053 — Sub-projects: group, hide and control loading, borrowing still decides who edits

- Status: Proposed
- Decided by: not yet: proposed from the wireframes v01, for Pierre
- Reversal cost: medium (one field per element in the model and the file, one IFC grouping)
- Extends: ADR 0038 (layers, point 5), ADR 0033 (choose what to load), ADR 0044 (hide in a view)

## Context / Problem

Revit groups elements in worksets. Bimble has no group of this kind. ADR 0038 point 5 proposes
"layers": a named group per element, shown or hidden, and *borrowed* by one person as a lock.
The wireframes v01 (decision 3 of `DESIGN-SPEC.md`) keep the group but drop the lock: sub-projects
group, hide and control loading, and element borrowing (ADR 0038) still decides who edits.
A weak machine also needs to skip a part of the model at open (ADR 0033 chooses storeys, classes
and materials, not a named group).

## Decision

1. **Data.** `Model.subprojects`: a map GlobalId → `{ name, visible_by_default, open_on_load }`.
   Each element has one `subproject` GlobalId. A model without sub-projects keeps its hash
   (the map and the field are absent when empty).
2. **The active sub-project** is session state, not model data. New elements go into it. The
   status bar shows "New elements → active sub-project: <name>".
3. **No lock.** A sub-project never refuses an edit. Who changes an element is decided by element
   borrowing until Synchronize (ADR 0038). This replaces the "a layer can be borrowed" bullet of
   ADR 0038 point 5.
4. **Views.** A view stores the sub-projects it hides (by GlobalId), like hide in a view (ADR 0044).
   A new view starts from each sub-project's "visible by default".
5. **Loading.** "Open on load" off: the open skips that sub-project's elements, as ADR 0033's cut.
   The skipped elements stay in the file and in history. Edits never touch them.
6. **The page** (Collaborate → Sub-projects): a table with Name, Elements, Visible by default,
   Open on load, Current borrows, Active. Buttons: New sub-project, Rename, Move selection to….
   The properties panel shows the selected sub-project's count, classes and creator.
   Changes apply at once (ADR 0052).
7. **Linked models** (the "Architecture (linked)" row) appear in the table read-only. They are
   not sub-projects of this model.

## API needed

- Commands: `AddSubproject { id, name, visible_by_default, open_on_load }`,
  `SetSubproject { … }`, `RemoveSubproject { id }` (refused while elements use it: `InUse`),
  `SetElementSubproject { ids, subproject }`.
- Verbs: `subproject_new {name}`, `subproject_set {id, name?, visible_by_default?, open_on_load?}`,
  `subproject_remove {id}`, `subproject_move {ids: [..], subproject}`,
  `subproject_activate {id}` (session, no Command).
- Queries: `subprojects` → `[{id, name, elements, classes, visible_by_default, open_on_load,
  borrowed_by: [user], active}]`. `elements` gains a `subproject` filter.
- IFC: no standard place for worksets. Export each sub-project as an `IfcGroup` with
  `IfcRelAssignsToGroup`, and the flags in a Bimble property set `Bimble_Subproject`
  (superset, ADR 0003). Import reads them back when present.

## Alternatives considered

- Layers that lock, as ADR 0038 point 5 proposed: two locking systems (layer and element) to
  explain, and a lock on a large group blocks the whole team on one file.
- No sub-projects, only hide by class or storey: no way to skip "Rebar" (2 310 elements in the
  wireframe) on a weak machine with one click.
- IFC presentation layers (`IfcPresentationLayerAssignment`): they carry representations, not
  elements, and other tools treat them as CAD layers.

## Evidence

- `docs/design/wireframes-v01/en/Subprojects.html`: the table above, with "What they do" and
  "What they don't do: they don't lock anything".
- `docs/adr/0038-collaborate-like-git.md:56-62`: layers with a borrow lock (not built, line 10).
- `crates/bimble-core/src/model.rs:502-528`: `Element` has no group field today.
- `crates/bimble-core/src/model.rs:547`: `Model` has no group map.

## Questions for Pierre

- Does "Open on load = off" persist per person (settings) or per model (shared)? The wireframe
  stores it in the model.
- Who has the right to create or rename a sub-project when rights exist (ADR 0038 point 4)?

## Revisit when

A team asks for a lock on a whole sub-project, or a server remote (ADR 0038) makes locks cheap.
