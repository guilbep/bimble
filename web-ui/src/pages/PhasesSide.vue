<script setup lang="ts">
// A5's left panel properties (fr/Phases.html `.props`): the current view's phase filter and phase, and the selected
// element's phases. No Rust source yet (ADR 0054): rows drawn as the reference, values empty, data-missing.
import { t } from "../i18n";
const chev = '<path d="m6 9 6 6 6-6"/>';
</script>

<template>
  <div class="pn">{{ t('phases.side.title') }}</div>
  <div class="row"><span class="k">{{ t('phases.side.view') }}</span><span data-missing="query: current view name" /></div>
  <div class="row"><span class="k">{{ t('phases.side.scale') }}</span><span class="mono" data-missing="query: current view scale" /></div>
  <div class="row" data-bb="phases-side-filter"><span class="k">{{ t('phases.side.filter') }}</span>
    <span class="sel-inp off" style="width:150px" data-missing="query: phases; verb: view_phase {view, filter, phase}"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" v-html="chev" /></span></div>
  <div class="row"><span class="k">{{ t('phases.side.phase') }}</span>
    <span class="sel-inp off" style="width:120px" data-missing="verb: view_phase {view, filter, phase}"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" v-html="chev" /></span></div>
  <div class="sec">{{ t('phases.side.selected') }}</div>
  <div class="row"><span class="k">{{ t('phases.side.created') }}</span><span data-missing="query: element phase_created" /></div>
  <div class="row"><span class="k">{{ t('phases.side.demolished') }}</span><span data-missing="query: element phase_demolished" /></div>
</template>

<style scoped>
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.k { color: var(--bb-stone); white-space: nowrap; }
.mono { font-family: 'DM Mono', monospace; }
/* .sec{font-size:12px;font-family:'DM Mono',monospace;letter-spacing:.08em;text-transform:uppercase;color:#5C574D;margin-top:6px} */
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
/* Missing in Rust: the reference's .off (color:#5C574D;opacity:.55). */
.off { color: var(--bb-stone); opacity: .55; }
</style>
