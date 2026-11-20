// Copy for Select and modify (B2): the Modifier band and the element properties. Keys are added here only.
export const fr = {
  "modify.title": "Modifier",
  "modify.one": "1 {cls} : {name}",
  "modify.many": "{n} éléments",
  "modify.transform": "Transformer",
  "modify.move": "Déplacer",
  "modify.copy": "Copier",
  "modify.rotate": "Rotation",
  "modify.mirror": "Miroir",
  "modify.geometry": "Géométrie",
  "modify.align": "Aligner",
  "modify.offset": "Décaler",
  "modify.trim": "Ajuster",
  "modify.split": "Scinder",
  "modify.visibility": "Visibilité",
  "modify.hide": "Masquer",
  "modify.isolate": "Isoler",
  "modify.vector": "Vecteur (mm)",
  "modify.moveBy": "Déplacer de ce vecteur",
  "modify.copyBy": "Copier de ce vecteur",
  "modify.delete": "Supprimer",
  "modify.error": "Erreur : {msg}",
  "modify.phase": "Créé à l'étape",
  "modify.originZ": "Origine Z",
  "modify.dim.width": "Largeur",
  "modify.dim.height": "Hauteur",
  "modify.dim.length": "Longueur",
  "modify.dim.depth": "Profondeur",
  "modify.dim.thickness": "Épaisseur",
  "modify.class.IfcBeam": "Poutre",
  "modify.class.IfcColumn": "Poteau",
  "modify.class.IfcWall": "Mur",
  "modify.class.IfcSlab": "Dalle",
  "modify.class.IfcFooting": "Semelle",
};
export const en: Record<keyof typeof fr, string> = {
  "modify.title": "Modify",
  "modify.one": "1 {cls}: {name}",
  "modify.many": "{n} elements",
  "modify.transform": "Transform",
  "modify.move": "Move",
  "modify.copy": "Copy",
  "modify.rotate": "Rotate",
  "modify.mirror": "Mirror",
  "modify.geometry": "Geometry",
  "modify.align": "Align",
  "modify.offset": "Offset",
  "modify.trim": "Trim",
  "modify.split": "Split",
  "modify.visibility": "Visibility",
  "modify.hide": "Hide",
  "modify.isolate": "Isolate",
  "modify.vector": "Vector (mm)",
  "modify.moveBy": "Move by this vector",
  "modify.copyBy": "Copy by this vector",
  "modify.delete": "Delete",
  "modify.error": "Error: {msg}",
  "modify.phase": "Phase created",
  "modify.originZ": "Origin Z",
  "modify.dim.width": "Width",
  "modify.dim.height": "Height",
  "modify.dim.length": "Length",
  "modify.dim.depth": "Depth",
  "modify.dim.thickness": "Thickness",
  "modify.class.IfcBeam": "Beam",
  "modify.class.IfcColumn": "Column",
  "modify.class.IfcWall": "Wall",
  "modify.class.IfcSlab": "Slab",
  "modify.class.IfcFooting": "Footing",
};

/** The class noun ("Poutre"), or the IFC class name when there is none. */
export function classNoun(cls: string, t: (k: string) => string): { noun: string; known: boolean } {
  const k = `modify.class.${cls}`;
  return k in fr ? { noun: t(k), known: true } : { noun: cls, known: false };
}

/** "−1 250" or "1'250,5" → -1250 / 1250.5; null when not a number. */
export function parseMm(s: string): number | null {
  const n = Number(s.replace(/[\s\u202f']/g, "").replace("\u2212", "-").replace(",", "."));
  return s.trim() && Number.isFinite(n) ? n : null;
}
