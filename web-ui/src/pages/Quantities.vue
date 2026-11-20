<script setup lang="ts">
// D6 Quantities (wireframes v01 Quantities.html; ADR 0029). The table is the real schedule from Rust
// (`schedule_rows`: text cells, subtotals and total as Rust writes them; nothing is summed here).
// Groups carry `ids` (one GlobalId or null per row): a row click selects, the selected element's row is pink
// (`tr.sl`). Group, filter, columns and CSV have no Rust call: drawn disabled.
import { computed } from "vue";
import { scheduleList, scheduleRows, select, state, type ScheduleDef, type ScheduleTable } from "../bimble";
import { t } from "../i18n";

// ponytail: the built-in "All elements" schedule is shown (it mixes classes, as the reference); no schedule
// picker in the reference. Add one when the navigator's schedule list opens a schedule here.
const def = computed<ScheduleDef | null>(() => {
  void state.value.rev;
  const all = scheduleList() as ScheduleDef[];
  return all.find((d) => d.name === "All elements") ?? all[0] ?? null;
});
// ponytail: `ids` is not in bimble.ts's ScheduleTable yet (not this ticket's file); widened here.
type Table = ScheduleTable & { groups: (ScheduleTable["groups"][number] & { ids?: (string | null)[] })[] };
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
const family = computed(() => {
  const f = def.value?.family as string | undefined;
  return f ? tr(`quantities.family.${f}`, f) : t("quantities.family.all");
});
</script>

<template>
  <div class="qt" data-bb="quantities">
    <div class="head">
      <h2 style="font-size:20px" data-bb="quantities-title">{{ table?.name ?? def?.name ?? '' }}</h2>
      <span style="flex:1" />
      <span class="sel-inp off" style="width:170px" data-missing="verb: change a schedule's grouping (SetSchedules)" data-bb="quantities-group">{{ t('quantities.group', { by: t(def?.group_by_level ? 'quantities.group.level' : 'quantities.group.none') }) }}<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6" /></svg></span>
      <span class="sel-inp off" style="width:170px" data-missing="verb: change a schedule's filter (SetSchedules)" data-bb="quantities-filter">{{ t('quantities.filter', { family }) }}<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6" /></svg></span>
      <button class="btn s" type="button" disabled data-missing="verb: change a schedule's columns (SetSchedules)" data-bb="quantities-columns">{{ t('quantities.columns') }}</button>
      <button class="btn p" type="button" disabled data-missing="action: export a schedule as CSV (egui schedules.rs to_csv)" data-bb="quantities-export">{{ t('quantities.exportCsv') }}</button>
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
