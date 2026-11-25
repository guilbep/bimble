<script lang="ts">
export const hint = "steel.hint";
export const capability = "section_catalogue"; // FAKE1
</script>

<script setup lang="ts">
// Screen B4, Section catalogue (wireframes v01 fr/Steel.html; ADR 0059): the 560-wide card at the
// viewport's top right. No catalogue in Rust yet (query sections {family}): the table is empty, the
// family tabs and search are disabled. Real rows replace the reference's sample rows when it lands.
import { onBeforeUnmount, onMounted, ref } from "vue";
import { t } from "../i18n";
import type * as copy from "../i18n/parts/steel";

const tx = (k: keyof typeof copy.fr) => t(k);
const FAMILIES = ["HEA", "HEB", "IPE", "UPN", "RHS", "CHS"];
// Layout only: the reference's card is absolute in .vp (right:16px; top:14px).
const box = ref<Record<string, string>>({});
let ro: ResizeObserver | null = null;
onMounted(() => {
  const vp = document.querySelector<HTMLElement>('[data-bb="viewport"]');
  if (!vp) return;
  const place = () => {
    const r = vp.getBoundingClientRect();
    if (r.width) box.value = { top: `${r.top + 14}px`, right: `${window.innerWidth - r.right + 16}px` };
  };
  place();
  ro = new ResizeObserver(place);
  ro.observe(vp);
});
onBeforeUnmount(() => ro?.disconnect());
</script>

<template>
  <div class="sc" role="dialog" :aria-label="tx('steel.cat.label')" :style="box" data-bb="sections">
    <div style="display:flex;align-items:center;gap:10px">
      <h2 class="disp" style="margin:0;font-size:16px;font-weight:500" data-bb="sections-title">{{ tx('steel.cat.title') }}</h2>
      <span style="flex:1"></span>
      <input class="inp" :placeholder="tx('steel.cat.search')" style="width:180px" :aria-label="tx('steel.cat.searchLabel')" disabled data-missing="query sections {q} (ADR 0059)">
    </div>
    <span class="seg" data-bb="sections-families" data-missing="query sections {family} (ADR 0059)">
      <span v-for="f in FAMILIES" :key="f">{{ f }}</span>
    </span>
    <div style="display:flex;gap:14px">
      <table class="t" style="background:var(--bb-sheet)" data-bb="sections-table" data-missing="query sections {family} → rows {name, h, b, tw, tf, kg_m} (ADR 0059)">
        <thead><tr><th>{{ tx('steel.cat.section') }}</th><th class="r">h</th><th class="r">b</th><th class="r">tw</th><th class="r">tf</th><th class="r">kg/m</th></tr></thead>
        <tbody></tbody>
      </table>
    </div>
    <p class="note" style="margin:0">{{ tx('steel.cat.timber') }}</p>
  </div>
</template>

<style scoped>
/* role=dialog style="position:absolute;right:16px;top:14px;width:560px;background:#F5F2EB;border:1px solid #5C574D;border-radius:16px;padding:16px;display:flex;flex-direction:column;gap:10px" */
.sc { position: fixed; width: 560px; background: var(--bb-cream); border: 1px solid var(--bb-stone); border-radius: 16px; padding: 16px; display: flex; flex-direction: column; gap: 10px; box-sizing: border-box; }
.disp { font-family: 'Sora', 'Source Sans 3', sans-serif; }
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.seg { display: inline-flex; gap: 2px; padding: 2px; border-radius: 999px; background: var(--bb-sunk); align-self: flex-start; }
.seg span { padding: 2px 12px; border-radius: 999px; color: var(--bb-stone); font-size: 13px; white-space: nowrap; }
.t { border-collapse: collapse; width: 100%; font-size: 13.5px; }
.t th { text-align: left; font-weight: 600; color: var(--bb-stone); border-bottom: 1px solid var(--bb-stone); padding: 6px 8px; white-space: nowrap; font-size: 13px; }
.t th.r { text-align: right; }
.note { font-size: 13px; color: var(--bb-stone); }
</style>

<style>
/* Non-modal in the reference (the card sits in .vp, no overlay): keep the window backdrop clear. */
.bb-app > .bb-backdrop:has(> .sc) { background: none; }
</style>
