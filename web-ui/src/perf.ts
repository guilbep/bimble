// The performance panel's numbers (ticket GPU1, ADR 0083): one source of truth for what the panel
// shows and "Copy report" copies. The viewport (glviewport.ts) calls `perfFrame` once per frame it
// draws and `perfMesh` once per mesh it shows; the rest is read from `window.__bbGl` (the counters
// ccore/tools/frames.mjs and ux.mjs read too), `window.__bbCcore` (wasm memory) and the renderer
// switch (gpu/switch.ts: which renderer draws, its adapter, why WebGPU fell back).
//
// Frame time: from the start of the frame's draw to its pixels. While the panel is open (or a
// recording runs) the viewport waits for the GPU after each frame (WebGL2: a 1x1 readPixels;
// WebGPU: onSubmittedWorkDone), so the time includes the GPU's work; when it is closed nothing waits
// and only the submit time is known (the input -> frame list then says "submitted").
// Inputs: a press, a drag move, a wheel step, a click or a key on the viewport; its time to the end
// of the next frame drawn (dropped when no frame comes within 2 s: it changed nothing on screen).
// Nothing here is sent anywhere; file names leave the page only in the copied text, when asked.

/** One frame drawn: when it started (performance.now), what it took (ms), moving or still, what it drew. */
export interface PerfFrame { at: number; ms: number; moving: boolean; tris: number; draws: number; synced: boolean }
export interface PerfInput { what: string; ms: number; at: number; synced: boolean }
export interface PerfAdapter { vendor: string; architecture: string; description: string; software: boolean }
/** What draws the 3D view: asked (the setting or ?gpu=), in use, the adapter, why WebGPU is not in use. */
export interface PerfRenderer { asked: "auto" | "webgl2" | "webgpu"; using: "WebGL2" | "WebGPU"; adapter: PerfAdapter | null; reason: string | null; gpuCull: boolean }
export interface PerfLink { name: string; elements: number; triangles: number; visible: boolean;
  /** LNK3D (ADR 0078 amendment 3): its detail on the GPU (false: its proxies only; null: not told) and its GPU bytes. */
  detail?: boolean | null; gpuBytes?: number | null }

const RING = 120;
const frames: PerfFrame[] = [];
const inputs: PerfInput[] = [];
let pending: { what: string; at: number } | null = null;
/** The last open (navigation start for the page's own model), its first mesh, that mesh's first frame's end and
 *  complete picture (later meshes of the same file, after an edit, do not move them). */
let openAt = 0, openMesh: number | null = null, firstAt: number | null = null, completeAt: number | null = null, meshElements = 0, meshTris = 0;
let links: PerfLink[] = [];
let glAdapter: PerfAdapter | null = null;
const px = new Uint8Array(4);
/** Panels and recordings that want the GPU waited for (a count: the panel open, a recording). */
let waiters = 0;
let recording: { until: number; frames: PerfFrame[]; inputs: PerfInput[] } | null = null;
let lastRecord: { seconds: number; frames: PerfFrame[]; inputs: PerfInput[] } | null = null;

export const renderer: PerfRenderer = { asked: "auto", using: "WebGL2", adapter: null, reason: null, gpuCull: false };

/** The words that mean a CPU rasteriser behind the adapter (SwiftShader, llvmpipe, Microsoft's Basic Render). */
export const isSoftware = (s: string): boolean => /swiftshader|llvmpipe|softpipe|lavapipe|software|basic render/i.test(s);

/** The WebGL2 context's adapter (WEBGL_debug_renderer_info; the masked names without it). */
function webglAdapter(gl: WebGL2RenderingContext): PerfAdapter {
  const ext = gl.getExtension("WEBGL_debug_renderer_info");
  const vendor = String(ext ? gl.getParameter(ext.UNMASKED_VENDOR_WEBGL) : gl.getParameter(gl.VENDOR));
  const description = String(ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER));
  return { vendor, architecture: "", description, software: isSoftware(`${vendor} ${description}`) };
}

/** Whether the viewport waits for the GPU after each frame (the panel is open or a recording runs). */
export const perfWaits = (): boolean => waiters > 0;
export function perfWait(on: boolean): void { waiters = Math.max(0, waiters + (on ? 1 : -1)); }

