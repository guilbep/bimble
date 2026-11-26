// Copy for the section box's orientation (ticket ISS71, issue #71): the actions box_north_project and
// box_north_true (palette, ribbon), the View tab's two-state control (shell/BoxNorth.vue).
export const fr = {
  "action.box_north_project": "Zone de coupe : nord du projet",
  "action.box_north_true": "Zone de coupe : nord géographique",
  "boxNorth.label": "Orientation",
  "boxNorth.project": "Projet",
  "boxNorth.true": "Géo",
  "boxNorth.projectTip": "Nord du projet : la zone de coupe suit les axes du bâtiment",
  "boxNorth.trueTip": "Nord géographique : la zone de coupe suit le nord de la carte",
  "boxNorth.off": "Le modèle n'a pas de géoréférence tournée : le nord géographique y est le nord du projet",
  "boxNorth.flat": "Disponible dans la vue 3D",
};
export const en: Record<keyof typeof fr, string> = {
  "action.box_north_project": "Section box: project north",
  "action.box_north_true": "Section box: true north",
  "boxNorth.label": "Orientation",
  "boxNorth.project": "Project",
  "boxNorth.true": "True",
  "boxNorth.projectTip": "Project north: the section box follows the building's axes",
  "boxNorth.trueTip": "True north: the section box follows the map's north",
  "boxNorth.off": "The model has no turned georeference: true north is project north there",
  "boxNorth.flat": "Available in the 3D view",
};
