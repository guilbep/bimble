// The one typed client of the wasm viewer (ADR 0048). Components never touch the wasm directly.
import { shallowRef } from "vue";

export interface State {
  selection: string[];
  rev: number;
  unsaved: boolean;
  file: string | null;
  /** on_change: a signature of what the view shows (section box, hidden/isolated, On paper, tabs). */
  view?: number[];
  /** on_change: the current tab, null on Home. */
  tab?: OpenTab | null;
  /** `query("state")`: ".bimble", "IFC2X3", "IFC4"…, or null. */
  schema?: string | null;
  /** `query("state")`: Settings → You, null when no name is set. */
  user?: { name: string; trigram: string; email: string | null } | null;
  /** `query("state")`: the viewer's version, e.g. "0.17.0". */
  version?: string | null;
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
  /** IfcSite and IfcBuilding GlobalIds, as the IFC export writes them. */
  site_id: string;
  building_id: string;
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
  /** `open_views` only: set while the default name is kept, so the page says it in its language. */
  auto_name?: { kind: "3d" | "plan" | "ceiling"; storey: string | null } | null;
  [kindField: string]: unknown;
}

/** `query("pointer")`: where the pointer is over the model; null off the model. */
export interface Pointer {
  world_mm: number[];
  text: string;
}

/** `query("status")`: the status bar's counts; null without a model. */
export interface Status {
  visible: number;
  total: number;
  triangles: number;
  load_ms: number;
  model_bytes: number;
  /** 0: not known on this platform. */
  app_bytes: number;
  fps: number | null;
  idle: boolean;
  text: string;
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
  /** `ids`: one GlobalId (or null) per row, same order as `rows`. */
  groups: { level: number; rows: string[][]; ids: (string | null)[]; subtotal: string[] }[];
  total: string[] | null;
  count: number;
  truncated: boolean;
}

