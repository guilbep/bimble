<script setup lang="ts">
// A8's left panel (fr/Terrain.html `.props`): the selected excavation, shown on the Site tab while nothing else is selected.
// No terrain or excavation in Rust yet (ADR 0057): every value is empty and disabled, data-missing names the call it needs.
import { t } from "../i18n";
const chevron = "m6 9 6 6 6-6";
</script>

<template>
  <div class="pn" data-bb="site-side-title">{{ t('site.side.title') }}</div>
  <div class="row"><label class="k" for="site-bottom">{{ t('site.side.bottom') }}</label><span style="display:inline-flex;align-items:center;gap:6px"><input id="site-bottom" class="inp mono off" disabled data-missing="excavation_set {id, bottom_m} (Generator::Excavation)" style="width:110px;text-align:right" /><span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">m</span></span></div>
  <div class="row"><span class="k">{{ t('site.side.slope') }}</span><span class="sel-inp off" style="width:140px" data-missing="excavation_set {id, slope}"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="chevron"/></svg></span></div>
  <div class="row"><label class="k" for="site-ws">{{ t('site.side.workingSpace') }}</label><span style="display:inline-flex;align-items:center;gap:6px"><input id="site-ws" class="inp mono off" disabled data-missing="excavation_set {id, working_space_mm}" style="width:110px;text-align:right" /><span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">mm</span></span></div>
  <div class="row"><span class="k">{{ t('site.side.ifcClass') }}</span><span class="mono off" data-missing="excavation element (IfcEarthworksCut)"></span></div>
  <div class="row"><span class="k">{{ t('site.side.phase') }}</span><span class="off" data-missing="excavation element phase (ADR 0054)"></span></div>
  <div class="sec">{{ t('site.side.volumes') }}</div>
  <div class="row"><span class="k">{{ t('site.side.cut') }}</span><span class="mono off" data-missing="earthworks query (cut_m3)"></span></div>
  <div class="row"><span class="k">{{ t('site.side.fill') }}</span><span class="mono off" data-missing="earthworks query (fill_m3)"></span></div>
  <div class="row"><span class="k">{{ t('site.side.terrain') }}</span><span class="off" data-missing="terrain query (points)"></span></div>
</template>

<style scoped>
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.k { color: var(--bb-stone); white-space: nowrap; }
.mono { font-family: 'DM Mono', monospace; }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.inp.mono { font-family: 'DM Mono', monospace; font-size: 13px; }
/* Disabled = stone, no extra opacity (ADR 0049, as PlaceSide). */
.off { color: var(--bb-stone); }
/* .sec{font-size:12px;font-family:'DM Mono',monospace;letter-spacing:.08em;text-transform:uppercase;color:#5C574D;margin-top:6px} */
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
</style>
