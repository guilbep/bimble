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
import { linkGpu } from "./gl/linkmem"; // SIZE3 (C)
import { reactive } from "vue";
import { setSettings, userSettings } from "./bimble";

/** One frame drawn: when it started (performance.now), what it took (ms), moving or still, what it drew. */
export interface PerfFrame { at: number; ms: number; moving: boolean; tris: number; draws: number; synced: boolean }
export interface PerfInput { what: string; ms: number; at: number; synced: boolean }
/** The adapter as the API names it (WebGPU's GPUAdapterInfo, or WebGL2's debug renderer strings), shown as
 *  given; `software`: decided from evidence (REAL1), not from these names alone: WebGPU's isFallbackAdapter,
 *  a CPU rasteriser's name, and a measured fill rate (`evidence` says which). */
export interface PerfAdapter { vendor: string; architecture: string; device?: string; description: string; software: boolean;
  fallback?: boolean; evidence?: string;
  /** WebGPU: the fill rate measured when the device opened (megapixels a second; device.ts). */
  fillMpxS?: number | null }
/** What draws the 3D view: asked (the setting or ?gpu=), in use, the adapter, why WebGPU is not in use. */
export interface PerfRenderer { asked: "auto" | "webgl2" | "webgpu"; using: "WebGL2" | "WebGPU"; adapter: PerfAdapter | null; reason: string | null; gpuCull: boolean;
  /** GPU2: the other renderer being made ready while this one still draws ("WebGPU: 3 of 17 models on the GPU"), else null. */
  progress?: string | null;
  /** GPU2: the last live switch (the toggle): to which renderer, when it was asked (performance.now), the ms from
   *  the ask to the end of the first frame the other renderer drew (null while it is not in). */
  switchTo?: "WebGL2" | "WebGPU" | null; switchAt?: number | null; switchMs?: number | null }
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

/** Reactive (GPU2): the status bar's chip and the panel's switch follow it at once. */
export const renderer: PerfRenderer = reactive<PerfRenderer>({ asked: "auto", using: "WebGL2", adapter: null, reason: null, gpuCull: false,
  progress: null, switchTo: null, switchAt: null, switchMs: null });

// ---- GPU2: the live toggle (the panel's switch, the status bar's chip, the palette's "renderer webgpu|webgl2"). Here,
// not in gpu/switch.ts, so the main chunk does not carry the switch: the viewport's switches listen. ----
let chosen: "webgl2" | "webgpu" | null = null;
const appliers = new Set<() => void>();
/** The toggle's choice (over ?gpu= and the setting until the page reloads), or null. */
export const chosenRenderer = (): "webgl2" | "webgpu" | null => chosen;
export const onRendererChosen = (f: () => void): void => { appliers.add(f); };
/** Switches the 3D view's renderer live and remembers it (the setting `renderer`). Returns the setting's error, if
 *  any (the switch still happens). */
export function chooseRenderer(a: "webgl2" | "webgpu"): string | null {
  chosen = a;
  renderer.asked = a;
  renderer.switchTo = a === "webgpu" ? "WebGPU" : "WebGL2", renderer.switchAt = performance.now(), renderer.switchMs = null;
  if (renderer.using === renderer.switchTo && !renderer.progress) renderer.switchMs = 0; // already there
  let err: string | null = null;
  try { err = userSettings.value?.values.renderer === a ? null : setSettings({ renderer: a }); } catch (e) { err = e instanceof Error ? e.message : String(e); }
  for (const f of appliers) f();
  return err;
}
/** The toggle's other side: WebGPU when WebGL2 was asked (or automatic), else WebGL2. */
export const toggleRenderer = (): string | null => chooseRenderer(renderer.asked === "webgpu" ? "webgl2" : "webgpu");
/** Why WebGPU is not drawing, in plain words (an i18n key and its value), from the switch's reason. */
export type WhyKey = "perf.why.starting" | "perf.why.noWebgpu" | "perf.why.noAdapter" | "perf.why.software" | "perf.why.lost" | "perf.why.feature" | "perf.why.failed" | "perf.why.notAsked";
export const FEATURE_REASON = "not drawn by WebGPU yet: ";
export function whyPlain(reason: string | null): { key: WhyKey; what?: string } | null {
  if (!reason) return null;
  if (reason === "starting…") return { key: "perf.why.starting" };
  if (reason.startsWith(FEATURE_REASON)) return { key: "perf.why.feature", what: reason.slice(FEATURE_REASON.length) };
  if (/linked models are drawn by WebGL2/.test(reason)) return { key: "perf.why.feature", what: "linked models" };
  if (/no navigator\.gpu|no WebGPU/.test(reason)) return { key: "perf.why.noWebgpu" };
  if (/no adapter/.test(reason)) return { key: "perf.why.noAdapter" };
  if (/fallback|software|CPU\)/.test(reason)) return { key: "perf.why.software" };
  if (/lost/.test(reason)) return { key: "perf.why.lost" };
  if (/WebGL2 asked|automatic is WebGL2/.test(reason)) return { key: "perf.why.notAsked" };
  return { key: "perf.why.failed" };
}
/** What the user can do when WebGPU has no adapter or no API, per platform (the orchestrator's note: on Linux
 *  Chromium 153 gives WebGPU no adapter without flags; these three were verified on Asahi, Chromium 153, a browser
 *  process with a headless display, GPU2). null: nothing to say (WebGPU draws, or another reason). */
