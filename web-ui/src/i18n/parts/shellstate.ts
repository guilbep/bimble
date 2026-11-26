// Copy for the shell's own states (ticket UX1, shell/machine.ts): a file being opened, the
// unsaved-changes question, an error with its way back. Keys are added here only.
export const fr = {
  "shellstate.opening.new": "Nouveau modèle…",
  "shellstate.opening.open": "Ouverture d'un fichier…",
  "shellstate.opening.import": "Import d'un IFC…",
  "shellstate.opening.close": "Fermeture du modèle…",
  "shellstate.cancel": "Annuler",
  "shellstate.confirm.title": "Modifications non enregistrées",
  "shellstate.confirm.text": "Le modèle ouvert a des modifications non enregistrées. Les abandonner ?",
  "shellstate.confirm.discard": "Abandonner les modifications",
  "shellstate.error.title": "Impossible",
  "shellstate.error.back": "Retour",
} as const;
export const en: Record<keyof typeof fr, string> = {
  "shellstate.opening.new": "New model…",
  "shellstate.opening.open": "Opening a file…",
  "shellstate.opening.import": "Importing an IFC…",
  "shellstate.opening.close": "Closing the model…",
  "shellstate.cancel": "Cancel",
  "shellstate.confirm.title": "Unsaved changes",
  "shellstate.confirm.text": "The open model has unsaved changes. Discard them?",
  "shellstate.confirm.discard": "Discard the changes",
  "shellstate.error.title": "Not possible",
  "shellstate.error.back": "Back",
};
