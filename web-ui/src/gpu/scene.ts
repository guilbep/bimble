// The scene on the GPU for the WebGPU renderer (tickets GPU1, GPU2; ADR 0083, ADR 0078 amendments 3-4).
// GPU2: WebGPU draws the viewport's own models (glviewport.ts slots: the open model, then each link, each in
// its own buffers), from the same draw lists, batches and instanced groups WebGL2 made, so both renderers draw
// the same thing by construction. A model's buffers are TAKEN OVER from WebGL2's (read back once with
// getBufferSubData, WebGL2's emptied: never both on the GPU), and given back when WebGL2 draws again; a model
// the memory budget releases is released here too. Per element the state texel (gl/cull.ts packState, the
// viewport's array) is one storage buffer; per model its placement's rows and plain colour, one row of
// `models`. The open model's HLOD proxies and each link's baked proxies are uploaded from the page's copies.
import type { Proxies } from "../gl/proxies";
import { xfRows, type ModelXf } from "../gl/cull";
import { BU } from "./device";
import { GL_NAMES, type GlArrays, type GlName, type GpuLinkPrx } from "./switch";

const PALETTE: [number, number, number][] = [ // glviewport.ts CATEGORY_SRGB (style.rs): the proxies' parts
  [214, 214, 210], [150, 170, 196], [226, 196, 150], [206, 150, 128], [236, 220, 120], [176, 160, 132], [176, 214, 226], [238, 238, 236],
];
/** The per-draw uniform's stride (dynamic offsets: 256 bytes). */
export const SLOT = 256;

/** The usage of each taken-over buffer (all can be copied out: picks, the hand back). */
const USAGE: Record<GlName, number> = {
  pos: BU.VERTEX, el: BU.VERTEX, edge: BU.VERTEX, edgeNrm: BU.VERTEX, edgeEl: BU.VERTEX, shapeIdx: BU.INDEX,
  idx: BU.INDEX, edgeIdx: BU.INDEX, instMat: BU.STORAGE, instEl: BU.STORAGE,
};

/** A model's buffers on the GPU, taken over from WebGL2's ModelGl `gl`. */
export class GModel {
  gl: object;
  b: Partial<Record<GlName, GPUBuffer>> = {};
  size: Partial<Record<GlName, number>> = {};
  /** The compacted copies (GPU culling) and the copies' bind group (made again when a buffer changes). */
  visible: GPUBuffer | null = null; meta: GPUBuffer | null = null; group: GPUBindGroup | null = null; key: unknown[] = [];
  cullGroup: GPUBindGroup | null = null; cullKey: unknown[] = [];
  /** What the viewport's slot looked like when taken (its bytes, the lists' key): changed, it is read again. */
  seen: unknown = null;
  constructor(gl: object) { this.gl = gl; }
  get bytes(): number { let n = 0; for (const k of GL_NAMES) n += this.size[k] ?? 0; return n + (this.visible?.size ?? 0) + (this.meta?.size ?? 0); }
}

export class Scene {
  device: GPUDevice;
  /** Per model k (0 the open one, k + 1 link k): its buffers, when WebGPU holds them. */
  models: (GModel | undefined)[] = [];
  /** The state texels (an u32 an element) and each model's row; the dummy copies' bind group. */
  states: GPUBuffer; statesFor: unknown = null; statesLen = 0;
  rows: GPUBuffer; rowsLen = 0;
  empty: GPUBuffer; emptyGroup: GPUBindGroup | null = null;
  prx: Proxies | null = null;
  prxPos: GPUBuffer | null = null; prxCol: GPUBuffer | null = null; prxIdx: GPUBuffer | null = null;
  prxBytes = 0;
  /** Each link's baked proxies (by its proxies object: the page keeps it). */
  lprx = new Map<Proxies, { pos: GPUBuffer; idx: GPUBuffer; bytes: number; used: boolean }>();
  private old: GPUBuffer[] = [];

  constructor(device: GPUDevice) {
    this.device = device;
    this.states = this.buf(16, BU.STORAGE);
    this.rows = this.buf(64, BU.STORAGE);
    this.empty = this.buf(64, BU.STORAGE);
  }

  /** A buffer of at least 16 bytes holding `data` (or zeros), COPY_DST and COPY_SRC added. */
  buf(size: number, usage: number, data?: ArrayBufferView | ArrayBuffer): GPUBuffer {
    const bytes = data ? (data instanceof ArrayBuffer ? new Uint8Array(data) : new Uint8Array(data.buffer, data.byteOffset, data.byteLength)) : null;
    const n = Math.max(16, Math.ceil(Math.max(size, bytes?.byteLength ?? 0) / 4) * 4);
    const b = this.device.createBuffer({ size: n, usage: usage | BU.COPY_DST | BU.COPY_SRC, mappedAtCreation: !!bytes });
    if (bytes) { new Uint8Array(b.getMappedRange()).set(bytes); b.unmap(); }
    return b;
  }
  /** Buffers replaced are destroyed after the frame that last used them is submitted. */
  drop(b: GPUBuffer | null | undefined): void { if (b) this.old.push(b); }
  flushOld(): void { for (const b of this.old) b.destroy(); this.old = []; }

