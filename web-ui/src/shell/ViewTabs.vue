<script setup lang="ts">
import { computed } from "vue";
import { openViews, sheets, tabCall, runAction, type OpenTab } from "../bimble";
import { t } from "../i18n";
import { viewName } from "./viewName";
import { currentPage } from "../pages";
import { send } from "./send";

// `query("open_views")` (bridge.rs): the open tabs in order; `current` is what the main view shows.
const same = (a: OpenTab | null, b: OpenTab | null) => JSON.stringify(a) === JSON.stringify(b);

const tabs = computed(() => {
  const o = openViews.value;
  const page = currentPage.value;
  // Sheet names only when a sheet tab is open (one query per change).
  const sh = o.tabs.some((x) => x.type === "sheet") ? sheets() : [];
  return o.tabs.map((x) => {
    const on = !page && same(x, o.current);
    switch (x.type) {
      case "view": return { tab: x, on, name: ((v) => (v ? viewName(v) : ""))(o.views.find((v) => v.id === x.id)) };
      case "sheet": { const s = sh.find((s) => s.id === x.id); return { tab: x, on, name: s ? `${s.number} · ${s.name}` : "" }; }
      case "schedule": return { tab: x, on, name: t("viewtabs.schedule", { name: x.name }) };
      case "project": return { tab: x, on, name: t("viewtabs.project") };
      case "materials": return { tab: x, on, name: t("viewtabs.materials") };
    }
  });
});
// An open page is the current tab: its name is the i18n key `page.<id>` from the page's own part (the id when it has none).
const pageName = (id: string) => { const s = t(`page.${id}` as never); return s === `page.${id}` ? id : s; };
const paper = computed(() => openViews.value.paper);
// FAKE1: the switch only where the core draws On paper (capability paper_view): no dead control.
import { has } from "./capability";
// The switch is one action: click the side that is not on.
const picture = (onPaper: boolean) => { if (onPaper !== paper.value.on) runAction("paper_view"); };
</script>

<template>
  <!-- Reference: .main > .vtabs (fr/View3D.html, fr/PlanOnPaper.html). -->
  <div class="vtabs" data-bb="viewtabs">
    <!-- An open page comes first, as the reference (fr/ProjectInfo.html: the page tab, then the views). -->
    <a v-if="currentPage" class="vtab on" href="#" aria-current="page" data-bb="viewtab-page" @click.prevent>{{ pageName(currentPage) }}<span class="xx" role="button" tabindex="0" :aria-label="t('viewtabs.close')"
       @click.prevent.stop="send({ do: 'back' })" @keydown.enter.prevent.stop="send({ do: 'back' })"><svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></span></a>
    <a v-for="(tab, i) in tabs" :key="i" class="vtab" :class="{ on: tab.on }" href="#"
       :aria-current="tab.on ? 'page' : undefined" @click.prevent="tabCall('activate', tab.tab) ?? send({ do: 'showView' })">{{ tab.name }}<span class="xx" role="button" tabindex="0" :aria-label="t('viewtabs.close')"
       @click.prevent.stop="tabCall('close', tab.tab)" @keydown.enter.prevent.stop="tabCall('close', tab.tab)"><svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></span></a>
    <!-- No view open (a model without stored views): an empty pill holds the strip's height. -->
    <span v-if="!tabs.length && !currentPage" class="vtab" aria-hidden="true" style="visibility:hidden"><span class="xx" /></span>
    <span style="flex:1"></span>
    <!-- Model / On paper: the "paper_view" action toggles it (pressed = on paper). -->
    <span v-if="paper.offered && has('paper_view')" class="seg" data-bb="view-picture" role="group" :aria-label="t('viewtabs.picture')"><span
        role="button" tabindex="0" :class="{ on: !paper.on }" :aria-pressed="!paper.on" @click="picture(false)" @keydown.enter="picture(false)">{{ t('viewtabs.model') }}</span><span
        role="button" tabindex="0" :class="{ on: paper.on }" :aria-pressed="paper.on" @click="picture(true)" @keydown.enter="picture(true)">{{ t('viewtabs.onPaper') }} <span class="kbd">P</span></span></span>
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
/* PERF2: a 24 px target (Lighthouse target-size), drawn in the 20 px of the reference: the 2 px margins give it back */
.xx { width: 24px; height: 24px; margin: -2px; display: inline-flex; align-items: center; justify-content: center; border-radius: var(--bb-radius-pill); }
/* .seg{display:inline-flex;gap:2px;padding:2px;border-radius:999px;background:#EDE7DB} */
.seg { display: inline-flex; gap: 2px; padding: 2px; border-radius: var(--bb-radius-pill); background: var(--bb-sunk); }
/* .seg span{padding:2px 12px;border-radius:999px;color:#5C574D;text-decoration:none;font:inherit;font-size:13px;border:0;background:none;cursor:pointer;white-space:nowrap} */
.seg span { padding: 2px 12px; border-radius: var(--bb-radius-pill); color: var(--bb-stone); text-decoration: none; font: inherit; font-size: var(--bb-size-small); border: 0; background: none; cursor: pointer; white-space: nowrap; }
/* .seg .on{background:#141414;color:#F5F2EB} */
.seg .on { background: var(--bb-ink); color: var(--bb-on-ink); }
/* (.seg span outranks .kbd, as in the reference.) .kbd{font-family:'DM Mono',monospace;font-size:11.5px;padding:1px 6px;border-radius:6px;background:#EDE7DB;color:#141414} */
.kbd { font-family: var(--bb-font-data); font-size: 11.5px; padding: 1px 6px; border-radius: 6px; background: var(--bb-sunk); color: var(--bb-ink); }
</style>
