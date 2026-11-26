<script setup lang="ts">
// The Setting out tab's "Project zero" box (ticket SETOUT, ADR 0075): the survey height of the
// model's z = 0, i.e. the georeference's height (ADR 0056 decision 1: project zero is
// Georef.height_m, no field of its own). Read from `query setting_out`; typed in metres it runs
// `georef_set {height_m}` (one undoable step; the verb's refusal shown as the box's title).
// Without an editable model it is a readout ("—"); a core that does not serve setting_out draws
// it "not yet" as any other box (capability.ts).
import { computed, ref } from "vue";
import { georefSet, settingOut } from "../bimble";
import { t } from "../i18n";
import type { RibbonKey } from "./ribbon-tabs";
import { editableModel, has, HIDE_NOT_AVAILABLE } from "./capability";

const props = defineProps<{ label: RibbonKey; bb: string }>();
const served = computed(() => has("setting_out"));
const zero = computed(() => settingOut.value?.project_zero_m ?? null);
const editable = computed(() => served.value && zero.value !== null && editableModel());
const error = ref<string | null>(null);
const ver = ref(0);
/** "+432.800" (m, three decimals: mm), "±0.000" at zero. */
const text = computed(() => {
  const z = zero.value;
  if (z === null) return "—";
  return (z === 0 ? "±" : z > 0 ? "+" : "−") + Math.abs(z).toFixed(3);
});
function set(e: Event): void {
  const s = (e.target as HTMLInputElement).value.trim().replace("−", "-").replace("±", "").replace(",", ".");
  const v = Number(s);
  if (s === "" || !Number.isFinite(v)) error.value = t("settingout.badNumber");
  else if (v !== zero.value) error.value = georefSet({ height_m: v });
  ver.value++;
}
</script>

<template>
  <div v-if="served || !HIDE_NOT_AVAILABLE" class="rbox readout" :class="{ ny: !served }" :data-bb="props.bb"
       :data-not-yet="served ? undefined : ''" :title="served ? (error ?? t('settingout.zeroTip')) : t('na.tooltip', { name: t(props.label) })">
    <label class="lbl" for="so-zero">{{ t(props.label) }}</label>
    <span v-if="editable" class="z"><input id="so-zero" :key="ver" class="inp mono" :value="text" data-bb="ribbon-project-zero"
      :aria-invalid="error ? 'true' : undefined" @change="set"><span class="mono u">m</span></span>
    <span v-else class="mono ro" data-bb="ribbon-project-zero-readout">{{ served ? text : "" }}</span>
  </div>
</template>

<style scoped>
.rbox { display: flex; flex-direction: column; justify-content: center; gap: 4px; padding: 4px 10px; font-size: 12.5px; }
.rbox.readout { border: 1px solid var(--bb-stone); border-radius: 12px; margin: 4px 2px; }
.rbox.ny { opacity: .8; border: 1px dashed var(--bb-line); cursor: help; color: var(--bb-stone); }
.lbl { font-size: 13px; color: var(--bb-stone); }
.mono { font-family: var(--bb-font-data); }
.ro { font-size: 15px; min-height: 1.2em; }
.z { display: inline-flex; align-items: center; gap: 4px; }
.u { font-size: 12px; color: var(--bb-stone); }
.inp { width: 104px; height: 28px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 6px; font-size: 14px; color: var(--bb-ink); text-align: right; }
.inp[aria-invalid="true"] { border-color: var(--bb-attention); }
.inp:focus-visible { outline: var(--bb-focus); outline-offset: 1px; }
</style>