/** A frame drawn by the viewport, started at `t0`. `sync`: the WebGL2 context (a 1x1 read waits for
 *  its pixels), or a promise of the GPU's end (WebGPU), or null (an overlay-only frame). */
export function perfFrame(moving: boolean, t0: number, sync: WebGL2RenderingContext | Promise<unknown> | null): void {
  const g = window.__bbGl;
  const wait = perfWaits();
  if (sync && !(sync instanceof Promise)) {
    if (!glAdapter) glAdapter = webglAdapter(sync);
    if (wait) sync.readPixels(0, 0, 1, 1, sync.RGBA, sync.UNSIGNED_BYTE, px);
  }
  const f: PerfFrame = { at: t0, ms: 0, moving, tris: g?.frameTris ?? 0, draws: g?.frameDraws ?? 0, synced: wait && sync !== null };
  const done = () => {
    f.ms = performance.now() - t0;
    push(f);
  };
  if (sync instanceof Promise && wait) void sync.then(done, done);
  else done();
}

function push(f: PerfFrame): void {
  frames.push(f);
  if (frames.length > RING) frames.splice(0, frames.length - RING);
  if (recording) recording.frames.push(f), endRecord();
  const g = window.__bbGl;
  if (openMesh === null && g?.meshAt !== undefined && g.meshAt >= openAt) openMesh = g.meshAt, firstAt = completeAt = null;
  if (openMesh !== null && firstAt === null && f.at >= openMesh) firstAt = f.at + f.ms;
  if (openMesh !== null && completeAt === null && g?.fullFrameMs != null && g.fullFrameMs >= openMesh) completeAt = g.fullFrameMs === f.at ? f.at + f.ms : g.fullFrameMs; // the frame's end
  if (pending) {
    const end = f.at + f.ms;
    if (end - pending.at > 2000) pending = null;
    else if (f.at >= pending.at - 1) {
      const i: PerfInput = { what: pending.what, ms: end - pending.at, at: pending.at, synced: f.synced };
      pending = null;
      inputs.push(i);
      inputs.sort((a, b) => b.ms - a.ms);
      if (inputs.length > 10) inputs.length = 10;
      recording?.inputs.push(i);
    }
  }
}

/** The mesh shown: its elements and triangles, per link too (the open model's elements are `model` 0). */
export function perfMesh(m: { ids: string[]; ni: Uint32Array; model?: Uint8Array; links?: { name: string; visible: boolean }[] }): void {
  const n = m.links?.length ?? 0, el = new Array<number>(n + 1).fill(0), tr = new Array<number>(n + 1).fill(0);
  for (let e = 0; e < m.ids.length; e++) { const k = m.model?.[e] ?? 0; el[k]! += 1; tr[k]! += (m.ni[e] ?? 0) / 3; }
  meshElements = el[0]!, meshTris = tr[0]!;
  links = (m.links ?? []).map((l, k) => ({ name: l.name, elements: el[k + 1]!, triangles: tr[k + 1]!, visible: l.visible }));
}

/** The shell started opening a file (send.ts): the next mesh's "open -> first frame" counts from here. */
export function perfOpening(): void { openAt = performance.now(), openMesh = firstAt = completeAt = null; }

/** Watches the viewport canvas's inputs (once). */
export function perfInputs(canvas: HTMLElement): void {
  let down: { x: number; y: number; moved: boolean } | null = null;
  const note = (what: string, at: number) => { if (!pending) pending = { what, at }; };
  canvas.addEventListener("pointerdown", (e) => { down = { x: e.clientX, y: e.clientY, moved: false }; note(`press (button ${e.button})`, e.timeStamp); }, true);
  canvas.addEventListener("pointermove", (e) => {
    if (!down || !e.buttons) return;
    if (Math.hypot(e.clientX - down.x, e.clientY - down.y) > 4) down.moved = true;
    if (down.moved) note(e.buttons & 1 && !e.shiftKey ? "drag (orbit)" : "drag (pan)", e.timeStamp);
  }, true);
  canvas.addEventListener("pointerup", (e) => { if (down && !down.moved) note(`click (button ${e.button})`, e.timeStamp); down = null; }, true);
  canvas.addEventListener("wheel", (e) => note("wheel (zoom)", e.timeStamp), { capture: true, passive: true });
  window.addEventListener("keydown", (e) => { if (!(e.target instanceof HTMLInputElement) && !(e.target instanceof HTMLTextAreaElement)) note(`key ${e.key}`, e.timeStamp); }, true);
}

