<script setup lang="ts">
// A7's left panel properties (fr/LevelsGrids.html `.props`): the storey picked in the Levels table.
import { computed } from "vue";
import { storeys } from "../bimble";
import { t } from "../i18n";
import { picked } from "./levelsPick";

const s = computed(() => storeys.value.find((x) => x.id === picked.value) ?? null);
</script>

<template>
  <div class="pn">{{ t('levelsgrids.side.title') }}</div>
  <template v-if="s">
    <div class="row"><span class="k">{{ t('levelsgrids.side.level') }}</span><span>{{ s.name }}</span></div>
    <!-- Absolute elevation needs the project zero (ADR 0056): not in Rust yet. -->
    <div class="row" data-missing="query: project zero (ADR 0056)"><span class="k">{{ t('levelsgrids.side.altitude') }}</span><span class="mono">—</span></div>
    <div class="row" data-bb="levels-side-count"><span class="k">{{ t('levelsgrids.side.count') }}</span><span class="mono">{{ s.element_count }}</span></div>
    <div class="row"><span class="k">{{ t('levelsgrids.side.ifc') }}</span><span class="mono">IfcBuildingStorey</span></div>
  </template>
</template>

<style scoped>
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.k { color: var(--bb-stone); white-space: nowrap; }
.mono { font-family: 'DM Mono', monospace; }
</style>
