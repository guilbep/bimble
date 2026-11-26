// The render pipelines of the WebGPU renderer (ticket GPU1, ADR 0083), made on first use and kept:
// one shader module (shaders.wgsl), one bind group layout for all of them, and a pipeline per
// (what is drawn, face culling, front face, depth bias). WebGL2's polygonOffset(factor, units) is
// WebGPU's depthBiasSlopeScale and depthBias (whole units), set per pipeline instead of per draw.
import SHADERS from "./shaders.wgsl?raw";
import { SS } from "./device";

export type Kind = "merged" | "inst" | "proxy" | "edge" | "edgeInst" | "box";
export interface PipeKey { kind: Kind; cull?: GPUCullMode; front?: GPUFrontFace; slope?: number; bias?: number; depthTest?: boolean }

export const DEPTH: GPUTextureFormat = "depth24plus";
const F3: GPUVertexFormat = "float32x3";

export class Pipelines {
  device: GPUDevice;
  format: GPUTextureFormat;
  module: GPUShaderModule;
  layout: GPUBindGroupLayout;
  pipeLayout: GPUPipelineLayout;
  private cache = new Map<string, GPURenderPipeline>();

  constructor(device: GPUDevice, format: GPUTextureFormat) {
    this.device = device, this.format = format;
    this.module = device.createShaderModule({ code: SHADERS });
    const V = SS.VERTEX, VF = SS.VERTEX | SS.FRAGMENT;
    this.layout = device.createBindGroupLayout({ entries: [
      { binding: 0, visibility: VF, buffer: { type: "uniform" } },
      { binding: 1, visibility: V, buffer: { type: "read-only-storage" } },
      { binding: 2, visibility: V, buffer: { type: "read-only-storage" } },
      { binding: 3, visibility: V, buffer: { type: "read-only-storage" } },
      { binding: 4, visibility: V, buffer: { type: "uniform", hasDynamicOffset: true, minBindingSize: 32 } },
    ] });
    this.pipeLayout = device.createPipelineLayout({ bindGroupLayouts: [this.layout] });
  }

  get(k: PipeKey): GPURenderPipeline {
    const key = `${k.kind}|${k.cull ?? "none"}|${k.front ?? "ccw"}|${k.slope ?? 0}|${k.bias ?? 0}|${k.depthTest ?? true}`;
    let p = this.cache.get(key);
    if (p) return p;
    const lines = k.kind === "edge" || k.kind === "edgeInst" || k.kind === "box";
    const pos: GPUVertexBufferLayout = { arrayStride: 12, attributes: [{ shaderLocation: 0, offset: 0, format: F3 }] };
    const nrm: GPUVertexBufferLayout = { arrayStride: 8, attributes: [{ shaderLocation: 1, offset: 0, format: "snorm8x4" }, { shaderLocation: 2, offset: 4, format: "snorm8x4" }] };
    const buffers: GPUVertexBufferLayout[] =
      k.kind === "merged" ? [pos, { arrayStride: 4, attributes: [{ shaderLocation: 1, offset: 0, format: "uint32" }] }]
      : k.kind === "inst" ? [pos]
      : k.kind === "proxy" ? [pos, { arrayStride: 4, attributes: [{ shaderLocation: 1, offset: 0, format: "unorm8x4" }] }]
      : k.kind === "edge" || k.kind === "edgeInst" ? [pos, nrm]
      : [pos, { arrayStride: 48, stepMode: "instance", attributes: [0, 1, 2, 3].map((c) => ({ shaderLocation: 1 + c, offset: 12 * c, format: F3 })) }];
    const vs = { merged: "vs_merged", inst: "vs_inst", proxy: "vs_proxy", edge: "vs_edge", edgeInst: "vs_edge_inst", box: "vs_box" }[k.kind];
    const fs = lines ? (k.kind === "box" ? "fs_line" : "fs_edge") : "fs_face";
    const test = k.depthTest ?? true;
    p = this.device.createRenderPipeline({
      layout: this.pipeLayout,
      vertex: { module: this.module, entryPoint: vs, buffers },
      fragment: { module: this.module, entryPoint: fs, targets: [{ format: this.format }] },
      primitive: { topology: lines ? "line-list" : "triangle-list", cullMode: k.cull ?? "none", frontFace: k.front ?? "ccw" },
      depthStencil: {
        format: DEPTH, depthWriteEnabled: !lines, depthCompare: test ? "less-equal" : "always",
        ...(lines ? {} : { depthBias: Math.round(k.bias ?? 0), depthBiasSlopeScale: k.slope ?? 0 }),
      },
    });
    this.cache.set(key, p);
    return p;
  }
}