/** `query("placing")`: what is placed (edit.rs `Editor::placing_json`). Sizes and offset in mm. */
export interface Placing {
  active: boolean;
  kind: "column" | "beam" | "wall" | "slab";
  storey: string | null;
  storey_name: string | null;
  material: string | null;
  sizes: { column: number; beam_width: number; beam_height: number; wall: number; slab: number };
  offset_mm: number[];
  /** A new slab's top relative to the level above, mm. */
  top_offset_mm: number;
  /** A new slab's shape: two corners, or a clicked outline (Enter or a click on the first point closes it). */
  shape: "rectangle" | "polygon";
  points: number[][];
  clicks_needed: number;
  hint: string | null;
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
  /** What the main view shows: one of `tabs`, or null (Home). */
  current: OpenTab | null;
  /** Model / On paper switch: offered on a plan or section. */
  paper: { offered: boolean; on: boolean };
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
  tab(json: string): void;
  on_change(cb: (json: string) => void): void;
  set_placing(json: string): void;
  dismiss_belongs(): void;
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
const NO_VIEWS: OpenViews = { views: [], active: null, tabs: [], current: null, paper: { offered: false, on: false } };
export const openViews = shallowRef<OpenViews>(NO_VIEWS);
export const project = shallowRef<ProjectSummary | null>(null);
/** Null without an editable model. */
export const placing = shallowRef<Placing | null>(null);
/** Not pushed by on_change: polled while the pointer is over the viewport (`watchPointer`). */
export const pointer = shallowRef<Pointer | null>(null);
/** Refreshed on each change, and every second while the viewer draws (not idle). */
export const status = shallowRef<Status | null>(null);

let resolveReady!: () => void;
/** Resolves once the viewer started; `start()` failing leaves it pending (see `startError`). */
export const ready: Promise<void> = new Promise((r) => (resolveReady = r));
let resolveModel!: () => void;
/** Resolves after the first refresh with a model loaded. */
export const modelReady: Promise<void> = new Promise((r) => (resolveModel = r));

// Served as is from public/bimble/ (npm run wasm), not bundled. Relative to the page, so the lab
// also runs under a sub-path (GitHub Pages serves it at /bimble/).
const WASM = new URL("bimble/", document.baseURI).href;
let wasm: Wasm | null = null;

function wasmOrThrow(): Wasm {
  if (!wasm) throw new Error("Bimble is not started");
  return wasm;
}

// Native twin, experiment 2 (ADR 0065 follow-up). Dev only: with `?native=1&port=<p>&token=<t>`
// the lab talks to a native Bimble process on 127.0.0.1 instead of the wasm module.
// Protocol:
// - A call is `POST http://127.0.0.1:<port>/<op>/<name>?t=<token>`, `Content-Type: text/plain`,
//   body = the JSON args string (or empty). A CORS simple request: no preflight.
//   <op>: query, apply, undo, redo, select, action, verb, tab, set_placing, dismiss_belongs,
//   standards_action. <name>: the name argument of query, verb, action, standards_action, else `-`.
// - 200: the text body is what wasm returns (empty for void). Any other status: wasm would throw;
//   the body is the error text, thrown as a string (as wasm does).
// - Calls are synchronous (sync XMLHttpRequest): every Wasm function but default() is sync.
// - Changes: long-poll `GET /change?since=<seq>&t=<token>` -> `{"seq":n,"change":<on_change JSON>}`,
//   then poll again with since=n. On a network error, wait 1 s and retry.

function nativeParams(p: URLSearchParams): { port: number; token: string } {
  const port = Number(p.get("port"));
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error("native=1 needs a valid port=<p>");
  return { port, token: p.get("token") ?? "" };
}

/** A `Wasm` on top of synchronous XHR to a native Bimble process (dev only, see above). */
function nativeClient(port: number, token: string): Wasm & { standards_action(name: string): void } {
  const base = `http://127.0.0.1:${port}`;
  const t = encodeURIComponent(token);
  const call = (op: string, name: string | null, body?: string): string => {
    const xhr = new XMLHttpRequest();
    xhr.open("POST", `${base}/${op}/${name === null ? "-" : encodeURIComponent(name)}?t=${t}`, false);
    xhr.setRequestHeader("Content-Type", "text/plain");
    try {
      xhr.send(body ?? "");
    } catch (e) {
      throw `native ${op}: ${message(e)}`;
    }
    if (xhr.status !== 200) throw xhr.responseText || `native ${op}: HTTP ${xhr.status}`;
    return xhr.responseText;
  };
  return {
    default: async () => undefined,
    query: (name, argsJson) => call("query", name, argsJson),
    apply: (cmdsJson) => void call("apply", null, cmdsJson),
    undo: () => void call("undo", null),
    redo: () => void call("redo", null),
    select: (idsJson) => void call("select", null, idsJson),
    action: (name) => void call("action", name),
    verb: (name, argsJson) => void call("verb", name, argsJson),
    tab: (json) => void call("tab", null, json),
    set_placing: (json) => void call("set_placing", null, json),
    dismiss_belongs: () => void call("dismiss_belongs", null),
    standards_action: (name) => void call("standards_action", name),
    on_change: (cb) => void pollChanges(base, t, cb),
  };
}

/** Long-poll loop for the native client; never rejects, retries every 1 s on error. */
async function pollChanges(base: string, t: string, cb: (json: string) => void): Promise<void> {
  let since = 0;
  let epoch: string | undefined;
  for (;;) {
    try {
      // The epoch lets a restarted twin answer at once, even when its seq equals ours.
      const e = epoch === undefined ? "" : `&epoch=${encodeURIComponent(epoch)}`;
      const r = await fetch(`${base}/change?since=${since}${e}&t=${t}`);
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const m = (await r.json()) as { epoch?: string; seq: number; change: unknown };
      // A new epoch is a restarted twin: its seq began again, so ask for its current change.
      if (typeof m.epoch === "string" && m.epoch !== epoch) {
        const restarted = epoch !== undefined;
        epoch = m.epoch;
        if (restarted) {
          since = 0;
          continue;
        }
      }
      if (typeof m.seq === "number") since = m.seq;
      if (m.change !== undefined && m.change !== null) cb(JSON.stringify(m.change));
    } catch (e) {
      console.warn("native change poll:", e);
      await new Promise((res) => setTimeout(res, 1000));
    }
  }
}

interface ViewportSample {
  latency_ms: number | null;
  changed: boolean;
  render_ms: number | null;
  read_ms: number | null;
  fetch_ms: number;
}
interface ViewportStats {
  frames: number;
  samples: ViewportSample[];
}
declare global {
  interface Window {
    __bbViewport?: ViewportStats;
  }
}

/** Spike (native mode only): draw RGBA frames streamed from the native twin into #bimble and send
 *  orbit, pan and zoom back with async fetch. Returns a function that marks the view dirty. */
function streamViewport(base: string, token: string): () => void {
  const canvas = document.getElementById("bimble");
  const ctx = canvas instanceof HTMLCanvasElement ? canvas.getContext("2d") : null;
  if (!(canvas instanceof HTMLCanvasElement) || !ctx) return () => undefined;
  const t = encodeURIComponent(token);
  const stats: ViewportStats = { frames: 0, samples: [] };
  window.__bbViewport = stats;
  let dirty = true;
  let inFlight = false;
  let backoffUntil = 0;
  let orbit = { dx: 0, dy: 0 };
  let pan = { dx: 0, dy: 0 };
  let scroll = 0;
  let oldestInput: number | null = null;
  let lastSum: number | null = null;
  let drag: { pan: boolean; x: number; y: number } | null = null;
  const note = (ts: number) => { if (oldestInput === null) oldestInput = ts; };
  const clamp = (v: number) => Math.min(4096, Math.max(16, Math.round(v)));
  new ResizeObserver(() => {
    const dpr = window.devicePixelRatio || 1;
    canvas.width = clamp(canvas.clientWidth * dpr);
    canvas.height = clamp(canvas.clientHeight * dpr);
    dirty = true;
  }).observe(canvas);
  canvas.addEventListener("pointerdown", (e) => {
    canvas.setPointerCapture(e.pointerId);
    drag = { pan: e.button === 1 || e.button === 2 || (e.button === 0 && e.shiftKey), x: e.clientX, y: e.clientY };
  });
  canvas.addEventListener("pointermove", (e) => {
    if (!drag) return;
    const dpr = window.devicePixelRatio || 1;
    const dx = (e.clientX - drag.x) * dpr;
    const dy = (e.clientY - drag.y) * dpr;
    drag.x = e.clientX;
    drag.y = e.clientY;
    if (dx === 0 && dy === 0) return;
    const acc = drag.pan ? pan : orbit;
    acc.dx += dx;
    acc.dy += dy;
    note(e.timeStamp);
  });
  const release = (e: PointerEvent) => {
    drag = null;
    if (canvas.hasPointerCapture(e.pointerId)) canvas.releasePointerCapture(e.pointerId);
  };
  canvas.addEventListener("pointerup", release);
  canvas.addEventListener("pointercancel", release);
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());
  canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    scroll += e.deltaY;
    note(e.timeStamp);
  }, { passive: false });
  const post = async (op: string, body: object): Promise<void> => {
    const r = await fetch(`${base}/${op}/-?t=${t}`, { method: "POST", headers: { "Content-Type": "text/plain" }, body: JSON.stringify(body) });
    if (!r.ok) throw new Error(`${op}: HTTP ${r.status}`);
  };
  const timing = (h: string | null, key: string): number | null => {
    const m = h?.match(new RegExp(`(?:^|;)\\s*${key}=([0-9.]+)`));
    return m ? Number(m[1]) : null;
  };
  const step = async (): Promise<void> => {
    const served = oldestInput;
    oldestInput = null;
    const o = orbit, p = pan, s = scroll;
    orbit = { dx: 0, dy: 0 };
    pan = { dx: 0, dy: 0 };
    scroll = 0;
    dirty = false;
    const h = canvas.height, w = canvas.width;
    try {
      if (o.dx !== 0 || o.dy !== 0) await post("orbit", o);
      if (p.dx !== 0 || p.dy !== 0) await post("pan", { dx: p.dx / h, dy: p.dy / h });
      if (s !== 0) await post("zoom", { scroll: s });
      const f0 = performance.now();
      const r = await fetch(`${base}/frame?w=${w}&h=${h}&t=${t}`, { cache: "no-store" });
      if (!r.ok) throw new Error(`frame: HTTP ${r.status}`);
      const buf = await r.arrayBuffer();
      const fetch_ms = performance.now() - f0;
      if (buf.byteLength !== w * h * 4) return; // Resized meanwhile: skip; the observer set dirty.
      const px = new Uint8ClampedArray(buf);
      ctx.putImageData(new ImageData(px, w, h), 0, 0);
      const latency_ms = served === null ? null : performance.now() - served;
      let sum = 0;
      for (let i = 0; i < px.length; i += 997) sum = (sum * 31 + (px[i] ?? 0)) >>> 0;
      const changed = lastSum !== sum;
      lastSum = sum;
      const xt = r.headers.get("X-Timing");
      stats.frames++;
      stats.samples.push({ latency_ms, changed, render_ms: timing(xt, "render"), read_ms: timing(xt, "read"), fetch_ms });
      if (stats.samples.length > 1000) stats.samples.splice(0, stats.samples.length - 1000);
    } catch (e) {
      console.warn("native viewport:", e);
      dirty = true; // The twin may be restarting: retry the frame after a back-off.
      if (served !== null && oldestInput === null) oldestInput = served;
      backoffUntil = performance.now() + 500;
    }
  };
  const loop = (now: number) => {
    const pending = orbit.dx !== 0 || orbit.dy !== 0 || pan.dx !== 0 || pan.dy !== 0 || scroll !== 0;
    if ((dirty || pending) && !inFlight && now >= backoffUntil) {
      inFlight = true;
      void step().finally(() => { inFlight = false; });
    }
    requestAnimationFrame(loop);
  };
  requestAnimationFrame(loop);
  return () => { dirty = true; };
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
    let w: Wasm;
    let viewportDirty: (() => void) | null = null;
    const native = url.searchParams.get("native") === "1" ? nativeParams(url.searchParams) : null;
    if (native) {
      // Dev only: no manifold, no wasm; on_change below starts the long-poll loop.
      w = nativeClient(native.port, native.token);
      viewportDirty = streamViewport(`http://127.0.0.1:${native.port}`, native.token);
    } else {
      try {
        const bridge = (await import(/* @vite-ignore */ `${WASM}manifold-bridge.js`)) as { loadManifold: () => Promise<void> };
        await bridge.loadManifold();
      } catch (e) {
        console.warn("manifold not loaded:", e);
      }
      w = (await import(/* @vite-ignore */ `${WASM}bimble_web.js`)) as Wasm;
      await w.default();
    }
    // Timings from navigation start, read by Lighthouse's user-timings audit (make perf-web).
    performance.mark("bimble:wasm-ready");
    performance.measure("bimble:wasm-ready", { start: 0, end: "bimble:wasm-ready" });
    wasm = w;
    let shown = false;
    // The callback runs inside a frame: calls back into the viewer would throw "busy", so defer.
    w.on_change((json) => {
      viewportDirty?.();
      const s = JSON.parse(json) as State;
      if (!shown && s.file !== null) {
        shown = true;
        performance.mark("bimble:model-shown");
        performance.measure("bimble:model-shown", { start: 0, end: "bimble:model-shown" });
      }
      queueMicrotask(() => {
        const prev = state.value;
        // schema and user are not in on_change: read them from query("state").
        const q = tryQuery<Pick<State, "schema" | "user" | "version">>("state", {});
        state.value = { ...s, schema: q.schema ?? null, user: q.user ?? null, version: q.version ?? null };
        refresh(prev.rev !== s.rev || prev.file !== s.file);
      });
    });
    watchPointer();
    labelTextAgent();
    // ponytail: one 1 s timer for the whole session; it queries only while the viewer draws.
    setInterval(() => { if (status.value && !status.value.idle) status.value = tryQuery<Status | null>("status", null); }, 1000);
    resolveReady();
  } catch (e) {
    startError.value = message(e);
    throw e;
  }
}

