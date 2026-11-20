// Copy for shell gaps (status hint on a selection, panel chip, Collaborate left panel). Keys are added here only.
export const fr = {
  "shell.selectionHint": "Clic droit : actions · Ctrl+clic : ajouter à la sélection · Échap : vider",
  "shell.standardsOpen": "Standards ouverts",
  "shell.closePanel": "Fermer",
} as const;
export const en: Record<keyof typeof fr, string> = {
  "shell.selectionHint": "Right-click: actions · Ctrl+click: add to selection · Esc: clear",
  "shell.standardsOpen": "Standards open",
  "shell.closePanel": "Close",
};
