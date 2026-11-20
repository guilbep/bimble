// Copy for the status bar while placing (B1). Rust's `query("placing").hint` is English only, so the hint is keyed
// on `kind` and the click step (0: first click, 1: next click). The reference shows only the slab's second click;
// the slab's first click and the column, beam and wall hints follow the same pattern.
export const fr = {
  "placing.chip": "Placement : {kind}",
  "placing.stop": "Arrêter le placement",
  "placing.kind.slab": "dalle",
  "placing.kind.column": "poteau",
  "placing.kind.beam": "poutre",
  "placing.kind.wall": "mur",
  "placing.slab.0": "Clic : premier coin de la dalle · Maj : sans accrochage · Échap : arrêter",
  "placing.slab.1": "Clic : second coin de la dalle · Maj : sans accrochage · Échap : arrêter",
  "placing.column.0": "Clic : position du poteau · Maj : sans accrochage · Échap : arrêter",
  "placing.column.1": "Clic : position du poteau · Maj : sans accrochage · Échap : arrêter",
  "placing.beam.0": "Clic : début de la poutre · Maj : sans accrochage · Échap : arrêter",
  "placing.beam.1": "Clic : fin de la poutre · Maj : sans accrochage · Échap : arrêter",
  "placing.wall.0": "Clic : début du mur · Maj : sans accrochage · Échap : arrêter",
  "placing.wall.1": "Clic : fin du mur · Maj : sans accrochage · Échap : arrêter",
} as const;
export const en: Record<keyof typeof fr, string> = {
  "placing.chip": "Placing: {kind}",
  "placing.stop": "Stop placing",
  "placing.kind.slab": "slab",
  "placing.kind.column": "column",
  "placing.kind.beam": "beam",
  "placing.kind.wall": "wall",
  "placing.slab.0": "Click: first corner of the slab · Shift: no snapping · Esc: stop",
  "placing.slab.1": "Click: second corner of the slab · Shift: no snapping · Esc: stop",
  "placing.column.0": "Click: column position · Shift: no snapping · Esc: stop",
  "placing.column.1": "Click: column position · Shift: no snapping · Esc: stop",
  "placing.beam.0": "Click: start of the beam · Shift: no snapping · Esc: stop",
  "placing.beam.1": "Click: end of the beam · Shift: no snapping · Esc: stop",
  "placing.wall.0": "Click: start of the wall · Shift: no snapping · Esc: stop",
  "placing.wall.1": "Click: end of the wall · Shift: no snapping · Esc: stop",
};