/** Records every frame and input for 10 s (the GPU waited for meanwhile); `done` when it ends. */
export function perfRecord(): void {
  if (recording) return;
  perfWait(true);
  recording = { until: performance.now() + 10000, frames: [], inputs: [] };
  lastRecord = null;
  setTimeout(endRecord, 10050); // a still view draws no frame: the recording still ends
}
function endRecord(): void {
  if (!recording || performance.now() < recording.until) return;
  lastRecord = { seconds: 10, frames: recording.frames, inputs: recording.inputs };
  recording = null;
  perfWait(false);
}
export const perfRecording = (): boolean => recording !== null;

const quantile = (xs: number[], q: number): number => {
  if (!xs.length) return NaN;
  const s = [...xs].sort((a, b) => a - b);
  return s[Math.min(s.length - 1, Math.floor(q * s.length))]!;
};
export interface FrameStats { n: number; last: number; median: number; p95: number; max: number }
const stats = (fs: PerfFrame[]): FrameStats => {
  const ms = fs.map((f) => f.ms);
  return { n: fs.length, last: ms[ms.length - 1] ?? NaN, median: quantile(ms, 0.5), p95: quantile(ms, 0.95), max: ms.length ? Math.max(...ms) : NaN };
};

/** What the panel shows now. */
export interface PerfSnapshot {
  renderer: PerfRenderer; adapter: PerfAdapter | null; moving: FrameStats; still: FrameStats; synced: boolean;
  tris: number; draws: number; batches: number; culled: number; proxiesDrawn: number; proxyTris: number; gpuBytes: number | null;
  jsHeap: number | null; wasmBytes: number | null; elements: number; triangles: number;
  openToFirst: number | null; openToComplete: number | null; links: PerfLink[]; inputs: PerfInput[]; frames: number;
  record: { seconds: number; moving: FrameStats; still: FrameStats; inputs: PerfInput[] } | null; recording: boolean;
  canvas: { w: number; h: number; dpr: number };
}

export function perfSnapshot(): PerfSnapshot {
  const g = window.__bbGl;
  const mem = (performance as unknown as { memory?: { usedJSHeapSize: number } }).memory;
  const cc = (window as unknown as { __bbCcore?: { memBytes?: () => number } }).__bbCcore;
  const c = document.getElementById("bimble") as HTMLCanvasElement | null;
  return {
    renderer, adapter: renderer.using === "WebGPU" ? renderer.adapter : glAdapter,
    moving: stats(frames.filter((f) => f.moving)), still: stats(frames.filter((f) => !f.moving)), synced: frames.some((f) => f.synced),
    tris: g?.frameTris ?? 0, draws: g?.frameDraws ?? 0, batches: g?.moving?.batches ?? 0, culled: g?.moving?.culled ?? 0,
    proxiesDrawn: g?.proxies?.drawnCells ?? 0, proxyTris: g?.proxies?.drawnTris ?? 0, gpuBytes: g?.gpuBytes ?? null,
    jsHeap: mem ? mem.usedJSHeapSize : null, wasmBytes: cc?.memBytes ? cc.memBytes() : null,
    elements: meshElements, triangles: meshTris,
    openToFirst: firstAt !== null ? firstAt - openAt : null,
    openToComplete: completeAt !== null ? completeAt - openAt : null,
    links: links.map((l) => { // LNK3D: shown, detail and bytes as the viewport has them now (the memory budget)
      const d = g?.linkDetail?.find((x) => x.name === l.name), w = g?.linkDraw?.find((x) => x.name === l.name);
      return { ...l, visible: w ? w.visible : l.visible, detail: d ? d.detail : null, gpuBytes: d ? d.bytes : null };
    }), inputs: [...inputs], frames: g?.frames ?? 0,
    record: lastRecord ? { seconds: lastRecord.seconds, moving: stats(lastRecord.frames.filter((f) => f.moving)), still: stats(lastRecord.frames.filter((f) => !f.moving)), inputs: lastRecord.inputs } : null,
    recording: recording !== null,
    canvas: { w: c?.width ?? 0, h: c?.height ?? 0, dpr: window.devicePixelRatio || 1 },
  };
}

