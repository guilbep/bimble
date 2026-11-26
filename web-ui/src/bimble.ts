// The one typed client of the wasm viewer (ADR 0048). Components never touch the wasm directly.
import { shallowRef } from "vue";
import { applyLook } from "./shell/theme"; // SET1

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
  /** `query("state")` (FAKE1): false for a file open read-only (an IFC as it is, a .bimbleview). */
  editable?: boolean;
  /** on_change (C core, ccore/session/links.h): a counter of changes to the linked models; absent before any. */
  links?: number;
  /** on_change (C core): the selected element of a linked model (its id is then `selection`), absent otherwise. */
  linked?: { id: string; link: string } | null;
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
  kind: "column" | "beam" | "wall" | "slab" | "footing" | "strip_footing" | "raft" | "opening" | "sleeve" | "grid_axis"; // CONC1: the Concrete tab's kinds; WAVE1B: a grid axis
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
  /** CONC1 (ccore/session/concrete.h): the Concrete tab's sizes, mm; only while one of its kinds is chosen. */
  concrete?: ConcreteSizes;
}
export interface ConcreteSizes {
  footing_depth: number; footing_length: number; footing_width: number;
  opening_height: number; opening_length: number; opening_sill: number; opening_width: number;
  raft_thickness: number; sleeve_diameter: number; strip_depth: number; strip_width: number;
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
  /** The place tool (C core only, op `tool`): pointer, click, cancel. Answers the tool's state JSON. */
  tool?(name: string, json: string): string;
  /** An action with a JSON body (C core only: link_model {path}, manage_links {op, name}). Answers its text. */
  action_with?(name: string, json: string): string;
  /** D7: a call whose answer is bytes (C core only: `query sheet_pdf` / `view_pdf`, the PDF itself). */
  bytes?(op: string, name: string, body: string): Promise<Uint8Array>;
}

export const state = shallowRef<State>({ selection: [], rev: 0, unsaved: false, file: null });
/** Set when the viewer could not start. */
export const startError = shallowRef<string | null>(null);
// ---- PERF2: the start's open, step by step (ccore.ts OpenSteps), shown over the viewport until its first frame ----
export const startStep = shallowRef<"read" | "parse" | "scene" | "mesh" | "draw" | null>(null);
/** Clears `startStep` once the viewport drew the model (or gave up): polled per frame, 20 s at most. */
function clearStartStepOnFirstFrame(): void {
  let frames = 0;
  const tick = () => {
    const g = window.__bbGl;
    if (!g || g.firstFrameMs !== null || g.error !== null || ++frames > 1200 || (state.value.file === null && frames > 2)) startStep.value = null;
    else requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
}
// ---- end PERF2 ----

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
//   standards_action, tool (the C core's place tool, ccore/session/calls.h). <name>: the name argument of query, verb, action, standards_action, else `-`.
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
    tool: (name, json) => call("tool", name, json),
    action_with: (name, json) => call("action", name, json),
    // D7: the answer's bytes (a PDF): an async fetch, since a sync XHR cannot read binary.
    bytes: async (op, name, body) => {
      const r = await fetch(`${base}/${op}/${encodeURIComponent(name)}?t=${t}`, { method: "POST", headers: { "Content-Type": "text/plain" }, body });
      if (!r.ok) throw (await r.text()) || `native ${op}: HTTP ${r.status}`;
      return new Uint8Array(await r.arrayBuffer());
    },
    on_change: (cb) => void pollChanges(base, t, cb),
  };
}

// ---- FIXUI: the native host's connection, for the shell (shell/send.ts: `lost`, `reconnected`) ----
/** The poll failed for LOST_AFTER_MS: `lost` once; the first answer after it: `back`, with whether
 *  it is the same host process (same epoch) and the host's current change (file, rev, unsaved). */
export const connectionHooks: { lost?: () => void; back?: (sameProcess: boolean, change: unknown) => void } = {};
const LOST_AFTER_MS = 1500;
let wakePoll: (() => void) | null = null;
/** Retry now (the notice's Retry): the poll's back-off wait ends at once. */
export function retryConnection(): void { wakePoll?.(); }
// ---- end FIXUI ----

/** Long-poll loop for the native client; never rejects. On an error it retries after 20 ms,
 *  doubling up to 1 s: a dev rebuild restarts the engine in tens of ms (ccore/results/bench.md:
 *  the fixed 1 s wait was most of the C core's edit -> visible time). */
async function pollChanges(base: string, t: string, cb: (json: string) => void): Promise<void> {
  let since = 0;
  let epoch: string | undefined;
  let wait = 20;
  let failingSince: number | null = null, lost = false; // FIXUI
  for (;;) {
    try {
      // The epoch lets a restarted twin answer at once, even when its seq equals ours.
      const e = epoch === undefined ? "" : `&epoch=${encodeURIComponent(epoch)}`;
      const r = await fetch(`${base}/change?since=${since}${e}&t=${t}`);
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      const m = (await r.json()) as { epoch?: string; seq: number; change: unknown };
      wait = 20;
      failingSince = null;
      if (lost) { // FIXUI: back after an outage (since = 0: the host's current change, at once)
        lost = false;
        const sameProcess = typeof m.epoch !== "string" || m.epoch === epoch;
        if (typeof m.epoch === "string") epoch = m.epoch;
        if (typeof m.seq === "number") since = m.seq;
        connectionHooks.back?.(sameProcess, m.change ?? null);
        if (m.change !== undefined && m.change !== null) cb(JSON.stringify(m.change));
        continue;
      }
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
      const now = performance.now();
      failingSince ??= now;
      if (!lost && now - failingSince >= LOST_AFTER_MS) { // FIXUI: the shell blocks and says so
        lost = true;
        since = 0;
        console.warn("native change poll: the host does not answer:", e);
        connectionHooks.lost?.();
      }
      await new Promise<void>((res) => { wakePoll = res; setTimeout(res, wait); });
      wakePoll = null;
      wait = Math.min(1000, wait * 2);
    }
  }
}

