<script lang="ts">
export const capability = "schedules"; // QTY1: verb schedule_edit (ccore/src/verbs_schedules.c)
export const editable = true;
</script>

<script setup lang="ts">
// Quantities → Columns… (ticket QTY1, ADR 0080): the open list's columns (what the core computes per element:
// `query schedule_columns`, and the property keys the model's elements have), ticked and ordered, its filter and
// its sort; Apply is one schedule_edit (one undo step) and the Quantities page shows the list. Rust's schedule
// panel (schedules.rs schedule_properties) is the model: ticked columns show, in this order; a built-in list is
// stored changed with the model.
import { computed, ref } from "vue";
import { scheduleColumns, scheduleEdit, scheduleList, type ScheduleDef } from "../bimble";
import { t } from "../i18n";
import { pickedSchedule } from "../pages/quantitiesPick";
import { send } from "../shell/send";
import { currentDialog } from ".";

const cols = scheduleColumns();
const all = scheduleList() as ScheduleDef[];
const def = all.find((d) => d.name === (pickedSchedule.value ?? "All elements")) ?? all.find((d) => d.name === "All elements") ?? all[0] ?? null;
const listName = def?.name ?? "";
const builtin = cols.builtin.includes(listName);
const fields0 = (def?.fields as string[] | undefined) ?? [];
const props0 = (def?.properties as string[] | undefined) ?? [];
// Rows: the list's columns in order (ticked), then the other fields (unticked), then the properties.
type Row = { id: string; prop: boolean; on: boolean };
const rows = ref<Row[]>([
  ...fields0.map((id) => ({ id, prop: false, on: true })),
  ...cols.fields.filter((f) => !fields0.includes(f.id)).map((f) => ({ id: f.id, prop: false, on: false })),
  ...props0.map((id) => ({ id, prop: true, on: true })),
  ...cols.properties.filter((p) => !props0.includes(p)).map((id) => ({ id, prop: true, on: false })),
]);
const fieldRows = computed(() => rows.value.filter((r) => !r.prop));
const propRows = computed(() => rows.value.filter((r) => r.prop));
const title = (id: string) => { const f = cols.fields.find((x) => x.id === id)?.title ?? id; const k = `quantities.col.${f}`; const s = t(k); return s === k ? f : s; };
const filter0 = def?.filter as { column: string; value: string } | undefined;
const sort0 = def?.sort as { column: string; descending: boolean } | undefined;
const filterCol = ref(filter0?.column ?? "");
const filterVal = ref(filter0?.value ?? "");
const sortCol = ref(sort0?.column ?? "");
const sortDesc = ref(sort0?.descending ?? false);
const error = ref<string | null>(null);
const close = () => { currentDialog.value = null; };

/** Moves a row of its kind one place up (-1) or down (+1). */
function move(r: Row, by: -1 | 1): void {
  const v = rows.value, i = v.indexOf(r), j = i + by;
  if (j < 0 || j >= v.length || v[j]!.prop !== r.prop) return;
  [v[i], v[j]] = [v[j]!, v[i]!];
}

function apply(): void {
  const columns = fieldRows.value.filter((r) => r.on).map((r) => r.id);
  const properties = propRows.value.filter((r) => r.on).map((r) => r.id);
  error.value = scheduleEdit(listName, {
    columns, properties,
    filter: filterCol.value ? { column: filterCol.value, value: filterVal.value } : null,
    sort: sortCol.value ? { column: sortCol.value, descending: sortDesc.value } : null,
  });
  if (error.value) return;
  close();
  pickedSchedule.value = listName;
  send({ do: "showPage", page: "quantities" }); // shell_graph: control quantitycolumns-apply
}
</script>

