// The WebGPU device of the 3D view (ticket GPU1, ADR 0083), with a real-adapter check: ADR 0020
// found Chrome on Linux exposing navigator.gpu with no adapter behind it, and ADR 0040 a headless
// Chrome whose WebGPU drew black. So: an adapter must come; a fallback (software) adapter is refused
// unless asked for (`?gpuSoftware=1`: the lab tests and benchmarks on SwiftShader); and a first tiny
// render, read back, must give the right pixel. Any refusal is a reason, kept for the performance
// panel; the view then stays on WebGL2. A device lost later calls `lost` (the view falls back live).
// "Software" is decided from evidence, not from Dawn's architecture string (REAL1: Dawn calls Mesa's
// Asahi driver on an Apple M1 Pro architecture "software"; it is a GPU, not a fallback adapter):
// the API's own isFallbackAdapter, a CPU rasteriser's name (SwiftShader, llvmpipe, lavapipe), and a
// measured fill rate (FILL_SOFTWARE_MPXS). The API's flag and the names refuse (unless
// ?gpuSoftware=1); the fill rate only labels (an old GPU may fill slowly and still beat a CPU).
import { isCpuName, type PerfAdapter } from "../perf";

export interface GpuDevice { device: GPUDevice; adapter: PerfAdapter; format: GPUTextureFormat; fallback: boolean }

/** GPUBufferUsage, GPUTextureUsage, GPUShaderStage and GPUMapMode bits (the WebGPU spec's values). */
export const BU = { MAP_READ: 1, COPY_SRC: 4, COPY_DST: 8, INDEX: 16, VERTEX: 32, UNIFORM: 64, STORAGE: 128, INDIRECT: 256 } as const;
export const TU = { COPY_SRC: 1, COPY_DST: 2, TEXTURE_BINDING: 4, RENDER_ATTACHMENT: 16 } as const;
export const SS = { VERTEX: 1, FRAGMENT: 2, COMPUTE: 4 } as const;

const PROBE = `@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
  var p = array(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  return vec4f(p[i], 0.0, 1.0);
}
@fragment fn fs() -> @location(0) vec4f { return vec4f(1.0, 0.5, 0.0, 1.0); }`;

/** A 4x4 render of an orange triangle over blue, read back: the pixel (0, 0) must be orange. */
async function probe(device: GPUDevice): Promise<string | null> {
  const tex = device.createTexture({ size: [4, 4], format: "rgba8unorm", usage: TU.RENDER_ATTACHMENT | TU.COPY_SRC });
  const sm = device.createShaderModule({ code: PROBE });
  const pl = device.createRenderPipeline({ layout: "auto", vertex: { module: sm, entryPoint: "vs" }, fragment: { module: sm, entryPoint: "fs", targets: [{ format: "rgba8unorm" }] } });
  const buf = device.createBuffer({ size: 256 * 4, usage: BU.COPY_DST | BU.MAP_READ });
  const e = device.createCommandEncoder();
  const p = e.beginRenderPass({ colorAttachments: [{ view: tex.createView(), loadOp: "clear", clearValue: [0, 0, 1, 1], storeOp: "store" }] });
  p.setPipeline(pl);
  p.draw(3);
  p.end();
  e.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow: 256 }, [4, 4]);
  device.queue.submit([e.finish()]);
  await buf.mapAsync(1);
  const px = Array.from(new Uint8Array(buf.getMappedRange(), 0, 4));
  buf.unmap();
  buf.destroy(), tex.destroy();
  const ok = Math.abs(px[0]! - 255) <= 2 && Math.abs(px[1]! - 128) <= 2 && px[2]! <= 2 && px[3] === 255;
  return ok ? null : `a test render gave the wrong pixel (${px.join(", ")}, expected 255, 128, 0, 255): the black picture of ADR 0040`;
}

/** Below this fill rate (megapixels a second) the adapter draws like a CPU rasteriser (REAL1, measured by
 *  fillRate on the Asahi dev machine: SwiftShader's Vulkan under Dawn 722-1,111 on 8 cores, the M1 Pro
 *  through Mesa 5,785-12,428). A label only: the panel and the report say it, nothing is refused on it. */
export const FILL_SOFTWARE_MPXS = 2000;
const FILL_SIZE = 1024, FILL_DRAWS = 32;

/** Fill rate: FILL_DRAWS full-screen triangles into a FILL_SIZE² target, after one warm-up pass (the
 *  pipeline's first use), timed on the CPU from submit to onSubmittedWorkDone. Megapixels a second. */
