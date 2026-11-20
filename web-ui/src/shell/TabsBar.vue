<script setup lang="ts">
import { t } from "../i18n";
import { TABS, activeTab } from "./tabs";
</script>

<template>
  <!-- Reference: nav.tabs, 14 a.tab in three groups split by span.tabsep. -->
  <nav class="tabs" data-bb="tabsbar" :aria-label="t('tabsbar.label')">
    <template v-for="(tab, i) in TABS" :key="tab.id">
      <span v-if="i > 0 && tab.group !== TABS[i - 1]?.group" class="tabsep" aria-hidden="true"></span>
      <a class="tab" :class="{ on: activeTab === tab.id }" href="#" :aria-current="activeTab === tab.id ? 'page' : undefined"
         @click.prevent="activeTab = tab.id">{{ t(`tab.${tab.id}`) }}</a>
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
</style>
