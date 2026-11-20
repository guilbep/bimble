<script lang="ts">
export const hint = "standards.hint";
export const label = "standards.title";
</script>

<script setup lang="ts">
// Screen D5, Graphic standards (wireframes v01 fr/Standards.html): the 400-wide right panel.
// Rust today: the standards (PenTable in pens.rs, HatchType/MaterialHatch in hatches.rs, the egui style_panel)
// have no query or verb on the bridge. Only the material families are readable (query "materials").
// Everything else is drawn as the reference, empty or disabled, with data-missing naming the call.
import { computed } from "vue";
import { materials } from "../bimble";
import { t } from "../i18n";
import type * as copy from "../i18n/parts/standards";

const tx = (k: keyof typeof copy.fr) => t(k);
// One tile per material family in the model, in query order.
const families = computed(() => [...new Set(materials.value.map((m) => m.category))]);
</script>

<template>
  <div class="st" :aria-label="tx('standards.title')" data-bb="standards">
    <div style="display:flex;align-items:center;gap:8px">
      <span class="pn" style="margin:0" data-bb="standards-title">{{ tx('standards.title') }}</span>
      <span style="flex:1"></span>
      <!-- The "Changed, not saved" chip: state::unsaved() knows "standards", the bridge's state.unsaved is one boolean. -->
      <span data-missing="query('state').unsaved_parts: [&quot;standards&quot;] when the standards are changed (state::unsaved)"></span>
    </div>
    <div class="note" data-missing="query('standards').source: the file the standards come from (office or model)"></div>
    <input class="inp" :placeholder="tx('standards.search')" :aria-label="tx('standards.searchLabel')" disabled data-bb="standards-search" data-missing="query('standards', {q}): the style_editor filter, not bridged">
    <div class="sec">{{ tx('standards.pens') }}</div>
    <table class="t" style="background:var(--bb-sheet)" data-bb="standards-pens" data-missing="query('standards').pens: PenTable::model_mm(pen, scale) per scale + verb set_pen(scale, pen, mm)">
      <thead><tr><th>{{ tx('standards.scale') }}</th><th class="r">{{ tx('standards.cut') }}</th><th class="r">{{ tx('standards.seen') }}</th><th class="r">{{ tx('standards.thin') }}</th></tr></thead>
      <tbody></tbody>
    </table>
    <div class="sec">{{ tx('standards.hatches') }}</div>
    <div style="display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px" data-bb="standards-hatches">
      <div v-for="f in families" :key="f" style="display:flex;flex-direction:column;gap:4px;align-items:flex-start">
        <!-- No pattern from Rust: a blank swatch (no hatch drawn in TS). -->
        <svg width="72" height="48" role="img" :aria-label="t(`standards.fam.${f}`)" data-missing="query('standards').hatches: HatchType lines + MaterialHatch (cut, seen) per family (hatches.rs)"><rect x="1" y="1" width="70" height="46" stroke-width="1" style="fill:var(--bb-sheet);stroke:var(--bb-ink)"/></svg>
        <span style="font-size:12.5px">{{ t(`standards.fam.${f}`) }}</span>
      </div>
    </div>
    <div class="sec" data-bb="standards-moved" data-missing="query('standards').moved: families moved to another (from → to, element count) + verb to undo one">{{ tx('standards.moved') }}</div>
    <span style="flex:1"></span>
    <!-- ADR 0052: changes apply at once; these three are file operations of the egui style_panel, not ribbon actions. -->
    <div style="display:flex;gap:8px;flex-wrap:wrap">
      <button class="btn s" type="button" disabled data-bb="standards-revert" data-missing="action revert_standards (egui style_panel only)">{{ tx('standards.revert') }}</button>
      <button class="btn s" type="button" disabled data-bb="standards-reset" data-missing="action reset_standards (egui style_panel only)">{{ tx('standards.reset') }}</button>
      <button class="btn p" type="button" disabled data-bb="standards-save" data-missing="action save_standards (egui style_panel only)">{{ tx('standards.save') }}</button>
    </div>
  </div>
</template>

<style scoped>
/* aside: padding:14px;display:flex;flex-direction:column;gap:10px;overflow:hidden (width, padding, border, ground: .bb-right) */
.st { display: flex; flex-direction: column; gap: 10px; height: 100%; color: var(--bb-ink); font-size: 14px; line-height: 1.35; }
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .note{font-size:13px;color:#5C574D} */
.note { font-size: 13px; color: var(--bb-stone); }
/* .sec{font-size:12px;font-family:'DM Mono',monospace;letter-spacing:.08em;text-transform:uppercase;color:#5C574D;margin-top:6px} */
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
/* table.t{border-collapse:collapse;width:100%;font-size:13.5px} */
table.t { border-collapse: collapse; width: 100%; font-size: 13.5px; }
/* table.t th{text-align:left;font-weight:600;color:#5C574D;border-bottom:1px solid #5C574D;padding:6px 8px;white-space:nowrap;font-size:13px} */
table.t th { text-align: left; font-weight: 600; color: var(--bb-stone); border-bottom: 1px solid var(--bb-stone); padding: 6px 8px; white-space: nowrap; font-size: 13px; }
/* table.t td.r,table.t th.r{text-align:right} */
table.t th.r { text-align: right; }
/* .btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;min-height:32px;padding:5px 16px;border-radius:999px;font:inherit;font-weight:600;color:#141414;cursor:pointer;text-decoration:none;white-space:nowrap} */
.btn { display: inline-flex; align-items: center; justify-content: center; gap: 6px; min-height: 32px; padding: 5px 16px; border-radius: 999px; font: inherit; font-weight: 600; color: var(--bb-ink); cursor: pointer; text-decoration: none; white-space: nowrap; }
/* .btn.p{background:#C06A41;border:0} .btn.s{background:#FFFFFF;border:1px solid #141414;font-weight:400} */
.btn.p { background: var(--bb-clay); border: 0; }
.btn.s { background: var(--bb-sheet); border: 1px solid var(--bb-ink); font-weight: 400; }
/* Unavailable: .btn.d{background:#EDE7DB;border:0;color:#5C574D;font-weight:400;cursor:default} */
.btn:disabled { background: var(--bb-sunk); border: 0; color: var(--bb-stone); font-weight: 400; cursor: default; }
.btn:focus-visible, .inp:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
