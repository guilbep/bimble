// The compute pass of GPU culling (tickets GPU1, GPU2; ADR 0083; cull.wgsl): each frame, with the setting
// gpu_cull on, the copies of every group are tested on the GPU and compacted into their model's visible
// list; each group is then one drawIndexedIndirect (faces) and its edges one drawIndirect. The batches'
// ranges (which copies the frame may draw: the frustum test of the batch, the moving budget, the proxies)
// come from the CPU, as gl/cull.ts and gl/proxies.ts decide them for WebGL2. GPU2: the viewport's groups
// and batches (every model's), a dispatch per model holding copies, its planes in its own frame.
import CULL from "./cull.wgsl?raw";
import { BU, SS } from "./device";
import type { GModel, Scene } from "./scene";
import type { GpuDb, GpuGroup, GpuModels } from "./switch";
import type { ModelXf } from "../gl/cull";

export class GpuCull {
  device: GPUDevice;
  pipeline: GPUComputePipeline;
  layout: GPUBindGroupLayout;
  uniform: GPUBuffer; uniformCap = 0;
  /** Made per lists: each inst batch's index in `ranges`, the groups' arguments and bases. */
  private listsFor: unknown = null;
  ri = new Map<GpuDb, number>();
  ranges: GPUBuffer | null = null; args: GPUBuffer | null = null; bases: GPUBuffer | null = null; boxes: GPUBuffer | null = null;
  argsInit = new Uint32Array(0);
  edgeArgs: GPUBuffer | null = null;
  private boxesFor: unknown = null;
  bytes = 0;

  constructor(device: GPUDevice) {
    this.device = device;
    const C = SS.COMPUTE;
    this.layout = device.createBindGroupLayout({ entries: [
      { binding: 0, visibility: C, buffer: { type: "uniform", hasDynamicOffset: true, minBindingSize: 128 } },
      ...[1, 2, 3, 4].map((b) => ({ binding: b, visibility: C, buffer: { type: "read-only-storage" as const } })),
      { binding: 5, visibility: C, buffer: { type: "storage" } },
      { binding: 6, visibility: C, buffer: { type: "storage" } },
      { binding: 7, visibility: C, buffer: { type: "read-only-storage" } },
    ] });
    this.pipeline = device.createComputePipeline({
      layout: device.createPipelineLayout({ bindGroupLayouts: [this.layout] }),
      compute: { module: device.createShaderModule({ code: CULL }), entryPoint: "main" },
    });
    this.uniform = device.createBuffer({ size: 256, usage: BU.UNIFORM | BU.COPY_DST });
    this.uniformCap = 1;
  }

  private mk(size: number, usage: number, data?: Uint32Array | Float32Array): GPUBuffer {
    const b = this.device.createBuffer({ size: Math.max(16, Math.ceil((data?.byteLength ?? size) / 4) * 4), usage: usage | BU.COPY_DST, mappedAtCreation: !!data });
    if (data) { new Uint8Array(b.getMappedRange()).set(new Uint8Array(data.buffer, data.byteOffset, data.byteLength)); b.unmap(); }
    return b;
  }

  /** The per-lists data: ranges' slots, the arguments (5 u32 a group: ni, count, i0, 0, 0), the bases, the edges'
   *  arguments (4 u32: 2 ns, count, 2 s0, 0), and per model its copies' batch, rank and group (meta). */
  private lists(M: GpuModels, scene: Scene): void {
    if (this.listsFor === M.listsKey && M.groups.every((g) => !scene.models[g.model] || scene.models[g.model]!.meta)) return;
    this.listsFor = M.listsKey;
    for (const b of [this.ranges, this.args, this.bases, this.edgeArgs]) b?.destroy();
    this.ri.clear();
    let ri = 0;
    const metaOf = new Map<number, Uint32Array>();
    const nOf = new Map<number, number>();
    M.groups.forEach((g) => nOf.set(g.model, Math.max(nOf.get(g.model) ?? 0, g.first + g.count)));
    for (const [k, n] of nOf) metaOf.set(k, new Uint32Array(4 * n));
    const args = new Uint32Array(5 * M.groups.length), edge = new Uint32Array(4 * M.groups.length), bases = new Uint32Array(M.groups.length);
    M.groups.forEach((g: GpuGroup, gi) => {
      args.set([g.ni, 0, g.i0, 0, 0], 5 * gi), edge.set([2 * g.ns, 0, 2 * g.s0, 0], 4 * gi), bases[gi] = g.first;
      const meta = metaOf.get(g.model)!;
      for (const d of g.dbs) {
        this.ri.set(d, ri);
        for (let j = 0; j < d.b.n; j++) meta.set([ri, j, gi, 0], 4 * (g.first + d.b.first + j));
        ri++;
      }
    });
    this.ranges = this.mk(8 * ri, BU.STORAGE);
    this.args = this.mk(0, BU.STORAGE | BU.INDIRECT | BU.COPY_SRC, args);
    this.edgeArgs = this.mk(0, BU.INDIRECT, edge);
    this.bases = this.mk(0, BU.STORAGE, bases);
    this.argsInit = args;
    for (const [k, meta] of metaOf) {
      const g = scene.models[k];
      if (!g) continue;
      scene.drop(g.meta), scene.drop(g.visible);
      g.meta = this.mk(0, BU.STORAGE, meta), g.visible = this.mk(meta.byteLength / 4, BU.STORAGE), g.key = [];
    }
    this.bytes = 8 * ri + args.byteLength + edge.byteLength + bases.byteLength;
  }

