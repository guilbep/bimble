# ADR 0060 — Synchronize with per-element conflicts: keep mine or keep theirs

- Status: Proposed
- Decided by: not yet: proposed from the wireframes v01, for Pierre
- Reversal cost: low (a panel and a merge step, the remote format does not change)
- Extends: ADR 0038 (collaborate like git, conflicts Q4), ADR 0017 (history, field-level merge),
  ADR 0052 (changes apply at once)

## Context / Problem

ADR 0038 decided conflicts in words: a list grouped by element, keep mine or keep theirs per
element, "all mine" and "all theirs", a row frames its element in 3D. It is not built: synchronize
takes `--prefer ours|theirs` for the whole sync only. The wireframe C3 draws the panel. This ADR
turns it into data, commands and API.

## Decision

- Synchronize fetches and merges field by field (ADR 0017). Fields changed on one side only merge
  without a question ("14 changes merged without conflict").
- When two sides changed the same field of the same element differently, synchronize **stops**.
  Nothing is pushed. The Synchronize panel shows "Synchronize stopped: 2 conflicts. Nothing was
  sent; choose for each element."
- One row per element: class and name, who also changed it and when, "Show in 3D", "Keep mine",
  "Keep theirs", and under it each conflicting field with both values. The 3D view labels both
  values on the element ("Mine: 600 mm · Theirs (Ana): 650 mm").
- "Keep all mine" and "Keep all theirs" at the top. The choice is per element, not per field
  (ADR 0038 Q4, proposition 2).
- "Finish synchronizing" stays disabled until every element has a choice ("Choose for 1 more
  element"). It then commits the merge and pushes.
- The pending merge is state, not a dialog: the panel survives closing it, the status bar shows
  "Conflicts: 2", and the model stays at my version until finish. Choosing applies at once
  (ADR 0052) and undo reverses a choice.
- The panel also shows the shared work summary: folder, you and your right, last sync, borrowed
  counts per person.

## Alternatives considered

- Per field (proposition 1 in ADR 0038): finer, but a slab with thickness mine and material
  theirs is a slab nobody designed. Rejected there, kept rejected.
- Both versions side by side in 3D: later, if the list is not enough (ADR 0038).
- Last writer wins: silent loss. Rejected.

## Evidence

- `docs/design/wireframes-v01/en/Conflicts.html`: two conflicts (IfcBeam B12 height 600/650,
  IfcSlab D3 thickness 250/280 and material C30/37/C35/45, both "also changed by Ana (AMA)"),
  "Keep all mine", "Keep all theirs", "Finish synchronizing", "Conflicts: 2" in the status bar.
- `docs/adr/0038-collaborate-like-git.md:3-9`: built: fetch, push, `--prefer ours|theirs`. Not
  built: conflict resolution per element. Lines 103-115: the scoring that chose per element.
- `crates/bimble-api/src/dispatch.rs:176-194`: queries today are storeys, materials, elements,
  project, views, sheets, schedules, element. No sync query.

## API needed

- No new model Command for a choice: "keep theirs" is the existing Commands that set the element
  to their version (`SetGenerator`, `SetPlacement`, `Rename`, `SetMaterial`…), computed by the
  merge. "Keep mine" is no Command. Both are recorded in the merge commit.
- Verbs: `sync_start` `{}` → `{merged: n, conflicts: [...]}`, `sync_choose`
  `{"id":"<GlobalId>","keep":"mine"|"theirs"}`, `sync_choose_all` `{"keep":"mine"|"theirs"}`,
  `sync_finish` `{}` (refused while a conflict has no choice), `sync_abort` `{}`.
- Query `sync` `{}` → `{state: idle|conflicts, folder, you, right, last_sync, borrowed: {who: n},
  conflicts: [{id, class, name, by, at, choice, fields: [{field, mine, theirs}]}]}`.
- IFC: none. Merge state lives in the history store (ADR 0017), not in the model.

## Revisit when

Users ask to keep both values, or to see both versions side by side in 3D.
