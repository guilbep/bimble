# ADR 0052 — Changes apply at once: no OK and no Cancel, undo instead

- Status: Accepted (20 Nov 2026)
- Decided by: Pierre, through the wireframes v01 (decision 4 of
  `docs/design/wireframes-v01/DESIGN-SPEC.md`), confirmed on 20 Nov.
- Reversal cost: low (dialog buttons)
- Extends: ADR 0030 (project information already works this way), ADR 0005 (undo through Commands)

## Context / Problem

OK and Cancel ask the user to decide twice: first in the field, then again at the end. Bimble already
applies project information at once, and every change is a Command that undo reverses.

## Decision

- In panels and dialogs of the Vue lab, leaving a field or pressing Enter applies the change as one
  Command. Ctrl+Z undoes it like any other change.
- A dialog closes with ×, Esc or a click outside. It has no OK and no Cancel.
- A change that the API refuses shows its error under the field, in the "Error" band (ADR 0049), and
  the model does not change.
- Actions that cannot be undone (deleting a file, publishing) still ask once, in words, before
  acting (ADR 0028).

## Alternatives considered

- OK and Cancel: familiar from Revit, but a second decision for every edit, and Cancel duplicates undo.

## Revisit when

Someone loses work through an applied change that undo cannot reach.
