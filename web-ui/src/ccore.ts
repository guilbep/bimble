// The C core in the browser (ticket T13b, `?ccore=1`): a `Wasm` (bimble.ts) over ccore.wasm, the
// freestanding wasm32 build of ccore/ (ccore/build.sh wasm; exports in ccore/src/wasm_api.h). The
// module holds the same call code as the native C host (ccore/session/calls.c): every call is
// `call(op, name, body)`, exactly a `POST /<op>/<name>` of the host, with no server at all.
// - Loads `ccore/ccore.wasm` (relative to the page; `npm run ccore:wasm` puts it in public/ccore/)
//   or `?ccore_wasm=<url>`, and seeds the id generator from crypto.getRandomValues.
// - Opens `?model=<url>` (a .bimble, v1 text or v2 binary) or the built-in demo `ccore/frame.json`.
// - on_change: the change JSON is read after every call and the callback runs when it changed.
// - Files (ticket A5; the module has no files, ccore/session/calls.c file_action): the actions
//   `open` and `import_ifc` show a file picker (`<input type=file>`) and open the picked file's
//   bytes (`ccoreOpenFile`: .bimble v1 or v2 and .json editable; .ifc and .bimbleview read-only,
//   as the Rust viewer opens them (ticket A5c); `import_ifc`: the .ifc imported as an editable
//   copy); `new` starts an Untitled model; `save`, `export_ifc4`/`export_ifc2x3` and
//   `share_for_viewing` (the .bimbleview viewer file) download what the module wrote (Blob +
//   `<a download>`, as the Rust viewer's wasm Save downloads). With unsaved changes, new and open
//   ask first (UX1: the shell's question; FS1: the pickers are files.ts pickFile). `ccoreSaveBytes()`, `ccoreExportIfc(schema)` and
//   `ccoreShareForViewing()` give the bytes without a download. The module's clock is the page's
//   (host_clock, for IFC stamps).
// - `window.ccoreSave()` saves (when there are unsaved changes) and returns the last saved text,
//   and logs its size (a .json name: the v1 text).
// - The mesh for the WebGL2 viewport: `ccoreMesh()`, the BMSH bytes of GET /mesh (glviewport.ts).
// - Linked models (ticket B5, ccore/session/links.h): the action `link_model` shows a file picker and
//   links the picked file's bytes read-only beside the open model (`ccoreLinkFile`; host_open flag 4);
//   `manage_links` with a body ({op, name}) goes to the module (`action_with`). Reload reads the file
//   again: the module keeps no path, so it answers an error (link the file again: it replaces).
// - Reference plans (ticket L1, ccore/session/links_ref.h): the action `link_pdf` shows a PDF picker
//   and adds the picked file's bytes to the active 2D view (`ccoreLinkPdf`; host_open flag 8);
//   `link_dxf` the same with a DXF picker (ticket L2: `ccoreLinkDxf`; host_open flag 16).
// - PDF export (ticket D7, ccore/session/sheet_svg.h): `bytes("query", "sheet_pdf" | "view_pdf", body)` answers
//   the PDF's bytes. SIZE1: Helvetica, nothing fetched (no font is shipped); `host_fonts` stays for a
//   page that hands Roboto files over, and `query pdf_fonts` says whether it did.
// - PERF2 (ccore/results/perf.md): an open is staged so no single task holds it all: the parse
//   (host_open / host_open_text), then the scene (`query status` builds it once per rev, calls.c
//   scene_now), then the BMSH (host_mesh, cached per rev in the session: mesh.c cc_mesh_get), each
//   in its own task with a yield between (`yieldTask`). The page hears of the new model (the change
//   feed) only after them, so its first queries and the viewport's first /mesh are served from the
//   caches. `OpenSteps` reports the step (read, parse, scene, mesh) and is cancelled before the parse.
// - Settings (ticket SET1, ccore/session/settings.h): the text of Rust's settings file kept in
//   localStorage `bimble.settings` (settings.rs:246), loaded at start (`settings_load`), stored again
//   after each `set_settings`.

