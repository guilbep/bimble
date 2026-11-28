<script lang="ts">
import { element, state as st } from "../bimble";
import { t as tt } from "../i18n";
import { classNoun } from "../i18n/parts/modify";
// D6: "1 ligne sélectionnée : dalle D3" for one selected element; the reference has no other hint.
// ponytail: reads the selection, not whether its row is in the table (the "All elements" schedule lists every element).
export const capability = "quantities"; // FAKE1 (a read-only file has its schedules too: DEVIATIONS 17)
export const hint = () => {
  const s = st.value.selection;
  if (s.length !== 1 || st.value.editable === false) return ""; // FAKE1: query element refuses a read-only file
  try {
    const e = element(s[0]!);
    return { key: "quantities.hintOne", params: { cls: classNoun(e.class, (k) => tt(k as Parameters<typeof tt>[0])).noun.toLowerCase(), name: e.name } };
  } catch { return ""; }
};
</script>

<script setup lang="ts">
import { stateRev } from "../bimble"; // VIS2
// D6 Quantities (wireframes v01 Quantities.html; ADR 0029). The table is the real schedule from Rust
// (`schedule_rows`: text cells, subtotals and total as Rust writes them; nothing is summed here).
// Groups carry `ids` (one GlobalId or null per row): a row click selects, the selected element's row is pink
// (`tr.sl`). Group and filter change the schedule (`set_schedule`, undoable); Export CSV downloads
// `schedule_csv`. Columns has no picker in the reference: drawn disabled.
import { computed, ref, watch } from "vue";
import { materials, scheduleCsvAs, scheduleEdit, scheduleList, scheduleWindow, select, setSchedule, state, type SchedulePatch, type ScheduleDef, type ScheduleTable } from "../bimble";
import { currentDialog } from "../dialogs"; // QTY1: Columns… opens the dialog
import { t } from "../i18n";
import { pickedSchedule } from "./quantitiesPick";
import { openingRows, selectedOpening, type OpeningRow } from "../bimble"; // WAVE1B (B1)
import { foundationRows, type FoundationRow } from "../bimble"; // WAVE1B (B5)

// ponytail: the built-in "All elements" schedule is shown (it mixes classes, as the reference); no schedule
// picker in the reference. Add one when the navigator's schedule list opens a schedule here.
const def = computed<ScheduleDef | null>(() => {
  void stateRev.value;
  const all = scheduleList() as ScheduleDef[];
  return all.find((d) => d.name === (pickedSchedule.value ?? "All elements")) ?? all.find((d) => d.name === "All elements") ?? all[0] ?? null; // DEAD1: the palette's pick
});
type Table = ScheduleTable;
// QTY1: a page of rows (PAGE at a time; the core builds the table once per revision, a page is a window of it).
const PAGE = 200;
const openColumns = () => { if (def.value) pickedSchedule.value = def.value.name; currentDialog.value = "quantity-columns"; };
const offset = ref(0);
watch(() => def.value?.name, () => { offset.value = 0; });
const table = computed<Table | null>(() => {
  void stateRev.value;
  if (!def.value) return null;
  try { return scheduleWindow(def.value.name, offset.value, PAGE); } catch { return null; }
});
const rowCount = computed(() => table.value?.row_count ?? 0);
const allLists = computed(() => { void stateRev.value; return (scheduleList() as ScheduleDef[]).map((d) => d.name); });
const pick = (name: string) => { pickedSchedule.value = name; };
// Sort by a column: its field (the list's own order of fields, then its properties), stored in the list (one undo step).
const colIds = computed(() => [...((def.value?.fields as string[] | undefined) ?? []), ...((def.value?.properties as string[] | undefined) ?? [])]);
const sortOf = computed(() => def.value?.sort as { column: string; descending: boolean } | undefined);
function sortBy(i: number): void {
  const id = colIds.value[i];
  if (!def.value || !id) return;
  const s = sortOf.value;
  err.value = scheduleEdit(def.value.name, { sort: { column: id, descending: s?.column === id ? !s.descending : false } }) ?? "";
}
const mark = (i: number) => { const s = sortOf.value; return s && s.column === colIds.value[i] ? (s.descending ? " ↓" : " ↑") : ""; };
// A group's label: a material's line, a level within a material, a level, or a material (not by level).
const groupLabel = (g: Table["groups"][number]) => g.head || (g.material !== undefined && !g.level)
  ? (g.material ? t('qty1.material', { name: g.material }) : t('qty1.noMaterial'))
  : t('quantities.storey', { name: g.level });
