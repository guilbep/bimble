<script setup lang="ts">
// C5's left panel properties (fr/Subprojects.html `.props`): the active sub-project. No Rust source yet (ADR 0053):
// rows drawn as the reference, values empty, data-missing. "Stored in" is a fact of ADR 0053 (in the model), not data.
import { t } from "../i18n";
</script>

<template>
  <div class="pn" data-bb="subprojects-side">{{ t('subprojects.side.title', { name: '' }) }}</div>
  <div class="row"><span class="k">{{ t('subprojects.side.elements') }}</span><span class="mono" data-missing="query: subprojects (elements)" /></div>
  <div class="row"><span class="k">{{ t('subprojects.side.classes') }}</span><span class="mono" data-missing="query: subprojects (classes)" /></div>
  <div class="row"><span class="k">{{ t('subprojects.side.createdBy') }}</span><span class="mono" data-missing="query: subprojects (created_by)" /></div>
  <div class="row"><span class="k">{{ t('subprojects.side.storedIn') }}</span><span>{{ t('subprojects.side.storedInValue') }}</span></div>
</template>

<style scoped>
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.k { color: var(--bb-stone); white-space: nowrap; }
.mono { font-family: 'DM Mono', monospace; }
</style>