// ---- SET1: the browser's copy of the settings file (settings.rs:246) ----
export const SETTINGS_KEY = "bimble.settings";
// ---- end SET1 ----
// ---- FS1: one code path for pickers and the unsaved question (files.ts) ----
import { pickFile } from "./files";
// ---- end FS1 ----
import { linkInAddress } from "./openurl"; // URL1

interface Exports {
  memory: WebAssembly.Memory;
  cc_alloc(n: number): number;
  cc_free(p: number): void;
  guid_entropy(p: number, n: number): void;
  host_open_text(np: number, nn: number, tp: number, tn: number, res: number): number;
  call(op: number, on: number, np: number, nn: number, bp: number, bn: number, res: number): number;
  change_json(res: number): number;
  host_mesh(flags: number, res: number): number;
  host_proxies(budget: number, res: number): number; // 3D2
  host_saved(res: number): number;
  host_saved_path(res: number): number;
  host_open(flags: number, np: number, nn: number, bp: number, bn: number, res: number): number;
  host_clock(unixS: number): void;
  host_log(res: number): number;
  host_fonts(rp: number, rn: number, bp: number, bn: number, res: number): number; // D7
  host_action_file?(np: number, nn: number, bp: number, bn: number, fp: number, fn: number, res: number): number; // LNK1
}

export interface CcoreInfo {
  /** Bytes of ccore.wasm as fetched (the response body, decoded). */
  wasmBytes: number;
  /** ms from navigation start: module ready, model open. */
  readyMs: number;
  openMs: number;
  model: string;
  modelBytes: number;
  /** GPU1: the module's memory now (bytes), for the performance panel. */
  memBytes?: () => number;
}
declare global {
  interface Window {
    __bbCcore?: CcoreInfo;
    ccoreSave?: () => string;
  }
}

const enc = new TextEncoder();
const dec = new TextDecoder();

export interface CcoreClient {
  query(name: string, argsJson: string): string;
  apply(cmdsJson: string): void;
  undo(): void;
  redo(): void;
  select(idsJson: string): void;
  action(name: string): void;
  verb(name: string, argsJson: string): void;
  tab(json: string): void;
  set_placing(json: string): void;
  dismiss_belongs(): void;
  standards_action(name: string): void;
  tool(name: string, json: string): string;
  action_with(name: string, json: string): string;
  /** D7: a call whose answer is bytes (`query sheet_pdf` / `view_pdf`: the PDF). */
  bytes(op: string, name: string, body: string): Promise<Uint8Array>;
  on_change(cb: (json: string) => void): void;
  default: () => Promise<unknown>;
}

/** DEAD1 (ADR 0074): a call the core refused (status 400), as the native host's answer shows in the
 *  browser's console and network log: told on the console (info level: a refusal is often the
 *  expected answer to a value typed) and kept, the last 100, in `window.__bbRefused` for the tools
 *  (ccore/tools/ux.mjs, the lab tests). */
export interface Refusal { op: string; name: string; status: number; text: string; at: number }
function refused(op: string, name: string | null, status: number, text: string): void {
  const w = window as unknown as { __bbRefused?: Refusal[] };
  const list = (w.__bbRefused ??= []);
  list.push({ op, name: name ?? "-", status, text: text.slice(0, 300), at: performance.now() });
  if (list.length > 100) list.splice(0, list.length - 100);
  console.info(`ccore: ${op} ${name ?? "-"} refused (${status}): ${text.slice(0, 300)}`);
}

let module: { X: Exports; res: number } | null = null;
/** The client's call (log, change feed, throws the error text); set by ccoreClient. */
let callHost: ((op: string, name: string | null, body?: string) => string) | null = null;
/** Reads the change feed and runs the listener when it changed; set by ccoreClient. */
let emitChange: (() => void) | null = null;

/** Copies `data` into the module; returns [ptr, len]. */
function put(X: Exports, data: Uint8Array): [number, number] {
  const p = X.cc_alloc(data.length);
  if (!p) throw new Error("ccore: out of memory");
  new Uint8Array(X.memory.buffer).set(data, p);
  return [p, data.length];
}

