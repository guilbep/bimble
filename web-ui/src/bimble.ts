// The one typed client of the wasm viewer (ADR 0048). Components never touch the wasm directly.
import { shallowRef } from "vue";

export interface State {
  selection: string[];
  rev: number;
  unsaved: boolean;
  file: string | null;
}

/** `query("element")`: crates/bimble-api/src/query.rs ElementDetail (summary flattened). */
export interface ElementDetail {
  id: string;
  class: string;
  name: string;
  storey: string;
  material: string;
  generator: string;
  storey_name: string | null;
  material_name: string | null;
  placement: { origin: number[]; z_axis: number[]; x_axis: number[] };
  generator_params: Record<string, unknown>;
  openings: number;
  properties: Record<string, string>;
}

/** `query("actions")`: one ribbon action and its state now. */
export interface ActionState {
  name: string;
  label: string;
  tab: string;
  enabled: boolean;
  pressed: boolean;
  shortcut: string | null;
}

/** `query("history")`: top labels only, for now (bridge.rs). */
export interface History {
  undo: string[];
  redo: string[];
}

/** First 50 ids and the total. */
export interface Capped {
  ids: string[];
  count: number;
}

/** `query("visibility")`. Section box in mm relative to the scene origin. */
export interface Visibility {
  hidden_now: Capped;
  isolated_now: Capped;
  hidden_in_view: Capped;
  isolated_in_view: Capped;
  section_box: {
    on: boolean;
    box: { min: number[]; max: number[]; yaw: number } | null;
    origin_mm: number[] | null;
  };
}

export interface StoreySummary {
  id: string;
  name: string;
  /** mm */
  elevation: number;
  element_count: number;
}

export interface MaterialSummary {
  id: string;
  name: string;
  category: string;
  element_count: number;
}

export interface ProjectSummary {
  id: string;
  name: string;
  info: Record<string, string>;
}

/** `query("views")`: kind fields flattened (`type`: "three_d" | "plan" | "section", ...). */
export interface ViewSummary {
  id: string;
  name: string;
  type: string;
  scale: number;
  crop: unknown;
  true_north: boolean;
  references: number;
  annotations: number;
  [kindField: string]: unknown;
}

export interface SheetSummary {
  id: string;
  number: string;
  name: string;
  paper: string;
  portrait: boolean;
  fields: Record<string, string>;
  placed: unknown[];
  texts: number;
}

/** `query("schedules")`: definitions as stored (bimble-core ScheduleDef). */
export interface ScheduleDef {
  name: string;
  [field: string]: unknown;
}

/** `query("schedule_rows")`: text cells, at most 50 rows in all. */
export interface ScheduleTable {
  name: string;
  columns: string[];
  groups: { level: number; rows: string[][]; subtotal: string[] }[];
  total: string[] | null;
  count: number;
  truncated: boolean;
}

/** One open tab, as the bridge's `open_views` gives it. */
export type OpenTab =
  | { type: "view"; id: string }
  | { type: "sheet"; id: string }
  | { type: "schedule"; name: string }
  | { type: "project" }
  | { type: "materials" };
/** `query("open_views")`: the views the viewer shows, the active one and the open tabs in order. */
export interface OpenViews {
  views: ViewSummary[];
  active: string | null;
  tabs: OpenTab[];
}
/** @deprecated use `OpenViews`. */
export type OpenView = ViewSummary;

/** `query("verbs")` items and `query("describe")`: as bimble-api dispatch gives them. */
export type VerbInfo = Record<string, unknown>;

interface Wasm {
  default: () => Promise<unknown>;
  query(name: string, argsJson: string): string;
  apply(cmdsJson: string): void;
  undo(): void;
  redo(): void;
  select(idsJson: string): void;
  action(name: string): void;
  verb(name: string, argsJson: string): void;
  on_change(cb: (json: string) => void): void;
}

export const state = shallowRef<State>({ selection: [], rev: 0, unsaved: false, file: null });
/** Set when the viewer could not start. */
export const startError = shallowRef<string | null>(null);

// Wasm data: shallow refs, replaced as a whole after each change (never mutated in place).
export const actions = shallowRef<ActionState[]>([]);
export const historyLabels = shallowRef<History>({ undo: [], redo: [] });
export const visibility = shallowRef<Visibility | null>(null);
export const storeys = shallowRef<StoreySummary[]>([]);
export const materials = shallowRef<MaterialSummary[]>([]);
export const openViews = shallowRef<OpenViews>({ views: [], active: null, tabs: [] });
export const project = shallowRef<ProjectSummary | null>(null);

let resolveReady!: () => void;
/** Resolves once the viewer started; `start()` failing leaves it pending (see `startError`). */
export const ready: Promise<void> = new Promise((r) => (resolveReady = r));

// Served as is from public/bimble/ (npm run wasm), not bundled.
const WASM = "/bimble/";
let wasm: Wasm | null = null;

function wasmOrThrow(): Wasm {
  if (!wasm) throw new Error("Bimble is not started");
  return wasm;
}

