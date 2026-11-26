<script setup lang="ts">
// C6's left panel properties (fr/History.html `.props`): the branch, the version and variant counts from the
// C core's history store (ticket H1, ccore/session/versions.h; History.vue loads them). The folder and its
// size are not queried yet: data-missing.
import { computed } from "vue";
import { repoHistory } from "../bimble";
import { t } from "../i18n";

const v = computed(() => (repoHistory.value?.versions.history ? repoHistory.value.versions : null));
const b = computed(() => (repoHistory.value?.branches.history ? repoHistory.value.branches : null));
const branch = computed(() => v.value?.head.branch ?? v.value?.head.commit?.slice(0, 12) ?? "—");
</script>

<template>
  <div class="pn" data-bb="history-side-branch">{{ t('history.side.branch', { name: branch }) }}</div>
  <div class="row"><span class="k">{{ t('history.side.versions') }}</span><span class="mono" :class="{ off: !v }">{{ v ? v.versions.length : '—' }}</span></div>
  <div class="row"><span class="k">{{ t('history.side.variants') }}</span><span class="mono" :class="{ off: !b }">{{ b ? b.branches.length : '—' }}</span></div>
  <div class="row"><span class="k">{{ t('history.side.stored') }}</span><span class="off" data-missing="query: history folder (Repo path)">—</span></div>
  <div class="row"><span class="k">{{ t('history.side.size') }}</span><span class="mono off" data-missing="query: history folder size">—</span></div>
</template>

<style scoped>
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.k { color: var(--bb-stone); white-space: nowrap; }
.mono { font-family: 'DM Mono', monospace; }
.off { color: var(--bb-stone); opacity: .55; }
</style>