interface ViewportSample {
  latency_ms: number | null;
  changed: boolean;
  render_ms: number | null;
  read_ms: number | null;
  fetch_ms: number;
  half: boolean;
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
  // T5: while the user drags (or wheeled in the last 150 ms), stream at half resolution.
  let lastWheel = -Infinity;
  let wheelTimer: ReturnType<typeof setTimeout> | null = null;
  const offscreen = document.createElement("canvas");
  const offCtx = offscreen.getContext("2d");
  const interacting = () => drag !== null || performance.now() - lastWheel < 150;
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
    dirty = true; // The interaction ended: the next frame is full resolution.
    if (canvas.hasPointerCapture(e.pointerId)) canvas.releasePointerCapture(e.pointerId);
  };
  canvas.addEventListener("pointerup", release);
  canvas.addEventListener("pointercancel", release);
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());
  canvas.addEventListener("wheel", (e) => {
    e.preventDefault();
    scroll += e.deltaY;
    note(e.timeStamp);
    lastWheel = performance.now();
    if (wheelTimer !== null) clearTimeout(wheelTimer);
    wheelTimer = setTimeout(() => { wheelTimer = null; dirty = true; }, 150);
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
    const h = canvas.height;
    const half = interacting() && offCtx !== null;
    const fw = half ? Math.max(16, Math.round(canvas.width / 2)) : canvas.width;
    const fh = half ? Math.max(1, Math.round(h / 2)) : h;
    try {
      if (o.dx !== 0 || o.dy !== 0) await post("orbit", o);
      if (p.dx !== 0 || p.dy !== 0) await post("pan", { dx: p.dx / h, dy: p.dy / h });
      if (s !== 0) await post("zoom", { scroll: s });
      const f0 = performance.now();
      const r = await fetch(`${base}/frame?w=${fw}&h=${fh}&t=${t}`, { cache: "no-store" });
      if (!r.ok) throw new Error(`frame: HTTP ${r.status}`);
      const buf = await r.arrayBuffer();
      const fetch_ms = performance.now() - f0;
      if (buf.byteLength !== fw * fh * 4) return; // Bad size: skip; a resize set dirty.
      const px = new Uint8ClampedArray(buf);
      if (half && offCtx) {
        if (offscreen.width !== fw) offscreen.width = fw;
        if (offscreen.height !== fh) offscreen.height = fh;
        offCtx.putImageData(new ImageData(px, fw, fh), 0, 0);
        ctx.imageSmoothingEnabled = true;
        ctx.drawImage(offscreen, 0, 0, canvas.width, canvas.height);
      } else {
        if (fw !== canvas.width || fh !== canvas.height) return; // Resized meanwhile.
        ctx.putImageData(new ImageData(px, fw, fh), 0, 0);
      }
      const latency_ms = served === null ? null : performance.now() - served;
      let sum = 0;
      for (let i = 0; i < px.length; i += 997) sum = (sum * 31 + (px[i] ?? 0)) >>> 0;
      const changed = lastSum !== sum;
      lastSum = sum;
      const xt = r.headers.get("X-Timing");
      stats.frames++;
      stats.samples.push({ latency_ms, changed, render_ms: timing(xt, "render"), read_ms: timing(xt, "read"), fetch_ms, half });
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
    // The C lab's build (`npm run build:ccore`, ticket PG1): `ccore=1&gl=1` without parameters; `?view=1`: the viewer-only wasm.
    if (import.meta.env.VITE_CCORE === "1" && !url.searchParams.has("ccore")) { url.searchParams.set("ccore", "1"); url.searchParams.set("gl", "1"); }
    if (import.meta.env.VITE_CCORE === "1" && url.searchParams.get("view") === "1" && !url.searchParams.has("ccore_wasm")) url.searchParams.set("ccore_wasm", "ccore/ccore-view.wasm");
    const native = url.searchParams.get("native") === "1" ? nativeParams(url.searchParams) : null;
    if (url.searchParams.get("ccore") === "1") {
      // The C core's wasm module in the page (ticket T13b): no server, no Rust. `&gl=1` draws its mesh.
      // PERF1: the viewport's chunk is fetched while the wasm and the model load, not after them.
      const glChunk = url.searchParams.get("gl") === "1" ? import("./glviewport") : null;
      glChunk?.catch(() => undefined); // awaited below; no unhandled rejection if ccoreClient throws first
      startStep.value = "read"; // PERF2
      const cc = await import("./ccore");
      w = await cc.ccoreClient(url.searchParams, { step: (s) => { startStep.value = s; } });
      if (glChunk) {
        startStep.value = "draw";
        viewportDirty = (await glChunk).glViewport({
          mesh: async () => cc.ccoreMesh().bytes,
          epoch: async () => null,
        });
        clearStartStepOnFirstFrame();
      } else startStep.value = null;
    } else if (native) {
      // Dev only: no manifold, no wasm; on_change below starts the long-poll loop.
      w = nativeClient(native.port, native.token);
      // `&gl=1`: draw the host's /mesh with WebGL2 (the C host, spec §2) instead of /frame.
      viewportDirty = url.searchParams.get("gl") === "1"
        ? (await import("./glviewport")).glViewport(`http://127.0.0.1:${native.port}`, native.token)
        : streamViewport(`http://127.0.0.1:${native.port}`, native.token);
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
    backend = url.searchParams.get("ccore") === "1" ? "ccore" : native ? "native" : "rust"; // UX1
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
        const q = tryQuery<Pick<State, "schema" | "user" | "version" | "editable">>("state", {});
        state.value = { ...s, schema: q.schema ?? null, user: q.user ?? null, version: q.version ?? null, editable: q.editable ?? true };
        loadCapabilities(q.version ?? null); // FAKE1
        if (!settingsAsked) loadSettings(); // SET1: the look and the readout's system, once at start
        refresh(prev.rev !== s.rev || prev.file !== s.file);
      });
    });
    watchPointer();
    labelTextAgent();
    // ponytail: one 1 s timer for the whole session; it queries only while the viewer draws.
    setInterval(() => { if (status.value && !status.value.idle) status.value = tryQuery<Status | null>("status", null); }, 1000);
    resolveReady();
  } catch (e) {
    startStep.value = null; // PERF2
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
  if (!canvas || backend !== "rust") return; // KEY1: the C core's page computes it (glviewport.ts)
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
    // A linked element selected (C core, links.h): its read-only detail from its link.
    const lk = state.value.linked;
    if (lk && lk.id === id) return JSON.parse(wasmOrThrow().query("element", JSON.stringify({ id, link: lk.link }))) as ElementDetail;
    // FIXUI: a file open as it is (an IFC, a .bimbleview): `element` refuses it as Rust's does; the
    // scene's rows (class, name, GlobalId, storey, material, family, triangles, volume) come from
    // `scene_element` (ccore/session/links.h), as Rust's viewer reads them from its scene (lib.rs:4570).
    const name = state.value.file !== null && state.value.editable === false ? "scene_element" : "element";
    return JSON.parse(wasmOrThrow().query(name, JSON.stringify({ id }))) as ElementDetail;
  } catch (e) {
    throw new Error(message(e));
  }
}

/** Rename one element as one undoable step: the verb `rename` (ADR 0068 rule 1; a C core
 * addition, Rust's dispatch has none). On Rust's wasm module ("unknown verb") the raw Command is
 * applied instead, until Rust has the verb. Returns the error, or null. */
export function rename(id: string, name: string): string | null {
  const ro = linkedReadOnly({ id });
  if (ro) return ro;
  const err = runVerb("rename", { id, name });
  if (err === null || !err.startsWith("unknown verb")) return err;
  try {
    wasmOrThrow().apply(JSON.stringify([{ op: "rename", id, name }]));
    return null;
  } catch (e) {
    return message(e);
  }
}

/** A project key `set_project_field` gives back unchanged when used as the label (Rust `project::key_of`). */
const STABLE_KEY = /^[a-z0-9]+(?:_[a-z0-9]+)*$/;

/** Change the project information (ADR 0030) to `project`: one `set_project_field` verb per changed
 * key (ADR 0068 rule 1), an empty value for a removed key. The project page changes one field at a
 * time, so that is one undoable step. A key the verb cannot address (not letters, digits and `_`,
 * or longer than 64) is refused. Returns the error, or null. */
export function setProject(next: Record<string, string>): string | null {
  const cur = project.value?.info ?? {};
  const keys = [...new Set([...Object.keys(cur), ...Object.keys(next)])].filter((k) => (cur[k] ?? "") !== (next[k] ?? ""));
  for (const k of keys) {
    if (!STABLE_KEY.test(k) || k.length > 64) return `the field ${JSON.stringify(k)} cannot be changed here`;
    const err = setProjectField(k, next[k] ?? "");
    if (err) return err;
  }
  return null;
}

/** Change what is placed: `{kind?, storey?, material?, sizes?}`, all or nothing. Returns the Rust error, or null. */
export function setPlacing(patch: { kind?: Placing["kind"]; storey?: string; material?: string; sizes?: Partial<Placing["sizes"]>; top_offset_mm?: number; shape?: Placing["shape"]; concrete?: Partial<ConcreteSizes> }): string | null {
  try {
    wasmOrThrow().set_placing(JSON.stringify(patch));
    return null;
  } catch (e) {
    return message(e);
  }
}

/** The place tool's state (ccore/session/calls.h, op `tool` and `query ghost`). World mm. */
export interface ToolState {
  active: boolean;
  /** What the next click adds, as the BMSH mesh: positions relative to `origin` (world mm). */
  ghost: { edges: number[]; idx: number[]; origin: [number, number, number]; pos: number[] } | null;
  /** Why the preview has no ghost (the place verb's error). */
  ghost_error: string | null;
  hover: { at: [number, number]; snap: string | null; z: number } | null;
  kind: Placing["kind"];
  /** The GlobalId a click just added. */
  placed: string | null;
  points: [number, number][];
}
/** A pointer for the tool: a ray in scene coordinates (as the BMSH mesh) and the ray 12 px beside it
 *  (the snap tolerance), or a plan point `at` (world mm) with `tol_mm`. `free`: no snap; `ortho`:
 *  the second point along x or y. */
