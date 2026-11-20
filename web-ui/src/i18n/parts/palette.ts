// Copy for screen D8, Command palette (wireframes v01 fr/Palette.html, en/Palette.html).
// Command labels come from the ribbon copy (same strings as the reference); the rest from Rust (English).
export const fr = {
  "palette.title": "Palette de commandes",
  "palette.query": "Commande",
  "palette.goto": "Aller à : {name}",
  "palette.help": "↑ ↓ pour choisir · Entrée pour lancer · chaque outil, vue, feuille et métré est ici",
  "palette.hint": "Palette de commandes ouverte",
} as const;
export const en: Record<keyof typeof fr, string> = {
  "palette.title": "Command palette",
  "palette.query": "Command",
  "palette.goto": "Go to: {name}",
  "palette.help": "↑ ↓ to choose · Enter to run · every tool, view, sheet and quantities list is here",
  "palette.hint": "Command palette open",
};