export type HowKey = "perf.how.linuxChromium" | "perf.how.chrome" | "perf.how.firefox" | "perf.how.other";
export function howToWebgpu(reason: string | null): HowKey | null {
  const w = whyPlain(reason);
  if (!w || (w.key !== "perf.why.noAdapter" && w.key !== "perf.why.noWebgpu" && w.key !== "perf.why.software")) return null;
  const ua = navigator.userAgent;
  if (/Firefox\//.test(ua)) return "perf.how.firefox";
  const chromium = /Chrome\/|Chromium\//.test(ua);
  if (chromium && /Linux/.test(ua) && !/Android/.test(ua)) return "perf.how.linuxChromium";
  return chromium ? "perf.how.chrome" : "perf.how.other";
}
// ---- end GPU2 ----

/** The names of CPU rasterisers (SwiftShader, llvmpipe, lavapipe, Microsoft's Basic Render). Not the
 *  word "software": Dawn calls Mesa's Asahi GPU (the M1 Pro, Vulkan) architecture "software" (REAL1). */
export const isCpuName = (s: string): boolean => /swiftshader|llvmpipe|softpipe|lavapipe|basic render/i.test(s);
/** A WebGL renderer string that names a CPU rasteriser (ANGLE says "SwiftShader"; some drivers "Software Rasterizer"). */
export const isSoftware = (s: string): boolean => isCpuName(s) || /software rasteri[sz]er/i.test(s);

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
  // GPU2: the first frame of the renderer a live switch asked for ends the switch's time (WebGL2: a context; WebGPU: a promise)
  const by = sync === null ? null : sync instanceof Promise ? "WebGPU" : "WebGL2";
  const sw = renderer.switchTo && renderer.switchMs === null && renderer.switchAt !== null && by === renderer.switchTo && t0 >= renderer.switchAt! ? renderer.switchAt! : null;
  const done = () => {
    f.ms = performance.now() - t0;
    if (sw !== null && renderer.switchMs === null) renderer.switchMs = f.at + f.ms - sw;
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
  /** GPU2: under WebGPU, the bytes each renderer holds (WebGPU's buffers, WebGL2's left) and the models taken over. */
  gpuSplit: { webgpu: number; webgl: number; taken: number; models: number } | null;
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
    proxiesDrawn: g?.proxies?.drawnCells ?? 0, proxyTris: g?.proxies?.drawnTris ?? 0,
    gpuBytes: renderer.using === "WebGPU" && g?.gpuSplit ? g.gpuSplit.webgpu + g.gpuSplit.webgl : g?.gpuBytes ?? null, // GPU2: what each renderer holds
    gpuSplit: g?.gpuSplit ?? null,
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
/** The adapter's names as the API gives them: vendor, architecture, device, description (WebGPU's GPUAdapterInfo). */
export const adapterNames = (a: PerfAdapter, sep = " / "): string =>
  [["vendor", a.vendor], ["architecture", a.architecture], ["device", a.device ?? ""], ["description", a.description]]
    .filter(([, v]) => v).map(([k, v]) => (a.architecture || a.device ? `${k} ${v}` : v)).join(sep);
const num = (x: number) => Math.round(x).toLocaleString("en-US");
const fline = (s: FrameStats) => (s.n ? `n ${s.n}, last ${ms(s.last)}, median ${ms(s.median)}, p95 ${ms(s.p95)}, max ${ms(s.max)}` : "none");

/** The report as plain text (English: it goes into bug reports). File names only with `names`. */
export function perfReport(names: boolean, file: string | null): string {
  const s = perfSnapshot(), r = s.renderer, a = s.adapter;
  const L: string[] = [];
  L.push("Bimble performance report");
  L.push(`date: ${new Date().toISOString()}; page: ${location.search.includes("ccore=1") || import.meta.env.VITE_CCORE === "1" ? "wasm" : location.search.includes("native=1") ? "native host" : "other"}; user agent: ${navigator.userAgent}`);
  L.push(`renderer: ${r.using} (asked: ${r.asked})${r.reason ? `; WebGPU not used: ${r.reason}` : ""}${r.using === "WebGPU" ? `; GPU culling of copies: ${r.gpuCull ? "on" : "off"}` : ""}`);
  L.push(`adapter: ${a ? `${adapterNames(a) || "(no name given)"}${a.software ? " - SOFTWARE (a CPU rasteriser, not a GPU)" : ""}${a.evidence ? ` (${a.evidence})` : ""}` : "unknown"}`);
  L.push(`canvas: ${s.canvas.w} x ${s.canvas.h} px, device pixel ratio ${s.canvas.dpr}`);
  L.push(`frame time (draw to pixels${s.synced ? "" : ", NOT waited for: submit time only"}), last ${RING} frames:`);
  L.push(`  moving: ${fline(s.moving)}`);
  L.push(`  still:  ${fline(s.still)}`);
  L.push(`last frame: ${num(s.tris)} triangles, ${num(s.draws)} draw calls; batches ${num(s.batches)}, culled ${num(s.culled)}; proxies drawn ${num(s.proxiesDrawn)} (${num(s.proxyTris)} triangles)`);
  L.push(`memory: GPU buffers ${mb(s.gpuBytes)}${s.gpuSplit && r.using === "WebGPU" ? ` (WebGPU ${mb(s.gpuSplit.webgpu)}, WebGL2 ${mb(s.gpuSplit.webgl)}; ${s.gpuSplit.taken} of ${s.gpuSplit.models} models on WebGPU)` : ""}, JS heap ${mb(s.jsHeap)}, wasm ${mb(s.wasmBytes)}`);
  if (linkGpu.value) L.push(`linked files on the GPU: ${mb(linkGpu.value.used)} of a ${mb(linkGpu.value.budget)} budget`); // SIZE3 (C)
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
