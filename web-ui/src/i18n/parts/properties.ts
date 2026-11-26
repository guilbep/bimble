// Copy for the properties panel's read-only line (ticket U1): a linked model's element, or a file open as it is.
// The file's wording follows the core's refusal ("this file is read only: import it to edit", viewer/lib.rs:65).
export const fr = {
  "props.linkedReadOnly": "Modèle lié, en lecture seule : {name}",
  "props.fileReadOnly": "Fichier en lecture seule : importez-le pour le modifier",
} as const;
export const en: Record<keyof typeof fr, string> = {
  "props.linkedReadOnly": "Linked model, read-only: {name}",
  "props.fileReadOnly": "Read-only file: import it to edit",
};
