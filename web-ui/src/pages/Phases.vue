<script lang="ts">
import PhasesSide from "./PhasesSide.vue";
export const hint = "phases.hint";
export const side = PhasesSide;
</script>

<script setup lang="ts">
// A5 Project phases (wireframes v01 Phases.html; ADR 0054, Proposed). No phase data in Rust yet: the tabs, the
// filter table header and "Add a filter" are drawn as the reference; the filter rows (the `phases` query) are absent.
import { ref } from "vue";
import { t } from "../i18n";

// Panel state only: which tab is shown. The reference opens on "View filters".
const TABS = ["definitions", "filters", "overrides"] as const;
const tab = ref<(typeof TABS)[number]>("filters");
</script>

<template>
  <div class="ph" data-bb="phases">
    <div class="head">
      <h2 style="font-size:20px" data-bb="phases-title">{{ t('phases.title') }}</h2>
      <span class="seg" role="tablist" data-bb="phases-tabs">
        <button v-for="k in TABS" :key="k" type="button" role="tab" :aria-selected="tab === k" :class="{ on: tab === k }"
                @click="tab = k">{{ t(`phases.tab.${k}`) }}</button>
      </span>
      <span style="flex:1" />
      <span class="note">{{ t('phases.applied') }}</span>
    </div>
    <template v-if="tab === 'filters'">
      <!-- The reference's filter rows (name, Existing / New / Temporary: Normal, Halftone, Hidden): no Rust source yet. -->
      <table class="t" style="max-width:900px" data-bb="phases-filters" data-missing="query: phases (filters: id, name, existing, new, temporary)">
        <thead><tr><th style="width:40px">{{ t('phases.no') }}</th><th>{{ t('phases.filterName') }}</th><th>{{ t('phases.existing') }}</th><th>{{ t('phases.new') }}</th><th>{{ t('phases.temporary') }}</th></tr></thead>
        <tbody />
      </table>
      <button type="button" class="qb add" disabled data-missing="verb: phase_filter_add {name}" data-bb="phases-add-filter">
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg> {{ t('phases.addFilter') }}
      </button>
      <p class="note" style="margin:6px 0 0;max-width:900px">{{ t('phases.note') }}</p>
    </template>
    <!-- Phase definitions and Graphic overrides: no Rust source yet (query: phases; standards overrides, ADR 0026). -->
    <div v-else :data-missing="tab === 'definitions' ? 'query: phases (phases list)' : 'query: phase graphic overrides (standards, ADR 0026)'" />
  </div>
</template>

<style scoped>
/* .page style="display:flex;flex-direction:column;gap:14px" (padding from .bb-page) */
.ph { display: flex; flex-direction: column; gap: 14px; }
/* style="display:flex;align-items:center;gap:16px;flex-wrap:wrap" */
.head { display: flex; align-items: center; gap: 16px; flex-wrap: wrap; }
/* .page h2{font-family:'Sora',sans-serif;margin:0} */
h2 { font-family: 'Sora', sans-serif; margin: 0; }
/* .note{font-size:13px;color:#5C574D} */
.note { font-size: 13px; color: var(--bb-stone); }
/* .seg{display:inline-flex;gap:2px;padding:2px;border-radius:999px;background:#EDE7DB} */
.seg { display: inline-flex; gap: 2px; padding: 2px; border-radius: 999px; background: var(--bb-sunk); }
/* .seg button{padding:2px 12px;border-radius:999px;color:#5C574D;text-decoration:none;font:inherit;font-size:13px;border:0;background:none;cursor:pointer;white-space:nowrap} */
.seg button { padding: 2px 12px; border-radius: 999px; color: var(--bb-stone); font: inherit; font-size: 13px; border: 0; background: none; cursor: pointer; white-space: nowrap; }
/* style="background:#141414;color:#F5F2EB" */
.seg button.on { background: var(--bb-ink); color: var(--bb-on-ink); }
/* table.t … */
table.t { border-collapse: collapse; width: 100%; font-size: 13.5px; }
table.t th { text-align: left; font-weight: 600; color: var(--bb-stone); border-bottom: 1px solid var(--bb-stone); padding: 6px 8px; white-space: nowrap; font-size: 13px; }
/* .qb{display:inline-flex;align-items:center;gap:4px;height:32px;padding:0 12px;border-radius:999px;border:0;background:none;color:#141414;font:inherit;cursor:pointer;white-space:nowrap} + style="color:#A35A37;margin-top:6px" */
.qb { display: inline-flex; align-items: center; gap: 4px; height: 32px; padding: 0 12px; border-radius: 999px; border: 0; background: none; font: inherit; cursor: pointer; white-space: nowrap; align-self: flex-start; }
.add { color: var(--bb-clay-text); margin-top: 6px; }
button:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* Missing in Rust: the reference's .off (color:#5C574D;opacity:.55). */
button:disabled { color: var(--bb-stone); opacity: .55; cursor: default; }
</style>
