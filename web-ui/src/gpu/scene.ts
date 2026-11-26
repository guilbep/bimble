// The scene on the GPU for the WebGPU renderer (ticket GPU1, ADR 0083): buffers from the mesh the
// viewport parsed (BMSH v3: own meshes merged, shapes once with a placement per copy; glviewport.ts
// parseMesh / bake), the draw lists of the elements shown cut into the batches of gl/cull.ts
// (makeBatches: Morton order, largest first, by HLOD leaf when the proxies' tree is in), and the
// proxies of gl/proxies.ts. The lists are the WebGL2 path's (drawLists): the merged faces and edges
// of the open model, then the links', and the copies of shapes with 4 or more copies grouped by
// (link, shape, mirrored); here a copy is a 64-byte record in a storage buffer (its placement, its
// element, its batch and rank) that the vertex shader reads, and that cull.wgsl tests on the GPU.
// A vertex of the merged lists carries its element (4 bytes): a selection rewrites 4 bytes an element.
import { BATCH_TRIS, makeBatches, type Batch } from "../gl/cull";
import type { Proxies } from "../gl/proxies";
import { BU } from "./device";
import type { GpuMesh } from "./switch";

/** A batch as the renderer draws it: merged (g -1: faces from index f0, edges from e0) or a group's. */
export interface GBatch { b: Batch; link: boolean; g: number; f0: number; e0: number; ri: number; k0: number; k1: number }
/** A run of copies of one shape: its packed indices, its segments, its copies [first, first + count). */
export interface Group { i0: number; ni: number; s0: number; ns: number; first: number; count: number; mirrored: boolean; link: boolean; batches: GBatch[] }

const PALETTE: [number, number, number][] = [ // glviewport.ts CATEGORY_SRGB (style.rs): the proxies' parts
  [214, 214, 210], [150, 170, 196], [226, 196, 150], [206, 150, 128], [236, 220, 120], [176, 160, 132], [176, 214, 226], [238, 238, 236],
];
/** The per-draw uniform slots (256 bytes each: dynamic offsets): 0 the merged main, 1 links, 2 proxies, 3 outlines, 4 the box, 5.. a group's. */
export const SLOT = 256, SLOT_MAIN = 0, SLOT_LINK = 1, SLOT_PRX = 2, SLOT_OUTLINE = 3, SLOT_BOX = 4, SLOT_GROUP0 = 5;

export class Scene {
  device: GPUDevice;
  mesh: GpuMesh | null = null;
  bytes = 0;
  pos!: GPUBuffer; vel!: GPUBuffer; edgePos!: GPUBuffer; edgeNrm!: GPUBuffer; shapeIdx!: GPUBuffer;
  colours!: GPUBuffer; boxes!: GPUBuffer;
  faceIdx!: GPUBuffer; edgeIdx!: GPUBuffer; insts!: GPUBuffer; visible!: GPUBuffer; ranges!: GPUBuffer; args!: GPUBuffer; bases!: GPUBuffer;
  draws!: GPUBuffer;
  argsInit = new Uint32Array(5);
  batches: GBatch[] = [];
  groups: Group[] = [];
  nInst = 0; nInstBatches = 0; faceN = 0; edgeN = 0;
  meshBytes = 0; listBytes = 0; prxBytes = 0;
  /** Lists made for: the mesh, its shown array and the proxies' leaves. */
  listKey: [unknown, unknown, unknown] = [null, null, null];
  shapeI0 = new Uint32Array(0);
  prx: Proxies | null = null;
  prxPos: GPUBuffer | null = null; prxCol: GPUBuffer | null = null; prxIdx: GPUBuffer | null = null; prxEdge: GPUBuffer | null = null; prxNrm: GPUBuffer | null = null;
  private old: GPUBuffer[] = [];

  constructor(device: GPUDevice) {
    this.device = device;
    const e = (u: number) => this.buf(16, u);
    this.pos = e(BU.VERTEX), this.vel = e(BU.VERTEX), this.edgePos = e(BU.VERTEX), this.edgeNrm = e(BU.VERTEX), this.shapeIdx = e(BU.INDEX);
    this.colours = e(BU.STORAGE), this.boxes = e(BU.STORAGE);
    this.faceIdx = e(BU.INDEX), this.edgeIdx = e(BU.INDEX), this.insts = e(BU.STORAGE), this.visible = e(BU.STORAGE), this.ranges = e(BU.STORAGE);
    this.args = e(BU.STORAGE | BU.INDIRECT | BU.COPY_SRC), this.bases = e(BU.STORAGE);
    this.draws = this.buf(SLOT * SLOT_GROUP0, BU.UNIFORM);
  }