/** eframe 0.37 inserts a hidden `<input type=text>` (its IME text agent) right after the canvas,
 *  with no name and no option to set one; it takes focus, so it needs a label, not aria-hidden.
 *  ponytail: polls 50 ms until eframe inserted it; stops after 10 s. */
function labelTextAgent(): void {
  let tries = 0;
  const id = window.setInterval(() => {
    const el = document.querySelector<HTMLInputElement>("#bimble ~ input:not([aria-label])");
    if (el) el.setAttribute("aria-label", document.documentElement.lang === "fr" ? "Saisie clavier de la vue" : "Viewer keyboard input");
    if (el || ++tries > 200) clearInterval(id);
  }, 50);
}

/** Poll `query("pointer")` every 100 ms while the pointer is over #bimble, on an animation frame
 *  (never inside on_change, whose callback runs in the viewer's frame). */
function watchPointer(): void {
  const canvas = document.getElementById("bimble");
  if (!canvas) return;
  let timer = 0;
  let queued = false;
  const poll = () => {
    if (queued) return;
    queued = true;
    requestAnimationFrame(() => { queued = false; if (timer) pointer.value = tryQuery<Pointer | null>("pointer", null); });
  };
  canvas.addEventListener("mouseenter", () => { if (!timer) timer = window.setInterval(poll, 100); });
  canvas.addEventListener("mouseleave", () => { clearInterval(timer); timer = 0; pointer.value = null; });
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

/** Replace the project information (ADR 0030) as one undoable step: the whole map. Returns the Rust error, or null. */
export function setProject(project: Record<string, string>): string | null {
  try {
    wasmOrThrow().apply(JSON.stringify([{ op: "set_project", project }]));
    return null;
  } catch (e) {
    return message(e);
  }
}

/** Change what is placed: `{kind?, storey?, material?, sizes?}`, all or nothing. Returns the Rust error, or null. */
export function setPlacing(patch: { kind?: Placing["kind"]; storey?: string; material?: string; sizes?: Partial<Placing["sizes"]>; top_offset_mm?: number; shape?: Placing["shape"] }): string | null {
  try {
    wasmOrThrow().set_placing(JSON.stringify(patch));
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
  openViews.value = tryQuery<OpenViews>("open_views", NO_VIEWS);
  status.value = tryQuery<Status | null>("status", null);
  placing.value = tryQuery<Placing | null>("placing", null);
  if (modelChanged) {
    storeys.value = tryQuery<StoreySummary[]>("storeys", []);
    materials.value = tryQuery<MaterialSummary[]>("materials", []);
    project.value = tryQuery<ProjectSummary | null>("project", null);
  }
  // A model is loaded and its first refresh is done (deeplink.ts then sets data-bb-ready for the design check).
  if (state.value.file !== null && actions.value.length > 0) resolveModel();
  if (!timed && modelChanged && state.value.file !== null) {
    timed = true;
    console.debug(`bimble: refresh after change ${(performance.now() - t0).toFixed(2)} ms`);
    const p0 = performance.now();
    for (let i = 0; i < 100; i++) tryQuery("pointer", null);
    console.debug(`bimble: 100 pointer polls ${(performance.now() - p0).toFixed(2)} ms`);
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
/** Actions the lab shows as a Vue screen instead of egui's (App.vue fills it: no import cycle). */
export const screenRoutes: Record<string, () => void> = {};

export function runAction(name: string): string | null {
  const route = screenRoutes[name];
  if (route) { route(); return null; }
  try { wasmOrThrow().action(name); return null; } catch (e) { return message(e); }
}

/** Activate or close one open tab (`openViews.tabs`). Returns the Rust error, or null. */
export function tabCall(op: "activate" | "close" | "open", tab: OpenTab): string | null {
  try { wasmOrThrow().tab(JSON.stringify({ op, tab })); return null; } catch (e) { return message(e); }
}

/** Open any view as its current tab (a navigator click). Returns the Rust error, or null. */
export const openView = (id: string) => tabCall("open", { type: "view", id });

/** Run one API verb as one undoable step. Returns the Rust error, or null. */
export function runVerb(name: string, args: unknown = {}): string | null {
  try { wasmOrThrow().verb(name, JSON.stringify(args)); return null; } catch (e) { return message(e); }
}

/** Set some of an element's sizes (generator field names, mm) as one undoable step. Returns the Rust error, or null. */
export const setGenerator = (id: string, params: Record<string, number>) => runVerb("set_generator", { id, params });
/** Give elements a material by name (model material or library grade) as one undoable step. Returns the Rust error, or null. */
export const setMaterial = (ids: string[], name: string) => runVerb("set_material", { ids, material: name });
/** Write a one-line note on an element (its "Note" property); an empty text removes it. One undoable step. Returns the Rust error, or null. */
export const setNote = (id: string, text: string) => runVerb("set_note", { id, text });
/** The property a note is kept in (bimble-api `NOTE_KEY`). */
export const NOTE_KEY = "Note";
/** Change only the given fields of a view (`set_view`); `crop: null` removes the crop. */
export type ViewPatch = {
  scale?: number;
  true_north?: boolean;
  crop?: { min: [number, number]; max: [number, number] } | null;
  range?: Partial<Record<"top" | "cut" | "bottom" | "depth", { offset_mm?: number }>>;
};
export const setView = (id: string, patch: ViewPatch) => runVerb("set_view", { id, ...patch });
/** Change only the given fields of a schedule (`set_schedule`); `family: null` lists every family. Columns are field names ("level", "volume"). */
export type SchedulePatch = { group_by_level?: boolean; family?: string | null; columns?: string[] };
export const setSchedule = (name: string, patch: SchedulePatch) => runVerb("set_schedule", { name, ...patch });

/** Select by GlobalId, primary first; `[]` clears. Returns the Rust error, or null. */
export function select(ids: string[]): string | null {
  try { wasmOrThrow().select(JSON.stringify(ids)); return null; } catch (e) { return message(e); }
}

export const scheduleRows = (name: string) => query<ScheduleTable>("schedule_rows", { name });
/** A schedule as CSV, every row (the egui export's `to_csv`). */
export const scheduleCsv = (name: string) => query<{ csv: string }>("schedule_csv", { name }).csv;
export const views = () => query<ViewSummary[]>("views");
export const sheets = () => query<SheetSummary[]>("sheets");
export const schedules = () => query<ScheduleDef[]>("schedules");
/** Empty until the `schedule_list` query is merged. */
export const scheduleList = () => tryQuery<unknown[]>("schedule_list", []);
export const verbs = () => query<VerbInfo[]>("verbs");
export const describe = (name: string) => query<VerbInfo>("describe", { name });
/** Set one project information field by its label; Rust makes the key (`project::key_of`), an empty value removes it. Returns the Rust error, or null. */
export const setProjectField = (label: string, value: string) => runVerb("set_project_field", { label, value });

/** Rotate elements by `angle` degrees (counter-clockwise from above) about the vertical axis through `about` [x, y] mm (default: the centre of their origins). Returns the Rust error, or null. */
export const rotate = (ids: string[], angle: number, about?: [number, number]) =>
  runVerb("rotate", about ? { ids, angle_deg: angle, about } : { ids, angle_deg: angle });
/** Mirror elements in the vertical plane through `axis.point` along `axis.dir` (plan, mm). Returns the Rust error, or null. */
export const mirror = (ids: string[], axis: { point: [number, number]; dir: [number, number] }) =>
  runVerb("mirror", { ids, axis });

/** Working together (ADR 0038, query "collab"). Desktop only today: on the web every field is null / 0 / []. */
export interface Collab {
  folder: string | null;
  user: string | null;
  right: string | null;
  /** Unix seconds. */
  last_sync: number | null;
  borrowed_by_me: number;
  borrowed_by_others: { who: string; count: number }[];
  /** The refused change: elements someone else borrowed. `element_id` and `since` are null today. */
  belongs: { element_id: string | null; element_name: string; class: string | null; owner: string; since: number | null }[] | null;
}
const NO_COLLAB: Collab = { folder: null, user: null, right: null, last_sync: null, borrowed_by_me: 0, borrowed_by_others: [], belongs: null };
/** Null fields without an editable model. */
export const collab = () => tryQuery<Collab>("collab", NO_COLLAB);
/** Close the "belongs to someone else" alert without synchronizing (clears Editor.belongs). Returns the Rust error, or null. */
export function dismissBelongs(): string | null {
  try { wasmOrThrow().dismiss_belongs(); return null; } catch (e) { return message(e); }
}
/** Graphic standards (D5, bridge query "standards"); swatch SVGs are drawn in Rust. */
export type Swatch = { name: string; svg: string } | null;
export type Standards = {
  source: string | null;
  pens: { scales: number[]; rows: { pen: number; mm: (number | null)[] }[]; roles: { cut: number | null; seen: number | null; thin: number | null } };
  hatches: { family: string; cut: Swatch; seen: Swatch }[];
  moved: { from: string; to: string; count: number }[];
  actions: { revert: boolean; reset: boolean; save: boolean };
};
export const standards = () => tryQuery<Standards | null>("standards", null);
/** The style panel's buttons: "revert", "reset" (egui asks first), "save" (asks where). Returns the Rust error, or null. */
export function standardsAction(name: "revert" | "reset" | "save"): string | null {
  try {
    (wasmOrThrow() as unknown as { standards_action(n: string): void }).standards_action(name);
    return null;
  } catch (e) {
    return message(e);
  }
}
/** Home's recent files, newest first (bridge query "recent_files"); empty when the settings hold none. */
export type RecentFile = { name: string; path: string; folder: string; opened_unix: number | null; schema: string | null; exists: boolean };
export const recentFiles = () => tryQuery<RecentFile[]>("recent_files", []);
/** Linked models and reference plans (bridge query "links"); `kind` is "ifc", "bimble", "dxf", "pdf"… */
export type Link = { id: string; name: string; kind: string; visible: boolean };
export const links = () => tryQuery<Link[]>("links", []);
/** Measurements kept in the viewer (bridge query "measurements"); `label` as the egui panel shows it. */
export type Measurement = { id: number; label: string; value_mm: number };
export const measurements = () => tryQuery<Measurement[]>("measurements", []);

/** Rust's palette filter (`palette_match`, the egui one): the matching indices of `items`, in order. */
export function paletteMatch(q: string, items: string[]): number[] {
  return query<number[]>("palette_match", { q, items });
}

/** The last 5 actions run through Rust (`recent_actions`), newest first: their names. */
export function recentActions(): string[] {
  return query<string[]>("recent_actions");
}
