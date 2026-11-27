// Which renderer draws the 3D view (ticket GPU1, ADR 0083). Small and loaded with the viewport; the
// WebGPU renderer itself (gpu/render.ts and the rest of gpu/) is a dynamic import, so a WebGL2 user
// does not download it. Asked: the live toggle (GPU2: the performance panel's switch, the status bar's
// chip, the palette's "renderer webgpu|webgl2": perf.ts `chooseRenderer`, written to the setting too), else
// `?gpu=webgpu|webgl2` in the address, else the setting `renderer` (Settings: "3D renderer": automatic,
// WebGL2, WebGPU; automatic is WebGL2 until the owner has compared them, ADR 0083). WebGPU asked: the
// device is checked (gpu/device.ts); on any refusal, or a device lost later, the view stays on (or goes
// back to) WebGL2 and the reason is kept for the performance panel (perf.ts `renderer`). `?gpuSoftware=1`
// accepts a software adapter (the tests).
// GPU2: the switch is live both ways and never blanks the view: WebGPU is made ready (its buffers taken
// over from WebGL2's, a model a frame: `prepare`) while WebGL2 still draws, with a progress line; going
// back, WebGL2's buffers are filled again from WebGPU's (`handBack`) while WebGPU still draws. The checked
// device is kept between switches (no probe, no fill-rate test the second time), dropped when lost.
import { watch } from "vue";
import { userSettings } from "../bimble";
import { chosenRenderer, onRendererChosen, renderer as perfRenderer } from "../perf";
import type { Proxies } from "../gl/proxies";
import type { Batch, ModelXf } from "../gl/cull";

/** The mesh as glviewport.ts parsed it (its Bmsh): the fields the renderer reads. */
export interface GpuMesh {
  rev: number; ids: string[]; cats: Uint8Array;
  v0: Uint32Array; nv: Uint32Array; i0: Uint32Array; ni: Uint32Array; s0: Uint32Array; ns: Uint32Array;
  pos: Float32Array; idx: Uint32Array; edges: Float32Array; normals: Int8Array;
  model?: Uint8Array; links?: { name: string; visible: boolean; elements: number; colour?: number; xf: ModelXf; e0: number }[];
  shape?: Uint32Array; place?: Float32Array;
}

/** GPU2: a model's buffers as WebGL2 holds them (glviewport.ts ModelGl): read back once, then emptied. */
export interface GlArrays { pos: ArrayBuffer; el: ArrayBuffer; edge: ArrayBuffer; edgeNrm: ArrayBuffer; edgeEl: ArrayBuffer; shapeIdx: ArrayBuffer;
  idx: ArrayBuffer; edgeIdx: ArrayBuffer; instMat: ArrayBuffer; instEl: ArrayBuffer }
export type GlName = keyof GlArrays;
export const GL_NAMES: GlName[] = ["pos", "el", "edge", "edgeNrm", "edgeEl", "shapeIdx", "idx", "edgeIdx", "instMat", "instEl"];

/** GPU2: a model's slot (glviewport.ts Slot): its ranges in the mesh and its WebGL2 buffers (null: released). */
export interface GpuSlot { k: number; gl: object | null; bytes: number }
/** GPU2: an instanced group and a draw batch as the viewport made them (glviewport.ts InstGroup, DrawBatch). */
export interface GpuGroup { i0: number; ni: number; s0: number; ns: number; first: number; count: number; mirrored: boolean; link: boolean; model: number; dbs: GpuDb[] }
export interface GpuDb { b: Batch; f0: number; e0: number; link: boolean; g: GpuGroup | null; k0: number; k1: number; model: number; items: Uint32Array; noEdge?: boolean }
/** GPU2: a link's baked proxies (glviewport.ts LinkPrx: the page keeps them) and what the last frame drew of them. */
export interface GpuLinkPrx { name: string; p: Proxies; drawnCells: number; drawnTris: number }

/** GPU2: the viewport's models, handed over by the seam (glviewport.ts gpuView): WebGPU draws what WebGL2
 *  draws, from the same batches, groups, per-element state texels and placements; its buffers are taken over
 *  from WebGL2's (read back, then WebGL2's emptied) and given back when WebGL2 draws again. */
