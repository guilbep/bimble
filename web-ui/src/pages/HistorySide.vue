<script setup lang="ts">
// C6's left panel properties (fr/History.html `.props`). The repository (branch, versions, variants, folder, size)
// is not bridged yet: every value is data-missing.
import { t } from "../i18n";
</script>

<template>
  <div class="pn" data-bb="history-side-branch" data-missing="query: current branch (bimble_core::repo::Repo)">{{ t('history.side.branch', { name: '—' }) }}</div>
  <div class="row"><span class="k">{{ t('history.side.versions') }}</span><span class="mono off" data-missing="query: version count (Repo log)">—</span></div>
  <div class="row"><span class="k">{{ t('history.side.variants') }}</span><span class="mono off" data-missing="query: variant (branch) count">—</span></div>
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