/** Runs f(...ptr,len pairs, res); returns the status and the answer bytes (copied, freed). */
function run(f: (...a: number[]) => number, args: Uint8Array[], lead: number[] = []): { status: number; bytes: Uint8Array; value: number } {
  if (!module) throw new Error("Bimble is not started");
  const { X, res } = module;
  const ins = args.map((a) => put(X, a));
  let status: number;
  try {
    status = f(...lead, ...ins.flat(), res);
  } finally {
    for (const [p] of ins) X.cc_free(p);
  }
  const dv = new DataView(X.memory.buffer);
  const ptr = dv.getUint32(res, true), len = dv.getUint32(res + 4, true), value = dv.getUint32(res + 8, true);
  const bytes = new Uint8Array(X.memory.buffer, ptr, len).slice();
  if (ptr) X.cc_free(ptr);
  return { status, bytes, value };
}

// ---- PERF2: an open in steps, each its own task ----
/** Gives the event loop a turn (input, paint) between two heavy steps: `scheduler.yield()` where
 *  the browser has it (its continuation runs first), else a message (not throttled as setTimeout is). */
export function yieldTask(): Promise<void> {
  const s = (globalThis as { scheduler?: { yield?: () => Promise<void> } }).scheduler;
  if (typeof s?.yield === "function") return s.yield();
  return new Promise((r) => { const c = new MessageChannel(); c.port1.onmessage = () => { c.port1.close(); r(); }; c.port2.postMessage(0); });
}
/** The steps of an open, in order (the shell's progress bar; the page's start line). */
export const OPEN_STEPS = ["read", "parse", "scene", "mesh"] as const;
export type OpenStep = (typeof OPEN_STEPS)[number];
export interface OpenSteps {
  /** Called when a step starts. */
  step?: (s: OpenStep) => void;
  /** True: the open is dropped. Asked before the parse only: after it, the core holds the new model. */
  cancelled?: () => boolean;
}
/** After a parse: the scene, then the BMSH, each in its own task (both cached in the session per
 *  rev, so the page's queries and the viewport's /mesh that follow are answered at once). An error
 *  here is not the open's: the page asks again and shows it as before. */
async function warm(X: Exports, steps: OpenSteps = {}): Promise<void> {
  await yieldTask();
  steps.step?.("scene");
  try { run(X.call.bind(X), [enc.encode("query"), enc.encode("status"), enc.encode("{}")]); } catch { /* asked again by the page */ }
  await yieldTask();
  steps.step?.("mesh");
  try { run(X.host_mesh.bind(X), [], [2 | 4]); } catch { /* asked again by the viewport (3D1: BMSH version 3; 3D2: bit 2, no copy) */ }
  await yieldTask();
}
// ---- end PERF2 ----

function hostLog(): void {
  if (!module) return;
  const t = dec.decode(run(module.X.host_log.bind(module.X), []).bytes);
  for (const line of t.split("\n")) if (line) console.info(line);
}

/** The BMSH bytes of the session's scene (native GET /mesh?shapes=1: version 3) and the revision they show. */
export function ccoreMesh(): { bytes: ArrayBuffer; rev: number } {
  if (!module) throw new Error("ccore: not loaded");
  const r = run(module.X.host_mesh.bind(module.X), [], [2]); // 3D1 (ADR 0078): BMSH version 3, shapes once
  if (r.status !== 200) throw new Error(`mesh: ${r.status} ${dec.decode(r.bytes)}`);
  return { bytes: r.bytes.buffer as ArrayBuffer, rev: r.value };
}

/** 3D2 (ADR 0078): the HLOD proxies in steps (host_proxies, session/mesh.h cc_mesh_proxies): `budget`
 *  triangles more a call; 202 while building, then 200 with the BPRX bytes (ccore/src/hlod.h). */
export function ccoreProxies(budget: number): { status: number; bytes: ArrayBuffer; rev: number } {
  if (!module) throw new Error("ccore: not loaded");
  const r = run(module.X.host_proxies.bind(module.X), [], [budget]);
  return { status: r.status, bytes: r.bytes.buffer as ArrayBuffer, rev: r.value };
}