  /** Encodes the pass over every model holding copies. `rangeOf(d)`: [k0, k1) of an inst batch this frame. */
  run(enc: GPUCommandEncoder, M: GpuModels, scene: Scene, planes: Float64Array | null, eye: number[], ortho: boolean, t: number, sizeTest: boolean,
    rangeOf: (d: GpuDb) => [number, number]): boolean {
    if (!M.groups.length) return false;
    if (this.boxesFor !== M.lboxes) { this.boxes?.destroy(); this.boxes = this.mk(0, BU.STORAGE, M.lboxes.length ? M.lboxes : new Float32Array(6)); this.boxesFor = M.lboxes; }
    this.lists(M, scene);
    const q = this.device.queue;
    const ranges = new Uint32Array(2 * this.ri.size);
    for (const [d, i] of this.ri) { const r = rangeOf(d); ranges[2 * i] = r[0], ranges[2 * i + 1] = r[1]; }
    if (ranges.length) q.writeBuffer(this.ranges!, 0, ranges);
    q.writeBuffer(this.args!, 0, this.argsInit);
    const ks = [...new Set(M.groups.map((g) => g.model))].filter((k) => scene.models[k]?.b.instEl && scene.models[k]!.meta);
    if (this.uniformCap < ks.length) { this.uniform.destroy(); this.uniform = this.device.createBuffer({ size: 256 * ks.length, usage: BU.UNIFORM | BU.COPY_DST }); this.uniformCap = ks.length; for (const k of ks) scene.models[k]!.key = []; }
    const u = new ArrayBuffer(256 * Math.max(1, ks.length));
    ks.forEach((k, j) => {
      const xf: ModelXf = M.xf[k] ?? { c: 1, s: 0, t: [0, 0, 0] }, f = new Float32Array(u, 256 * j, 32), w = new Uint32Array(u, 256 * j, 32);
      for (let p = 0; p < 6; p++) { // a plane (n, d) of the scene in the model's frame: (R^T n, d + n . t)
        const a = planes ? planes[4 * p]! : 0, b = planes ? planes[4 * p + 1]! : 0, c = planes ? planes[4 * p + 2]! : 0, d = planes ? planes[4 * p + 3]! : 1;
        f.set([xf.c * a + xf.s * b, -xf.s * a + xf.c * b, c, d + a * xf.t[0] + b * xf.t[1] + c * xf.t[2]], 4 * p);
      }
      const ex = eye[0]! - (ortho ? 0 : xf.t[0]), ey = eye[1]! - (ortho ? 0 : xf.t[1]); // (a direction turns, a point moves too)
      f.set([xf.c * ex + xf.s * ey, -xf.s * ex + xf.c * ey, eye[2]! - (ortho ? 0 : xf.t[2]), ortho ? 1 : 0], 24);
      f[28] = Number.isFinite(t) ? t : 3.4e38, w[29] = sizeTest ? 1 : 0, w[30] = M.groups.filter((g) => g.model === k).reduce((n, g) => Math.max(n, g.first + g.count), 0);
    });
    q.writeBuffer(this.uniform, 0, u);
    const p = enc.beginComputePass();
    p.setPipeline(this.pipeline);
    ks.forEach((k, j) => {
      const g: GModel = scene.models[k]!;
      const key = [g.b.instEl, g.meta, g.visible, this.boxes, this.ranges, this.args, this.bases, this.uniform];
      if (!g.cullGroup || key.some((b, i) => b !== g.cullKey[i])) {
        g.cullKey = key;
        g.cullGroup = this.device.createBindGroup({ layout: this.layout, entries: [
          { binding: 0, resource: { buffer: this.uniform, size: 128 } }, { binding: 1, resource: { buffer: g.b.instEl! } }, { binding: 2, resource: { buffer: this.boxes! } },
          { binding: 3, resource: { buffer: g.meta! } }, { binding: 4, resource: { buffer: this.ranges! } }, { binding: 5, resource: { buffer: g.visible! } },
          { binding: 6, resource: { buffer: this.args! } }, { binding: 7, resource: { buffer: this.bases! } },
        ] });
      }
      p.setPipeline(this.pipeline);
      p.setBindGroup(0, g.cullGroup, [256 * j]);
      p.dispatchWorkgroups(Math.ceil(w32(u, j, 30) / 64));
    });
    p.end();
    for (let gi = 0; gi < M.groups.length; gi++) enc.copyBufferToBuffer(this.args!, 20 * gi + 4, this.edgeArgs!, 16 * gi + 4, 4);
    return true;
  }

  destroy(): void { for (const b of [this.uniform, this.ranges, this.args, this.bases, this.edgeArgs, this.boxes]) b?.destroy(); }
}
const w32 = (u: ArrayBuffer, j: number, i: number) => new Uint32Array(u, 256 * j, 32)[i]!;
