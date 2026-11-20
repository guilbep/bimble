<script lang="ts">
import { element, state as st } from "../bimble";
import { t as tt } from "../i18n";
import { classNoun } from "../i18n/parts/modify";
// D6: "1 ligne sélectionnée : dalle D3" for one selected element; the reference has no other hint.
// ponytail: reads the selection, not whether its row is in the table (the "All elements" schedule lists every element).
export const hint = () => {
  const s = st.value.selection;
  if (s.length !== 1) return "";
  try {
    const e = element(s[0]!);
    return { key: "quantities.hintOne", params: { cls: classNoun(e.class, (k) => tt(k as Parameters<typeof tt>[0])).noun.toLowerCase(), name: e.name } };
  } catch { return ""; }
};
</script>

<script setup lang="ts">
// D6 Quantities (wireframes v01 Quantities.html; ADR 0029). The table is the real schedule from Rust
// (`schedule_rows`: text cells, subtotals and total as Rust writes them; nothing is summed here).
// Groups carry `ids` (one GlobalId or null per row): a row click selects, the selected element's row is pink
// (`tr.sl`). Group and filter change the schedule (`set_schedule`, undoable); Export CSV downloads
// `schedule_csv`. Columns has no picker in the reference: drawn disabled.
import { computed, ref } from "vue";
import { materials, scheduleCsv, scheduleList, scheduleRows, select, setSchedule, state, type SchedulePatch, type ScheduleDef, type ScheduleTable } from "../bimble";
import { t } from "../i18n";

// ponytail: the built-in "All elements" schedule is shown (it mixes classes, as the reference); no schedule
// picker in the reference. Add one when the navigator's schedule list opens a schedule here.
const def = computed<ScheduleDef | null>(() => {
  void state.value.rev;
  const all = scheduleList() as ScheduleDef[];
  return all.find((d) => d.name === "All elements") ?? all[0] ?? null;
});
type Table = ScheduleTable;
const table = computed<Table | null>(() => {
  void state.value.rev;
  if (!def.value) return null;
  try { return scheduleRows(def.value.name); } catch { return null; }
});
const shown = computed(() => table.value?.groups.reduce((n, g) => n + g.rows.length, 0) ?? 0);

const tr = (key: string, raw: string) => { const s = t(key); return s === key ? raw : s; };
const head = (c: string) => tr(`quantities.col.${c}`, c);
// A column with a unit holds numbers: right aligned, DM Mono. Class, material and GlobalId: DM Mono.
const num = (c: string) => c.endsWith(")");
const cls = (c = "") => (num(c) ? "r mono" : ["IFC class", "Material", "GlobalId"].includes(c) ? "mono" : "");
// A subtotal or total line: its label spans the cells before the first value Rust wrote.
const span = (cells: string[]) => Math.max(1, cells.findIndex((c) => c !== "") < 0 ? cells.length : cells.findIndex((c) => c !== ""));
const famLabel = (f: string) => tr(`quantities.family.${f}`, f);
// The model's families, and the schedule's own if no material has it any more.
const families = computed(() => [...new Set([...materials.value.map((m) => m.category), ...(def.value?.family ? [def.value.family as string] : [])])].sort());
const err = ref("");
const change = (patch: SchedulePatch) => { if (def.value) err.value = setSchedule(def.value.name, patch) ?? ""; };
function exportCsv() {
  if (!def.value) return;
  try {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([scheduleCsv(def.value.name)], { type: "text/csv" }));
    a.download = `${def.value.name}.csv`;
    a.click();
    URL.revokeObjectURL(a.href);
    err.value = "";
  } catch (e) { err.value = String(e); }
}
</script>

