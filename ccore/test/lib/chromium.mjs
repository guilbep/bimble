// Which Chromium and which flags, for every lab test and tool that starts one (ticket REAL1):
//   import { CHROMIUM, LAB_GPU, gpuFlags, webgpuFlags, rendererInfo, checkGpu } from "./lib/chromium.mjs";
//   spawn(CHROMIUM, ["--headless=new", ...gpuFlags(), ...(wantWebGpu ? webgpuFlags() : []), ...]);
//   const r = await rendererInfo(evalJs);  // { renderer, hardware } once the page is up
//   checkGpu(r);                           // throws when LAB_GPU asked a GPU and SwiftShader draws
// LAB_GPU (environment) picks what draws the page:
//   software (default; CI and every committed number before REAL1): SwiftShader, a CPU rasteriser;
//     with webgpuFlags() Dawn's Vulkan backend on SwiftShader (a fallback adapter, ?gpuSoftware=1).
//   gl: ANGLE on the system's OpenGL ES (Mesa's AGX driver on the Asahi dev machine); no WebGPU.
//   vulkan: ANGLE on the system's Vulkan (Mesa's Asahi driver) and, with webgpuFlags(), a WebGPU
//     adapter on the same GPU (Dawn's Vulkan backend).
// Flags found by spike NAT1 on Chromium 153 (DEV.md, "3D timings on the machine's GPU"). Headless
// only: no window is ever shown. Lives in test/lib/, not test/: ci/run.sh runs every test/*.mjs.
import { CHROMIUM } from "../ports.mjs";

export { CHROMIUM };
const MODES = ["software", "gl", "vulkan"];
export const LAB_GPU = process.env.LAB_GPU || "software";
if (!MODES.includes(LAB_GPU)) throw new Error(`LAB_GPU=${LAB_GPU}: one of ${MODES.join(", ")}`);

/** The flags that pick the rasteriser (WebGL2's ANGLE backend). */
export function gpuFlags(mode = LAB_GPU) {
  if (mode === "gl") return ["--use-angle=gl", "--ignore-gpu-blocklist"];
  if (mode === "vulkan") return ["--use-angle=vulkan", "--enable-features=Vulkan", "--ignore-gpu-blocklist"];
  return ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"];
}

/** The flags added to gpuFlags() for a WebGPU adapter. Software: Dawn's Vulkan backend on SwiftShader
 *  (--use-webgpu-adapter=swiftshader alone gives an adapter whose canvas cannot present here, "A valid
 *  external Instance reference no longer exists", Chromium 153); gl: none exists (empty). */
export function webgpuFlags(mode = LAB_GPU) {
  if (mode === "vulkan") return ["--enable-unsafe-webgpu"];
  if (mode === "gl") return [];
  return ["--enable-unsafe-webgpu", "--enable-features=Vulkan", "--use-vulkan=swiftshader", "--disable-vulkan-surface"];
}

/** The words of a CPU rasteriser in a renderer or adapter string (web-ui/src/perf.ts says the same). */
export const SOFTWARE_RE = /swiftshader|llvmpipe|softpipe|lavapipe|basic render/i;

/** A page expression: the WebGL2 renderer string (WEBGL_debug_renderer_info) and whether it is a GPU. */
export const RENDERER_EXPR = `(() => {
  const c = document.createElement('canvas'), gl = c.getContext('webgl2') ?? c.getContext('webgl');
  const ext = gl?.getExtension('WEBGL_debug_renderer_info');
  const renderer = gl ? String(gl.getParameter(ext ? ext.UNMASKED_RENDERER_WEBGL : gl.RENDERER)) : 'none';
  gl?.getExtension('WEBGL_lose_context')?.loseContext();
  return { renderer, hardware: !!gl && !${SOFTWARE_RE}.test(renderer) };
})()`;

/** After the page is up: `evalJs(expr)` gives a value (returnByValue). */
export const rendererInfo = (evalJs) => evalJs(RENDERER_EXPR);

/** Refuses a run whose GPU is not the one LAB_GPU asked (Chromium falls back to SwiftShader silently
 *  when a flag is refused): a number measured on the CPU is never labelled a GPU's. */
export function checkGpu(info, mode = LAB_GPU) {
  if (mode !== "software" && !info.hardware) throw new Error(`LAB_GPU=${mode} but the page draws with ${info.renderer} (a software rasteriser)`);
  if (mode === "software" && info.hardware) throw new Error(`LAB_GPU=software but the page draws with ${info.renderer}`);
  return info;
}
