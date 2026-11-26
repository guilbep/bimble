// Which renderer draws the 3D view (ticket GPU1, ADR 0083). Small and loaded with the viewport; the
// WebGPU renderer itself (gpu/render.ts and the rest of gpu/) is a dynamic import, so a WebGL2 user
// does not download it. Asked: `?gpu=webgpu|webgl2` in the address, else the setting `renderer`
// (Settings: "3D renderer": automatic, WebGL2, WebGPU; automatic is WebGL2 until the owner has
// compared them, ADR 0083). WebGPU asked: the device is checked (gpu/device.ts); on any refusal, or a
// device lost later, the view stays on (or goes back to) WebGL2 and the reason is kept for the
// performance panel (perf.ts `renderer`). `?gpuSoftware=1` accepts a software adapter (the tests).
import { watch } from "vue";
import { userSettings } from "../bimble";
import { renderer as perfRenderer } from "../perf";
import type { Proxies } from "../gl/proxies";

/** The mesh as glviewport.ts parsed it (its Bmsh): the fields the renderer reads. */
export interface GpuMesh {
  rev: number; ids: string[]; cats: Uint8Array;
  v0: Uint32Array; nv: Uint32Array; i0: Uint32Array; ni: Uint32Array; s0: Uint32Array; ns: Uint32Array;
  pos: Float32Array; idx: Uint32Array; edges: Float32Array; normals: Int8Array;
  model?: Uint8Array; links?: { name: string; visible: boolean }[]; shape?: Uint32Array; place?: Float32Array;
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
  /** Millimetres a pixel spans at distance d (the proxies' error test). */
  mmPerPx: (d: number) => number;
  /** A plan's edges pulled toward the eye (NDC). */
  zbias: number;
  w: number; h: number;
  noCull: boolean; noProxies: boolean; noEdges: boolean;
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
  /** Its canvas shown (false: WebGL2 draws this frame, the place tool's ghost). */
  show(on: boolean): void;
  destroy(): void;
}

export type Asked = "auto" | "webgl2" | "webgpu";
const asked = (): Asked => {
  const q = new URLSearchParams(location.search).get("gpu");
  if (q === "webgpu" || q === "webgl2") return q;
  const s = (userSettings.value?.values as { renderer?: string } | undefined)?.renderer;
  return s === "webgpu" || s === "webgl2" ? s : "auto";
};

/** Starts (and restarts, when the setting changes) the WebGPU renderer when it is asked for;
 *  `set` gets it (null: WebGL2 draws). `canvas`: the WebGL2 canvas (#bimble), under which nothing changes. */
export function gpuSwitch(canvas: HTMLCanvasElement, stats: object, set: (r: GpuRenderer | null) => void): void {
  let cur: GpuRenderer | null = null, want: Asked | null = null, gen = 0;
  const apply = async () => {
    const a = asked();
    perfRenderer.asked = a;
    perfRenderer.gpuCull = new URLSearchParams(location.search).get("gpuCull") === "1" || (userSettings.value?.values.gpu_cull ?? false); // (?gpuCull=1: tests)
    if (a === want) return;
    want = a;
    const g = ++gen;
    if (cur) cur.destroy(), cur = null, set(null);
    perfRenderer.using = "WebGL2", perfRenderer.adapter = null;
    if (a !== "webgpu") { perfRenderer.reason = a === "webgl2" ? "WebGL2 asked" : "automatic is WebGL2 for now (ADR 0083)"; return; }
    perfRenderer.reason = "starting…";
    const soft = new URLSearchParams(location.search).get("gpuSoftware") === "1";
    try {
      const { openDevice } = await import("./device");
      const d = await openDevice(soft, (why) => {
        if (g !== gen || !cur) return;
        console.warn("webgpu:", why, "- back to WebGL2");
        cur.destroy(), cur = null, set(null);
        perfRenderer.using = "WebGL2", perfRenderer.reason = why;
      });
      if (g !== gen) { if (!("reason" in d)) d.device.destroy(); return; }
      if ("reason" in d) { perfRenderer.reason = d.reason; console.warn("webgpu:", d.reason, "- WebGL2 draws"); return; }
      const { createRenderer } = await import("./render");
      if (g !== gen) { d.device.destroy(); return; }
      const r = createRenderer(canvas, d, stats), frame = r.frame.bind(r);
      r.frame = (v) => { // a frame that throws: back to WebGL2, the reason kept (the viewport's loop goes on)
        try { frame(v); } catch (e) {
          const why = `a WebGPU frame failed: ${e instanceof Error ? e.message : String(e)}`;
          console.warn("webgpu:", why, "- back to WebGL2");
          if (cur === r) cur = null, r.destroy(), set(null);
          perfRenderer.using = "WebGL2", perfRenderer.reason = why;
        }
      };
      cur = r;
      perfRenderer.using = "WebGPU", perfRenderer.adapter = d.adapter, perfRenderer.reason = null;
      set(cur);
    } catch (e) {
      perfRenderer.using = "WebGL2", perfRenderer.reason = `the WebGPU renderer failed to start: ${e instanceof Error ? e.message : String(e)}`;
      console.warn("webgpu:", perfRenderer.reason);
      if (cur) cur.destroy(), cur = null;
      set(null);
    }
  };
  watch(userSettings, () => void apply());
  void apply();
}
