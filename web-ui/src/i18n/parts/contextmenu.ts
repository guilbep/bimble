// Copy for the viewport's right-click menu (ticket ISS71, issue #71; shell/contextItems.ts).
// {m}: Box to selection's margin in metres (contextItems.ts BOX_SELECTION_MARGIN_MM).
export const fr = {
  "ctx.menu": "Actions de la sélection",
  "ctx.boxToSelection": "Boîte sur la sélection",
  "ctx.boxToSelectionTip": "Réduit la zone de coupe autour des éléments sélectionnés, avec {m} m de marge (rien de sélectionné : tout le modèle)",
  "ctx.hide": "Masquer",
  "ctx.isolate": "Isoler",
};
export const en: Record<keyof typeof fr, string> = {
  "ctx.menu": "Selection actions",
  "ctx.boxToSelection": "Box to selection",
  "ctx.boxToSelectionTip": "Shrinks the section box around the selected elements, with a {m} m margin (nothing selected: the whole model)",
  "ctx.hide": "Hide",
  "ctx.isolate": "Isolate",
};
