# ADR 0037 — One top row with a File menu; a Project tab first

- Status: Accepted (9 Nov 2026)
- Decided by: the office (issue #51, items 1–4); details and mapping by Claude
- Reversal cost: low (the frame of the window; actions are unchanged)
- Amends: ADR 0027 (title row, ribbon tabs)

## Context / Problem
The office (#51) asked for: the title row and the tab strip shared as one row; a "File" menu like
Rhino's; icons instead of words for Save, Open, Undo, Redo; the first tab "Project infos" with
project information, global settings and link management; a second tab "Collaborate" (users,
roles, permissions, layers, sync).

## Decision
- **One top row**: Bimble, the File menu, the ribbon's tabs; at the right the file's name and what
  isn't saved (cut short when the row is full, whole on hover), Open, Save, Undo, Redo as icons
  with the same tips as before ("Undo: …", what is unsaved), the palette button.
- **File menu**: Open, Import IFC, Save · Link a model, Reference plan · On paper · Project
  information, Settings, History (desktop) · Exit (desktop). Each entry is an `Action`, so the menu,
  the ribbon and the palette cannot disagree; shortcuts shown. Rhino's other entries (New, Save
  As, Save Small, Incremental Save, templates, worksessions, recent files) are not there: Bimble
  has no such commands yet.
- **Tabs**: Project (project information; Settings; links: model, reference plan), Model (place,
  measure: kept, not replaced, as the tools need a tab), Annotate, View, Sheets, Manage (graphic
  standards, materials, history). The Links tab is folded into Project. "Global settings" is the
  existing Settings window (memory, frame rate) until project-wide settings exist.
- **Project information** offers the office's fields: postcode and city, phase as its SIA number,
  phase description, the engineer's e-mail and phone. `{phase}` keeps its key: title blocks
  using it are unchanged.
- **Collaborate is not built here**: users, roles, permissions, layers and sync are multi-user
  editing, a decision of its own (where the shared state lives, how edits merge, who may do
  what).

## Alternatives considered
- Keep two rows: more room, but the office found it wasteful.
- Icons with words in quick access: clearer for a first use, but the row must fit at 1280 px; the
  tips name each.

## Evidence
Tests: every action has an icon on the 24 × 24 grid (now Save, Undo and Redo too; failed before),
palette entries listed once. Width estimated, not seen: about 820 px of fixed content, the file
name truncates; to check by hand at 1280 px.

## Revisit when
Collaboration gets its ADR; project-wide settings exist; the office tries it.
