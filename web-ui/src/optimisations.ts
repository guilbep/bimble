// The optimisations the user can see and turn off (the owner's rule, 27 Nov 2026: "every optimisation is a user
// setting with a good default and an off value, shown in the panel"). One registry: the performance panel lists its
// rows (panels/Performance.vue, section "Optimisations") and the copied performance report prints their values
// (perf.ts). A ticket adds a row with `registerOptimisation` from its own module, without editing the panel.
// Each row reads and writes its value itself (a setting of the C core through setSettings, or a page value).
import { setSettings, userSettings, type SettingsValues } from "./bimble";

export interface OptChoice { value: string | number; label: { fr: string; en: string } }
export interface Optimisation {
  /** The setting's key (as the report prints it). */
  key: string;
  label: { fr: string; en: string };
  /** What it does, one line (the row's title). */
  hint?: { fr: string; en: string };
  kind: "toggle" | "choice" | "number";
  /** The value when nothing is set, and the value that turns it off. */
  default: boolean | string | number;
  off: boolean | string | number;
  choices?: OptChoice[];
  min?: number; max?: number; step?: number; unit?: string;
  /** The value now, and a change (returns an error, or null). */
  get: () => boolean | string | number;
  set: (v: boolean | string | number) => string | null;
  /** False: not served by this build (the row says so, disabled). */
  applies?: () => boolean;
}

const rows: Optimisation[] = [];
/** Adds (or replaces, by key) a row of the panel's Optimisations section. */
export function registerOptimisation(o: Optimisation): void {
  const i = rows.findIndex((r) => r.key === o.key);
  if (i >= 0) rows[i] = o;
  else rows.push(o);
}
export const optimisations = (): readonly Optimisation[] => rows;

/** A row backed by a setting of the C core (`query settings` / `set_settings`). */
const setting = <K extends keyof SettingsValues>(key: K, o: Omit<Optimisation, "key" | "get" | "set" | "applies">): Optimisation => ({
  key, ...o,
  get: () => (userSettings.value?.values[key] as boolean | string | number | undefined) ?? o.default,
  set: (v) => setSettings({ [key]: v } as Partial<SettingsValues>),
  applies: () => !!userSettings.value && userSettings.value.applies[key] !== false,
});

// ---- the rows that exist today (GPU2); VIS1 adds occlusion, the edges' mode, back-face culling, the pixel-size cut ----
registerOptimisation(setting("renderer", {
  label: { fr: "Rendu 3D", en: "3D renderer" }, kind: "choice", default: "auto", off: "webgl2",
  hint: { fr: "WebGPU dessine avec la carte graphique quand elle le permet ; WebGL2 partout.", en: "WebGPU draws with the graphics card where it can; WebGL2 everywhere." },
  choices: [{ value: "auto", label: { fr: "automatique", en: "automatic" } }, { value: "webgl2", label: { fr: "WebGL2", en: "WebGL2" } }, { value: "webgpu", label: { fr: "WebGPU", en: "WebGPU" } }],
}));
registerOptimisation(setting("moving_fps", {
  label: { fr: "Budget en mouvement", en: "Moving budget" }, kind: "choice", default: 30, off: 0, unit: "fps",
  hint: { fr: "En mouvement, les plus grands éléments d'abord pour tenir cette cadence ; 0 : tout, toujours.", en: "While moving, the largest elements first to hold this rate; 0: everything, always." },
  choices: [0, 15, 30, 60, 120, 240].map((f) => ({ value: f, label: { fr: f ? `${f} i/s` : "inactif", en: f ? `${f} fps` : "off" } })),
}));
registerOptimisation(setting("moving_proxies", {
  label: { fr: "Parties lointaines simplifiées", en: "Simplified far parts" }, kind: "toggle", default: true, off: false,
  hint: { fr: "Les cellules lointaines (et les liens) dessinées par leurs simplifications.", en: "Far cells (and links) drawn as their proxies." },
}));
registerOptimisation(setting("link_gpu_mb", {
  label: { fr: "Mémoire graphique des liens", en: "Links' GPU memory" }, kind: "number", default: 768, off: 65536, min: 64, max: 65536, step: 64, unit: "MB",
  hint: { fr: "Au-delà, les liens lointains rendent leur détail et sont dessinés simplifiés (65 536 : jamais).", en: "Past it, far links give their detail back and are drawn as proxies (65,536: never)." },
}));
registerOptimisation(setting("link_detail", { // TESS1 (ADR 0085): off is "exact" (arcs as Rust read them)
  label: { fr: "Détail des liens (arcs)", en: "Links' detail (arcs)" }, kind: "choice", default: "standard", off: "exact",
  hint: { fr: "La finesse des arcs à la conversion d'un lien ; « exact » : comme Rust les lisait.", en: "How finely a link's arcs are converted; \u201cexact\u201d: as Rust read them." },
  choices: [{ value: "exact", label: { fr: "exact", en: "exact" } }, { value: "standard", label: { fr: "standard", en: "standard" } },
    { value: "light", label: { fr: "léger", en: "light" } }, { value: "custom", label: { fr: "personnalisé", en: "custom" } }],
}));
registerOptimisation(setting("gpu_cull", {
  label: { fr: "Tri des copies sur la carte", en: "GPU culling of copies" }, kind: "toggle", default: false, off: false,
  hint: { fr: "Avec WebGPU : les copies d'une forme testées par un compute shader.", en: "With WebGPU: a shape's copies tested by a compute shader." },
}));