export interface ToolPointer {
  ray?: number[];
  ray_tol?: number[];
  at?: [number, number];
  tol_mm?: number;
  free?: boolean;
  ortho?: boolean;
}

/** One call of the place tool (C core only): the tool's state, or the error text. */
export function toolCall(name: "pointer" | "click" | "cancel" | "close" /* SLAB1: Enter closes a polygon slab */, args: ToolPointer = {}): { state: ToolState | null; error: string | null } {
  try {
    const w = wasmOrThrow();
    if (!w.tool) return { state: null, error: "the place tool needs the C core" };
    return { state: JSON.parse(w.tool(name, JSON.stringify(args))) as ToolState, error: null };
  } catch (e) {
    return { state: null, error: message(e) };
  }
}

// ---- Dimensions and texts in a plan (ticket C4, ccore/src/annot.h; C core only) ----

/** An end of a dimension as `view_annotations` gives it: the element feature it follows, if any. */
export interface AnnotationAnchor { element: string | null; feature: string | null; follows: boolean }
/** `query("view_annotations")` items: resolved for drawing, world plan mm. */
export type ViewAnnotation =
  | { type: "dimension"; id: string; a: [number, number]; b: [number, number]; qa: [number, number] | null; qb: [number, number] | null;
      mid: [number, number] | null; dir: [number, number] | null; offset_mm: number; axis: "aligned" | "horizontal" | "vertical";
      value_mm: number; text: string; kind: string | null; anchors: [AnnotationAnchor, AnnotationAnchor] }
  | { type: "text"; id: string; at: [number, number]; text: string; size_mm: number; angle_deg: number; kind: string | null };
export interface ViewAnnotations { view: string; scale: number; unit: "mm" | "cm" | "m"; items: ViewAnnotation[] }
/** The dimensions and texts of a view (default: the view shown), or null (none shown, or not the C core). */
export const viewAnnotations = (view?: string, unit?: "mm" | "cm" | "m") =>
  tryQuery<ViewAnnotations | null>("view_annotations", null, { ...(view ? { view } : {}), ...(unit ? { unit } : {}) });

// ---- L1: reference plans in a 2D view (ccore/session/links_ref.h; C core only) ----
/** `query("view_references")`: a view's reference plans, placed, world plan mm (flat x, y pairs). */
export interface ViewReference { id: string; name: string; polylines: number[][]; segments: number; visible: boolean }
export interface ViewReferences { view: string; references: ViewReference[]; segments: number; shown: number }
/** The reference plans of a view (default: the view shown), or null (none shown, or not the C core). */
export const viewReferences = (view?: string) => tryQuery<ViewReferences | null>("view_references", null, view ? { view } : {});
/** Add a PDF to the active 2D view (`link_pdf {path, scale?}`; native host: a path it can read). Returns the error, or null. */
export function linkPdf(path: string, scale?: number, place?: { offset_mm?: [number, number]; rotation_deg?: number }): string | null {
  try { actionWith("link_pdf", { path, ...(scale ? { scale } : {}), ...(place ?? {}) }); return null; } catch (e) { return message(e); }
}

// ---- L2: DXF reference plans and managing a reference (session/links_dxf.h; C core only) ----
/** Add a DXF to the active 2D view (`link_dxf {path}`; native host: a path it can read). Returns the error, or null. */
export function linkDxf(path: string, place?: { offset_mm?: [number, number]; rotation_deg?: number }): string | null {
  try { actionWith("link_dxf", { path, ...(place ?? {}) }); return null; } catch (e) { return message(e); }
}
/** A reference plan's change, by its id (`query links`), PDF or DXF: one undo step each. */
export type ReferenceOp =
  | { op: "move"; offset_mm: [number, number] }
  | { op: "rotate"; rotation_deg: number }
  | { op: "show" | "hide" | "remove" }
  | { op: "reload"; path?: string };
/** `manage_links {op, id, …}` on a reference plan. Returns the error, or null. */
export function manageReference(id: string, o: ReferenceOp): string | null {
  try { actionWith("manage_links", { ...o, id }); return null; } catch (e) { return message(e); }
}

/** The annotate tool's state (op `tool` "annotate", then pointer / click / cancel while it is on). */
export interface AnnotateState {
  active: boolean;
  mode: "dimension" | "text" | null;
  /** The GlobalId the last click added. */
  added: string | null;
  /** The element feature under the pointer, as a click would anchor it. */
  anchor: { element: string; feature: string } | null;
  anchors: ({ element: string; feature: string } | null)[];
  hover: { at: [number, number]; snap: string | null; z: number } | null;
  points: [number, number][];
  /** The dimension the next click makes (as a view_annotations dimension, without id and anchors). */
  preview: { a: [number, number]; b: [number, number]; qa: [number, number]; qb: [number, number]; mid: [number, number];
    dir: [number, number]; offset_mm: number; axis: string; text: string; value_mm: number } | null;
  text: string | null;
  view: string | null;
}
/** The annotate tool's state as the last call left it (the viewport draws it and sends it the pointer). */
export const annotate = shallowRef<AnnotateState | null>(null);
/** One call of the annotate tool: the state, or the error text. */
export function annotateCall(name: "annotate" | "pointer" | "click" | "cancel", args: unknown = {}): { state: AnnotateState | null; error: string | null } {
  try {
    const w = wasmOrThrow();
    if (!w.tool) return { state: null, error: "dimensions need the C core" };
    const st = JSON.parse(w.tool(name, JSON.stringify(args))) as AnnotateState;
    annotate.value = st.active ? st : null;
    return { state: st, error: null };
  } catch (e) {
    return { state: null, error: message(e) };
  }
}
/** The Annotate tab's Dimension: in the plan shown, two clicks (snapped, anchored on element corners)
 *  and a third for the line. Esc stops. Returns the error, or null. */
export const startDimension = (): string | null => annotateCall("annotate", { mode: "dimension" }).error;
/** The Annotate tab's Text: one click in the plan shown places `text`. Returns the error, or null. */
export const startText = (text: string, sizeMm?: number): string | null =>
  annotateCall("annotate", sizeMm === undefined ? { mode: "text", text } : { mode: "text", text, size_mm: sizeMm }).error;
/** Stop the annotate tool. */
export const stopAnnotate = (): string | null => annotateCall("cancel").error;
/** A dimension between two points or element features (`{element, feature}`), `offset` mm to the left of a->b. One undoable step. */
export const addDimension = (view: string, a: [number, number] | { element: string; feature: string }, b: [number, number] | { element: string; feature: string }, offset: number) =>
  runVerb("add_dimension", { view, a, b, offset });
/** A text at a plan point. One undoable step. */
export const addText = (view: string, at: [number, number], text: string) => runVerb("add_text", { view, at, text });
/** Move a text by `by` (mm), or a dimension's line across itself. One undoable step. */
export const moveAnnotation = (view: string, id: string, by: [number, number]) => runVerb("move_annotation", { view, id, by });
/** Delete a dimension or a text. One undoable step. */
export const deleteAnnotation = (view: string, id: string) => runVerb("delete_annotation", { view, id });

/** `query("visibility_sets")` (C core only, ccore/session/calls.h): the whole sets `query("visibility")` caps at 50. */
export interface VisibilitySets {
  /** SEC1: what the view's class, family, storey and material lists hide (C core only). */
  by_view?: string[];
  hidden_in_view: string[];
  isolated_in_view: string[];
  now: { ids: string[]; isolate: boolean } | null;
  reveal: boolean;
}
export const visibilitySets = () => tryQuery<VisibilitySets | null>("visibility_sets", null);

/** The section box tool's state (C core only, op `tool` box_*): `box` is the drag's preview while `active`. Scene mm. */
export interface BoxToolState {
  active: boolean;
  box: { min: number[]; max: number[]; yaw: number } | null;
  face: number | null;
  on: boolean;
}
/** One call of the section box tool (C core only): `box_press` / `box_move` / `box_release` take
 *  `{ray, ray_tol}` (scene coordinates), `box_set` takes `{min, max, on?}`. Session state, no history step;
 *  the box is not in the change signature, so `visibility` is refreshed here after a change. */
