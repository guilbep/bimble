// Copy for Plan on paper (D2): the view properties of a plan on paper.
// Strings from docs/design/wireframes-v01/{fr,en}/PlanOnPaper.html; "trueNorth" and "off" have no reference string.
export const fr = {
  "onpaper.orientation": "Orientation",
  "onpaper.projectNorth": "Nord projet",
  "onpaper.trueNorth": "Nord géographique",
  "onpaper.crop": "Recadrage",
  "onpaper.on": "Activé",
  "onpaper.off": "Désactivé",
  "onpaper.range": "Plage de vue (mm)",
  "onpaper.top": "Haut",
  "onpaper.cut": "Plan de coupe",
  "onpaper.bottom": "Bas",
  "onpaper.depth": "Profondeur",
  "onpaper.refs": "Plans de référence",
  "onpaper.display": "Affichage",
  "onpaper.hint": "Clic : premier point de la cote · Sur papier = ce qui s'imprime",
} as const;
export const en: Record<keyof typeof fr, string> = {
  "onpaper.orientation": "Orientation",
  "onpaper.projectNorth": "Project north",
  "onpaper.trueNorth": "True north",
  "onpaper.crop": "Crop",
  "onpaper.on": "On",
  "onpaper.off": "Off",
  "onpaper.range": "View range (mm)",
  "onpaper.top": "Top",
  "onpaper.cut": "Cut plane",
  "onpaper.bottom": "Bottom",
  "onpaper.depth": "View depth",
  "onpaper.refs": "Reference plans",
  "onpaper.display": "Display",
  "onpaper.hint": "Click: first point of the dimension · On paper = what prints",
};
