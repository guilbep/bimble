# ADR 0051 — The window frame in the Vue lab: two top rows, 14 tabs in three groups

- Status: Accepted (20 Nov 2026)
- Decided by: Pierre, through the wireframes v01 (`docs/design/wireframes-v01/`, decisions 1 and 2
  of `DESIGN-SPEC.md`), confirmed on 20 Nov ("Accepted, from your design").
- Reversal cost: medium (the frame of every Vue screen, while tools and API underneath are unchanged)
- Amends: ADR 0027 and ADR 0037 for the Vue lab (`web-ui/`, ADR 0048). The egui window keeps its
  one top row until each screen is ported.

## Context / Problem

ADR 0037 put the File menu, the tabs and quick access on one row. The wireframes add trade tabs and
reach 14 tabs. They do not fit one row at 1 280 px.

## Decision

- Row 1, quick access, from left to right:
  - the mark, Home, File, Open, Save, Undo, Redo, Print and export, Synchronize
  - in the centre, the file name, its schema and the state chip
  - at the right, the command field (Ctrl K), the user's trigram, and the view's one primary action.
- Row 2, tabs, in three groups with a separator between them:
  - Projet, Collaborer, Implantation, Terrain
  - Béton, Armature, Acier, Bois
  - Annoter, Vue, Feuilles, Graphisme, ACV, Métré
  - English, in the same groups: Project, Collaborate, Setting out, Site · Concrete, Rebar, Steel,
    Timber · Annotate, View, Sheets, Graphics, LCA, Quantities.
- Trade tabs (Béton, Armature, Acier, Bois) hold the tools of each material. Terrain, ACV and Métré
  are tabs, not dialogs.
- Measurements: `docs/design/wireframes-v01/DESIGN-SPEC.md` § Window and the HTML of each screen.
- A tab or tool without its API verb is drawn disabled and listed as missing. It is never faked in
  TypeScript (ADR 0048).

## Alternatives considered

- One row as in ADR 0037: does not fit 14 tabs at 1 280 px.
- A Model tab holding all trades: hides the trade, which is how engineers think about tools.

## Revisit when

The tabs are used in the office: count the clicks between trades.
