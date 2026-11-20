<script setup lang="ts">
// B3's left panel (fr/Rebar.html `.props`): the selected bar set, shown on the Rebar tab while nothing else is selected.
// No rebar in Rust yet (ADR 0058: Generator::RebarSet): every value is empty and disabled, data-missing names the call it needs.
import { t } from "../i18n";
const chevron = "m6 9 6 6 6-6";
</script>

<template>
  <div class="pn" style="display:flex;align-items:center;gap:8px" data-bb="rebar-side-title"><span class="dot" style="background:var(--bb-pink)"></span>{{ t('rebar.side.title') }} <span class="off" data-missing="rebar query {host} (position)"></span></div>
  <div class="row"><span class="k">{{ t('rebar.side.diameter') }}</span><span class="sel-inp off" style="width:110px" data-missing="set_generator RebarSet {diameter}"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="chevron"/></svg></span></div>
  <div class="row"><label class="k" for="rebar-count">{{ t('rebar.side.count') }}</label><span style="display:inline-flex;align-items:center;gap:6px"><input id="rebar-count" class="inp mono off" disabled data-missing="set_generator RebarSet {count}" style="width:96px;text-align:right" /><span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px"></span></span></div>
  <div class="row"><span class="k">{{ t('rebar.side.steel') }}</span><span class="sel-inp off" style="width:110px" data-missing="set_generator RebarSet {material} (B500B, ADR 0031)"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="chevron"/></svg></span></div>
  <div class="row"><label class="k" for="rebar-cover">{{ t('rebar.side.cover') }}</label><span style="display:inline-flex;align-items:center;gap:6px"><input id="rebar-cover" class="inp mono off" disabled data-missing="set_generator RebarSet {cover}" style="width:96px;text-align:right" /><span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">mm</span></span></div>
  <div class="row"><span class="k">{{ t('rebar.side.shape') }}</span><span class="sel-inp off" style="width:140px" data-missing="rebar_codes query (rebar code setting unset)"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="chevron"/></svg></span></div>
  <div class="row"><span class="k">{{ t('rebar.side.host') }}</span><span class="off" data-missing="RebarSet host (SetHost command)"></span></div>
  <div class="row"><span class="k">{{ t('rebar.side.class') }}</span><span class="mono off" data-missing="rebar element (IfcReinforcingBar)"></span></div>
  <div class="sec">{{ t('rebar.side.computed') }}</div>
  <div class="row"><span class="k">{{ t('rebar.side.unitLength') }}</span><span class="mono off" data-missing="rebar query (length)"></span></div>
  <div class="row"><span class="k">{{ t('rebar.side.mass') }}</span><span class="mono off" data-missing="rebar query (mass)"></span></div>
  <div class="row"><span class="k">{{ t('rebar.side.ratio') }}</span><span class="mono off" data-missing="rebar query (host ratio kg/m³)"></span></div>
</template>

<style scoped>
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .dot{width:12px;height:12px;border-radius:50%;border:1px solid #141414;display:inline-block;flex:none} */
.dot { width: 12px; height: 12px; border-radius: 50%; border: 1px solid var(--bb-ink); display: inline-block; flex: none; }
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
