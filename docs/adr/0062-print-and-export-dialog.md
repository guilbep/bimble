# ADR 0062 — Print and export: one dialog for sheets, an "Export" action button, no OK

- Status: Proposed
- Decided by: not yet: proposed from the wireframes v01, for Pierre
- Reversal cost: low (a dialog over existing exporters)
- Extends: ADR 0015 (plans at scale), ADR 0023 (paper drawings), ADR 0024 (sheets), ADR 0047 (IFC
  exports), ADR 0051 (quick access), ADR 0052 (no OK and no Cancel)

## Context / Problem

Sheets export one at a time today, from the File menu. An office sends a set: the plans of
revision B, named the office way, in one go. The quick-access row has a Print and export button
(ADR 0051) with no screen behind it. The wireframe D4 draws the dialog.

ADR 0052 removes OK and Cancel: changes apply at once. An export is not a change to the model.
It is an action that writes files, and it needs a button that starts it. The wireframe shows
"Cancel" and "Export 3 sheets". This ADR resolves the two.

## Decision

- **One action button, no Cancel.** The dialog has the button "Export N sheets" (the count is the
  selection). There is no OK and no Cancel: ×, Esc or a click outside closes it, as ADR 0052 says.
  The settings (format, files, names, pens, colour, folder) apply at once and are remembered per
  project. "Cancel" in the wireframe becomes "stop" on a running export only.
- The dialog lists the sheets with number, name, paper size and revision, and quick filters "All"
  and "Revision B only". The selection starts from the sheets selected in the navigator.
- Formats: PDF and SVG (ADR 0023, 0024), DXF later, and Printer (the system print dialog with the
  PDF). Files: one file per sheet or one PDF. File names: a pattern with fields, shown on an
  example ("P0001-N01-203-B-Dalle sur N01.pdf"). Pens: the office standards. Colour or black and
  white. A folder.
- The export runs in the background. The window stays usable. The status bar shows progress. A
  file that exists is overwritten after one question, in words (ADR 0052, irreversible).
- The quick-access Print and export button and Sheets → Print and export… open this dialog. IFC
  and IFC2x3 exports (ADR 0047) stay in the File menu: they export a model, not sheets.

## Alternatives considered

- Keep "Cancel" and "Export": Cancel then means "close", which × already does. Against ADR 0052.
- Export on every settings change: writes files nobody asked for. Rejected.
- IFC in the same dialog: one place for every export, but a model is not a set of sheets. Revisit
  if users look for it here.

## Evidence

- `docs/design/wireframes-v01/en/Export.html`: "Sheets (3 of 9)", "All", "Revision B only",
  Format PDF/SVG/DXF/Printer, "One file per sheet / One PDF", File names with an example, Pens
  "Office standards (office.json)", Colour "Black and white", "Drawn in the background; the window
  stays usable", buttons "Cancel" and "Export 3 sheets".
- `docs/design/wireframes-v01/API-MAP.md:19,29`: Print and export is missing in the API. Sheets
  have `AddSheet`, `SetSheet` only.
- `crates/bimble-core/src/command.rs:41-46`: `AddSheet`, `RemoveSheet`, `SetSheet`, no export
  Command, and none needed: export does not change the model.
- `docs/adr/0052-changes-apply-at-once.md`: no OK, no Cancel, irreversible actions ask once.

## API needed

- No Command (the model does not change). The export settings are saved in the project as view
  state: `SetProject` gains an `export` field `{format, files, name_pattern, pens, colour, folder}`,
  or the settings live in the user settings. Pierre chooses.
- Verb `export_sheets` `{"sheets":["<GlobalId>"],"format":"pdf"|"svg"|"dxf"|"print",
  "files":"per_sheet"|"one","name_pattern":"{project}-{storey}-{number}-{rev}-{name}",
  "pens":"office","colour":"bw"|"colour","folder":"…"}` → a job id. `export_cancel` `{"job":id}`.
- Queries: `export_preview_name` `{"sheet":"<GlobalId>","name_pattern":"…"}` → file name,
  `jobs` `{}` → progress.
- IFC: none for sheets. Revision lives on the sheet (`IfcDocumentInformation.Revision` in the
  export, ADR 0024 items).

## Revisit when

DXF export exists, or offices ask to export IFC from the same dialog.
