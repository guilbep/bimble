<script lang="ts">
export const hint = "standards.hint";
export const label = "standards.title";
</script>

<script setup lang="ts">
// Screen D5, Graphic standards (wireframes v01 fr/Standards.html): the 400-wide right panel.
// Data: query("standards") (crates/bimble-viewer/src/standards.rs); the swatches are SVG drawn in
// Rust from the real HatchType, shown as <img>. Buttons: standardsAction (the egui style_panel's).
import { computed, shallowRef, watch } from "vue";
import { actions, standards, standardsAction } from "../bimble";
import { t } from "../i18n";
import type * as copy from "../i18n/parts/standards";

const tx = (k: keyof typeof copy.fr) => t(k);
const st = shallowRef(standards());
// Re-read after every change the viewer reports (actions is refreshed on each one).
watch(actions, () => { st.value = standards(); });
const err = shallowRef<string | null>(null);
function run(name: "revert" | "reset" | "save") {
  err.value = standardsAction(name);
  st.value = standards();
}
// One row per scale, the pens of the cut, seen and thin (hatch) lines, as the reference.
const pens = computed(() => {
  const p = st.value?.pens;
  if (!p) return [];
  const mm = (pen: number | null, i: number) => {
    const v = pen === null ? null : p.rows.find((r) => r.pen === pen)?.mm[i];
    return v === null || v === undefined ? "—" : v.toFixed(2);
  };
  return p.scales.map((s, i) => ({ scale: `1:${s}`, cut: mm(p.roles.cut, i), seen: mm(p.roles.seen, i), thin: mm(p.roles.thin, i) }));
});
const img = (svg: string) => "data:image/svg+xml," + encodeURIComponent(svg);
const fam = (f: string) => t(`standards.fam.${f}`);
</script>

<template>
  <div class="st" :aria-label="tx('standards.title')" data-bb="standards">
    <div style="display:flex;align-items:center;gap:8px">
      <span class="pn" style="margin:0" data-bb="standards-title">{{ tx('standards.title') }}</span>
      <span style="flex:1"></span>
      <!-- The "Changed, not saved" chip: state::unsaved() knows "standards", the bridge's state.unsaved is one boolean. -->
      <span v-if="st?.actions.revert" class="note">{{ tx('standards.changed') }}</span>
    </div>
    <div class="note" data-bb="standards-source">{{ st?.source ? tx('standards.source').replace('{file}', st.source) : tx('standards.default') }}</div>
    <input class="inp" :placeholder="tx('standards.search')" :aria-label="tx('standards.searchLabel')" disabled data-bb="standards-search" data-missing="query('standards', {q}): the style_editor filter, not bridged">
    <div class="sec">{{ tx('standards.pens') }}</div>
    <table class="t" style="background:var(--bb-sheet)" data-bb="standards-pens">
      <thead><tr><th>{{ tx('standards.scale') }}</th><th class="r">{{ tx('standards.cut') }}</th><th class="r">{{ tx('standards.seen') }}</th><th class="r">{{ tx('standards.thin') }}</th></tr></thead>
      <tbody>
        <tr v-for="r in pens" :key="r.scale"><td>{{ r.scale }}</td><td class="r">{{ r.cut }}</td><td class="r">{{ r.seen }}</td><td class="r">{{ r.thin }}</td></tr>
      </tbody>
    </table>
    <div class="sec">{{ tx('standards.hatches') }}</div>
    <div style="display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px" data-bb="standards-hatches">
      <div v-for="h in st?.hatches ?? []" :key="h.family" style="display:flex;flex-direction:column;gap:4px;align-items:flex-start">
        <div style="display:flex;gap:4px">
          <img v-if="h.cut" :src="img(h.cut.svg)" width="48" height="32" :alt="`${fam(h.family)}, ${tx('standards.cut')}: ${h.cut.name}`" :title="h.cut.name" class="sw">
          <img v-if="h.seen" :src="img(h.seen.svg)" width="48" height="32" :alt="`${fam(h.family)}, ${tx('standards.seen')}: ${h.seen.name}`" :title="h.seen.name" class="sw">
          <!-- A family with no hatch type keeps its own on-screen pattern: nothing to draw from Rust. -->
          <span v-if="!h.cut && !h.seen" class="sw blank"></span>
        </div>
        <span style="font-size:12.5px">{{ fam(h.family) }}</span>
      </div>
    </div>
    <div class="sec" data-bb="standards-moved">{{ tx('standards.moved') }}</div>
    <div v-for="m in st?.moved ?? []" :key="m.from + m.to" class="note">{{ fam(m.from) }} → {{ fam(m.to) }} · {{ m.count }}</div>
    <div v-if="st && !st.moved.length" class="note">{{ tx('standards.noMoved') }}</div>
    <span style="flex:1"></span>
    <div v-if="err" class="note" role="alert">{{ err }}</div>
    <!-- ADR 0052: changes apply at once; these three are the egui style_panel's file operations (Reset asks first, ADR 0028). -->
    <div style="display:flex;gap:8px;flex-wrap:wrap">
      <button class="btn s" type="button" :disabled="!st?.actions.revert" data-bb="standards-revert" @click="run('revert')">{{ tx('standards.revert') }}</button>
      <button class="btn s" type="button" :disabled="!st?.actions.reset" data-bb="standards-reset" @click="run('reset')">{{ tx('standards.reset') }}</button>
      <button class="btn p" type="button" :disabled="!st?.actions.save" data-bb="standards-save" @click="run('save')">{{ tx('standards.save') }}</button>
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
table.t th.r, table.t td.r { text-align: right; }
/* table.t td{border-bottom:1px solid #EDE7DB;padding:5px 8px;vertical-align:middle} */
table.t td { border-bottom: 1px solid var(--bb-sunk); padding: 5px 8px; vertical-align: middle; }
.sw { display: block; width: 48px; height: 32px; border: 1px solid var(--bb-ink); }
.sw.blank { background: var(--bb-sheet); }
/* .btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;min-height:32px;padding:5px 16px;border-radius:999px;font:inherit;font-weight:600;color:#141414;cursor:pointer;text-decoration:none;white-space:nowrap} */
/* .btn.p{background:#C06A41;border:0} .btn.s{background:#FFFFFF;border:1px solid #141414;font-weight:400} */
/* Unavailable: .btn.d{background:#EDE7DB;border:0;color:#5C574D;font-weight:400;cursor:default} */
.btn:disabled { background: var(--bb-sunk); border: 0; color: var(--bb-stone); font-weight: 400; cursor: default; }
.btn:focus-visible, .inp:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