export interface GpuModels {
  slots: GpuSlot[]; dbs: GpuDb[]; groups: GpuGroup[];
  /** The state texels (gl/cull.ts packState: RGBA8 an element) and a key that changes when they are written. */
  states: Uint8Array; statesKey: unknown;
  /** Per model its placement and its one colour when everything of it is shown in it (u_plain), else null. */
  xf: ModelXf[]; plain: (readonly number[] | null)[];
  /** Per element its box in its model's own frame (the compute cull's), changed only with the mesh. */
  lboxes: Float32Array;
  /** A key of the lists (drawLists made again), the selection of a linked element (not under a proxy). */
  listsKey: unknown; linkedSel: { link: string; id: string } | null;
  /** Per model its link's proxies (null: none), the links' proxies before the mesh (the first picture). */
  linkPrx: (GpuLinkPrx | null)[];
  prePrx: { x: GpuLinkPrx; xf: ModelXf; visible: boolean; colour: number }[];
  /** A link's colour (sRGB) by its colour index (-1: the reference colour). */
  linkColour: (c: number) => readonly number[];
  /** A link's proxies cut this frame (glviewport.ts cutLinkPrx: the eye in its frame, its cells placed). */
  cutLink: (x: GpuLinkPrx, xf: ModelXf, usable: Uint8Array, planes: Float64Array | null, eye: number[], px: number) => { cells: number[]; replaced: Uint8Array };
  /** The frame needed model k's detail (the memory budget's clock); its detail is released: ask it back. */
  need: (k: number) => void; want: (k: number) => void;
  /** WebGL2's buffers of a slot: read back (null: none, or emptied), emptied, filled again. */
  readGl: (gl: object) => Partial<GlArrays>; emptyGl: (gl: object) => void; fillGl: (gl: object, a: Partial<GlArrays>) => void;
  /** The device was lost with WebGL2's buffers emptied: the open model uploaded again from the page, the links released (asked back). */
  recover: () => void;
  /** WebGL2's proxies (the open model's, the links') emptied while WebGPU draws, filled again from the page's copies. */
  emptyPrx: () => void; fillPrx: () => void;
  /** The bytes WebGL2 holds now (its state texture, what is not emptied). */
  glHeld: () => number;
  /** The place tool's ghost (faces, edges) and markers (lines), in scene mm; a key that changes with them. */
  ghost: () => { key: unknown; pos: Float32Array; idx: Uint32Array; edges: Float32Array; marks: Float32Array } | null;
}

/** What one frame needs from the viewport: the scene, the camera and the states (the seam in glviewport.ts). */
export interface GpuView {
  mesh: GpuMesh | null;
  /** Per element its box (min xyz, max xyz), shown (0 no, 1 yes, 2 revealed), sRGB colour (0xRRGGBB). */
  boxes: Float32Array; shown: Uint8Array; colour: Uint32Array;
  /** The selection's elements are not under a proxy (proxies.rs usable). */
  selected: (e: number) => boolean;
  prx: Proxies | null; proxiesOn: boolean;
  mvp: Float32Array; eye: number[]; target: number[];
  /** The eye at infinity (a plan, a section, an orthographic 3D view); a plan or a section shown. */
  ortho: boolean; plan: boolean; mirror: boolean;
  clip: { min: number[]; max: number[]; yaw?: number } | null;
  edges: boolean; camKey: string; held: () => boolean; fps: number;
  /** Millimetres a pixel spans at distance d (the proxies' error test); the pixels a mm spans at 1 mm (edges' rule). */
  mmPerPx: (d: number) => number;
  /** A plan's edges pulled toward the eye (NDC). */
  zbias: number;
  w: number; h: number;
  noCull: boolean; noProxies: boolean; noEdges: boolean;
  /** GPU2: the viewport's models (absent: an older seam; GPU1's own lists). */
  models?: GpuModels;
}

export interface GpuRenderer {
  /** Draws the frame: moving when the camera changed (the budget), else the whole picture; after a
   *  moving frame the renderer completes the picture itself in slices (a call abandons them). */
  frame(v: GpuView): void;
  /** The last frame: moving or not, its start (performance.now). */
  readonly last: { moving: boolean; t0: number };
  /** The GPU's end of the work submitted so far. */
  done(): Promise<void>;
  /** The picture drawn last, RGBA rows top first. */
  pixels(): Promise<{ w: number; h: number; rgba: Uint8Array }>;
  /** Its canvas shown (false: WebGL2 draws this frame). */
  show(on: boolean): void;
  /** GPU2: one step of getting ready to draw this view (WebGL2's buffers taken over, a slice of them a call);
   *  true when ready (the next frame is WebGPU's). Absent: always ready. */
  prepare?(v: GpuView): boolean;
  /** GPU2: WebGL2's buffers filled again from this renderer's (before WebGL2 draws again). */
  handBack?(v: GpuView): Promise<void>;
  /** GPU2: the bytes of the buffers this renderer holds. */
  bytes?(): number;
  /** GPU2: element `el`'s triangles of model `k` for a pick, read back from this renderer's buffers: the cached
   *  answer, else null (a read back started: the next pick finds it); undefined: not this renderer's. */
  tris?(k: number, key: string, vFirst: number, nv: number, iAt: number, ni: number, shape: boolean): { pos: Float32Array; idx: Uint32Array } | null | undefined;
  /** Destroys its buffers and canvas; `keepDevice`: the device is kept for the next switch. */
  destroy(keepDevice?: boolean): void;
}

export type Asked = "auto" | "webgl2" | "webgpu";
/** GPU2 (ADR 0083 amended, decided from the real-GPU numbers): "automatic" draws with WebGPU when the adapter check
 *  passes with a hardware adapter (never a fallback or software one, never flags the user did not set: no adapter,
 *  no WebGPU), else WebGL2. */