// ---- VIS1 (ADR 0078 amendment 7): the 3D view's own; WebGL2 only for now (the WebGPU renderer draws everything, with
// lines, and completes its picture on screen: ADR 0078 amendment 7, "What the WebGPU renderer needs") ----
registerOptimisation(setting("draw_visible", {
  label: { fr: "Dessiner seulement ce qui se voit", en: "Draw only what can be seen" }, kind: "toggle", default: false, off: false,
  hint: { fr: "WebGL2 : une image au repos laisse de côté ce que l'image précédente montre caché (désactivé par défaut : en vérification sur de grands projets de liens).",
    en: "WebGL2: a still picture leaves out what the previous one shows hidden (off by default: being checked on large projects of links)." },
}));
registerOptimisation(setting("edge_mode", {
  label: { fr: "Arêtes en 3D", en: "Edges in 3D" }, kind: "choice", default: "screen", off: "lines",
  hint: { fr: "WebGL2 : trouvées à l'écran (une passe) ; « lignes » : chaque arête de chaque élément (exact, 2 à 3 fois plus lent).",
    en: "WebGL2: found on screen (one pass); \u201clines\u201d: every element's edges (exact, 2 to 3 times slower)." },
  choices: [{ value: "screen", label: { fr: "à l'écran", en: "on screen" } }, { value: "lines", label: { fr: "lignes", en: "lines" } }, { value: "none", label: { fr: "aucune", en: "none" } }],
}));
registerOptimisation(setting("cull_back", {
  label: { fr: "Faces arrière omises", en: "Back faces skipped" }, kind: "toggle", default: false, off: false,
  hint: { fr: "WebGL2 : exact seulement pour des solides fermés ; désactivé par défaut.", en: "WebGL2: exact only for closed solids; off by default." },
}));
registerOptimisation(setting("still_px", {
  label: { fr: "Seuil en pixels à l'arrêt", en: "Still pixel cut" }, kind: "number", default: 0, off: 0, min: 0, max: 8, step: 0.5, unit: "px",
  hint: { fr: "WebGL2 : à l'arrêt, laisser de côté ce qui fait moins de ce nombre de pixels (0 : rien).", en: "WebGL2: when still, leave out what is smaller than this many pixels (0: nothing)." },
}));
registerOptimisation(setting("refill_offscreen", {
  label: { fr: "Compléter hors écran", en: "Complete offscreen" }, kind: "toggle", default: true, off: false,
  hint: { fr: "WebGL2 : après un mouvement, l'image est complétée hors écran et montrée entière.", en: "WebGL2: after a move the picture is completed offscreen and shown whole." },
}));

/** The report's lines: each row's value, and its default when it differs. */
export function optimisationLines(): string[] {
  return rows.map((o) => {
    const v = o.get(), on = o.applies ? o.applies() : true;
    return `  ${o.key}: ${String(v)}${v === o.default ? " (default)" : ` (default ${String(o.default)})`}${v === o.off ? ", off" : ""}${on ? "" : ", not served here"}`;
  });
}
