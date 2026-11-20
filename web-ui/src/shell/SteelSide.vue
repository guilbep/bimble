<script setup lang="ts">
// B4's left panel (fr/Steel.html `.props`): placing a steel beam from the section catalogue (ADR 0059).
// Rust has no profile member yet (Generator::ProfileMember, place {class, section, ...}): every value is empty and disabled.
import { t } from "../i18n";
const chevron = "m6 9 6 6 6-6";
const SELECTS = [
  { k: "steel.side.storey", w: 140, miss: "verb place {class:IfcBeam, storey} (ADR 0059)" },
  { k: "steel.side.section", w: 140, miss: "query sections {family} + place {section} (ADR 0059)" },
  { k: "steel.side.material", w: 140, miss: "verb place {material} (ADR 0059)" },
  { k: "steel.side.justification", w: 150, miss: "verb place {justification} (ADR 0059)" },
] as const;
</script>

<template>
  <div class="pn" data-bb="steel-side-title">{{ t('steel.side.title') }}</div>
  <div v-for="s in SELECTS" :key="s.k" class="row"><span class="k">{{ t(s.k) }}</span><span class="sel-inp off" :style="{ width: s.w + 'px' }" :data-missing="s.miss"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="chevron"/></svg></span></div>
  <div class="row"><label class="k" for="steel-rot">{{ t('steel.side.rotation') }}</label><span style="display:inline-flex;align-items:center;gap:6px"><input id="steel-rot" class="inp mono off" disabled data-missing="verb place {rotation} (ADR 0059)" style="width:90px;text-align:right" /><span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">°</span></span></div>
  <div class="row"><span class="k">{{ t('steel.side.subproject') }}</span><span class="sel-inp off" style="width:140px" data-missing="sub-projects query + place {subproject}"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="chevron"/></svg></span></div>
  <div class="sec">{{ t('steel.side.how') }}</div>
  <p class="note" style="margin:0">{{ t('steel.side.howNote') }}</p>
</template>

<style scoped>
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.k { color: var(--bb-stone); white-space: nowrap; }
.mono { font-family: 'DM Mono', monospace; }
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.inp.mono { font-family: 'DM Mono', monospace; font-size: 13px; }
.off { color: var(--bb-stone); }
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
/* .note{font-size:13px;color:#5C574D} */
.note { font-size: 13px; color: var(--bb-stone); }
</style>
