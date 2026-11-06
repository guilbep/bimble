# ADR 0028 — Explicit state: chips, unsaved work, confirmations

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre ("think of reset, or implicit state, everything should be explicit");
  the design by Claude
- Reversal cost: low (one pure module and three places in the window)

## Context / Problem
Some of the viewer's state was only implied:
- **Modes**: measuring, placing, a sheet tool, the section box, edges, hatching or orthographic
  away from their default. Some showed only as a pressed button on a ribbon tab that may not be
  open.
- **Hidden things**: families, classes, storeys or links hidden in the view, with nothing on
  screen saying the view is incomplete.
- **Kept things**: measurements; plan options changed in the session.
- **Unsaved work**: model edits, an IFC's views, the graphic standards. The standards had no "not
  saved" state at all, and closing the window lost everything without a word.
- **Reset**: the standards' Reset threw every edit away at once, with no way back.

## Decision
- **One pure function**, from a snapshot of the viewer to its non-default states
  (`state::chips`) and to what isn't saved (`state::unsaved`). The default state says nothing.
- **Status bar chips**: every non-default state as a chip ("Measuring", "Edges off",
  "Hidden: 2 classes, 1 storey", "3 measurements", "Plan options changed"…). The × goes back to
  the default, and its tooltip says how ("Show everything in this view (saved with the view)").
- **Title row**: "Not saved: model, standards" in words, replacing a bare •.
- **Standards**: they show where they come from ("From office.json" or "Bimble's default") and
  whether they are "changed, not saved". **Revert** goes back to the file as loaded or last saved;
  **Reset to default…** asks first. Saving makes the current standards the new reference.
- **Plan options** are compared with the plan template last loaded; the chip's × goes back to it.
- **Closing with unsaved work** asks first: "Save and close" (closes only when everything got
  saved), "Close without saving", "Cancel". In a browser, leaving the page asks too
  (`beforeunload`).

## Alternatives considered
- Autosave: hides when things are written, and the standards would need a file anyway.
- A separate "state" panel: one more place to look; the status bar is already where the window
  says what is going on.

## Evidence
Tests: the default says nothing; each non-default state has its chip, label and way back; the
unsaved list. On the virtual display: chips for measuring, edges off and a hidden class, and
their ×; changing a standard → "changed, not saved" and "Not saved: model, standards"; Reset to
default asks, then the standards are saved; plan options chip appears and goes; closing with a
modified model asks, Cancel keeps the window, "Save and close" wrote the file and closed. The
browser's page-leave question is compiled, not tried in a browser yet.

## Revisit when
More kinds of state appear (filters, isolate, temporary hide): each gets its chip.