export const AUTO_WEBGPU = false;
const wantsGpu = (a: Asked | null) => a === "webgpu" || (a === "auto" && AUTO_WEBGPU);
const asked = (): Asked => {
  const c = chosenRenderer();
  if (c) return c;
  const q = new URLSearchParams(location.search).get("gpu");
  if (q === "webgpu" || q === "webgl2") return q;
  const s = userSettings.value?.values.renderer;
  return s === "webgpu" || s === "webgl2" ? s : "auto";
};

/** GPU2: the device checked once and kept between switches (null after a loss). */
let kept: Promise<import("./device").GpuDevice | { reason: string }> | null = null;

/** Starts (and restarts, when the setting changes) the WebGPU renderer when it is asked for;
 *  `set` gets it (null: WebGL2 draws). `canvas`: the WebGL2 canvas (#bimble), under which nothing changes.
 *  `view`: the viewport's current view (handing WebGL2's buffers back before it draws again). */
export function gpuSwitch(canvas: HTMLCanvasElement, stats: object, set: (r: GpuRenderer | null) => void, view?: () => GpuView): void {
  let cur: GpuRenderer | null = null, want: Asked | null = null, gen = 0;
  const off = async (g: number) => { // back to WebGL2: its buffers filled again first, WebGPU drawing meanwhile
    const r = cur;
    if (!r) return;
    if (r.handBack && view) {
      perfRenderer.progress = "WebGL2: its buffers filled again from WebGPU's";
      try { await r.handBack(view()); } catch (e) { console.warn("webgpu: hand back:", e); view().models?.recover(); }
      perfRenderer.progress = null;
    }
    if (cur !== r || wantsGpu(want)) return; // (WebGPU asked again meanwhile: it stays, taking WebGL2's buffers over again)
    cur = null, set(null);
    r.destroy(true);
    if (g === gen) perfRenderer.using = "WebGL2";
  };
  const apply = async () => {
    const a = asked();
    perfRenderer.asked = a;
    perfRenderer.gpuCull = new URLSearchParams(location.search).get("gpuCull") === "1" || (userSettings.value?.values.gpu_cull ?? false); // (?gpuCull=1: tests)
    if (a === want) return;
    want = a;
    const g = ++gen;
    if (!wantsGpu(a)) {
      perfRenderer.reason = a === "webgl2" ? "WebGL2 asked" : "automatic is WebGL2 (ADR 0083)";
      await off(g);
      if (g === gen) perfRenderer.using = "WebGL2", perfRenderer.adapter = null;
      return;
    }
    if (cur) { perfRenderer.reason = null; return; } // (WebGPU asked again while handing back: kept)
    perfRenderer.reason = "starting…";
    const soft = new URLSearchParams(location.search).get("gpuSoftware") === "1";
    try {
      const { openDevice } = await import("./device");
      kept ??= openDevice(soft, (why) => {
        kept = null;
        if (!cur) return;
        console.warn("webgpu:", why, "- back to WebGL2");
        const r = cur;
        cur = null, set(null);
        view?.().models?.recover(); // (WebGL2's buffers were emptied: the open model again from the page, the links asked back)
        r.destroy(false);
        perfRenderer.using = "WebGL2", perfRenderer.reason = why, perfRenderer.progress = null;
      });
      const d = await kept;
      if ("reason" in d) kept = null;
      if (g !== gen) return;
      if ("reason" in d) { perfRenderer.reason = a === "auto" ? `automatic: ${d.reason}` : d.reason; console.info("webgpu:", d.reason, "- WebGL2 draws"); return; } // (said in the performance panel)
      if (a === "auto" && d.adapter.software) { // GPU2: automatic never takes a software adapter (a fallback, a CPU's name, a CPU's fill rate)
        perfRenderer.reason = `automatic: the adapter is a software one (${d.adapter.evidence ?? "software"}): WebGL2`;
        return;
      }
      const { createRenderer } = await import("./render");
      if (g !== gen) return;
      const r = createRenderer(canvas, d, stats), frame = r.frame.bind(r);
      r.frame = (v) => { // a frame that throws: back to WebGL2, the reason kept (the viewport's loop goes on)
        try { frame(v); } catch (e) {
          const why = `a WebGPU frame failed: ${e instanceof Error ? e.message : String(e)}`;
          console.warn("webgpu:", why, "- back to WebGL2");
          if (cur === r) { cur = null, set(null); view?.().models?.recover(); r.destroy(true); }
          perfRenderer.using = "WebGL2", perfRenderer.reason = why;
        }
      };
      cur = r;
      perfRenderer.adapter = d.adapter;
      set(cur); // (the viewport's seam prepares it, WebGL2 drawing meanwhile, then says WebGPU is in use)
    } catch (e) {
      perfRenderer.using = "WebGL2", perfRenderer.reason = `the WebGPU renderer failed to start: ${e instanceof Error ? e.message : String(e)}`;
      console.warn("webgpu:", perfRenderer.reason);
      if (cur) { const r = cur; cur = null, set(null); view?.().models?.recover(); r.destroy(true); }
    }
  };
  onRendererChosen(() => void apply());
  watch(userSettings, () => void apply());
  void apply();
}
