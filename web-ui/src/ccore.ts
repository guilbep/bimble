// The C core in the browser (ticket T13b, `?ccore=1`): a `Wasm` (bimble.ts) over ccore.wasm, the
// freestanding wasm32 build of ccore/ (ccore/build.sh wasm; exports in ccore/src/wasm_api.h). The
// module holds the same call code as the native C host (ccore/session/calls.c): every call is
// `call(op, name, body)`, exactly a `POST /<op>/<name>` of the host, with no server at all.
// - Loads `ccore/ccore.wasm` (relative to the page; `npm run ccore:wasm` puts it in public/ccore/)
//   or `?ccore_wasm=<url>`, and seeds the id generator from crypto.getRandomValues.
// - Opens `?model=<url>` (a .bimble JSON text form) or the built-in demo `ccore/frame.json`.
// - on_change: the change JSON is read after every call and the callback runs when it changed.
// - Save: the module has no files; `action save` keeps the text in the module. `window.ccoreSave()`
//   saves (when there are unsaved changes) and returns the last saved text, and logs its size.
// - The mesh for the WebGL2 viewport: `ccoreMesh()`, the BMSH bytes of GET /mesh (glviewport.ts).

interface Exports {
  memory: WebAssembly.Memory;
  cc_alloc(n: number): number;
  cc_free(p: number): void;
  guid_entropy(p: number, n: number): void;
  host_open_text(np: number, nn: number, tp: number, tn: number, res: number): number;
  call(op: number, on: number, np: number, nn: number, bp: number, bn: number, res: number): number;
  change_json(res: number): number;
  host_mesh(flags: number, res: number): number;
  host_saved(res: number): number;
  host_log(res: number): number;
}

export interface CcoreInfo {
  /** Bytes of ccore.wasm as fetched (the response body, decoded). */
  wasmBytes: number;
  /** ms from navigation start: module ready, model open. */
  readyMs: number;
  openMs: number;
  model: string;
  modelBytes: number;
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
  on_change(cb: (json: string) => void): void;
  default: () => Promise<unknown>;
}

let module: { X: Exports; res: number } | null = null;

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

function hostLog(): void {
  if (!module) return;
  const t = dec.decode(run(module.X.host_log.bind(module.X), []).bytes);
  for (const line of t.split("\n")) if (line) console.info(line);
}

/** The BMSH bytes of the session's scene (native GET /mesh) and the revision they show. */
export function ccoreMesh(): { bytes: ArrayBuffer; rev: number } {
  if (!module) throw new Error("ccore: not loaded");
  const r = run(module.X.host_mesh.bind(module.X), [], [0]);
  if (r.status !== 200) throw new Error(`mesh: ${r.status} ${dec.decode(r.bytes)}`);
  return { bytes: r.bytes.buffer as ArrayBuffer, rev: r.value };
}

/** Loads the module, opens the model and returns a `Wasm` for bimble.ts. */
export async function ccoreClient(params: URLSearchParams): Promise<CcoreClient> {
  const base = document.baseURI;
  const wasmUrl = new URL(params.get("ccore_wasm") ?? "ccore/ccore.wasm", base).href;
  const modelUrl = new URL(params.get("model") ?? "ccore/frame.json", base).href;
  const [wr, mr] = await Promise.all([fetch(wasmUrl), fetch(modelUrl)]);
  if (!wr.ok) throw new Error(`ccore: ${wasmUrl}: HTTP ${wr.status} (run npm run ccore:wasm)`);
  if (!mr.ok) throw new Error(`ccore: ${modelUrl}: HTTP ${mr.status}`);
  const wasmBuf = await wr.arrayBuffer();
  const { instance } = await WebAssembly.instantiate(wasmBuf, {});
  const X = instance.exports as unknown as Exports;
  module = { X, res: X.cc_alloc(12) };
  const readyMs = performance.now();
  const seed = crypto.getRandomValues(new Uint8Array(32));
  const [sp, sn] = put(X, seed);
  X.guid_entropy(sp, sn);
  X.cc_free(sp);

  const text = new Uint8Array(await mr.arrayBuffer());
  const name = decodeURIComponent(new URL(modelUrl).pathname.split("/").pop() || "model.bimble");
  const o = run(X.host_open_text.bind(X), [enc.encode(name), text]);
  if (o.status !== 200) throw new Error(`ccore: ${name}: ${dec.decode(o.bytes)}`);
  window.__bbCcore = { wasmBytes: wasmBuf.byteLength, readyMs, openMs: performance.now(), model: name, modelBytes: text.length };

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
    const r = run(X.call.bind(X), [enc.encode(op), enc.encode(name ?? "-"), enc.encode(body ?? "")]);
    hostLog();
    emit();
    const t = dec.decode(r.bytes);
    if (r.status !== 200) throw t || `ccore ${op}: ${r.status}`; // as wasm: the error text as a string
    return t;
  };
  window.ccoreSave = () => {
    // Save is enabled only with unsaved changes (as the native host): else the last saved text.
    if ((JSON.parse(last || "{}") as { unsaved?: boolean }).unsaved) call("action", "save");
    const r = run(X.host_saved.bind(X), []);
    if (r.status !== 200) throw new Error("ccore: nothing saved yet (no change since the model was opened)");
    console.info(`ccore: saved ${name}, ${r.bytes.length} bytes (ccoreSave() returns the text)`);
    return dec.decode(r.bytes);
  };
  return {
    default: async () => undefined,
    query: (n, argsJson) => call("query", n, argsJson),
    apply: (cmdsJson) => void call("apply", null, cmdsJson),
    undo: () => void call("undo", null),
    redo: () => void call("redo", null),
    select: (idsJson) => void call("select", null, idsJson),
    action: (n) => void call("action", n),
    verb: (n, argsJson) => void call("verb", n, argsJson),
    tab: (json) => void call("tab", null, json),
    set_placing: (json) => void call("set_placing", null, json),
    dismiss_belongs: () => void call("dismiss_belongs", null),
    standards_action: (n) => void call("standards_action", n),
    on_change: (cb) => {
      listener = cb;
      last = "";
      queueMicrotask(emit); // the first change, as the native host publishes one at start
    },
  };
}
