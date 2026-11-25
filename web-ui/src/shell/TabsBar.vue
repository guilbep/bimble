<script setup lang="ts">
import { t } from "../i18n";
import { TABS, activeTab } from "./tabs";
// ---- FAKE1: a tab whose every tool is "not yet" in this core gets a small "preview" mark ----
import { RIBBON } from "./ribbon-tabs";
import { toolNotYet } from "./capability";
const preview = (id: keyof typeof RIBBON) => RIBBON[id].every((g) => g.tools.every((tl) => toolNotYet(tl)));
// ---- end FAKE1 ----
</script>

<template>
  <!-- Reference: nav.tabs, 14 a.tab in three groups split by span.tabsep. -->
  <nav class="tabs" data-bb="tabsbar" :aria-label="t('tabsbar.label')">
    <template v-for="(tab, i) in TABS" :key="tab.id">
      <span v-if="i > 0 && tab.group !== TABS[i - 1]?.group" class="tabsep" aria-hidden="true"></span>
      <a class="tab" :class="{ on: activeTab === tab.id }" href="#" :aria-current="activeTab === tab.id ? 'page' : undefined"
         @click.prevent="activeTab = tab.id">{{ t(`tab.${tab.id}`) }}<span v-if="preview(tab.id)" class="pv" :title="t('na.tabTitle')" :data-bb="`tab-preview-${tab.id}`">{{ t('na.tab') }}</span></a>
    </template>
  </nav>
</template>

<style scoped>
/* .tabs{display:flex;gap:2px;padding:2px 12px 6px;background:#FFFFFF;flex-wrap:wrap;align-items:center} */
.tabs { display: flex; gap: 2px; padding: 2px 12px 6px; background: var(--bb-sheet); flex-wrap: wrap; align-items: center; }
/* .tab{padding:4px 12px;border-radius:999px;color:#5C574D;text-decoration:none;font-size:14px;line-height:20px;white-space:nowrap} */
.tab { padding: 4px 12px; border-radius: 999px; color: var(--bb-stone); text-decoration: none; font-size: var(--bb-size-ui); line-height: 20px; white-space: nowrap; }
/* .tab:hover{background:#EDE7DB;color:#141414} */
.tab:hover { background: var(--bb-sunk); color: var(--bb-ink); }
/* .tab.on{background:#141414;color:#F5F2EB} */
.tab.on { background: var(--bb-ink); color: var(--bb-on-ink); }
/* .tabsep{width:1px;height:18px;background:#DDD6C8;margin:0 6px} */
.tabsep { width: 1px; height: 18px; background: var(--bb-line); margin: 0 6px; }
/* FAKE1: the "preview" mark of a tab with no working tool yet */
.pv { margin-left: 6px; padding: 0 6px; border: 1px dashed currentColor; border-radius: var(--bb-radius-pill); font-family: var(--bb-font-data); font-size: 11px; letter-spacing: var(--bb-tracking-eyebrow); text-transform: uppercase; }
</style>