  /** A buffer of at least 16 bytes holding `data` (or zeros), COPY_DST added. */
  buf(size: number, usage: number, data?: ArrayBufferView): GPUBuffer {
    const n = Math.max(16, Math.ceil(Math.max(size, data?.byteLength ?? 0) / 4) * 4);
    const b = this.device.createBuffer({ size: n, usage: usage | BU.COPY_DST, mappedAtCreation: !!data });
    if (data) {
      new Uint8Array(b.getMappedRange()).set(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
      b.unmap();
    }
    return b;
  }
  private swap(b: GPUBuffer): GPUBuffer { this.old.push(b); return b; }
  /** Buffers replaced are destroyed after the frame that last used them is submitted. */
  flushOld(): void { for (const b of this.old) b.destroy(); this.old = []; }

  /** The mesh's vertices, edges, shapes' indices, element boxes (static until the next mesh). */
  setMesh(m: GpuMesh, boxes: Float32Array): void {
    this.mesh = m;
    const nE = m.ids.length, nV = m.pos.length / 3, nS = m.edges.length / 6;
    const vel = new Uint32Array(nV);
    for (let e = 0; e < nE; e++) if (!m.shape?.[e]) vel.fill(e, m.v0[e]!, m.v0[e]! + m.nv[e]!);
    // the two face normals of each segment for both its vertices (as glviewport.ts upload)
    const nrm = new Uint32Array(4 * nS);
    const sw = m.normals.byteOffset % 4 === 0 ? new Uint32Array(m.normals.buffer, m.normals.byteOffset, 2 * nS) : new Uint32Array(m.normals.slice().buffer, 0, 2 * nS);
    for (let k = 0; k < nS; k++) { const a = sw[2 * k]!, b = sw[2 * k + 1]!; nrm[4 * k] = a, nrm[4 * k + 1] = b, nrm[4 * k + 2] = a, nrm[4 * k + 3] = b; }
    let nsh = 0;
    for (let e = 0; e < nE; e++) nsh = Math.max(nsh, m.shape?.[e] ?? 0);
    this.shapeI0 = new Uint32Array(nsh);
    const seen = new Uint8Array(nsh);
    let si = 0;
    for (let e = 0; e < nE; e++) { const k = m.shape?.[e] ?? 0; if (k && !seen[k - 1]) seen[k - 1] = 1, this.shapeI0[k - 1] = si, si += m.ni[e]!; }
    const sidx = new Uint32Array(si);
    for (let e = 0; e < nE; e++) {
      const k = m.shape?.[e] ?? 0;
      if (k && seen[k - 1] === 1) seen[k - 1] = 2, sidx.set(m.idx.subarray(m.i0[e]!, m.i0[e]! + m.ni[e]!), this.shapeI0[k - 1]!);
    }
    for (const b of [this.pos, this.vel, this.edgePos, this.edgeNrm, this.shapeIdx, this.colours, this.boxes]) this.swap(b);
    this.pos = this.buf(0, BU.VERTEX, m.pos);
    this.vel = this.buf(0, BU.VERTEX, vel);
    this.edgePos = this.buf(0, BU.VERTEX, m.edges);
    this.edgeNrm = this.buf(0, BU.VERTEX, nrm);
    this.shapeIdx = this.buf(0, BU.INDEX, sidx);
    this.colours = this.buf(4 * nE, BU.STORAGE);
    this.boxes = this.buf(0, BU.STORAGE, boxes);
    this.meshBytes = m.pos.byteLength + vel.byteLength + m.edges.byteLength + nrm.byteLength + sidx.byteLength + 4 * nE + boxes.byteLength;
    this.listKey = [null, null, null];
    this.total();
  }

  setColours(c: Uint32Array): void {
    if (this.mesh && c.length === this.mesh.ids.length && c.length) this.device.queue.writeBuffer(this.colours, 0, c.buffer, c.byteOffset, c.byteLength);
  }

  /** The draw lists of the elements shown (glviewport.ts drawLists' order), by leaf when the tree is in. */
  setLists(m: GpuMesh, boxes: Float32Array, shown: Uint8Array, p: Proxies | null): void {
    const nE = m.ids.length, lk = (e: number) => (m.model?.[e] ?? 0) > 0;
    const grp = (e: number) => (p && !lk(e) ? p.leafOf[e] ?? -1 : -1);
    let nf = 0, ne = 0;
    for (let e = 0; e < nE; e++) if (shown[e] && !m.shape?.[e]) nf += m.ni[e]!, ne += m.ns[e]!;
    const fi = new Uint32Array(nf), ei = new Uint32Array(2 * ne);
    let f = 0, k = 0;
    const main: GBatch[] = [], links: GBatch[] = [];
    for (const link of [false, true]) {
      const items: number[] = [];
      for (let e = 0; e < nE; e++) if (shown[e] && lk(e) === link && !m.shape?.[e]) items.push(e);
      const { order, batches } = makeBatches(items, boxes, (e) => m.ni[e]! / 3, (e) => m.ns[e]!, BATCH_TRIS, grp);
      for (const b of batches) {
        (link ? links : main).push({ b, link, g: -1, f0: f, e0: k, ri: -1, k0: 0, k1: b.n });
        for (let j = b.first; j < b.first + b.n; j++) {
          const e = order[j]!;
          fi.set(m.idx.subarray(m.i0[e]!, m.i0[e]! + m.ni[e]!), f);
          f += m.ni[e]!;
          for (let q = m.s0[e]!; q < m.s0[e]! + m.ns[e]!; q++) ei[k++] = 2 * q, ei[k++] = 2 * q + 1;
        }
      }
    }
    // copies: grouped by (link, shape, mirrored), each group's in batches
    const P = m.place ?? new Float32Array(0);
    const mirrored = (e: number) => {
      const q = P.subarray(12 * e, 12 * e + 12);
      return q[0]! * (q[4]! * q[8]! - q[5]! * q[7]!) + q[1]! * (q[5]! * q[6]! - q[3]! * q[8]!) + q[2]! * (q[3]! * q[7]! - q[4]! * q[6]!) < 0;
    };
    const order: number[] = [];
    const keys = new Map<number, number>();
    if (m.shape) for (let e = 0; e < nE; e++) if (shown[e] && m.shape[e]) order.push(e), keys.set(e, (lk(e) ? 1 : 0) * 2 ** 33 + m.shape[e]! * 2 + (mirrored(e) ? 1 : 0));
    order.sort((a, b) => keys.get(a)! - keys.get(b)! || a - b);
    const groups: Group[] = [], byGroup: number[][] = [];
    for (const e of order) {
      const last = byGroup[byGroup.length - 1];
      if (last && keys.get(last[0]!) === keys.get(e)) { last.push(e); groups[groups.length - 1]!.count++; continue; }
      byGroup.push([e]);
      groups.push({ i0: this.shapeI0[m.shape![e]! - 1] ?? 0, ni: m.ni[e]!, s0: m.s0[e]!, ns: m.ns[e]!, first: 0, count: 1, mirrored: mirrored(e), link: lk(e), batches: [] });
    }
    const inst: number[] = [], batchOf: number[] = [], rankOf: number[] = [], groupOf: number[] = [];
    let ri = 0;
    groups.forEach((g, gi) => {
      g.first = inst.length;
      const { order: ord, batches } = makeBatches(byGroup[gi]!, boxes, () => g.ni / 3, () => g.ns, BATCH_TRIS, grp);
      for (const b of batches) {
        const d: GBatch = { b, link: g.link, g: gi, f0: 0, e0: 0, ri, k0: 0, k1: b.n };
        g.batches.push(d);
        (g.link ? links : main).push(d);
        for (let j = 0; j < b.n; j++) batchOf.push(ri), rankOf.push(j), groupOf.push(gi);
        ri++;
      }
      inst.push(...ord);
    });
    const recs = new ArrayBuffer(64 * inst.length), rf = new Float32Array(recs), ru = new Uint32Array(recs);
    inst.forEach((e, i) => {
      rf.set(P.subarray(12 * e, 12 * e + 12), 16 * i);
      ru[16 * i + 12] = e, ru[16 * i + 13] = batchOf[i]!, ru[16 * i + 14] = rankOf[i]!, ru[16 * i + 15] = groupOf[i]!;
    });
    const argsInit = new Uint32Array(5 * groups.length), bases = new Uint32Array(groups.length);
    groups.forEach((g, gi) => { argsInit.set([g.ni, 0, g.i0, 0, 0], 5 * gi); bases[gi] = g.first; });
    // the per-draw slots: base and whether the copies come from the compacted list, the edge colour
    const slots = new ArrayBuffer(SLOT * (SLOT_GROUP0 + groups.length));
    const put = (s: number, base: number, list: number, rgb: readonly number[]) => {
      const u = new Uint32Array(slots, SLOT * s, 4), c = new Float32Array(slots, SLOT * s + 16, 4);
      u[0] = base, u[1] = list;
      c.set([rgb[0]! / 255, rgb[1]! / 255, rgb[2]! / 255, 1]);
    };
    put(SLOT_MAIN, 0, 0, EDGE), put(SLOT_LINK, 0, 0, REFERENCE_EDGE), put(SLOT_PRX, 0, 0, EDGE), put(SLOT_OUTLINE, 0, 0, OUTLINE), put(SLOT_BOX, 0, 0, BOX);
    groups.forEach((g, gi) => put(SLOT_GROUP0 + gi, g.first, 1, g.link ? REFERENCE_EDGE : EDGE));
    for (const b of [this.faceIdx, this.edgeIdx, this.insts, this.visible, this.ranges, this.args, this.bases, this.draws]) this.swap(b);
    this.faceIdx = this.buf(0, BU.INDEX, fi);
    this.edgeIdx = this.buf(0, BU.INDEX, ei);
    this.insts = this.buf(0, BU.STORAGE, new Uint8Array(recs));
    this.visible = this.buf(4 * inst.length, BU.STORAGE);
    this.ranges = this.buf(8 * ri, BU.STORAGE);
    this.args = this.buf(0, BU.STORAGE | BU.INDIRECT | BU.COPY_SRC, argsInit);
    this.bases = this.buf(0, BU.STORAGE, bases);
    this.draws = this.buf(0, BU.UNIFORM, new Uint8Array(slots));
    this.argsInit = argsInit;
    this.batches = [...main, ...links];
    this.groups = groups;
    this.nInst = inst.length, this.nInstBatches = ri, this.faceN = nf, this.edgeN = 2 * ne;
    this.listBytes = fi.byteLength + ei.byteLength + recs.byteLength + 4 * inst.length + 8 * ri + argsInit.byteLength + bases.byteLength + slots.byteLength;
    this.listKey = [m, shown, p?.leafOf ?? null];
    this.total();
  }

  /** The proxies' faces (a colour per vertex from its part's family) and crease edges. */
  setProxies(p: Proxies | null): void {
    if (p === this.prx) return;
    this.prx = p;
    for (const b of [this.prxPos, this.prxCol, this.prxIdx, this.prxEdge, this.prxNrm]) if (b) this.swap(b);
    this.prxPos = this.prxCol = this.prxIdx = this.prxEdge = this.prxNrm = null;
    this.prxBytes = 0;
    if (p) {
      const col = new Uint8Array(4 * (p.pos.length / 3));
      for (const c of p.cells) for (const q of c.parts) {
        const rgb = PALETTE[q.cat] ?? PALETTE[7]!;
        for (let v = q.v0; v < q.v0 + q.nv; v++) col.set([rgb[0], rgb[1], rgb[2], 255], 4 * v);
      }
      const nrm = new Int8Array(2 * p.normals.length);
      for (let k = 0; k < p.normals.length / 8; k++) nrm.set(p.normals.subarray(8 * k, 8 * k + 8), 16 * k), nrm.set(p.normals.subarray(8 * k, 8 * k + 8), 16 * k + 8);
      this.prxPos = this.buf(0, BU.VERTEX, p.pos);
      this.prxCol = this.buf(0, BU.VERTEX, col);
      this.prxIdx = this.buf(0, BU.INDEX, p.idx);
      this.prxEdge = this.buf(0, BU.VERTEX, p.edges);
      this.prxNrm = this.buf(0, BU.VERTEX, nrm);
      this.prxBytes = p.pos.byteLength + col.byteLength + p.idx.byteLength + p.edges.byteLength + nrm.byteLength;
    }
    this.total();
  }

  private total(): void { this.bytes = this.meshBytes + this.listBytes + this.prxBytes; }

  destroy(): void {
    this.flushOld();
    for (const b of [this.pos, this.vel, this.edgePos, this.edgeNrm, this.shapeIdx, this.colours, this.boxes, this.faceIdx, this.edgeIdx, this.insts,
      this.visible, this.ranges, this.args, this.bases, this.draws, this.prxPos, this.prxCol, this.prxIdx, this.prxEdge, this.prxNrm]) b?.destroy();
  }
}

// glviewport.ts's colours (sRGB): edges, a link's edges, an unfinished batch's outline, the section box.
export const EDGE = [26, 26, 28] as const, REFERENCE_EDGE = [52, 110, 180] as const, OUTLINE = [120, 120, 124] as const, BOX = [40, 110, 200] as const;
export const BACKGROUND = [247, 247, 245] as const;
