// Copy for the statusbar band. Keys are added here only (one file per component ticket).
export const fr = {
  "status.hint": "Glisser : orbite · Clic droit + glisser : panoramique · Molette : zoom · Poignées : zone de coupe",
  "status.unsaved": "Non enregistré",
  "status.saved": "Enregistré",
  "status.selected": "Sélection : {n}",
  "status.counts": "rév. {rev} · {n} sélectionnés",
  "status.langGroup": "Langue / Language",
  "status.langFr": "Français",
  "status.langEn": "English",
} as const;
export const en: Record<keyof typeof fr, string> = {
  "status.hint": "Drag: orbit · Right-drag: pan · Scroll: zoom · Handles: section box",
  "status.unsaved": "Not saved",
  "status.saved": "Saved",
  "status.selected": "Selection: {n}",
  "status.counts": "rev. {rev} · {n} selected",
  "status.langGroup": "Langue / Language",
  "status.langFr": "Français",
  "status.langEn": "English",
};