/** Opens a file's bytes in the session: .bimble v1/v2 and .json as an editable model, .ifc and
 *  .bimbleview read-only (no model: the scene only), or with `importIfc` an .ifc imported as an
 *  editable copy. With unsaved changes and no `discard`, refused (the shell asks first: UX1).
 *  Throws the C core's error text when the file does not open. */
export async function ccoreOpenFile(file: Blob & { name?: string }, discard = false, importIfc = false, steps: OpenSteps = {}): Promise<boolean> {
  if (!module) throw new Error("ccore: not loaded");
  const { X } = module;
  // FS1: the question is the shell's (confirmClose, shell/ShellNotice.vue): without `discard`, a model
  // with unsaved changes is refused by the module, as the native host refuses it.
  steps.step?.("read");
  const bytes = new Uint8Array(await file.arrayBuffer());
  await yieldTask(); // PERF2: the Cancel pressed while reading is seen here
  if (steps.cancelled?.()) return false;
  const name = file.name || "model.bimble";
  steps.step?.("parse");
  const r = run(X.host_open.bind(X), [enc.encode(name), bytes], [(discard ? 1 : 0) | (importIfc ? 2 : 0)]);
  hostLog();
  if (r.status === 200) await warm(X, steps); // PERF2: the scene and the mesh, each in its own task
  emitChange?.();
  if (r.status !== 200) throw dec.decode(r.bytes) || `ccore open: ${r.status}`;
  return true;
}

/** Links a file's bytes read-only beside the open model (`action link_model`, host_open flag 4): an
 *  .ifc, a .bimbleview, a .bimble or .json. The same file name again replaces that link. Throws the
 *  C core's error text when it does not load. */
export function ccoreLinkFile(name: string, bytes: Uint8Array): void {
  if (!module) throw new Error("ccore: not loaded");
  const { X } = module;
  const r = run(X.host_open.bind(X), [enc.encode(name), bytes], [4]);
  hostLog();
  emitChange?.();
  if (r.status !== 200) throw dec.decode(r.bytes) || `ccore link: ${r.status}`;
}

/** L1: adds a PDF's bytes to the active 2D view as a reference plan (`action link_pdf`, host_open
 *  flag 8, the scale in the bits above: 0 = the view's). Throws the C core's error text. */
export function ccoreLinkPdf(name: string, bytes: Uint8Array, scale = 0): void {
  if (!module) throw new Error("ccore: not loaded");
  const { X } = module;
  const r = run(X.host_open.bind(X), [enc.encode(name), bytes], [8 | (Math.max(0, Math.min(100000, Math.round(scale))) << 8)]);
  hostLog();
  emitChange?.();
  if (r.status !== 200) throw dec.decode(r.bytes) || `ccore link_pdf: ${r.status}`;
}

/** L2: adds a DXF's bytes to the active 2D view as a reference plan (`action link_dxf`, host_open
 *  flag 16). Throws the C core's error text. */
export function ccoreLinkDxf(name: string, bytes: Uint8Array): void {
  if (!module) throw new Error("ccore: not loaded");
  const { X } = module;
  const r = run(X.host_open.bind(X), [enc.encode(name), bytes], [16]);
  hostLog();
  emitChange?.();
  if (r.status !== 200) throw dec.decode(r.bytes) || `ccore link_dxf: ${r.status}`;
}

// ---- LNK1 ---- an action whose file is the bytes given (the module keeps no path): the Links
// panel's Reload / Reload from… of a reference plan and Locate… of a missing linked model
// (wasm_api.h host_action_file). Throws the C core's error text.
export function ccoreActionFile(action: string, body: unknown, bytes: Uint8Array): void {
  if (!module) throw new Error("ccore: not loaded");
  const { X } = module;
  if (!X.host_action_file) throw new Error("this C core has no host_action_file");
  const r = run(X.host_action_file.bind(X), [enc.encode(action), enc.encode(JSON.stringify(body)), bytes]);
  hostLog();
  emitChange?.();
  if (r.status !== 200) throw dec.decode(r.bytes) || `ccore ${action}: ${r.status}`;
}
// ---- end LNK1 ----
/** PERF2: builds the session's BMSH now, in a task of its own (cached per rev and links: the
 *  viewport's next /mesh is a copy). After a link: the parse was the link's task, the mesh is this one. */