export function boxTool(
  name: "box_press" | "box_move" | "box_release" | "box_cancel" | "box_set",
  args: { ray?: number[]; ray_tol?: number[]; min?: number[]; max?: number[]; on?: boolean } = {},
): { state: BoxToolState | null; error: string | null } {
  try {
    const w = wasmOrThrow();
    if (!w.tool) return { state: null, error: "the section box tool needs the C core" };
    const state = JSON.parse(w.tool(name, JSON.stringify(args))) as BoxToolState;
    if (name !== "box_move" && name !== "box_press") visibility.value = tryQuery<Visibility | null>("visibility", null);
    return { state, error: null };
  } catch (e) {
    return { state: null, error: message(e) };
  }
}
/** Set the section box (scene mm, as `visibility.section_box.box`); `on` also turns it on or off. Returns the error, or null. */
export const setSectionBox = (min: number[], max: number[], on?: boolean) =>
  boxTool("box_set", on === undefined ? { min, max } : { min, max, on }).error;

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

// ---- FAKE1: what the core serves (ccore/session/capabilities.h `query capabilities`): pages,
// panels, data sets and exports by name, each true or false; read through shell/capability.ts.
// null: everything (the Rust viewer and the native twin have no such query, and serve it all).
// Asked once, of the C core only (its version ends in "ccore": no 400 asked of Rust). ----
export const capabilities = shallowRef<Record<string, boolean> | null>(null);
let capsAsked = false;
function loadCapabilities(version: string | null): void {
  if (capsAsked) return;
  capsAsked = true;
  const c = backend === "ccore" || (version ?? "").endsWith("ccore") ? tryQuery<Record<string, boolean> | null>("capabilities", null) : null;
  capabilities.value = c && typeof c === "object" ? c : null;
}
// ---- end FAKE1 ----

// ---- SET1: the user's settings (ccore/session/settings.h `query settings`, `action set_settings`): the
// same file as the Rust viewer (settings.rs). Read once at start (the look applied: shell/theme.ts) and
// after each change; `state.user` follows (query state). null: not served (the Rust wasm, the viewer-only
// build). ----
/** settings.rs `Settings` as to_json writes it. */
export interface SettingsValues {
  coordinates: string; gpu_cull: boolean; memory_share: number; moving_fps: number; moving_proxies: boolean;
  recent: string[]; text_size: number; theme: string; ui_font: string; user_email: string; user_name: string;
}
/** `query settings`. */
export interface UserSettings {
  applies: Record<keyof SettingsValues, boolean>; file: string | null; fonts: string[]; needs: Partial<Record<keyof SettingsValues, string>>;
  note: string | null; text_sizes: [number, number]; themes: string[]; values: SettingsValues;
}
export const userSettings = shallowRef<UserSettings | null>(null);
let settingsAsked = false;
/** Reads the settings (the file may have changed: the Rust app shares it) and applies the look. */
export function loadSettings(): void {
  settingsAsked = true;
  const s = capabilities.value === null || capabilities.value["settings"] ? tryQuery<UserSettings | null>("settings", null) : null;
  userSettings.value = s && typeof s === "object" && s.values ? s : null;
  if (userSettings.value) applyLook(userSettings.value.values);
}
/** Changes settings (`{field: value}`, settings.rs names): validated by the core as a whole. Returns
 *  the core's error text, or null; the settings and `state.user` are read again. */
export function setSettings(patch: Partial<SettingsValues>): string | null {
  try { actionWith("set_settings", patch); } catch (e) { return message(e); }
  loadSettings();
  const q = tryQuery<Pick<State, "user">>("state", {});
  state.value = { ...state.value, user: q.user ?? null };
  return null;
}
/** The readout's coordinate system (settings.rs coordinates; coords.rs:57), "survey" by default. */
export const readoutSystem = (): string => userSettings.value?.values.coordinates ?? "survey";
/** `query about` (lib.rs:4081): the page gives its WebGL renderer and its build's commit. */
export interface AboutInfo { build: string; commit: string | null; graphics: string | null; platform: string; text: string; version: string }
export const aboutInfo = (graphics: string | null, commit: string | null): AboutInfo | null =>
  tryQuery<AboutInfo | null>("about", null, { ...(graphics ? { graphics } : {}), ...(commit ? { commit } : {}) });
// ---- end SET1 ----

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
  if (modelChanged) setMeasure(tryQuery<MeasureState | null>("measure", null)); // another model: none kept
  if (modelChanged) {
    // ---- FAKE1: a file open read only (an IFC as it is, a .bimbleview) and no file answer these
    // "this file is read only" (400, calls.c MODEL): not asked ----
    const editable = state.value.file !== null && state.value.editable !== false;
    storeys.value = editable ? tryQuery<StoreySummary[]>("storeys", []) : [];
    materials.value = editable ? tryQuery<MaterialSummary[]>("materials", []) : [];
    project.value = editable ? tryQuery<ProjectSummary | null>("project", null) : null;
    // ---- end FAKE1 ----
    refreshNorth(); // G2: project north (viewcube.ts projectNorth) and the setting-out panel
  }
  refreshSettingOut(); // SETOUT: the markers (session state) and the georeference they show
  refreshRefPlanes(); // WAVE1B: the planes and the work plane (a deleted one is no work plane)
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

// ---- H1: the model's history, read only (ccore/session/versions.h). The Rust viewer has no
// such queries: the fallback says so. Writing (commit, switch, merge) is ticket H2.
export interface VersionInfo { hash: string; parents: string[]; author: string; time: number; message: string }
export interface HistoryHead { branch: string | null; commit: string | null }
export interface VersionField { path: string; before: unknown; after: unknown }
/** version::Change in serde's form. */
export interface VersionChange { change: "added" | "removed" | "modified"; kind: string; id: string; name: string; fields?: VersionField[] }
/** A history answer, or why there is none ("no history next to …", "no history folder in the browser"). */
export type HistoryAnswer<T> = ({ history: true } & T) | { history: false; reason: string };
export interface RepoHistory {
  versions: HistoryAnswer<{ head: HistoryHead; versions: VersionInfo[] }>;
  branches: HistoryAnswer<{ head: HistoryHead; branches: { name: string; commit: string }[] }>;
  pending: HistoryAnswer<{ base: string | null; changes: VersionChange[] }>;
}
export const repoHistory = shallowRef<RepoHistory | null>(null);

function historyQuery<T>(name: string, args?: unknown): HistoryAnswer<T> {
  try {
    return query<HistoryAnswer<T>>(name, args);
  } catch (e) {
    return { history: false, reason: message(e) };
  }
}

/** Reads versions, branches and pending changes; the History page calls it when shown and on each change. */
export function loadRepoHistory(): void {
  repoHistory.value = {
    versions: historyQuery("versions"),
    branches: historyQuery("branches"),
    pending: historyQuery("pending"),
  };
}

/** What changed from version `a` to version `b` (version::diff). */
export function versionChanges(a: string, b: string): HistoryAnswer<{ a: string; b: string; changes: VersionChange[] }> {
  return historyQuery("version_changes", { a, b });
}

/** Typed query by name (bridge.rs, then bimble-api dispatch). Throws with the Rust message. */
export function query<T>(name: string, args?: unknown): T {
  try {
    return JSON.parse(wasmOrThrow().query(name, args === undefined ? "{}" : JSON.stringify(args))) as T;
  } catch (e) {
    throw new Error(message(e));
  }
}

// ---- D7: the PDF export (C core: ccore/session/sheet_svg.h `sheet_pdf`, `view_pdf`) ----
/** A sheet as PDF (the sheet export of the Rust viewer, `Sheet::to_pdf`): the file's bytes. Throws the
 *  C core's error text (also "this build is view-only", and outside the C core). */
