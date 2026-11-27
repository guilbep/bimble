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
registerOptimisation(setting("gpu_cull", {
  label: { fr: "Tri des copies sur la carte", en: "GPU culling of copies" }, kind: "toggle", default: false, off: false,
  hint: { fr: "Avec WebGPU : les copies d'une forme testées par un compute shader.", en: "With WebGPU: a shape's copies tested by a compute shader." },
}));

/** The report's lines: each row's value, and its default when it differs. */
export function optimisationLines(): string[] {
  return rows.map((o) => {
    const v = o.get(), on = o.applies ? o.applies() : true;
    return `  ${o.key}: ${String(v)}${v === o.default ? " (default)" : ` (default ${String(o.default)})`}${v === o.off ? ", off" : ""}${on ? "" : ", not served here"}`;
  });
}