export async function ccoreWarmMesh(): Promise<void> {
  if (!module) return;
  await yieldTask();
  const { X } = module;
  try { run(X.host_mesh.bind(X), [], [2 | 4]); } catch { /* asked again by the viewport (3D1: BMSH version 3; 3D2: no copy) */ }
}

/** The bytes the module wrote last and their file name (host_saved, host_saved_path). */
function lastWritten(): { name: string; bytes: Uint8Array } {
  if (!module) throw new Error("ccore: not loaded");
  const { X } = module;
  const b = run(X.host_saved.bind(X), []);
  if (b.status !== 200) throw new Error("ccore: nothing written");
  const path = dec.decode(run(X.host_saved_path.bind(X), []).bytes);
  return { name: path.split("/").pop() || "model.bimble", bytes: b.bytes };
}

/** Save as under the current name (the module has no paths): the file's bytes (v2, or the v1
 *  text for a .json name) and its name. The model is saved after. */
export function ccoreSaveBytes(): { name: string; bytes: Uint8Array } {
  if (!callHost) throw new Error("ccore: not loaded");
  callHost("action", "save_as", "{}");
  return lastWritten();
}

/** The model as IFC (`export_ifc4` / `export_ifc2x3`): the STEP text and its file name. */
export function ccoreExportIfc(schema: "ifc4" | "ifc2x3" = "ifc4"): { name: string; text: string } {
  if (!callHost) throw new Error("ccore: not loaded");
  callHost("action", `export_${schema}`, "{}");
  const w = lastWritten();
  return { name: w.name, text: dec.decode(w.bytes) };
}

/** The viewer file of the open model (`share_for_viewing`): the gzipped .bimbleview bytes and
 *  their file name ("<stem>.bimbleview"). */
export function ccoreShareForViewing(): { name: string; bytes: Uint8Array } {
  if (!callHost) throw new Error("ccore: not loaded");
  callHost("action", "share_for_viewing", "{}");
  return lastWritten();
}