/** Load and start the viewer. The canvas #bimble must be in the DOM. */
export async function start(): Promise<void> {
  try {
    // The viewer reads its flags from the address before init(); other flags (?url=) stay.
    const url = new URL(location.href);
    if (!url.searchParams.has("viewport")) {
      url.searchParams.set("viewport", "1");
      history.replaceState(null, "", url);
    }
    try {
      const bridge = (await import(/* @vite-ignore */ `${WASM}manifold-bridge.js`)) as { loadManifold: () => Promise<void> };
      await bridge.loadManifold();
    } catch (e) {
      console.warn("manifold not loaded:", e);
    }
    const w = (await import(/* @vite-ignore */ `${WASM}bimble_web.js`)) as Wasm;
    await w.default();
    // Timings from navigation start, read by Lighthouse's user-timings audit (make perf-web).
    performance.mark("bimble:wasm-ready");
    performance.measure("bimble:wasm-ready", { start: 0, end: "bimble:wasm-ready" });
    wasm = w;
    let shown = false;
    // The callback runs inside a frame: calls back into the viewer would throw "busy", so defer.
    w.on_change((json) => {
      const s = JSON.parse(json) as State;
      if (!shown && s.file !== null) {
        shown = true;
        performance.mark("bimble:model-shown");
        performance.measure("bimble:model-shown", { start: 0, end: "bimble:model-shown" });
      }
      queueMicrotask(() => {
        const prev = state.value;
        state.value = s;
        refresh(prev.rev !== s.rev || prev.file !== s.file);
      });
    });
    resolveReady();
  } catch (e) {
    startError.value = message(e);
    throw e;
  }
}

/** Rust errors arrive as strings (JsValue); keep their message. */
function message(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

export function element(id: string): ElementDetail {
  try {
    return JSON.parse(wasmOrThrow().query("element", JSON.stringify({ id }))) as ElementDetail;
  } catch (e) {
    throw new Error(message(e));
  }
}

/** Rename one element as one undoable step. Returns the Rust error, or null. */
export function rename(id: string, name: string): string | null {
  try {
    wasmOrThrow().apply(JSON.stringify([{ op: "rename", id, name }]));
    return null;
  } catch (e) {
    return message(e);
  }
}

export function undo(): string | null {
  try { wasmOrThrow().undo(); return null; } catch (e) { return message(e); }
}

export function redo(): string | null {
  try { wasmOrThrow().redo(); return null; } catch (e) { return message(e); }
}

/** A query that may fail (no model yet, query not merged yet): the fallback instead. */
function tryQuery<T>(name: string, fallback: T, args?: unknown): T {
  try {
    return query<T>(name, args);
  } catch {
    return fallback;
  }
}

let timed = false;
/** Refresh the shallow refs; model-derived lists only when the model changed. Runs deferred. */
function refresh(modelChanged: boolean): void {
  const t0 = performance.now();
  actions.value = tryQuery<ActionState[]>("actions", []);
  historyLabels.value = tryQuery<History>("history", { undo: [], redo: [] });
  visibility.value = tryQuery<Visibility | null>("visibility", null);
  openViews.value = tryQuery<OpenViews>("open_views", { views: [], active: null, tabs: [] });
  if (modelChanged) {
    storeys.value = tryQuery<StoreySummary[]>("storeys", []);
    materials.value = tryQuery<MaterialSummary[]>("materials", []);
    project.value = tryQuery<ProjectSummary | null>("project", null);
  }
  // The design check waits for this: a model is loaded and its first refresh is done.
  if (state.value.file !== null && actions.value.length > 0) document.documentElement.dataset.bbReady = "1";
  if (!timed && modelChanged && state.value.file !== null) {
    timed = true;
    console.debug(`bimble: refresh after change ${(performance.now() - t0).toFixed(2)} ms`);
  }
}

/** Typed query by name (bridge.rs, then bimble-api dispatch). Throws with the Rust message. */
export function query<T>(name: string, args?: unknown): T {
  try {
    return JSON.parse(wasmOrThrow().query(name, args === undefined ? "{}" : JSON.stringify(args))) as T;
  } catch (e) {
    throw new Error(message(e));
  }
}

/** Run one ribbon action by name (see `actions`). Returns the Rust error, or null. */
export function runAction(name: string): string | null {
  try { wasmOrThrow().action(name); return null; } catch (e) { return message(e); }
}

/** Run one API verb as one undoable step. Returns the Rust error, or null. */
export function runVerb(name: string, args: unknown = {}): string | null {
  try { wasmOrThrow().verb(name, JSON.stringify(args)); return null; } catch (e) { return message(e); }
}

/** Select by GlobalId, primary first; `[]` clears. Returns the Rust error, or null. */
export function select(ids: string[]): string | null {
  try { wasmOrThrow().select(JSON.stringify(ids)); return null; } catch (e) { return message(e); }
}

export const scheduleRows = (name: string) => query<ScheduleTable>("schedule_rows", { name });
export const views = () => query<ViewSummary[]>("views");
export const sheets = () => query<SheetSummary[]>("sheets");
export const schedules = () => query<ScheduleDef[]>("schedules");
/** Empty until the `schedule_list` query is merged. */
export const scheduleList = () => tryQuery<unknown[]>("schedule_list", []);
export const verbs = () => query<VerbInfo[]>("verbs");
export const describe = (name: string) => query<VerbInfo>("describe", { name });
