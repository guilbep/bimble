<script setup lang="ts">
// B1's left panel properties while a slab is placed (fr/PlaceSlab.html `.props`). Each field applies at once (ADR 0052).
import { computed, ref, watch } from "vue";
import { placing, storeys, materials, setPlacing } from "../bimble";
import { t } from "../i18n";

const error = ref<string | null>(null);
const thickness = ref("");
watch(() => placing.value?.sizes.slab, (v) => { thickness.value = v === undefined ? "" : String(v); }, { immediate: true });

const apply = (patch: Parameters<typeof setPlacing>[0]) => { error.value = setPlacing(patch); };
function commitThickness(): void {
  if (thickness.value === String(placing.value?.sizes.slab)) return;
  // Rust validates: a non-number goes as null and comes back as its error.
  apply({ sizes: { slab: Number(thickness.value.replace(/[\s']/g, "")) } });
}

const level = (e: number) => `${e >= 0 ? "+" : ""}${(e / 1000).toFixed(2)}`;
// ponytail: the level above, for the note only (the slab's top is at the storey height).
const above = computed(() => {
  const sorted = [...storeys.value].sort((a, b) => a.elevation - b.elevation);
  const i = sorted.findIndex((s) => s.id === placing.value?.storey);
  return (sorted[i + 1] ?? sorted[i])?.name ?? "—";
});
</script>

<template>
  <template v-if="placing">
    <div class="pn" data-bb="place-title">{{ t('placeslab.title') }}</div>
    <div class="row">
      <label class="k" for="ps-storey">{{ t('placeslab.storey') }}</label>
      <span class="sel-inp" style="width:140px" data-bb="place-storey">
        <span class="ell">{{ storeys.find((s) => s.id === placing!.storey)?.name ?? '—' }} ({{ level(storeys.find((s) => s.id === placing!.storey)?.elevation ?? 0) }})</span><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg>
        <select id="ps-storey" :value="placing.storey ?? ''" @change="apply({ storey: ($event.target as HTMLSelectElement).value })">
          <option v-for="s in storeys" :key="s.id" :value="s.id">{{ s.name }} ({{ level(s.elevation) }})</option>
        </select>
      </span>
    </div>
    <div class="row">
      <label class="k" for="ps-thickness">{{ t('placeslab.thickness') }}</label>
      <span style="display:inline-flex;align-items:center;gap:6px">
        <input id="ps-thickness" data-bb="place-thickness" v-model="thickness" class="inp mono" style="width:100px;text-align:right" :aria-invalid="error ? 'true' : 'false'" aria-describedby="ps-error" @keydown.enter="commitThickness" @blur="commitThickness" />
        <span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">mm</span>
      </span>
    </div>
    <p v-if="error" id="ps-error" role="alert" class="error" data-bb="place-error">{{ t('placeslab.error', { msg: error }) }}</p>
    <div class="row">
      <label class="k" for="ps-material">{{ t('placeslab.material') }}</label>
      <span class="sel-inp" style="width:160px">
        <span class="ell">{{ placing.material ?? '—' }}</span><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg>
        <select id="ps-material" :value="placing.material ?? ''" @change="apply({ material: ($event.target as HTMLSelectElement).value })">
          <option v-if="!placing.material" value="" disabled>—</option>
          <option v-for="m in materials" :key="m.id" :value="m.name">{{ m.name }}</option>
        </select>
      </span>
    </div>
    <div class="row">
      <label class="k" for="ps-offset">{{ t('placeslab.topOffset') }}</label>
      <span style="display:inline-flex;align-items:center;gap:6px">
        <!-- The slab's top is at the storey height: 0 until Rust takes an offset. -->
        <input id="ps-offset" class="inp mono off" value="0" disabled data-missing="set_placing top_offset" style="width:100px;text-align:right" />
        <span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">mm</span>
      </span>
    </div>
    <div class="row">
      <span class="k">{{ t('placeslab.subproject') }}</span>
      <span class="sel-inp off" style="width:140px" data-missing="sub-projects"><span class="ell" /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span>
    </div>
    <div class="row">
      <span class="k">{{ t('placeslab.shape') }}</span>
      <span class="seg"><span class="on">{{ t('placeslab.rectangle') }}</span><span class="off" data-missing="polygon slab">{{ t('placeslab.polygon') }}</span><span class="off" data-missing="sloped slab">{{ t('placeslab.sloped') }}</span></span>
    </div>
    <div class="row">
      <label class="k" for="ps-slope">{{ t('placeslab.slope') }}</label>
      <span style="display:inline-flex;align-items:center;gap:6px">
        <input id="ps-slope" class="inp mono off" value="0.0" disabled data-missing="sloped slab" style="width:100px;text-align:right" />
        <span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">%</span>
      </span>
    </div>
    <div class="sec">{{ t('placeslab.how') }}</div>
    <p class="note" style="margin:0">{{ t('placeslab.howNote', { above }) }}</p>
  </template>
</template>

<style scoped>
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
/* .row .k{color:#5C574D;white-space:nowrap} */
.k { color: var(--bb-stone); white-space: nowrap; }
.mono { font-family: 'DM Mono', monospace; }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} .inp.mono{font-family:'DM Mono',monospace;font-size:13px} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.inp.mono { font-family: 'DM Mono', monospace; font-size: 13px; }
/* .off{color:#5C574D;opacity:.55}: no Rust call yet (data-missing). */
.off { color: var(--bb-stone); opacity: .55; }
/* .sel-inp{display:inline-flex;align-items:center;justify-content:space-between;gap:6px;height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 6px 0 8px;font-size:13.5px;white-space:nowrap} */
.sel-inp { display: inline-flex; align-items: center; justify-content: space-between; gap: 6px; height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 6px 0 8px; font-size: 13.5px; white-space: nowrap; position: relative; }
/* The native select covers the drawn field: the reference's look, the platform's list and keyboard. */
.sel-inp select { position: absolute; inset: 0; width: 100%; opacity: 0; cursor: pointer; font: inherit; }
.sel-inp:has(select:focus-visible), .inp:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
.ell { overflow: hidden; text-overflow: ellipsis; min-width: 0; }
/* .seg{display:inline-flex;gap:2px;padding:2px;border-radius:999px;background:#EDE7DB} .seg span{padding:2px 12px;border-radius:999px;color:#5C574D;font-size:13px;white-space:nowrap} .seg .on{background:#141414;color:#F5F2EB} */
.seg { display: inline-flex; gap: 2px; padding: 2px; border-radius: 999px; background: var(--bb-sunk); }
.seg span { padding: 2px 12px; border-radius: 999px; color: var(--bb-stone); font-size: 13px; white-space: nowrap; }
.seg .on { background: var(--bb-ink); color: var(--bb-on-ink); }
/* .sec{font-size:12px;font-family:'DM Mono',monospace;letter-spacing:.08em;text-transform:uppercase;color:#5C574D;margin-top:6px} */
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
.note { font-size: 13px; color: var(--bb-stone); }
.error { color: var(--bb-ink); background: var(--bb-error); font-size: 13.5px; margin: 0; padding: 2px 8px; overflow-wrap: anywhere; }
</style>