  /** Model k's buffers from WebGL2's read back (only those WebGL2 wrote since: the others are kept). */
  take(k: number, gl: object, a: Partial<GlArrays>, seen: unknown): GModel {
    let g = this.models[k];
    if (!g || g.gl !== gl) { if (g) this.release(k); g = this.models[k] = new GModel(gl); }
    for (const name of GL_NAMES) {
      const d = a[name];
      if (!d) continue;
      this.drop(g.b[name]);
      g.b[name] = this.buf(d.byteLength, USAGE[name], d), g.size[name] = d.byteLength;
      if (name === "instMat" || name === "instEl") g.group = null;
    }
    g.seen = seen;
    return g;
  }
  /** Model k's buffers freed (released by the budget, handed back to WebGL2, or the mesh gone). */
  release(k: number): void {
    const g = this.models[k];
    if (!g) return;
    for (const n of GL_NAMES) this.drop(g.b[n]);
    this.drop(g.visible), this.drop(g.meta);
    this.models[k] = undefined;
  }

  /** The state texels (the viewport's array, RGBA8 an element: one u32 each), when they changed. */
  setStates(t: Uint8Array, key: unknown): void {
    if (key === this.statesFor && t.length === this.statesLen) return;
    this.statesFor = key;
    const n = Math.max(16, t.byteLength);
    if (this.states.size < n || this.statesLen !== t.length) { this.drop(this.states); this.states = this.buf(n, BU.STORAGE); }
    this.statesLen = t.length;
    if (t.byteLength) this.device.queue.writeBuffer(this.states, 0, t.buffer, t.byteOffset, t.byteLength - (t.byteLength % 4));
  }
  /** Each model's row: its placement and its plain colour (sRGB 0..255 or null). */
  setRows(xf: ModelXf[], plain: (readonly number[] | null)[]): void {
    const n = Math.max(1, xf.length), a = new Float32Array(16 * n);
    for (let k = 0; k < n; k++) {
      xfRows(xf[k] ?? { c: 1, s: 0, t: [0, 0, 0] }, a, 16 * k);
      const p = plain[k];
      if (p) a.set([p[0]! / 255, p[1]! / 255, p[2]! / 255, 1], 16 * k + 12);
    }
    if (this.rows.size < a.byteLength) { this.drop(this.rows); this.rows = this.buf(a.byteLength, BU.STORAGE); }
    this.rowsLen = n;
    this.device.queue.writeBuffer(this.rows, 0, a);
  }

  /** The open model's proxies' faces (a colour per vertex from its part's family). */
  setProxies(p: Proxies | null): void {
    if (p === this.prx) return;
    this.prx = p;
    for (const b of [this.prxPos, this.prxCol, this.prxIdx]) this.drop(b);
    this.prxPos = this.prxCol = this.prxIdx = null;
    this.prxBytes = 0;
    if (p) {
      const col = new Uint8Array(4 * (p.pos.length / 3));
      for (const c of p.cells) for (const q of c.parts) {
        const rgb = PALETTE[q.cat] ?? PALETTE[7]!;
        for (let v = q.v0; v < q.v0 + q.nv; v++) col.set([rgb[0], rgb[1], rgb[2], 255], 4 * v);
      }
      this.prxPos = this.buf(0, BU.VERTEX, p.pos);
      this.prxCol = this.buf(0, BU.VERTEX, col);
      this.prxIdx = this.buf(0, BU.INDEX, p.idx);
      this.prxBytes = p.pos.byteLength + col.byteLength + p.idx.byteLength;
    }
  }
  /** A link's proxies on the GPU (uploaded at their first use from the page's copy). */
  linkProxies(x: GpuLinkPrx): { pos: GPUBuffer; idx: GPUBuffer } {
    let e = this.lprx.get(x.p);
    if (!e) this.lprx.set(x.p, (e = { pos: this.buf(0, BU.VERTEX, x.p.pos), idx: this.buf(0, BU.INDEX, x.p.idx), bytes: x.p.pos.byteLength + x.p.idx.byteLength, used: true }));
    e.used = true;
    return e;
  }
  /** Link proxies no longer held by the viewport are freed. */
  keepLinkProxies(held: Set<Proxies>): void {
    for (const [p, e] of this.lprx) if (!held.has(p)) this.drop(e.pos), this.drop(e.idx), this.lprx.delete(p);
  }

  /** The bytes WebGPU holds for the scene: the models', the states, the rows, the proxies. */
  get bytes(): number {
    let n = this.states.size + this.rows.size + this.prxBytes;
    for (const g of this.models) if (g) n += g.bytes;
    for (const e of this.lprx.values()) n += e.bytes;
    return n;
  }

  destroy(): void {
    for (let k = 0; k < this.models.length; k++) this.release(k);
    for (const e of this.lprx.values()) this.drop(e.pos), this.drop(e.idx);
    this.lprx.clear();
    this.drop(this.states), this.drop(this.rows), this.drop(this.empty), this.drop(this.prxPos), this.drop(this.prxCol), this.drop(this.prxIdx);
    this.flushOld();
  }
}

// glviewport.ts's colours (sRGB): edges, a link's edges, an unfinished batch's outline, the section box.
export const EDGE = [26, 26, 28] as const, REFERENCE_EDGE = [52, 110, 180] as const, OUTLINE = [120, 120, 124] as const, BOX = [40, 110, 200] as const;
export const BACKGROUND = [247, 247, 245] as const;
export const GHOST = [74, 128, 214] as const, GHOST_EDGE = [32, 70, 150] as const, GHOST_ALPHA = 0.45;