async function fillRate(device: GPUDevice): Promise<number> {
  const tex = device.createTexture({ size: [FILL_SIZE, FILL_SIZE], format: "rgba8unorm", usage: TU.RENDER_ATTACHMENT });
  const sm = device.createShaderModule({ code: PROBE });
  const pl = device.createRenderPipeline({ layout: "auto", vertex: { module: sm, entryPoint: "vs" }, fragment: { module: sm, entryPoint: "fs", targets: [{ format: "rgba8unorm" }] } });
  const pass = async (n: number) => {
    const e = device.createCommandEncoder();
    const p = e.beginRenderPass({ colorAttachments: [{ view: tex.createView(), loadOp: "clear", clearValue: [0, 0, 0, 1], storeOp: "store" }] });
    p.setPipeline(pl);
    for (let i = 0; i < n; i++) p.draw(3);
    p.end();
    const t0 = performance.now();
    device.queue.submit([e.finish()]);
    await device.queue.onSubmittedWorkDone();
    return performance.now() - t0;
  };
  await pass(1);
  const ms = Math.max(await pass(FILL_DRAWS), 0.05);
  tex.destroy();
  return (FILL_DRAWS * FILL_SIZE * FILL_SIZE) / 1e6 / (ms / 1000);
}

/** The device, or the reason there is none. `allowSoftware`: accept a fallback adapter (tests). */
export async function openDevice(allowSoftware: boolean, lost: (why: string) => void): Promise<GpuDevice | { reason: string }> {
  const gpu = (navigator as Navigator & { gpu?: GPU }).gpu;
  if (!gpu) return { reason: "this browser has no WebGPU (no navigator.gpu)" };
  let a: GPUAdapter | null = null;
  try { a = await gpu.requestAdapter({ powerPreference: "high-performance" }); } catch (e) { return { reason: `requestAdapter failed: ${String(e)}` }; }
  if (!a) return { reason: "navigator.gpu gave no adapter (ADR 0020: WebGPU exposed with no GPU behind it)" };
  const i = a.info ?? ({} as GPUAdapterInfo);
  const fallback = !!(i.isFallbackAdapter ?? (a as unknown as { isFallbackAdapter?: boolean }).isFallbackAdapter);
  const names = `${i.vendor ?? ""} ${i.architecture ?? ""} ${i.device ?? ""} ${i.description ?? ""}`;
  const cpuName = isCpuName(names);
  const adapter: PerfAdapter = { vendor: i.vendor ?? "", architecture: i.architecture ?? "", device: i.device ?? "", description: i.description ?? "",
    software: fallback || cpuName, fallback, fillMpxS: null };
  const name = [adapter.vendor, adapter.architecture, adapter.device, adapter.description].filter(Boolean).join(" / ") || "no name";
  if ((fallback || cpuName) && !allowSoftware) return { reason: `the only adapter is a ${fallback ? "fallback" : "CPU"} (software) one (${name}); ?gpuSoftware=1 accepts it` };
  let device: GPUDevice;
  try {
    device = await a.requestDevice({ requiredLimits: { maxStorageBufferBindingSize: a.limits.maxStorageBufferBindingSize, maxBufferSize: a.limits.maxBufferSize } });
  } catch (e) { return { reason: `requestDevice failed (${name}): ${String(e)}` }; }
  let gone = false;
  void device.lost.then((info) => { gone = true; lost(`device lost (${info.reason ?? "unknown"}): ${info.message}`); });
  device.addEventListener("uncapturederror", (ev) => console.warn("webgpu:", (ev as GPUUncapturedErrorEvent).error.message));
  let bad: string | null;
  try { bad = await probe(device); } catch (e) { bad = `the test render failed: ${String(e)}`; }
  if (gone) return { reason: "the device was lost during the test render" };
  if (bad) { device.destroy(); return { reason: `${bad} (${name})` }; }
  let fill: number | null = null;
  try { fill = await fillRate(device); } catch (e) { console.warn("webgpu: the fill-rate check failed:", String(e)); }
  if (gone) return { reason: "the device was lost during the fill-rate check" };
  const slow = fill !== null && fill < FILL_SOFTWARE_MPXS;
  adapter.fillMpxS = fill === null ? null : Math.round(fill);
  adapter.software = fallback || cpuName || slow;
  adapter.evidence = [fallback ? "isFallbackAdapter" : "not a fallback adapter", cpuName ? "a CPU rasteriser's name" : null,
    fill === null ? "fill rate not measured" : `fill ${Math.round(fill).toLocaleString("en-US")} Mpx/s${slow ? ` (under ${FILL_SOFTWARE_MPXS}: a CPU's)` : ""}`].filter(Boolean).join(", ");
  return { device, adapter, format: gpu.getPreferredCanvasFormat(), fallback };
}
