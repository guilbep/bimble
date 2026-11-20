<script setup lang="ts">
import { computed } from "vue";
import { openViews, sheets, type ViewSummary } from "../bimble";
import { t } from "../i18n";

// `query("open_views")` (bridge.rs): the viewer's views, the active view and the open tabs.
// ponytail: bimble.ts types it as OpenView[] for now; read it as the bridge sends it.
type Tab = { type: "view" | "sheet"; id: string } | { type: "schedule"; name: string } | { type: "project" | "materials" };
interface Open { views: ViewSummary[]; active: string | null; tabs: Tab[] }
const open = computed(() => (openViews.value as unknown as Open | null) ?? { views: [], active: null, tabs: [] });

const tabs = computed(() => {
  const o = open.value;
  // Sheet names only when a sheet tab is open (one query per change).
  const sh = o.tabs?.some((x) => x.type === "sheet") ? sheets() : [];
  // ponytail: with no chrome (ADR 0048) the viewer never runs view_tabs(), so open_tabs stays empty;
  // the viewer's rule "what the main view shows always has its tab" belongs in bridge.rs open_views.
  const list: Tab[] = o.tabs?.length ? o.tabs : o.active ? [{ type: "view", id: o.active }] : [];
  return list.map((x) => {
    switch (x.type) {
      case "view": return { name: o.views.find((v) => v.id === x.id)?.name ?? "", on: x.id === o.active };
      case "sheet": { const s = sh.find((s) => s.id === x.id); return { name: s ? `${s.number} · ${s.name}` : "", on: false }; }
      case "schedule": return { name: t("viewtabs.schedule", { name: x.name }), on: false };
      case "project": return { name: t("viewtabs.project"), on: false };
      case "materials": return { name: t("viewtabs.materials"), on: false };
    }
  });
});

// Model / On paper: shown on a plan or section, as the viewer does (ribbon.rs view_tabs: `two_pictures`).
const twoPictures = computed(() => {
  const type = open.value.views?.find((v) => v.id === open.value.active)?.type;
  return type === "plan" || type === "section";
});
</script>

<template>
  <!-- Reference: .main > .vtabs (fr/View3D.html, fr/PlanOnPaper.html). Switching and closing have no call yet. -->
  <div class="vtabs" data-bb="viewtabs">
    <a v-for="(tab, i) in tabs" :key="i" class="vtab" :class="{ on: tab.on }" href="#" data-missing="activate-tab"
       :aria-current="tab.on ? 'page' : undefined" @click.prevent>{{ tab.name }}<span class="xx" :aria-label="t('viewtabs.close')" data-missing="close-tab"><svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></span></a>
    <!-- No view open (a model without stored views): an empty pill holds the strip's height. -->
    <span v-if="!tabs.length" class="vtab" aria-hidden="true" style="visibility:hidden"><span class="xx" /></span>
    <span style="flex:1"></span>
    <!-- The "on_paper" action is "Export view… (PDF, SVG)" (ribbon.rs), not this switch: no call, no state yet. -->
    <span v-if="twoPictures" class="seg" data-bb="view-picture" role="group" :aria-label="t('viewtabs.picture')" data-missing="on-paper"><span>{{ t('viewtabs.model') }}</span><span>{{ t('viewtabs.onPaper') }} <span class="kbd">P</span></span></span>
  </div>
</template>

<style scoped>
/* .vtabs{display:flex;gap:4px;padding:6px 10px;border-bottom:1px solid #DDD6C8;align-items:center;flex-wrap:wrap} */
.vtabs { display: flex; gap: 4px; padding: 6px 10px; border-bottom: var(--bb-border-hairline); align-items: center; flex-wrap: wrap; }
/* .vtab{padding:2px 4px 2px 12px;border-radius:999px;font-size:13px;display:inline-flex;gap:4px;align-items:center;color:#5C574D;text-decoration:none;white-space:nowrap;border:1px solid transparent} */
.vtab { padding: 2px 4px 2px 12px; border-radius: var(--bb-radius-pill); font-size: var(--bb-size-small); display: inline-flex; gap: 4px; align-items: center; color: var(--bb-stone); text-decoration: none; white-space: nowrap; border: 1px solid transparent; }
/* .vtab.on{background:#FFFFFF;color:#141414;border-color:#DDD6C8} */
.vtab.on { background: var(--bb-sheet); color: var(--bb-ink); border-color: var(--bb-line); }
/* .vtab .xx{width:20px;height:20px;display:inline-flex;align-items:center;justify-content:center;border-radius:999px} */
.xx { width: 20px; height: 20px; display: inline-flex; align-items: center; justify-content: center; border-radius: var(--bb-radius-pill); }
/* .seg{display:inline-flex;gap:2px;padding:2px;border-radius:999px;background:#EDE7DB} */
.seg { display: inline-flex; gap: 2px; padding: 2px; border-radius: var(--bb-radius-pill); background: var(--bb-sunk); }
/* .seg span{padding:2px 12px;border-radius:999px;color:#5C574D;text-decoration:none;font:inherit;font-size:13px;border:0;background:none;cursor:pointer;white-space:nowrap} */
.seg span { padding: 2px 12px; border-radius: var(--bb-radius-pill); color: var(--bb-stone); text-decoration: none; font: inherit; font-size: var(--bb-size-small); border: 0; background: none; cursor: pointer; white-space: nowrap; }
/* .seg .on{background:#141414;color:#F5F2EB} — no .on: the picture's state is not readable yet. */
/* (.seg span outranks .kbd, as in the reference.) .kbd{font-family:'DM Mono',monospace;font-size:11.5px;padding:1px 6px;border-radius:6px;background:#EDE7DB;color:#141414} */
.kbd { font-family: var(--bb-font-data); font-size: 11.5px; padding: 1px 6px; border-radius: 6px; background: var(--bb-sunk); color: var(--bb-ink); }
</style>