const ms = (x: number | null) => (x === null || !Number.isFinite(x) ? "-" : `${x < 10 ? x.toFixed(1) : Math.round(x)} ms`);
const mb = (x: number | null) => (x === null ? "-" : `${(x / 1048576).toFixed(1)} MB`);
const num = (x: number) => Math.round(x).toLocaleString("en-US");
const fline = (s: FrameStats) => (s.n ? `n ${s.n}, last ${ms(s.last)}, median ${ms(s.median)}, p95 ${ms(s.p95)}, max ${ms(s.max)}` : "none");

/** The report as plain text (English: it goes into bug reports). File names only with `names`. */
export function perfReport(names: boolean, file: string | null): string {
  const s = perfSnapshot(), r = s.renderer, a = s.adapter;
  const L: string[] = [];
  L.push("Bimble performance report");
  L.push(`date: ${new Date().toISOString()}; page: ${location.search.includes("ccore=1") || import.meta.env.VITE_CCORE === "1" ? "wasm" : location.search.includes("native=1") ? "native host" : "other"}; user agent: ${navigator.userAgent}`);
  L.push(`renderer: ${r.using} (asked: ${r.asked})${r.reason ? `; WebGPU not used: ${r.reason}` : ""}${r.using === "WebGPU" ? `; GPU culling of copies: ${r.gpuCull ? "on" : "off"}` : ""}`);
  L.push(`adapter: ${a ? `${[a.vendor, a.architecture, a.description].filter(Boolean).join(" / ") || "(no name given)"}${a.software ? " - SOFTWARE (a CPU rasteriser, not a GPU)" : ""}` : "unknown"}`);
  L.push(`canvas: ${s.canvas.w} x ${s.canvas.h} px, device pixel ratio ${s.canvas.dpr}`);
  L.push(`frame time (draw to pixels${s.synced ? "" : ", NOT waited for: submit time only"}), last ${RING} frames:`);
  L.push(`  moving: ${fline(s.moving)}`);
  L.push(`  still:  ${fline(s.still)}`);
  L.push(`last frame: ${num(s.tris)} triangles, ${num(s.draws)} draw calls; batches ${num(s.batches)}, culled ${num(s.culled)}; proxies drawn ${num(s.proxiesDrawn)} (${num(s.proxyTris)} triangles)`);
  L.push(`memory: GPU buffers ${mb(s.gpuBytes)}, JS heap ${mb(s.jsHeap)}, wasm ${mb(s.wasmBytes)}`);
  L.push(`model${names && file ? ` ${file}` : ""}: ${num(s.elements)} elements, ${num(s.triangles)} triangles; open -> first frame ${ms(s.openToFirst)}, -> complete picture ${ms(s.openToComplete)}`);
  if (s.links.length) {
    L.push(`links (${s.links.length}):`);
    s.links.forEach((l, k) => L.push(`  ${names ? l.name : `link ${k + 1}`}: ${num(l.elements)} elements, ${num(l.triangles)} triangles, ${l.visible ? "shown" : "hidden"}` +
      (l.detail === true ? `, detail loaded (${mb(l.gpuBytes ?? null)})` : l.detail === false ? ", proxies only" : "")));
  }
  L.push("slowest input -> frame (this session):");
  if (!s.inputs.length) L.push("  none yet");
  s.inputs.forEach((i, k) => L.push(`  ${k + 1}. ${ms(i.ms)} ${i.what}${i.synced ? "" : " (submitted)"}`));
  if (s.record) {
    L.push(`recording (${s.record.seconds} s):`);
    L.push(`  moving: ${fline(s.record.moving)}`);
    L.push(`  still:  ${fline(s.record.still)}`);
    L.push(`  inputs: ${s.record.inputs.length}, slowest ${ms(s.record.inputs.length ? Math.max(...s.record.inputs.map((i) => i.ms)) : null)}`);
  }
  return L.join("\n") + "\n";
}

declare global {
  interface Window {
    /** GPU1: the performance panel's source (perf.ts), for the lab tests. */
    __bbPerf?: { snapshot: typeof perfSnapshot; report: typeof perfReport; record: typeof perfRecord };
  }
}
window.__bbPerf = { snapshot: perfSnapshot, report: perfReport, record: perfRecord };