export async function sheetPdf(id: string, font: PdfFont = "helvetica"): Promise<Uint8Array> {
  const w = wasmOrThrow();
  if (!w.bytes) throw new Error("PDF export: the C core only");
  // SIZE1: Helvetica by default (nothing embedded); "roboto" only when pdfFonts() says it is there.
  const body = font === "roboto" ? { sheet: id, font } : { sheet: id };
  try { return await w.bytes("query", "sheet_pdf", JSON.stringify(body)); } catch (e) { throw new Error(message(e)); }
}
// ---- SIZE1: the PDF's font: Helvetica (the default, no file) or Roboto (optional files) ----
export type PdfFont = "helvetica" | "roboto";
/** `query pdf_fonts`: which fonts the C core can set the PDF in. Outside the C core: Helvetica only. */
export function pdfFonts(): { helvetica: boolean; roboto: boolean } {
  try { return query<{ helvetica: boolean; roboto: boolean }>("pdf_fonts"); } catch { return { helvetica: true, roboto: false }; }
}
// ---- end SIZE1 ----
/** A 2D view as PDF (the plan panel's "Export PDF…": its scale, the smallest paper it fits on). */
export async function viewPdf(id: string, font: PdfFont = "helvetica"): Promise<Uint8Array> { // PAPER1: the font
  const w = wasmOrThrow();
  if (!w.bytes) throw new Error("PDF export: the C core only");
  const body = font === "roboto" ? { view: id, font } : { view: id };
  try { return await w.bytes("query", "view_pdf", JSON.stringify(body)); } catch (e) { throw new Error(message(e)); }
}
/** Rust's file names: "<number> <name>.pdf" for a sheet (viewing.rs export_sheet), "plan-<view>-1-<scale>.pdf" for a view (lib.rs). */
export const sheetPdfName = (s: { number: string; name: string }) => `${s.number} ${s.name}.pdf`;
export const viewPdfName = (v: { name: string; scale: number }) => `plan-${v.name}-1-${v.scale}.pdf`;
// ---- end D7 ----
// ---- PAPER1: a view On paper and Export view as SVG (`view_svg`), several sheets as one PDF (`sheets_pdf`) ----
/** `query view_svg`: the view's paper as SVG, its paper and scale. `bare`: the On paper picture (paper.rs). */
export interface ViewSvg { svg: string; paper: string; portrait: boolean; scale: number; width_mm: number; height_mm: number }
export const viewSvg = (id: string, bare = false, font?: PdfFont): ViewSvg =>
  query<ViewSvg>("view_svg", { view: id, ...(bare ? { bare } : {}), ...(font ? { font } : {}) });
/** Rust's view export name (lib.rs plan_panel): "plan-<view>-1-<scale>.svg". */
export const viewSvgName = (v: { name: string; scale: number }) => `plan-${v.name}-1-${v.scale}.svg`;
/** Several sheets as one PDF, one page each in that order (C core only; Rust has no such file). */
export async function sheetsPdf(ids: string[], font: PdfFont = "helvetica"): Promise<Uint8Array> {
  const w = wasmOrThrow();
  if (!w.bytes) throw new Error("PDF export: the C core only");
  const body = font === "roboto" ? { sheets: ids, font } : { sheets: ids };
  try { return await w.bytes("query", "sheets_pdf", JSON.stringify(body)); } catch (e) { throw new Error(message(e)); }
}
/** The one-PDF name (a C choice, DEVIATIONS 22): "<first number>-<last number>.pdf", "<number>.pdf" for one. */
export const sheetsPdfName = (s: { number: string }[]) =>
  `${[...new Set([s[0]?.number ?? "sheets", s[s.length - 1]?.number ?? "sheets"])].join("-")}.pdf`;
// ---- end PAPER1 ----

/** Run one ribbon action by name (see `actions`). Returns the Rust error, or null. */
/** Actions the lab shows as a Vue screen instead of egui's (App.vue fills it: no import cycle). */
export const screenRoutes: Record<string, () => void> = {};

/** Q2: the ribbon's view switches (ribbon.rs:695-723), run by the C core then drawn by the page. */
const VIEW_SWITCHES = ["edges", "hatching", "orthographic", "fit", "top",
  "section_labels", "openings", "section_marks", "slab_levels"]; // KEY1: the plan annotation toggles (pressed state)
/** Q2: what the viewport does after a view switch ran (glviewport.ts fills it: fit, top). */
export const viewActions: Record<string, () => void> = {};
// ---- KEY1 ---- what the viewport does for a key (shell/keys.ts calls it; glviewport.ts fills it)
export const viewportKeys: {
  /** Esc: stops the tool running (annotate, place, measure: its first point, then the tool). True if one ran. */
  cancelTool?: () => boolean;
  /** The annotation selected in the plan (a click on it), or null. */
  selectedAnnotation?: () => string | null;
  /** Delete: the selected annotation goes (delete_annotation). True if one was selected. */
  deleteAnnotation?: () => boolean;
  /** Esc: the selected annotation is let go. */
  dropAnnotation?: () => void;
  /** SLAB1: Enter closes the polygon slab being clicked (edit.rs close_polygon). True if one was. */
  enterTool?: () => boolean;
} = {};
// ---- end KEY1 ----

// ---- FS1: the file actions of the native host go to files.ts (its dialogs; set there, no import cycle) ----
export const fileRoutes: { run?: (name: string) => boolean } = {};
/** An action with a JSON body (`{path, discard}`): throws the error text. */
export function actionBody(name: string, body: unknown): void { actionWith(name, body); }
// ---- end FS1 ----
// ---- H2b: an action's answer, parsed (merge: {merged, commit?, conflicts}) ----
export function actionAnswer<T>(name: string, body: unknown): T { return JSON.parse(actionWith(name, body)) as T; }
// ---- end H2b ----

export function runAction(name: string): string | null {
  const route = screenRoutes[name];
  if (route) { route(); return null; }
  if (fileRoutes.run?.(name)) return null; // FS1
  // DEAD1: New sheet shows the sheet it made (the core says it is shown: Place view… is enabled), so
  // the screen and the session agree: the new id is the one not listed before.
  const sheetsBefore = name === "new_sheet" ? new Set(tryQuery<SheetSummary[]>("sheets", []).map((x) => x.id)) : null;
  try {
    wasmOrThrow().action(name);
    if (sheetsBefore) {
      const made = tryQuery<SheetSummary[]>("sheets", []).find((x) => !sheetsBefore.has(x.id));
      if (made) shellHooks.sheetShown?.(made.id);
    }
    // Measuring is session state, not in the change JSON: its state and pressed button here (ticket T1).
    if (name === "measure" || name.startsWith("place_") || name === "reference_planes") { // WAVE1B: the plane tool measures
      setMeasure(tryQuery<MeasureState | null>("measure", null));
      actions.value = tryQuery<ActionState[]>("actions", []);
      refreshRefPlanes(); // WAVE1B: pick.on
    }
    // The box moves without a change event (its geometry is not in the change signature, calls.rs:152).
    if (name === "box_to_selection") visibility.value = tryQuery<Visibility | null>("visibility", null);
    // ---- ISS71 ---- the box's orientation: the box turns and the pressed choice moves, no change event
    if (name === "box_north_project" || name === "box_north_true") {
      visibility.value = tryQuery<Visibility | null>("visibility", null);
      actions.value = tryQuery<ActionState[]>("actions", []);
    }
    // ---- end ISS71 ----
    // ---- SETOUT ---- the markers shown and the box's north are session state: no change event
    if (SETOUT_ACTIONS.includes(name)) {
      actions.value = tryQuery<ActionState[]>("actions", []);
      visibility.value = tryQuery<Visibility | null>("visibility", null);
      refreshSettingOut();
    }
    // ---- end SETOUT ----
    // ---- VIEWSHEET ---- the section tool is session state: its pressed button here
    if (name === "draw_section") actions.value = tryQuery<ActionState[]>("actions", []);
    // ---- Q2: the view switches are session state, not in the change JSON: their pressed state
    // here; fit and top move the page's camera (glviewport.ts registers `viewActions`). ----
    if (VIEW_SWITCHES.includes(name)) {
      actions.value = tryQuery<ActionState[]>("actions", []);
      viewActions[name]?.();
    }
    return null;
  } catch (e) { return message(e); }
}

