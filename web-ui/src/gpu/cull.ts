// The compute pass of GPU culling (ticket GPU1, ADR 0083; cull.wgsl): each frame, with the setting
// gpu_cull on, the copies of every group are tested on the GPU and compacted into the visible list;
// each group is then one drawIndexedIndirect (faces) and its edges one drawIndirect. The batches'
// ranges (which copies the frame may draw: the frustum test of the batch, the moving budget, the
// proxies) come from the CPU, as gl/cull.ts and gl/proxies.ts decide them for WebGL2.
import CULL from "./cull.wgsl?raw";
import { SS } from "./device";
import type { Scene } from "./scene";

export class GpuCull {
  device: GPUDevice;
  pipeline: GPUComputePipeline;
  layout: GPUBindGroupLayout;
  uniform: GPUBuffer;
  /** The scene's buffers the bind group was made for (it is made again when they change). */
  private key: unknown[] = [];
  private group: GPUBindGroup | null = null;
  /** Edge draws' arguments (drawIndirect: vertexCount, instanceCount, firstVertex, firstInstance), copied from the faces'. */
  edgeArgs: GPUBuffer | null = null;
  private edgeArgsFor: GPUBuffer | null = null;

  constructor(device: GPUDevice) {
    this.device = device;
    const C = SS.COMPUTE;
    this.layout = device.createBindGroupLayout({ entries: [
      { binding: 0, visibility: C, buffer: { type: "uniform" } },
      { binding: 1, visibility: C, buffer: { type: "read-only-storage" } },
      { binding: 2, visibility: C, buffer: { type: "read-only-storage" } },
      { binding: 3, visibility: C, buffer: { type: "read-only-storage" } },
      { binding: 4, visibility: C, buffer: { type: "storage" } },
      { binding: 5, visibility: C, buffer: { type: "storage" } },
      { binding: 6, visibility: C, buffer: { type: "read-only-storage" } },
    ] });
    this.pipeline = device.createComputePipeline({
      layout: device.createPipelineLayout({ bindGroupLayouts: [this.layout] }),
      compute: { module: device.createShaderModule({ code: CULL }), entryPoint: "main" },
    });
    this.uniform = device.createBuffer({ size: 128, usage: 64 | 8 }); // UNIFORM | COPY_DST
  }

  /** Writes the arguments' start values and the ranges, then encodes the pass. `ranges`: per copy batch [k0, k1). */
  run(enc: GPUCommandEncoder, s: Scene, planes: Float64Array | null, eye: number[], ortho: boolean, t: number, sizeTest: boolean, ranges: Uint32Array): void {
    if (!s.nInst) return;
    const q = this.device.queue;
    const u = new ArrayBuffer(128), f = new Float32Array(u), w = new Uint32Array(u);
    for (let i = 0; i < 24; i++) f[i] = planes ? planes[i]! : i % 4 === 3 ? 1 : 0; // no planes: every copy inside
    f.set([eye[0]!, eye[1]!, eye[2]!, ortho ? 1 : 0], 24);
    f[28] = Number.isFinite(t) ? t : 3.4e38, w[29] = sizeTest ? 1 : 0, w[30] = s.nInst;
    q.writeBuffer(this.uniform, 0, u);
    q.writeBuffer(s.args, 0, s.argsInit.buffer, s.argsInit.byteOffset, s.argsInit.byteLength);
    if (ranges.length) q.writeBuffer(s.ranges, 0, ranges.buffer, ranges.byteOffset, ranges.byteLength);
    const key = [s.insts, s.boxes, s.ranges, s.visible, s.args, s.bases];
    if (!this.group || key.some((b, i) => b !== this.key[i])) {
      this.key = key;
      this.group = this.device.createBindGroup({ layout: this.layout, entries: [
        { binding: 0, resource: { buffer: this.uniform } }, ...key.map((b, i) => ({ binding: i + 1, resource: { buffer: b } })),
      ] });
    }
    const p = enc.beginComputePass();
    p.setPipeline(this.pipeline);
    p.setBindGroup(0, this.group);
    p.dispatchWorkgroups(Math.ceil(s.nInst / 64));
    p.end();
    // the edges' arguments: per group (2 ns, count, 2 s0, 0): the count copied from the faces' (instanceCount)
    if (this.edgeArgsFor !== s.args || !this.edgeArgs) {
      this.edgeArgs?.destroy();
      const init = new Uint32Array(4 * s.groups.length);
      s.groups.forEach((g, gi) => init.set([2 * g.ns, 0, 2 * g.s0, 0], 4 * gi));
      this.edgeArgs = this.device.createBuffer({ size: Math.max(16, init.byteLength), usage: 256 | 8 | 4, mappedAtCreation: true }); // INDIRECT | COPY_DST | COPY_SRC
      new Uint32Array(this.edgeArgs.getMappedRange()).set(init);
      this.edgeArgs.unmap();
      this.edgeArgsFor = s.args;
    }
    for (let gi = 0; gi < s.groups.length; gi++) enc.copyBufferToBuffer(s.args, 20 * gi + 4, this.edgeArgs, 16 * gi + 4, 4);
  }

  destroy(): void { this.uniform.destroy(); this.edgeArgs?.destroy(); }
}