// ---- WAVE1B (B1): the openings and sleeves, a row each (a sleeve's volume: the exact cylinder) ----
const opRows = computed<OpeningRow[]>(() => { void stateRev.value; return state.value.editable === false ? [] : openingRows(); });
const opSize = (r: OpeningRow) => r.kind === "sleeve" ? `Ø ${r.diameter ?? 0}` : r.kind === "opening" ? `${r.width ?? 0} × ${r.height ?? r.length ?? 0}` : "—";
const opTotal = computed(() => opRows.value.reduce((s, r) => s + (r.volume_m3 ?? 0), 0));
const opPick = (r: OpeningRow) => { select([r.host]); selectedOpening.value = r.id ? { host: r.host, id: r.id } : null; };
// ---- B5: the foundations (footings and rafts), apart from the slabs ----
const fdRows = computed<FoundationRow[]>(() => { void stateRev.value; return state.value.editable === false ? [] : foundationRows(); });
const fdTotal = computed(() => fdRows.value.reduce((s, r) => s + r.volume_m3, 0));
// ---- end WAVE1B ----
const shown = computed(() => table.value?.groups.reduce((n, g) => n + g.rows.length, 0) ?? 0);
const from = computed(() => (shown.value ? offset.value + 1 : 0));

const tr = (key: string, raw: string) => { const s = t(key); return s === key ? raw : s; };
const head = (c: string) => tr(`quantities.col.${c}`, c);
// A column with a unit holds numbers: right aligned, DM Mono. Class, material and GlobalId: DM Mono.
const num = (c: string) => c.endsWith(")") || c === "Count"; // QTY1: the count
const cls = (c = "") => (num(c) ? "r mono" : ["IFC class", "Material", "GlobalId"].includes(c) ? "mono" : "");
// A subtotal or total line: its label spans the cells before the first value Rust wrote.
const span = (cells: string[]) => Math.max(1, cells.findIndex((c) => c !== "") < 0 ? cells.length : cells.findIndex((c) => c !== ""));
const famLabel = (f: string) => tr(`quantities.family.${f}`, f);
// The model's families, and the schedule's own if no material has it any more.
const families = computed(() => [...new Set([...materials.value.map((m) => m.category), ...(def.value?.family ? [def.value.family as string] : [])])].sort());
const err = ref("");
const change = (patch: SchedulePatch) => { if (def.value) err.value = setSchedule(def.value.name, patch) ?? ""; };
function exportCsv() {
  if (!def.value || !table.value) return;
  try {
    const a = document.createElement("a");
    // QTY1: the headers the table shows (an audit found Level / Storey: one source now, the table's)
    a.href = URL.createObjectURL(new Blob([scheduleCsvAs(def.value.name, table.value.columns.map(head))], { type: "text/csv" }));
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
      <select class="sel-inp" style="width:190px" :value="def?.name ?? ''" :aria-label="t('qty1.pick')" data-bb="quantities-pick" @change="pick(($event.target as HTMLSelectElement).value)">
        <option v-for="n in allLists" :key="n" :value="n">{{ n }}</option>
      </select>
      <span style="flex:1" />
      <select class="sel-inp" style="width:170px" :disabled="!def" :value="def?.group_by_level ? 'level' : 'none'" :aria-label="t('quantities.group', { by: '' }).replace(/[\s:]+$/, '')" data-bb="quantities-group" @change="change({ group_by_level: ($event.target as HTMLSelectElement).value === 'level' })">
        <option v-for="b in ['none', 'level']" :key="b" :value="b">{{ t('quantities.group', { by: t(b === 'level' ? 'quantities.group.level' : 'quantities.group.none') }) }}</option>
      </select>
      <select class="sel-inp" style="width:170px" :disabled="!def" :value="(def?.family as string | undefined) ?? ''" :aria-label="t('quantities.filter', { family: '' }).replace(/[\s:]+$/, '')" data-bb="quantities-filter" @change="change({ family: ($event.target as HTMLSelectElement).value || null })">
        <option value="">{{ t('quantities.filter', { family: t('quantities.family.all') }) }}</option>
        <option v-for="f in families" :key="f" :value="f">{{ t('quantities.filter', { family: famLabel(f) }) }}</option>
      </select>
      <button class="btn s" type="button" :disabled="!def || state.editable === false" data-bb="quantities-columns" @click="openColumns">{{ t('quantities.columns') }}</button>
      <button class="btn p" type="button" :disabled="!table" data-bb="quantities-export" @click="exportCsv">{{ t('quantities.exportCsv') }}</button>
    </div>
    <table v-if="table" class="t" data-bb="quantities-table">
      <thead><tr><th v-for="(c, i) in table.columns" :key="c" :class="{ r: num(c) }" :aria-sort="mark(i) ? (mark(i).includes('↓') ? 'descending' : 'ascending') : undefined"><button type="button" class="sort" :title="t('qty1.sortTip')" :disabled="state.editable === false" :data-bb="`quantities-sort-${i}`" @click="sortBy(i)">{{ head(c) }}{{ mark(i) }}</button></th></tr></thead>
      <tbody>
        <template v-for="(g, gi) in table.groups" :key="gi">
          <tr v-if="g.level || g.material !== undefined" class="g" :class="{ m: g.head || (g.material !== undefined && !g.level), sub: g.material !== undefined && !!g.level }" data-bb="quantities-group">
            <td :colspan="span(g.subtotal)">{{ groupLabel(g) }}</td>
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
    <!-- QTY1: every row, a page of PAGE at a time (was "50 of 72 rows shown" with no way to the rest) -->
    <div v-if="table && rowCount > PAGE" class="head" data-bb="quantities-paging">
      <button class="btn s" type="button" :disabled="offset === 0" data-bb="quantities-prev" @click="offset = Math.max(0, offset - PAGE)">{{ t('qty1.prev') }}</button>
      <span class="note" data-bb="quantities-rows">{{ t('qty1.rows', { from, to: offset + shown, count: rowCount }) }}</span>
      <button class="btn s" type="button" :disabled="offset + PAGE >= rowCount" data-bb="quantities-next" @click="offset = offset + PAGE">{{ t('qty1.next') }}</button>
    </div>
    <p v-else-if="table && rowCount" class="note" style="margin:0" data-bb="quantities-rows">{{ t('qty1.rows', { from, to: offset + shown, count: rowCount }) }}</p>
    <p v-if="table && def?.summary" class="note" style="margin:0" data-bb="quantities-summary">{{ t('qty1.summaryNote', { count: table.count }) }}</p>
    <!-- WAVE1B (B5): foundations (footings and rafts) -->
    <template v-if="fdRows.length">
      <h3 style="font-family:'Sora',sans-serif;font-size:16px;margin:8px 0 0">{{ t('quantities.fd.title') }}</h3>
      <table class="t" data-bb="quantities-foundations">
        <thead><tr><th>{{ t('quantities.op.level') }}</th><th>{{ t('quantities.fd.name') }}</th><th>{{ t('quantities.op.kind') }}</th><th>{{ t('quantities.fd.section') }}</th><th class="r">{{ t('quantities.op.volume') }}</th></tr></thead>
        <tbody>
          <tr v-for="r in fdRows" :key="r.id" :class="{ sl: state.selection.includes(r.id) }" data-bb="quantities-foundation" :data-kind="r.kind" @click="select([r.id])">
            <td>{{ r.level }}</td><td>{{ r.name }}</td><td>{{ t(`quantities.fd.${r.kind}`) }}</td><td class="mono">{{ r.section }}</td><td class="r mono">{{ r.volume_m3.toFixed(3) }}</td>
          </tr>
        </tbody>
        <tfoot><tr><td colspan="4" style="font-weight:600;padding:6px 8px">{{ t('quantities.total') }}</td><td class="r mono" style="font-weight:600;padding:6px 8px">{{ fdTotal.toFixed(3) }}</td></tr></tfoot>
      </table>
    </template>
    <!-- WAVE1B (B1): openings and sleeves -->
    <template v-if="opRows.length">
      <h3 style="font-family:'Sora',sans-serif;font-size:16px;margin:8px 0 0">{{ t('quantities.op.title') }}</h3>
      <table class="t" data-bb="quantities-openings">
        <thead><tr><th>{{ t('quantities.op.level') }}</th><th>{{ t('quantities.op.host') }}</th><th>{{ t('quantities.op.kind') }}</th><th class="r">{{ t('quantities.op.size') }}</th><th class="r">{{ t('quantities.op.volume') }}</th></tr></thead>
        <tbody>
          <tr v-for="r in opRows" :key="`${r.host}/${r.id}`" :class="{ sl: selectedOpening?.id === r.id }" data-bb="quantities-opening" :data-kind="r.kind" @click="opPick(r)">
            <td>{{ r.level }}</td><td>{{ r.host_name }}</td><td>{{ t(`quantities.op.${r.kind}`) }}</td>
            <td class="r mono">{{ opSize(r) }}</td><td class="r mono">{{ r.volume_m3 === null ? "—" : r.volume_m3.toFixed(4) }}</td>
          </tr>
        </tbody>
        <tfoot><tr><td colspan="4" style="font-weight:600;padding:6px 8px">{{ t('quantities.total') }}</td><td class="r mono" style="font-weight:600;padding:6px 8px">{{ opTotal.toFixed(4) }}</td></tr></tfoot>
      </table>
      <p class="note" style="margin:0">{{ t('quantities.op.note') }}</p>
    </template>
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
.sel-inp { box-sizing: border-box; } /* rest of .sel-inp in shell.css */
/* .btn{...} .btn.p{background:#C06A41;border:0} .btn.s{background:#FFFFFF;border:1px solid #141414;font-weight:400} */
.btn:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* Missing in Rust: the reference's .off (color:#5C574D;opacity:.55). */
.off, button:disabled { color: var(--bb-stone); opacity: .55; cursor: default; }
/* table.t ... */
table.t { border-collapse: collapse; width: 100%; font-size: 13.5px; }
table.t th { text-align: left; font-weight: 600; color: var(--bb-stone); border-bottom: 1px solid var(--bb-stone); padding: 6px 8px; white-space: nowrap; font-size: 13px; }
table.t td { border-bottom: 1px solid var(--bb-sunk); padding: 5px 8px; vertical-align: middle; }
table.t tr.g td { background: var(--bb-cream); font-weight: 600; }
table.t tr.sl td { background: var(--bb-pink); }
table.t tr.g.m td { background: var(--bb-sunk); } /* QTY1: a material's line */
table.t tr.g.sub td:first-child { padding-left: 20px; }
.sort { all: unset; cursor: pointer; }
.sort:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
.sort:disabled { cursor: default; }
table.t td.r, table.t th.r { text-align: right; }
</style>
