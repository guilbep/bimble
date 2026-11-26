<script lang="ts">
export const capability = "schedules"; // QTY1: verb schedule_add (ccore/src/verbs_schedules.c)
export const editable = true;
</script>

<script setup lang="ts">
// Quantities → New quantities… (ticket QTY1, ADR 0080): a name, what it lists (IFC classes: the built-in families or
// any class typed), grouping (storey, material) and subtotals only; then one schedule_add (one undo step: the
// default columns of Rust's new_schedule) and the Quantities page shows the new list; Columns… chooses its columns,
// filter and sort. Openings are not elements of a list: the page's Openings table lists them, and a host's gross
// volume column adds them back.
import { computed, ref } from "vue";
import { scheduleAdd, scheduleList, type ScheduleDef } from "../bimble";
import { t } from "../i18n";
import { pickedSchedule } from "../pages/quantitiesPick";
import { send } from "../shell/send";
import { currentDialog } from ".";

const FAMILIES = [
  { k: "walls", cls: ["IfcWall", "IfcWallStandardCase"] }, { k: "slabs", cls: ["IfcSlab"] }, { k: "columns", cls: ["IfcColumn"] },
  { k: "beams", cls: ["IfcBeam"] }, { k: "footings", cls: ["IfcFooting"] },
] as const;
const picked = ref<string[]>([]);
const other = ref("");
const name = ref("");
const byLevel = ref(true);
const byMaterial = ref(false);
const summary = ref(false);
const error = ref<string | null>(null);
// The name proposed: Rust's "Schedule N", the first free (the core gives the same when the field is left empty).
const proposed = computed(() => {
  const taken = new Set((scheduleList() as ScheduleDef[]).map((d) => d.name.toLowerCase()));
  let n = 1;
  while (taken.has(`schedule ${n}`)) n++;
  return `Schedule ${n}`;
});
const close = () => { currentDialog.value = null; };

function create(): void {
  const classes = [...FAMILIES.filter((f) => picked.value.includes(f.k)).flatMap((f) => f.cls),
    ...other.value.split(",").map((c) => c.trim()).filter((c) => c)];
  const nm = name.value.trim() || proposed.value;
  error.value = scheduleAdd(nm, { classes, group_by_level: byLevel.value, group_by_material: byMaterial.value, summary: summary.value });
  if (error.value) return;
  close();
  pickedSchedule.value = nm;
  send({ do: "showPage", page: "quantities" }); // shell_graph: control newquantities-create
}
</script>

<template>
  <div class="bb-dialog nq" role="dialog" aria-modal="true" :aria-label="t('qty1.new.title')" data-bb="new-quantities-dialog">
    <div class="head">
      <h2 class="disp">{{ t('qty1.new.title') }}</h2>
      <span style="flex:1"></span>
      <button class="btn s" type="button" data-bb="new-quantities-close" @click="close">{{ t('qty1.new.cancel') }}</button>
    </div>
    <div class="grid">
      <label class="k" for="nq-name">{{ t('qty1.new.name') }}</label>
      <input id="nq-name" v-model="name" class="inp" :placeholder="proposed" maxlength="100" data-bb="new-quantities-name">
    </div>
    <fieldset class="fams">
      <legend class="k">{{ t('qty1.new.what') }}</legend>
      <label v-for="f in FAMILIES" :key="f.k"><input v-model="picked" type="checkbox" :value="f.k" :data-bb="`new-quantities-${f.k}`"> {{ t(`qty1.new.${f.k}`) }}</label>
      <span class="k" style="flex-basis:100%">{{ picked.length || other.trim() ? '' : t('qty1.new.all') }}</span>
    </fieldset>
    <div class="grid">
      <label class="k" for="nq-other">{{ t('qty1.new.classes') }}</label>
      <input id="nq-other" v-model="other" class="inp mono" placeholder="IfcMember, IfcPile" maxlength="400" data-bb="new-quantities-classes">
    </div>
    <fieldset class="fams">
      <legend class="k">{{ t('qty1.new.group') }}</legend>
      <label><input v-model="byLevel" type="checkbox" data-bb="new-quantities-by-level"> {{ t('qty1.new.groupLevel') }}</label>
      <label><input v-model="byMaterial" type="checkbox" data-bb="new-quantities-by-material"> {{ t('qty1.new.groupMaterial') }}</label>
      <label><input v-model="summary" type="checkbox" :disabled="!byLevel && !byMaterial" data-bb="new-quantities-summary"> {{ t('qty1.new.summary') }}</label>
    </fieldset>
    <p class="k" style="margin:0">{{ t('qty1.new.next') }}</p>
    <p v-if="error" class="err" role="alert" data-bb="new-quantities-error">{{ error }}</p>
    <div><button class="btn p" type="button" data-bb="new-quantities-create" @click="create">{{ t('qty1.new.create') }}</button></div>
  </div>
</template>

<style scoped>
.nq { width: min(460px, calc(100vw - 32px)); color: var(--bb-ink); display: flex; flex-direction: column; gap: 12px; }
.head { display: flex; align-items: center; gap: 10px; }
.disp { font-family: var(--bb-font-display); margin: 0; font-size: var(--bb-size-heading); font-weight: 700; }
.fams { border: 0; padding: 0; margin: 0; display: flex; flex-wrap: wrap; gap: 6px 14px; }
.fams legend { padding: 0; margin-bottom: 4px; }
.fams label { display: inline-flex; align-items: center; gap: 4px; }
.grid { display: grid; grid-template-columns: 150px 1fr; gap: 8px 10px; align-items: center; }
.k { color: var(--bb-stone); font-size: 13px; }
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.mono { font-family: var(--bb-font-data); }
.err { margin: 0; color: var(--bb-attention); font-size: 13px; }
:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
