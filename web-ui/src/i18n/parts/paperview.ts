// Copy for a view On paper (ticket PAPER1, shell/PaperView.vue). Keys are added here only.
export const fr = {
  "paperview.label": "Vue sur papier : {title}",
  "paperview.failed": "Le dessin n’a pas abouti : {e}",
  "paperview.fit": "Ajuster",
} as const;
export const en: Record<keyof typeof fr, string> = {
  "paperview.label": "View on paper: {title}",
  "paperview.failed": "The drawing failed: {e}",
  "paperview.fit": "Fit",
};
