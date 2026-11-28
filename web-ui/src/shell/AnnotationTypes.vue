<script setup lang="ts">
import { stateRev } from "../bimble"; // VIS2
// The Annotate tab's Text type and Dimension type boxes (ticket VIEWSHEET, ADR 0076; Rust ribbon.rs:1285
// annotation_types): the graphic standards' types (`query annotation_types`, the default standards: the C core
// keeps no other), "Default" first. The one chosen is session state (action annotation_type) and goes to the next
// text or dimension the annotate tool places (its kind, a text its size). Not served: "not yet", as any box.
import { computed, ref } from "vue";
import { annotationTypes, setAnnotationType, state } from "../bimble";
import { t } from "../i18n";
import { HIDE_NOT_AVAILABLE } from "./capability";

const props = defineProps<{ bb: string }>();
const tick = ref(0);
const types = computed(() => { void stateRev.value; void tick.value; return annotationTypes(); });
const error = ref<string | null>(null);
function pick(which: "text" | "dimension", e: Event): void {
  const v = (e.target as HTMLSelectElement).value;
  error.value = setAnnotationType(which, v === "" ? null : v);
  tick.value++;
}
const rows = computed(() => types.value ? [
  { which: "text" as const, key: "ribbon.annotate.g2.t1" as const, cur: types.value.text, names: types.value.text_types.map((x) => x.name) },
  { which: "dimension" as const, key: "ribbon.annotate.g2.t2" as const, cur: types.value.dimension, names: types.value.dimension_types.map((x) => x.name) },
] : []);
</script>

<template>
  <div v-if="types" class="types" :data-bb="props.bb" :title="error ?? t('vs.types.tip')">
    <label v-for="r in rows" :key="r.which" class="row">
      <span class="lbl">{{ t(r.key) }}</span>
      <select class="inp" :value="r.cur ?? ''" :data-bb="`annotation-type-${r.which}`" @change="pick(r.which, $event)">
        <option value="">{{ t('vs.types.default') }}</option>
        <option v-for="n in r.names" :key="n" :value="n">{{ n }}</option>
      </select>
    </label>
  </div>
  <div v-else-if="!HIDE_NOT_AVAILABLE" class="types ny" :data-bb="props.bb" data-not-yet="" aria-disabled="true"
       :title="t('na.tooltip', { name: t('ribbon.annotate.g2') })">
    <span class="lbl">{{ t('ribbon.annotate.g2.t1') }}</span><span class="lbl">{{ t('ribbon.annotate.g2.t2') }}</span>
  </div>
</template>

<style scoped>
.types { display: flex; flex-direction: column; justify-content: center; gap: 4px; padding: 2px 10px; font-size: 12.5px; }
.types.ny { opacity: .8; border: 1px dashed var(--bb-line); border-radius: 12px; cursor: help; color: var(--bb-stone); }
.row { display: flex; align-items: center; gap: 8px; justify-content: space-between; }
.lbl { font-size: 13px; color: var(--bb-stone); white-space: nowrap; }
.inp { width: 170px; height: 28px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 6px; font: inherit; font-size: 13px; color: var(--bb-ink); }
.inp:focus-visible { outline: var(--bb-focus); outline-offset: 1px; }
</style>