/** Activate or close one open tab (`openViews.tabs`). Returns the Rust error, or null. */
export function tabCall(op: "activate" | "close" | "open", tab: OpenTab): string | null {
  try { wasmOrThrow().tab(JSON.stringify({ op, tab })); } catch (e) { return message(e); }
  // A sheet tab opens the Sheets page on that sheet (SheetsSide.vue watches it).
  if (tab.type === "sheet") sheetTab.value = op === "close" ? null : { id: tab.id };
  if (tab.type === "view" && op !== "close") shellHooks.viewShown?.(); // UX1: a view shown leaves Home or a page
  return null;
}

// ---- UX1 ---- the shell's calls into the session (shell/send.ts; the shell's state is not kept here)
/** Which client runs: the C host over HTTP, the C core's wasm in the page, or the Rust wasm. */
export let backend: "native" | "ccore" | "rust" = "rust";
/** Set by shell/send.ts: told when a view tab was opened or activated (tabCall). */
export const shellHooks: { viewShown?: () => void; /** DEAD1: a sheet made by New sheet is shown */ sheetShown?: (id: string) => void } = {};
/** A file action (new, open, import_ifc, close) with its body {path, discard}, never through
 *  `screenRoutes`. Returns the session's error text, or null. */
export function fileCall(name: string, body: { path?: string; discard?: boolean; dropped?: boolean /* FS1 */ }): string | null {
  try {
    const w = wasmOrThrow();
    if (w.action_with) w.action_with(name, JSON.stringify(body));
    else w.action(name); // the Rust wasm: its own dialogs
    return null;
  } catch (e) { return message(e); }
}
// ---- end UX1 ----

/** Open any view as its current tab (a navigator click). Returns the Rust error, or null. */
export const openView = (id: string) => tabCall("open", { type: "view", id });

/** What the viewport draws for the view shown (the C core's `query view_clip`, ticket C1,
 *  ccore/session/views.h): a plan's look, cut (world mm) and clip box (scene coordinates: the
 *  model's bounds cut to the view range). null without a view shown or where the query is not
 *  known (the Rust bridge): the viewport stays 3D. */
export interface ViewClip {
  id: string;
  type: "three_d" | "plan" | "section";
  look: "down" | "up" | null;
  storey: string | null;
  elevation: number | null;
  cut: number | null;
  clip: { min: number[]; max: number[] } | null;
}
export const viewClip = (): ViewClip | null => tryQuery<ViewClip | null>("view_clip", null);

// ---- SEC1 ---- views made, changed and removed by verb (ccore/src/verbs_views.h), a section's
// frame in `view_clip` (ccore/session/calls.c q_view_clip), what the active view hides.
/** A section's extra fields in `view_clip`: its look (world x, y, turned), facing, cut line point
 *  (world mm), depth (0: all) and turn (project north, radians). */
export interface SectionClip extends ViewClip { dir?: number[]; facing?: string; origin?: number[]; depth_mm?: number; yaw?: number }
export type ViewAdd = { kind?: "plan" | "ceiling" | "section" | "three_d"; storey?: string; facing?: string; origin?: number[]; from?: string; name?: string;
  scale?: number; depth_mm?: number /* VIEWSHEET */ };
/** `view_add`, then the new view's id (the one `open_views` did not list before), or the error. */
export function viewAdd(args: ViewAdd): { id: string | null; error: string | null } {
  const ids = (v: unknown) => new Set(((v as { views?: { id: string }[] }).views ?? []).map((x) => x.id));
  const before = ids(tryQuery<unknown>("open_views", {}));
  const error = runVerb("view_add", args);
  if (error) return { id: null, error };
  const after = [...ids(tryQuery<unknown>("open_views", {}))].filter((id) => !before.has(id));
  return { id: after[0] ?? null, error: null };
}
export type ViewVisibility = { id: string; hidden_classes: string[]; hidden_elements: string[]; hidden_families: string[]; hidden_links: string[]; hidden_materials: string[]; hidden_storeys: string[]; isolated_elements: string[] };
export type ViewEdit = { name?: string; origin?: number[]; facing?: string; depth_mm?: number; levels?: Record<string, string>; visibility?: Partial<Omit<ViewVisibility, "id">> };
export const viewEdit = (id: string, patch: ViewEdit) => runVerb("view_edit", { id, ...patch });
export const viewRemove = (id: string) => runVerb("view_remove", { id });
/** What the active view hides (`query view_visibility`, C core only); null without a view. */
export const viewVisibility = (): ViewVisibility | null => tryQuery<ViewVisibility | null>("view_visibility", null);
// ---- end SEC1 ----