<template>
  <div class="qt" data-bb="quantities">
    <div class="head">
      <h2 style="font-size:20px" data-bb="quantities-title">{{ table?.name ?? def?.name ?? '' }}</h2>
      <span style="flex:1" />
      <select class="sel-inp" style="width:170px" :disabled="!def" :value="def?.group_by_level ? 'level' : 'none'" :aria-label="t('quantities.group', { by: '' }).replace(/[\s:]+$/, '')" data-bb="quantities-group" @change="change({ group_by_level: ($event.target as HTMLSelectElement).value === 'level' })">
        <option v-for="b in ['none', 'level']" :key="b" :value="b">{{ t('quantities.group', { by: t(b === 'level' ? 'quantities.group.level' : 'quantities.group.none') }) }}</option>
      </select>
      <select class="sel-inp" style="width:170px" :disabled="!def" :value="(def?.family as string | undefined) ?? ''" :aria-label="t('quantities.filter', { family: '' }).replace(/[\s:]+$/, '')" data-bb="quantities-filter" @change="change({ family: ($event.target as HTMLSelectElement).value || null })">
        <option value="">{{ t('quantities.filter', { family: t('quantities.family.all') }) }}</option>
        <option v-for="f in families" :key="f" :value="f">{{ t('quantities.filter', { family: famLabel(f) }) }}</option>
      </select>
      <button class="btn s" type="button" disabled data-missing="UI: a column picker (not in the reference); set_schedule takes columns" data-bb="quantities-columns">{{ t('quantities.columns') }}</button>
      <button class="btn p" type="button" :disabled="!table" data-bb="quantities-export" @click="exportCsv">{{ t('quantities.exportCsv') }}</button>
    </div>
    <table v-if="table" class="t" data-bb="quantities-table">
      <thead><tr><th v-for="c in table.columns" :key="c" :class="{ r: num(c) }">{{ head(c) }}</th></tr></thead>
      <tbody>
        <template v-for="(g, gi) in table.groups" :key="gi">
          <tr v-if="g.level" class="g">
            <td :colspan="span(g.subtotal)">{{ t('quantities.storey', { name: g.level }) }}</td>
            <td v-for="(c, i) in g.subtotal.slice(span(g.subtotal))" :key="i" :class="cls(table.columns[i + span(g.subtotal)])">{{ c }}</td>
          </tr>
          <tr v-for="(r, ri) in g.rows" :key="ri" :class="{ sl: !!g.ids?.[ri] && state.selection.includes(g.ids[ri]!) }" @click="g.ids?.[ri] && select([g.ids[ri]!])">
            <td v-for="(c, i) in r" :key="i" :class="cls(table.columns[i])">{{ c }}</td>
          </tr>
        </template>
      </tbody>
      <tfoot v-if="table.total">
        <tr>
          <td :colspan="span(table.total)" style="font-weight:600;padding:6px 8px">{{ t('quantities.total') }}</td>
          <td v-for="(c, i) in table.total.slice(span(table.total))" :key="i" :class="cls(table.columns[i + span(table.total)])" style="font-weight:600;padding:6px 8px">{{ c }}</td>
        </tr>
      </tfoot>
    </table>
    <p v-else class="note" style="margin:0">{{ t('quantities.empty') }}</p>
    <p v-if="err" class="note" style="margin:0" role="alert" data-bb="quantities-error">{{ err }}</p>
    <p v-if="table?.truncated" class="note" style="margin:0" data-bb="quantities-truncated">{{ t('quantities.truncated', { shown, count: table.count }) }}</p>
    <p class="note" style="margin:0">{{ t('quantities.note') }}</p>
  </div>
</template>

<style scoped>
/* .page style="display:flex;flex-direction:column;gap:12px" (padding from .bb-page) */
.qt { display: flex; flex-direction: column; gap: 12px; }
/* style="display:flex;align-items:center;gap:10px;flex-wrap:wrap" */
.head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
/* .page h2{font-family:'Sora',sans-serif;margin:0} */
h2 { font-family: 'Sora', sans-serif; margin: 0; }
.mono { font-family: 'DM Mono', monospace; }
.note { font-size: 13px; color: var(--bb-stone); }
/* .sel-inp{display:inline-flex;align-items:center;justify-content:space-between;gap:6px;height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 6px 0 8px;font-size:13.5px;white-space:nowrap} */
.sel-inp { display: inline-flex; align-items: center; justify-content: space-between; gap: 6px; height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 6px 0 8px; font-size: 13.5px; white-space: nowrap; box-sizing: border-box; }
/* .btn{...} .btn.p{background:#C06A41;border:0} .btn.s{background:#FFFFFF;border:1px solid #141414;font-weight:400} */
.btn { display: inline-flex; align-items: center; justify-content: center; gap: 6px; min-height: 32px; padding: 5px 16px; border-radius: var(--bb-radius-pill); font: inherit; font-weight: 600; color: var(--bb-ink); cursor: pointer; text-decoration: none; white-space: nowrap; }
.btn.p { background: var(--bb-clay); border: 0; }
.btn.s { background: var(--bb-sheet); border: 1px solid var(--bb-ink); font-weight: 400; }
.btn:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* Missing in Rust: the reference's .off (color:#5C574D;opacity:.55). */
.off, button:disabled { color: var(--bb-stone); opacity: .55; cursor: default; }
/* table.t ... */
table.t { border-collapse: collapse; width: 100%; font-size: 13.5px; }
table.t th { text-align: left; font-weight: 600; color: var(--bb-stone); border-bottom: 1px solid var(--bb-stone); padding: 6px 8px; white-space: nowrap; font-size: 13px; }
table.t td { border-bottom: 1px solid var(--bb-sunk); padding: 5px 8px; vertical-align: middle; }
table.t tr.g td { background: var(--bb-cream); font-weight: 600; }
table.t tr.sl td { background: var(--bb-pink); }
table.t td.r, table.t th.r { text-align: right; }
</style>
