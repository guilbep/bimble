// LNK3D (D, ADR 0078 amendment 3): what the viewport holds of each link on the GPU, for the Links panel
// (LinksPanel.vue: "detail loaded" / "proxies only"). Written by glviewport.ts (its GPU memory budget across
// links: a link's detailed buffers released least recently needed first, uploaded again when needed),
// read by the panel; a renderer that keeps every link (the WebGPU one, GPU1) may leave it empty.
import { shallowRef } from "vue";

/** By link name: true when its detailed meshes are on the GPU, false when only its proxies (or boxes) are. */
export const linkDetail = shallowRef<Record<string, boolean>>({});

/** SIZE3 (ADR 0078 amendment 5): the default of the setting `link_gpu_mb`. */
export const LINK_GPU_MB_DEFAULT = 768;

/** The GPU bytes the links' detail may take: the stored setting `link_gpu_mb` (MB, 64 .. 65536; 768 when unset or
 *  absent: an older core). SIZE3: before, `memory_share` of half of navigator.deviceMemory (1.6 GB on a 16 GB
 *  machine, Chromium caps deviceMemory at 8): it never bit on a folder of 17 links (1.62 GB on the GPU). */
export function gpuBudget(linkGpuMb: number | undefined): number {
  const mb = linkGpuMb !== undefined && linkGpuMb >= 64 && linkGpuMb <= 65536 ? linkGpuMb : LINK_GPU_MB_DEFAULT;
  return mb * 2 ** 20;
}

/** SIZE3: what the links' detail takes on the GPU now and the budget (bytes), for the performance panel. */
export const linkGpu = shallowRef<{ used: number; budget: number } | null>(null);