/** Run one API verb as one undoable step. Returns the Rust error, or null. */
export function runVerb(name: string, args: unknown = {}): string | null {
  const ro = linkedReadOnly(args);
  if (ro) return ro;
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
/** One view placed on a sheet, as `sheet_layout` gives it (C core, ccore/session/sheets.h). Paper mm from the
 *  sheet's top-left corner; `box: "estimate"` until the drawing is made (D5): the view's crop, else the model's box,
 *  divided by the scale. */
export interface PlacedLayout {
  id: string;
  view: string;
  view_name: string | null;
  kind: "three_d" | "plan" | "section" | null;
  /** 1:N the view is drawn at (ADR 0046: the view's own). */
  scale: number;
  at_mm: [number, number];
  box_mm: [number, number, number, number];
  box: "estimate" | "drawing";
}
/** `query("sheet_layout", { id })`: the paper in the sheet's orientation and its placed views (C core only). */
export interface SheetLayout {
  id: string;
  number: string;
  name: string;
  paper: string;
  portrait: boolean;
  width_mm: number;
  height_mm: number;
  placed: PlacedLayout[];
}
export const sheetLayout = (id: string) => query<SheetLayout>("sheet_layout", { id });
/** Place a view on a sheet at `at` (paper mm); the scale is the view's (`scale` only for the first placement of a
 *  view whose scale is not said). One undoable step. Returns the error, or null. */
export const sheetPlace = (sheet: string, view: string, at: [number, number], scale?: number) =>
  runVerb("sheet_place", scale === undefined ? { sheet, view, at_mm: at } : { sheet, view, at_mm: at, scale });
/** Move a placed view to `at` (paper mm). One undoable step. Returns the error, or null. */
export const sheetMove = (sheet: string, placed: string, at: [number, number]) => runVerb("sheet_move", { sheet, placed, at_mm: at });
/** Take a placed view off its sheet (the view stays). One undoable step. Returns the error, or null. */
export const sheetUnplace = (sheet: string, placed: string) => runVerb("sheet_unplace", { sheet, placed });
/** Delete a sheet (its views stay). One undoable step. Returns the error, or null. */
export const sheetRemove = (id: string) => runVerb("sheet_remove", { id });
/** The sheet a sheet tab opened last (`tabCall` with type "sheet"), a new object per call: the Sheets page shows it. */
export const sheetTab = shallowRef<{ id: string } | null>(null);
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
export type RecentFile = { name: string; path: string; folder: string; opened_unix: number | null; schema: string | null; exists: boolean;
  /** FS1 (C core): "bimble", "ifc", "bimbleview", "json", "pdf", "dxf" or "other", by extension; the file's mtime (s), null when missing. */
  kind?: string; mtime?: number | null };
export const recentFiles = () => tryQuery<RecentFile[]>("recent_files", []);
/** Linked models and reference plans (bridge query "links"); `kind` is "ifc", "bimble", "dxf", "pdf"… */
export type Link = { id: string; name: string; kind: string; visible: boolean };
export const links = () => tryQuery<Link[]>("links", []);

// ---- Linked models (ticket B5, C core: ccore/session/links.h) ----

/** `query("element", {id, link})`: an element of a linked model, read-only (ElementDetail's shape plus these). */
/** `query("scene_element", {id})` (C core, FIXUI): an element of a file open read only, the rows Rust's viewer shows. */
export interface SceneElementDetail extends ElementDetail {
  read_only: true;
  family: string;
  triangles: number;
  volume_m3: number;
}

export interface LinkedElementDetail extends ElementDetail {
  link: string;
  read_only: true;
  family: string;
  volume_m3: number;
}

/** A linked element is read-only: the edit verbs naming the selected linked id are refused here (its
 *  GlobalId may also be one of the open model's, e.g. a link of an exported copy). */
function linkedReadOnly(args: unknown): string | null {
  const lk = state.value.linked;
  if (!lk || typeof args !== "object" || args === null) return null;
  const a = args as { id?: unknown; ids?: unknown };
  const named = a.id === lk.id || (Array.isArray(a.ids) && a.ids.includes(lk.id));
  return named ? `${lk.id} is in the linked model ${lk.link}: read only` : null;
}

/** Select one element of a linked model (`id` as the link's mesh gives it); `null` clears. Returns the error, or null. */
export function selectLinked(link: string, id: string | null): string | null {
  try { wasmOrThrow().select(JSON.stringify({ link, ids: id === null ? [] : [id] })); return null; } catch (e) { return message(e); }
}

/** An action with a JSON body (C core). Throws the error text. */
function actionWith(name: string, args: unknown): string {
  const w = wasmOrThrow();
  if (!w.action_with) throw new Error(`${name}: not in this viewer`);
  return w.action_with(name, JSON.stringify(args));
}

// ---- VIEWSHEET ---- the standards' text and dimension types, the one new annotations get (ccore/session/view_sheet.h)
export interface AnnotationTypes {
  dimension: string | null; dimension_types: { decimals: number; name: string; text_mm: number; unit: string }[];
  text: string | null; text_types: { font: string; name: string; size_mm: number }[];
}
/** `query annotation_types`; null where the core does not answer it. */
export const annotationTypes = (): AnnotationTypes | null => tryQuery<AnnotationTypes | null>("annotation_types", null);
/** The type new texts or dimensions get (null: Default). Returns the error, or null. */
export function setAnnotationType(which: "text" | "dimension", name: string | null): string | null {
  try { actionWith("annotation_type", { [which]: name }); return null; } catch (e) { return message(e); }
}
// ---- end VIEWSHEET ----

/** Link a model read-only beside the open one (`link_model {path}`; native host: a path it can read). Returns the error, or null. */
export function linkModel(path: string): string | null {
  try { actionWith("link_model", { path }); return null; } catch (e) { return message(e); }
}

/** Manage links: remove, reload, show or hide one by its file name (`links()` ids). Returns the error, or null. */
export function manageLink(op: "remove" | "reload" | "show" | "hide", name: string): string | null {
  try { actionWith("manage_links", { op, name }); return null; } catch (e) { return message(e); }
}
/** Measurements kept in the viewer (bridge query "measurements"); `label` as the egui panel shows it. */
export type Measurement = { id: number; label: string; value_mm: number };
export const measurements = () => { void measureKept.value; return tryQuery<Measurement[]>("measurements", []); };

// ---- The measure tool (ticket T1, ccore/session/measure.h; C core only) ----

/** The measure tool's state (op `tool` measure_*, `query measure`). Points in world mm. */
export interface MeasureState {
  active: boolean;
  /** The first point of the measurement being made. */
  first: [number, number, number] | null;
  /** Where the next click measures to: a corner, an edge point or a face point. */
  hover: { at: [number, number, number]; snap: "corner" | "edge" | "face" } | null;
  /** The measurements kept: `label` as the list shows it, `text` as the view draws it. */
  items: { a: [number, number, number]; b: [number, number, number]; id: number; label: string; text: string; value_mm: number }[];
  /** From the first point to the hover. */
  preview: { text: string; value_mm: number } | null;
}
/** The measure tool's state as the last call left it (the viewport draws it; the lists read it). */
export const measure = shallowRef<MeasureState | null>(null);
/** The kept measurements' labels: changes only when one is added or removed, so the lists that
 *  read `measurements()` follow the tool without a query per pointer move. */
const measureKept = shallowRef("");
const setMeasure = (st: MeasureState | null) => {
  measure.value = st;
  measureKept.value = st ? st.items.map((i) => i.label).join("\n") : "";
};
/** A pointer for the measure tool: the ray in scene coordinates and the ray `tol_px` pixels beside
 *  it; `free`: no snap (Alt), `ortho`: along an axis from the first point (Shift), `chain`: the
 *  next measurement starts where this one ends (Ctrl). */
export interface MeasurePointer { ray?: number[]; ray_tol?: number[]; tol_px?: number; free?: boolean; ortho?: boolean; chain?: boolean }
/** One call of the measure tool: the state, or the error text. No history step, no rev change. */
export function measureCall(name: "measure_pointer" | "measure_click" | "measure_cancel" | "measure_clear", args: MeasurePointer = {}): { state: MeasureState | null; error: string | null } {
  try {
    const w = wasmOrThrow();
    if (!w.tool) return { state: null, error: "the measure tool needs the C core" };
    const st = JSON.parse(w.tool(name, JSON.stringify(args))) as MeasureState;
    setMeasure(st);
    if (name === "measure_cancel" && !st.active) actions.value = tryQuery<ActionState[]>("actions", []); // its pressed state
    if (refPlanes.value?.pick.on) { // WAVE1B: the three-point tool's points (its pressed state when Esc stopped it)
      refreshRefPlanes();
      if (!refPlanes.value?.pick.on) actions.value = tryQuery<ActionState[]>("actions", []);
    }
    return { state: st, error: null };
  } catch (e) {
    return { state: null, error: message(e) };
  }
}
/** Drop every measurement. Returns the error, or null. */
export const clearMeasurements = (): string | null => measureCall("measure_clear").error;

/** Rust's palette filter (`palette_match`, the egui one): the matching indices of `items`, in order. */
export function paletteMatch(q: string, items: string[]): number[] {
  return query<number[]>("palette_match", { q, items });
}

/** The last 5 actions run through Rust (`recent_actions`), newest first: their names. */
export function recentActions(): string[] {
  return query<string[]>("recent_actions");
}

// ---- G2: levels, grids and the georeference (ccore/src/verbs_levels.c, src/georef.c, query.c
// `grids`, `north`, `coordinates`; session/views.h `grid_axes`). C additions (ADR 0045, 0056):
// Rust's bridge has none of them, so every read falls back to empty and every verb answers its
// "unknown verb" text there. ----

/** `query("storeys")` when the model uses levels or grids: is_storey and height_mm (query.c q_storeys). */
export type LevelSummary = StoreySummary & { is_storey?: boolean; height_mm?: number | null };
export interface GridAxisSummary { id: string; tag: string; start_mm: [number, number]; end_mm: [number, number] }
/** `query("grids")`: the model's grids by id, axes in order. */
export interface GridSummary { id: string; name: string; axes: GridAxisSummary[] }
export const grids = () => tryQuery<GridSummary[]>("grids", []);
/** `query("grid_axes")`: the axes (plan, world mm) and levels of what is open, then of each link. */
export interface GridAxesScene {
  axes: { tag: string; start_mm: [number, number]; end_mm: [number, number] }[];
  levels: { elevation_mm: number; key: string; name: string }[];
}
export type GridAxes = GridAxesScene & { links: (GridAxesScene & { name: string; visible: boolean })[] };
export const gridAxes = () => tryQuery<GridAxes | null>("grid_axes", null);
/** `query("state").editable`: an editable model is open (false for a read-only IFC or viewer file). */
export const editableNow = () => tryQuery<{ editable?: boolean }>("state", {}).editable === true;

export const levelAdd = (name: string, level_mm: number, storey?: boolean) =>
  runVerb("level_add", storey === undefined ? { name, level_mm } : { name, level_mm, storey });
export const levelSet = (id: string, patch: { name?: string; level_mm?: number; storey?: boolean }) => runVerb("level_set", { id, ...patch });
export const levelRemove = (id: string) => runVerb("level_remove", { id });
/** DEAD1: what the core would refuse on the Levels page, told before the click (query
 *  level_refusals: the verbs level_remove and level_set {storey: false} run dry), per storey id:
 *  the refusal texts, null where it would run. A control the core refuses is disabled with it. */
export type LevelRefusals = Record<string, { remove: string | null; storey: string | null }>;
export const levelRefusals = (): LevelRefusals => tryQuery<LevelRefusals>("level_refusals", {});
/** A rectangular grid: axes at `x` (numbered 1, 2…) and at `y` (lettered A, B…), as the verb tags them. */
export const gridAdd = (name: string, x: number[], y: number[], tags?: string[]) =>
  runVerb("grid_add", tags ? { name, x, y, tags } : { name, x, y });
export const gridAxisSet = (grid: string, axis: string, patch: { tag?: string; start?: [number, number]; end?: [number, number] }) =>
  runVerb("grid_axis_set", { grid, axis, ...patch });
export const gridRemove = (id: string) => runVerb("grid_remove", { id });
// ---- SLAB1: one axis more or less (verbs_levels.c grid_axis_add, grid_axis_remove) ----
export const gridAxisAdd = (grid: string, tag: string, start: [number, number], end: [number, number]) =>
  runVerb("grid_axis_add", { grid, tag, start, end });
export const gridAxisRemove = (grid: string, axis: string) => runVerb("grid_axis_remove", { grid, axis });
// ---- end SLAB1 ----

/** `query("north")`: the georeference as the viewport turns by it; null without an editable model. */
export interface NorthInfo { crs: string | null; has_map: boolean; map_rotation_deg: number; plan_turn_project: number; plan_turn_true: number; rotation_deg: number }
export const northInfo = shallowRef<NorthInfo | null>(null);
export const refreshNorth = () => (northInfo.value = tryQuery<NorthInfo | null>("north", null));
/** `query("coordinates")`: a world point (mm) in the survey system (m) and its status bar text. */
export interface Coordinates { survey: [number, number, number]; text: string; wgs84: [number, number] | null; world_mm: [number, number, number] }
export const coordinates = (at: [number, number, number], system: string | undefined = readoutSystem()) => // SET1: the setting's system
  tryQuery<Coordinates | null>("coordinates", null, system && system !== "survey" ? { at, system } : { at });
export type GeorefPatch = { crs?: string; east_m?: number; north_m?: number; height_m?: number; rotation_deg?: number; map_rotation_deg?: number; clear?: boolean; survey_point_mm?: number[] };
export const georefSet = (patch: GeorefPatch) => runVerb("georef_set", patch);
// ---- SETOUT (ADR 0075, ccore/session/setting_out.h): the Setting out tab's markers ----
export interface SetOutPoint { at_mm: [number, number, number]; shown: boolean; survey_m: [number, number, number] }
export interface SettingOut {
  base_point: SetOutPoint;
  crs: string | null;
  /** World plan unit vectors pointing to project north and to true north. */
  north: { project: [number, number]; rotation_deg: number; true: [number, number] };
  /** Project zero: the survey height of world z = 0, m (ADR 0056 decision 1). */
  project_zero_m: number;
  survey_point: SetOutPoint & { set: boolean };
}
/** The actions of the Setting out tab that change session state only. */
export const SETOUT_ACTIONS = ["show_base_point", "show_survey_point", "view_true_north"];
/** `query setting_out` as last asked (null: no editable model). */
export const settingOut = shallowRef<SettingOut | null>(null);
export const refreshSettingOut = () => { settingOut.value = tryQuery<SettingOut | null>("setting_out", null); };
/** Sets the survey point (world mm), or removes it (null). One undoable step (`georef_set`). */
export const surveyPointSet = (at: [number, number, number] | null) => runVerb("georef_set", { survey_point_mm: at ?? [] });
// ---- end SETOUT ----
// ---- WAVE1B (ADR 0075, ccore/session/refplanes.h): reference planes and the work plane ----
type V3 = [number, number, number];
export interface RefPlane { corners: V3[]; height: number; id: string; name: string; normal: V3; origin: V3; storey: string; width: number; x_axis: V3 }
export type ImplicitPlane =
  | { kind: "level"; id: string; name: string; elevation: number }
  | { kind: "axis"; id: string; name: string; start: [number, number]; end: [number, number] };
export interface RefPlanes {
  /** The work plane of the place tools and of Measure; null: the active storey's level. */
  active: { id: string; kind: "plane" | "level" | "axis"; name: string } | null;
  implicit: ImplicitPlane[];
  /** The three-point tool (action reference_planes): its points so far, world mm. */
  pick: { on: boolean; points: V3[] };
  planes: RefPlane[];
}
/** `query reference_planes` as last asked (null: no editable model). */
export const refPlanes = shallowRef<RefPlanes | null>(null);
export const refreshRefPlanes = () => { refPlanes.value = tryQuery<RefPlanes | null>("reference_planes", null); };
/** Choose the work plane (kind null: the storey's level again). Session state. Returns the error, or null. */
export function workPlaneSet(kind: "plane" | "level" | "axis" | null, id?: string): string | null {
  try {
    const w = wasmOrThrow();
    if (!w.tool) return "the work plane needs the C core";
    refPlanes.value = JSON.parse(w.tool("work_plane", JSON.stringify(kind ? { kind, id } : { kind: null }))) as RefPlanes;
    return null;
  } catch (e) { return message(e); }
}
export interface RefPlaneArgs { storey: string; name?: string; origin?: V3; normal?: V3; x_axis?: V3; points?: V3[]; width?: number; height?: number }
/** A new reference plane (verb reference_plane_add): one undoable step. Returns the error, or null. */
export const referencePlaneAdd = (args: RefPlaneArgs) => runVerb("reference_plane_add", args);
export const referencePlaneRename = (id: string, name: string) => runVerb("rename", { id, name });
export const referencePlaneDelete = (id: string) => runVerb("delete", { ids: [id] });
// ---- WAVE1B (B2, ADR 0077): an opening of a host, selected and changed (verb opening_set) ----
export interface OpeningInfo {
  id: string;
  /** "opening" (a box), "sleeve" (round), "other" (an imported shape: not editable here). */
  kind: "opening" | "sleeve" | "other";
  /** In a wall (sizes: width, height, sill) or a slab (width, length). */
  wall: boolean;
  width?: number; height?: number; length?: number; sill?: number; diameter?: number;
  /** Its centre (world mm) and its box's eight corners (world mm), for the overlay. */
  centre: [number, number, number];
  corners: [number, number, number][];
}
/** `query openings {host}`: the host's openings in order. */
export const openingsOf = (host: string) => tryQuery<OpeningInfo[]>("openings", [], { host });
/** The opening selected (its host and GlobalId), or null. Page state: the element selection stays the host. */
export const selectedOpening = shallowRef<{ host: string; id: string } | null>(null);
/** `query opening_at {ray}` (scene coordinates, as the measure tool's): the opening under the pointer, or null. */
export const openingAt = (ray: number[]) => tryQuery<{ host: string; id: string } | null>("opening_at", null, { ray });
export type OpeningPatch = { width?: number; height?: number; length?: number; sill?: number; diameter?: number; a?: [number, number] };
/** Changes an opening by its parameters (verb opening_set), its GlobalId kept. One step. Returns the error, or null. */
export const openingSet = (host: string, id: string, patch: OpeningPatch) => runVerb("opening_set", { host, id, ...patch });
/** WAVE1B (B1): every opening and sleeve of the model, a row each (`query opening_rows`): a sleeve's
 *  volume is the exact cylinder pi r^2 t, a box's width x height x t; another shape's null. */
export interface OpeningRow { host: string; host_name: string; id: string; kind: "opening" | "sleeve" | "other"; level: string;
  width?: number; height?: number; length?: number; diameter?: number; volume_m3: number | null }
export const openingRows = () => tryQuery<OpeningRow[]>("opening_rows", []);
// ---- end WAVE1B ----
// ---- end G2 ----