/** Hands bytes to the user as a file download (lab only: Blob + `<a download>`). */
export function ccoreDownload(name: string, data: Uint8Array | string, type = "application/octet-stream"): void {
  const blob = new Blob([typeof data === "string" ? data : (data.slice().buffer as ArrayBuffer)], { type });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

// ---- SIZE1: no font fetch (D7 fetched ccore/fonts/ on the first export; the files are not shipped) ----

/** The file picker of `link_model`; L1: of `link_pdf`; L2: of `link_dxf` (FS1: files.ts pickFile). */
function pickAndLink(purpose: "link_model" | "link_pdf" | "link_dxf"): void {
  void pickFile(purpose).then((r) => {
    const f = r?.file;
    if (!f) return;
    const link = purpose === "link_model" ? ccoreLinkFile : purpose === "link_pdf" ? ccoreLinkPdf : ccoreLinkDxf;
    void f.arrayBuffer().then((b) => link(f.name, new Uint8Array(b)))
      .then(() => { if (purpose === "link_model") return ccoreWarmMesh(); }) // PERF2: the new mesh in its own task, not in the viewport's frame
      .catch((e) => console.error(`ccore: link ${f.name}: ${String(e)}`));
  });
}

/** The file actions in the page (the rest go to the module as they are). */
function fileAction(name: string): boolean {
  if (!callHost) return false;
  switch (name) {
    case "link_model":
      if (!(JSON.parse(callHost("query", "actions", "{}")) as { name: string; enabled: boolean }[]).find((a) => a.name === "link_model")?.enabled)
        callHost("action", "link_model"); // nothing open: throws "not enabled", as the native host answers
      pickAndLink("link_model");
      return true;
    case "link_pdf": // L1
      if (!(JSON.parse(callHost("query", "actions", "{}")) as { name: string; enabled: boolean }[]).find((a) => a.name === "link_pdf")?.enabled)
        callHost("action", "link_pdf"); // nothing editable open: throws "not enabled", as the native host answers
      pickAndLink("link_pdf");
      return true;
    case "link_dxf": // L2
      if (!(JSON.parse(callHost("query", "actions", "{}")) as { name: string; enabled: boolean }[]).find((a) => a.name === "link_dxf")?.enabled)
        callHost("action", "link_dxf"); // nothing editable open: throws "not enabled", as the native host answers
      pickAndLink("link_dxf");
      return true;
    case "save_as": // FS1: Save as in the browser: a download under the current name
      { const w = ccoreSaveBytes(); ccoreDownload(w.name, w.bytes); }
      return true;
    case "save": {
      callHost("action", "save"); // throws when not enabled, as the native host answers
      const w = lastWritten();
      ccoreDownload(w.name, w.bytes);
      return true;
    }
    case "export_ifc4":
    case "export_ifc2x3": {
      const e = ccoreExportIfc(name === "export_ifc4" ? "ifc4" : "ifc2x3");
      ccoreDownload(e.name, e.text, "application/x-step");
      return true;
    }
    case "share_for_viewing": {
      const v = ccoreShareForViewing();
      ccoreDownload(v.name, v.bytes, "application/gzip");
      return true;
    }
  }
  return false;
}

/** Loads the module, opens the model and returns a `Wasm` for bimble.ts. */
export async function ccoreClient(params: URLSearchParams, steps: OpenSteps = {}): Promise<CcoreClient> {
  const base = document.baseURI;
  // ---- URL1 (ADR 0086): in a built page, ?ccore_wasm= and ?model= name a file of this site only (a
  // link fetches no other address without a click: another site's model opens with #open=, which asks).
  // With a link (#open= / ?open=) the start opens no model: the link's model is the first (openurl.ts). ----
  const here = (p: string | null) => { if (p === null || !import.meta.env.PROD) return p; try { if (new URL(p, base).origin === location.origin) return p; } catch { /* refused below */ } console.warn(`ccore: ${p}: not on this site, ignored (open another site's model with #open=<url>)`); return null; };
  const wasmUrl = new URL(here(params.get("ccore_wasm")) ?? "ccore/ccore.wasm", base).href;
  const modelUrl = linkInAddress() ? null : new URL(here(params.get("model")) ?? "ccore/frame.json", base).href;
  // ---- end URL1 ----
  steps.step?.("read");
  const [wr, mr] = await Promise.all([fetch(wasmUrl), modelUrl === null ? null : fetch(modelUrl)]);
  if (!wr.ok) throw new Error(`ccore: ${wasmUrl}: HTTP ${wr.status} (run npm run ccore:wasm)`);
  if (mr && !mr.ok) throw new Error(`ccore: ${modelUrl}: HTTP ${mr.status}`);
  const wasmBuf = await wr.arrayBuffer();
  const { instance } = await WebAssembly.instantiate(wasmBuf, {});
  const X = instance.exports as unknown as Exports;
  module = { X, res: X.cc_alloc(12) };
  const readyMs = performance.now();
  const seed = crypto.getRandomValues(new Uint8Array(32));
  const [sp, sn] = put(X, seed);
  X.guid_entropy(sp, sn);
  X.cc_free(sp);

  const text = mr ? new Uint8Array(await mr.arrayBuffer()) : new Uint8Array(0);
  const name = modelUrl ? decodeURIComponent(new URL(modelUrl).pathname.split("/").pop() || "model.bimble") : "";
  if (mr && modelUrl) { // URL1: none with a link
    await yieldTask(); // PERF2: the instantiation and the parse in two tasks
    steps.step?.("parse");
    const o = run(X.host_open_text.bind(X), [enc.encode(name), text]);
    if (o.status !== 200) throw new Error(`ccore: ${name}: ${dec.decode(o.bytes)}`);
  }
  window.__bbCcore = { wasmBytes: wasmBuf.byteLength, readyMs, openMs: performance.now(), model: name, modelBytes: text.length, memBytes: () => X.memory.buffer.byteLength }; // GPU1: memBytes
  if (mr) await warm(X, steps); // PERF2: the scene and the mesh, each in its own task, before the page asks

  let listener: ((json: string) => void) | null = null;
  let last = "";
  const emit = () => {
    const c = dec.decode(run(X.change_json.bind(X), []).bytes);
    if (c !== last) {
      last = c;
      listener?.(c);
    }
  };
  const call = (op: string, name: string | null, body?: string): string => {
    X.host_clock(Date.now() / 1000); // the module's wall clock (IFC export stamps)
    const r = run(X.call.bind(X), [enc.encode(op), enc.encode(name ?? "-"), enc.encode(body ?? "")]);
    hostLog();
    emit();
    const t = dec.decode(r.bytes);
    if (r.status !== 200) { // as wasm: the error text as a string
      refused(op, name, r.status, t); // DEAD1: seen, as the native host's 400 is (the browser logs it)
      throw t || `ccore ${op}: ${r.status}`;
    }
    return t;
  };
  callHost = call;
  emitChange = emit;
  // ---- SET1: the user's settings (ccore/session/settings.h). The module has no folder: the page keeps
  // the file's text under the Rust viewer's own key (settings.rs:246 `bimble.settings`, Rust's JSON form),
  // hands it in at start and stores it again after each change. The viewer-only build keeps none. ----
  try {
    const kept = localStorage.getItem(SETTINGS_KEY);
    if (kept !== null) call("action", "settings_load", JSON.stringify({ text: kept }));
  } catch (e) { console.info(`ccore: settings not loaded: ${String(e)}`); }
  const keepSettings = () => {
    try {
      const text = JSON.parse(call("query", "settings_text", "{}")) as string | null;
      if (text !== null) localStorage.setItem(SETTINGS_KEY, text);
    } catch (e) { console.warn(`ccore: settings not kept: ${String(e)}`); }
  };
  // ---- end SET1 ----
  window.ccoreSave = () => {
    // Save is enabled only with unsaved changes (as the native host): else the last saved text.
    if ((JSON.parse(last || "{}") as { unsaved?: boolean }).unsaved) call("action", "save");
    const r = run(X.host_saved.bind(X), []);
    if (r.status !== 200) throw new Error("ccore: nothing saved yet (no change since the model was opened)");
    console.info(`ccore: saved ${dec.decode(run(X.host_saved_path.bind(X), []).bytes)}, ${r.bytes.length} bytes (ccoreSave() returns the text)`);
    return dec.decode(r.bytes);
  };
  return {
    default: async () => undefined,
    query: (n, argsJson) => call("query", n, argsJson),
    apply: (cmdsJson) => void call("apply", null, cmdsJson),
    undo: () => void call("undo", null),
    redo: () => void call("redo", null),
    select: (idsJson) => void call("select", null, idsJson),
    action: (n) => void (fileAction(n) || call("action", n)),
    verb: (n, argsJson) => void call("verb", n, argsJson),
    tab: (json) => void call("tab", null, json),
    set_placing: (json) => void call("set_placing", null, json),
    dismiss_belongs: () => void call("dismiss_belongs", null),
    standards_action: (n) => void call("standards_action", n),
    tool: (n, json) => call("tool", n, json),
    action_with: (n, json) => { const r = call("action", n, json); if (n === "set_settings") keepSettings(); return r; }, // SET1
    bytes: async (op, n, body) => { // D7: the answer's bytes as they are (the PDF)
      X.host_clock(Date.now() / 1000);
      const r = run(X.call.bind(X), [enc.encode(op), enc.encode(n), enc.encode(body)]);
      hostLog();
      if (r.status !== 200) throw dec.decode(r.bytes) || `ccore ${op}: ${r.status}`;
      return r.bytes;
    },
    on_change: (cb) => {
      listener = cb;
      last = "";
      queueMicrotask(emit); // the first change, as the native host publishes one at start
    },
  };
}