<template>
  <div class="bb-dialog qc" role="dialog" aria-modal="true" :aria-label="t('qty1.cols.title', { name: listName })" data-bb="quantity-columns-dialog">
    <div class="head">
      <h2 class="disp" data-bb="quantity-columns-title">{{ t('qty1.cols.title', { name: listName }) }}</h2>
      <span style="flex:1"></span>
      <button class="btn s" type="button" data-bb="quantity-columns-close" @click="close">{{ t('qty1.new.cancel') }}</button>
    </div>
    <p v-if="builtin" class="k" style="margin:0">{{ t('qty1.cols.builtin') }}</p>
    <p class="k" style="margin:0">{{ t('qty1.cols.help') }}</p>
    <ol class="list" data-bb="quantity-columns-fields">
      <li v-for="r in fieldRows" :key="r.id" :data-col="r.id">
        <label><input v-model="r.on" type="checkbox" :data-bb="`quantity-column-${r.id}`"> {{ title(r.id) }}</label>
        <span style="flex:1"></span>
        <button class="btn s xs" type="button" :aria-label="`${t('qty1.cols.up')}: ${title(r.id)}`" :data-bb="`quantity-column-up-${r.id}`" @click="move(r, -1)">↑</button>
        <button class="btn s xs" type="button" :aria-label="`${t('qty1.cols.down')}: ${title(r.id)}`" @click="move(r, 1)">↓</button>
      </li>
    </ol>
    <div class="k">{{ t('qty1.cols.props') }}</div>
    <ol v-if="propRows.length" class="list" data-bb="quantity-columns-props">
      <li v-for="r in propRows" :key="r.id">
        <label class="mono"><input v-model="r.on" type="checkbox" :data-bb="`quantity-prop-${r.id}`"> {{ r.id }}</label>
        <span style="flex:1"></span>
        <button class="btn s xs" type="button" :aria-label="`${t('qty1.cols.up')}: ${r.id}`" @click="move(r, -1)">↑</button>
        <button class="btn s xs" type="button" :aria-label="`${t('qty1.cols.down')}: ${r.id}`" @click="move(r, 1)">↓</button>
      </li>
    </ol>
    <p v-else class="k" style="margin:0">{{ t('qty1.cols.noProps') }}</p>
    <div class="grid">
      <label class="k" for="qc-filter">{{ t('qty1.cols.filter') }}</label>
      <span class="pair">
        <select id="qc-filter" v-model="filterCol" class="inp" data-bb="quantity-columns-filter">
          <option value="">{{ t('qty1.cols.filterNone') }}</option>
          <option v-for="r in rows" :key="r.id" :value="r.id">{{ r.prop ? r.id : title(r.id) }}</option>
        </select>
        <input v-model="filterVal" class="inp" :placeholder="t('qty1.cols.filterValue')" :disabled="!filterCol" maxlength="200" data-bb="quantity-columns-filter-value">
      </span>
      <label class="k" for="qc-sort">{{ t('qty1.cols.sort') }}</label>
      <span class="pair">
        <select id="qc-sort" v-model="sortCol" class="inp" data-bb="quantity-columns-sort">
          <option value="">{{ t('qty1.cols.sortDefault') }}</option>
          <option v-for="r in rows" :key="r.id" :value="r.id">{{ r.prop ? r.id : title(r.id) }}</option>
        </select>
        <label><input v-model="sortDesc" type="checkbox" :disabled="!sortCol" data-bb="quantity-columns-desc"> {{ t('qty1.cols.desc') }}</label>
      </span>
    </div>
    <p v-if="error" class="err" role="alert" data-bb="quantity-columns-error">{{ error }}</p>
    <div><button class="btn p" type="button" :disabled="!listName" data-bb="quantity-columns-apply" @click="apply">{{ t('qty1.cols.apply') }}</button></div>
  </div>
</template>

<style scoped>
.qc { width: min(520px, calc(100vw - 32px)); max-height: calc(100vh - 64px); overflow: auto; color: var(--bb-ink); display: flex; flex-direction: column; gap: 10px; }
.head { display: flex; align-items: center; gap: 10px; }
.disp { font-family: var(--bb-font-display); margin: 0; font-size: var(--bb-size-heading); font-weight: 700; }
.list { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 2px; }
.list li { display: flex; align-items: center; gap: 6px; }
.list label { display: inline-flex; align-items: center; gap: 6px; }
.xs { padding: 0 6px; height: 22px; font-size: 12px; }
.grid { display: grid; grid-template-columns: 90px 1fr; gap: 8px 10px; align-items: center; }
.pair { display: flex; gap: 8px; align-items: center; }
.k { color: var(--bb-stone); font-size: 13px; }
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.mono { font-family: var(--bb-font-data); }
.err { margin: 0; color: var(--bb-attention); font-size: 13px; }
:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
