# ADR 0030 — Project information, once, for every title block

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre (proposed a project information page like Revit's); design by Claude
- Reversal cost: low (one map in the model, the views file and history trees)

## Context / Problem
Title blocks need the client, the architect, the project number, the phase… Until now these
were `fields` of the plan template or of each sheet: the same values typed again for every
template and sheet, and nowhere to see them together.

## Decision
- **`Model.project`**: a map key → value (absent when empty, so older models keep their hash).
  Keys are like placeholders (letters, digits, `_`); bounded sizes. Changed by one command,
  `SetProject` (undo, history: committed in trees, merged key by key, shown by diff).
- For an **IFC**, it goes in the views file next to it (version 3 when present); the IFC is not
  changed.
- **Title blocks**: the project's values join the template's fields: `{client}`,
  `{project_number}`… The project wins; the template's fields fill what it leaves empty; a sheet's
  own fields (revision, drawn by…) and `--field` win over both.
- **The page** (Manage → Project information, or the palette) opens in the main view with its
  tab: sections Project, Client, Architect, Engineer, Contractor with offered keys (the key shown
  beside each field, ready to write in a template), and fields of one's own (Other: a name becomes
  a key, "Project manager" → `project_manager`). A value is stored when its field is left.
- **Command line**: `bimble project FILE` prints it, `bimble project FILE key=value …` sets
  entries (an empty value removes one).

## Alternatives considered
- Fixed struct fields: every office has its own list; a map with offered keys covers both.
- Reading IfcProject / IfcBuilding / IfcPostalAddress from IFC: possible later, as initial values.

## Evidence
Tests: offered keys are valid and unique; bad keys refused; labels to keys; committed, read back
and merged key by key (one side changes the phase, the other adds the number: both kept); the
title block placeholders take the project's values over the template's, and the template's fill
the gaps. On the virtual display: the page, typing a value, "Not saved: model". CLI: set, print,
remove.

## Revisit when
Offices want per-sheet overrides listed on the page, or values read from the IFC.
