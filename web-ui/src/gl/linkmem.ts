// LNK3D (D, ADR 0078 amendment 3): what the viewport holds of each link on the GPU, for the Links panel
// (LinksPanel.vue: "detail loaded" / "proxies only"). Written by glviewport.ts (its GPU memory budget across
// links: a link's detailed buffers released least recently needed first, uploaded again when needed),
// read by the panel; a renderer that keeps every link (the WebGPU one, GPU1) may leave it empty.
import { shallowRef } from "vue";

/** By link name: true when its detailed meshes are on the GPU, false when only its proxies (or boxes) are. */
export const linkDetail = shallowRef<Record<string, boolean>>({});

/** The GPU bytes the links' detail may take (automatic): the stored setting `memory_share` (0.2 .. 1) of half
 *  the device's memory (navigator.deviceMemory, GiB; 4 when unknown: an integrated GPU shares it). */
export function autoGpuBudget(memoryShare: number | undefined): number {
  const gib = (navigator as Navigator & { deviceMemory?: number }).deviceMemory ?? 4;
  const share = memoryShare !== undefined && memoryShare >= 0.2 && memoryShare <= 1 ? memoryShare : 0.8;
  return Math.round(share * 0.5 * gib * 2 ** 30);
}
