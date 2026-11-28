<script lang="ts">
import { computed, ref, watch } from "vue";
import { openViews, sheetLayout, sheetTab, sheets, state, type PlacedLayout, type SheetSummary } from "../bimble";
import { send } from "../shell/send";

/** The sheet the page shows: the sheet a tab opened last, else Rust's current sheet tab, else the first sheet; null
 *  without sheets. */
export const currentSheet = computed<SheetSummary | null>(() => {
  void stateRev.value;
  let all: SheetSummary[] = [];
  try { all = sheets(); } catch { /* no model yet */ }
  const cur = openViews.value.current;
  return all.find((s) => s.id === sheetTab.value?.id) || (cur?.type === "sheet" && all.find((s) => s.id === cur.id)) || all[0] || null;
});
/** The placed view selected on the sheet (its `placed` id), shared by the page and this panel. */
export const selectedPlaced = ref<string | null>(null);
/** The last verb error of the page or this panel. */
export const sheetsError = ref<string | null>(null);

// A sheet tab (tabCall type "sheet": the palette, the sheet list, the small sheet) opens this page on that sheet.
watch(sheetTab, (t) => {
  if (!t) return;
  selectedPlaced.value = null;
  send({ do: "showSheet", sheet: t.id });
});

/** The selected placed view as the layout gives it (C core `sheet_layout`), null otherwise. */
export const selectedLayout = computed<PlacedLayout | null>(() => {
  void stateRev.value;
  const s = currentSheet.value, id = selectedPlaced.value;
  if (!s || !id) return null;
  try { return sheetLayout(s.id).placed.find((p) => p.id === id) ?? null; } catch { return null; }
});
</script>

<script setup lang="ts">
import { stateRev } from "../bimble"; // VIS2
// D3's left panel (fr/SheetLayout.html `.nav` sheet list, `.props`). Real: the sheets (number, name, revision),
// "+" (add_sheet, then opened), number, name, paper and orientation, revision (set_sheet, one per blur/Enter),
// delete (sheet_remove), "place view" (sheet_place at the paper's centre), the placed view selected on the sheet
// (its view and scale) and "take off the sheet" (sheet_unplace).
import { runVerb, sheetPlace, sheetRemove, sheetUnplace, tabCall } from "../bimble";
import { viewName } from "../shell/viewName";
import { t } from "../i18n";

const caret = "m6 9 6 6 6-6";
const PAPERS = ["A4", "A3", "A2", "A1", "A0"];
/** sheet.rs size_mm: landscape width and height, mm. */
const SIZE: Record<string, [number, number]> = { A4: [297, 210], A3: [420, 297], A2: [594, 420], A1: [841, 594], A0: [1189, 841] };
const error = sheetsError;
const allSheets = computed<SheetSummary[]>(() => {
  void stateRev.value;
  try { return [...sheets()].sort((a, b) => a.number.localeCompare(b.number, undefined, { numeric: true })); } catch { return []; }
});

function addSheet() {
  const before = new Set(allSheets.value.map((s) => s.id));
  error.value = runVerb("add_sheet");
  let fresh: SheetSummary | undefined;
  try { fresh = sheets().find((s) => !before.has(s.id)); } catch { /* no model */ }
  if (!error.value && fresh) error.value = tabCall("open", { type: "sheet", id: fresh.id });
}

/** One set_sheet; on error the field shows the stored value again. */
function setSheet(e: Event, patch: Record<string, unknown>) {
  const s = currentSheet.value;
  if (!s) return;
  error.value = runVerb("set_sheet", { id: s.id, ...patch });
  if (error.value) {
    const el = e.target as HTMLInputElement | HTMLSelectElement;
    el.value = el.id === "sheet-size" ? `${s.paper}|${s.portrait}` : el.id === "sheet-name" ? s.name : el.id === "sheet-revision" ? (s.fields.revision ?? "") : s.number;
  }
}
function setSize(e: Event) {
  const [paper, portrait] = (e.target as HTMLSelectElement).value.split("|");
  setSheet(e, { paper, portrait: portrait === "true" });
}
const val = (e: Event) => (e.target as HTMLInputElement).value;
const openSheet = (id: string) => { error.value = tabCall("open", { type: "sheet", id }); };

function removeSheet() {
  const s = currentSheet.value;
  if (!s) return;
  error.value = sheetRemove(s.id);
  if (!error.value) selectedPlaced.value = null;
}

// ---- place a view: the views the session shows, at the paper's centre ----
const toPlace = ref("");
const viewList = computed(() => openViews.value.views.map((v) => ({ id: v.id, name: viewName(v) })));
function placeView() {
  const s = currentSheet.value;
  if (!s || !toPlace.value) return;
  const [w, h] = SIZE[s.paper] ?? [841, 594];
  const at: [number, number] = s.portrait ? [h / 2, w / 2] : [w / 2, h / 2];
  let before = new Set<string>();
  try { before = new Set(sheetLayout(s.id).placed.map((p) => p.id)); } catch { /* the Rust bridge: no layout */ }
  error.value = sheetPlace(s.id, toPlace.value, at);
  if (error.value) return;
  try { selectedPlaced.value = sheetLayout(s.id).placed.find((p) => !before.has(p.id))?.id ?? null; } catch { /* idem */ }
}
/** The selected placed view's name: the page's language for a default view (`viewName`). */
const selectedName = computed(() => {
  const p = selectedLayout.value;
  if (!p) return null;
  const v = openViews.value.views.find((x) => x.id === p.view);
  return v ? viewName(v) : (p.view_name ?? p.view);
});
function unplace() {
  const s = currentSheet.value, p = selectedLayout.value;
  if (!s || !p) return;
  error.value = sheetUnplace(s.id, p.id);
  if (!error.value) selectedPlaced.value = null;
}
</script>

<template>
  <nav class="nav" :aria-label="t('page.sheets')" data-bb="sheet-list">
    <div class="pn" style="display:flex;align-items:center;justify-content:space-between">{{ t('page.sheets') }}<button class="ib" type="button" :aria-label="t('sheets.add')" :title="t('sheets.add')" style="width:26px;height:26px" data-bb="sheet-add" @click="addSheet"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg></button></div>
    <a v-for="s in allSheets" :key="s.id" class="ti" :class="{ cur: s.id === currentSheet?.id }" href="#" :aria-current="s.id === currentSheet?.id ? 'page' : undefined" @click.prevent="openSheet(s.id)"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5 3h10l4 4v14H5z" /><path d="M14 3v5h5" /><rect x="8" y="11" width="8" height="6" /></svg><span class="mono" style="font-size:12px;min-width:30px">{{ s.number }}</span><span>{{ s.name }}</span><span class="n">{{ s.fields.revision ?? '' }}</span></a>
    <p v-if="!allSheets.length" class="note" style="margin:6px 0" data-bb="sheet-list-none">{{ t('sheets.none') }}</p><!-- FAKE1: no empty panel -->
  </nav>
  <div v-if="error" class="err" role="alert" data-bb="sheet-side-error">{{ error }}</div>
  <template v-if="currentSheet">
  <div class="pn" data-bb="sheet-props-title">{{ t('sheets.sheet', { n: currentSheet?.number ?? '' }) }}</div>
  <div class="row">
    <label class="k" for="sheet-number">{{ t('sheets.number') }}</label>
    <span style="display:inline-flex;align-items:center;gap:6px">
      <input id="sheet-number" class="inp mono" :value="currentSheet.number" style="width:90px;text-align:right" data-bb="sheet-number" @change="setSheet($event, { number: val($event) })" @keydown.enter="($event.target as HTMLInputElement).blur()">
      <span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px" />
    </span>
  </div>
  <div class="row"><label class="k" for="sheet-name">{{ t('sheets.name') }}</label><input id="sheet-name" class="inp" :value="currentSheet.name" style="width:140px" data-bb="sheet-name" @change="setSheet($event, { name: val($event) })" @keydown.enter="($event.target as HTMLInputElement).blur()"></div>
  <div class="row">
    <label class="k" for="sheet-size">{{ t('sheets.size') }}</label>
    <select id="sheet-size" class="inp" style="width:140px" :value="`${currentSheet.paper}|${currentSheet.portrait}`" data-bb="sheet-size" @change="setSize">
      <template v-for="p in PAPERS" :key="p"><option :value="`${p}|false`">{{ p }} {{ t('sheets.landscape') }}</option><option :value="`${p}|true`">{{ p }} {{ t('sheets.portrait') }}</option></template>
    </select>
  </div>
  <div class="row">
    <span class="k">{{ t('sheets.template') }}</span>
    <span class="sel-inp off" style="width:140px" data-missing="query: title block templates" data-bb="sheet-template">—<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="caret" /></svg></span>
  </div>
  <div class="row">
    <label class="k" for="sheet-revision">{{ t('sheets.revision') }}</label>
    <input id="sheet-revision" class="inp mono" :value="currentSheet.fields.revision ?? ''" style="width:80px" data-bb="sheet-revision" @change="setSheet($event, { revision: val($event) })" @keydown.enter="($event.target as HTMLInputElement).blur()">
  </div>
  <div class="row"><span class="k">{{ t('sheets.drawnChecked') }}</span><span class="mono off" data-missing="query: drawn / checked (team members, ADR 0055)">—</span></div>
  <div class="row"><span /><button type="button" class="qb" data-bb="sheet-remove" @click="removeSheet">{{ t('sheets.remove') }}</button></div>
  <div class="sec">{{ t('sheets.placeView') }}</div>
  <div class="row">
    <label class="k" for="sheet-place-view">{{ t('sheets.view') }}</label>
    <select id="sheet-place-view" v-model="toPlace" class="inp" style="width:140px" data-bb="sheet-place-view">
      <option value="" disabled>{{ t('sheets.chooseView') }}</option>
      <option v-for="v in viewList" :key="v.id" :value="v.id">{{ v.name }}</option>
    </select>
  </div>
  <div class="row"><span /><button type="button" class="qb" :disabled="!toPlace" data-bb="sheet-place" @click="placeView">{{ t('sheets.place') }}</button></div>
  <div class="sec">{{ t('sheets.selectedView') }}</div>
  <div class="row"><span class="k">{{ t('sheets.view') }}</span><span :class="{ off: !selectedLayout }" data-bb="sheet-selected-view">{{ selectedName ?? '—' }}</span></div>
  <div class="row"><span class="k">{{ t('sheets.viewScale') }}</span><span class="mono" :class="{ off: !selectedLayout }" data-bb="sheet-selected-scale">{{ selectedLayout ? `1:${selectedLayout.scale}` : '—' }}</span></div>
  <div v-if="selectedLayout" class="row"><span /><button type="button" class="qb" data-bb="sheet-unplace" @click="unplace">{{ t('sheets.unplace') }}</button></div>
  </template>
</template>

<style scoped>
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.k { color: var(--bb-stone); white-space: nowrap; }
.mono { font-family: 'DM Mono', monospace; }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} .inp.mono{font-family:'DM Mono',monospace;font-size:13px} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.inp.mono { font-family: 'DM Mono', monospace; font-size: 13px; }
/* .sel-inp{display:inline-flex;align-items:center;justify-content:space-between;gap:6px;height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 6px 0 8px;font-size:13.5px;white-space:nowrap} */
/* .sec{font-size:12px;font-family:'DM Mono',monospace;letter-spacing:.08em;text-transform:uppercase;color:#5C574D;margin-top:6px} */
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
/* .ti{...} .ti .n{...}; the current sheet is not a selection (ADR 0049: pink = selection), so it is marked sunk and bold, not .sel pink. */
.nav { display: flex; flex-direction: column; gap: 1px; margin-bottom: 10px; }
.ti { display: flex; gap: 6px; align-items: center; padding: 2px 6px; border-radius: 8px; color: var(--bb-ink); text-decoration: none; font-size: 13.5px; line-height: 20px; white-space: nowrap; }
.ti:hover, .ti.cur { background: var(--bb-sunk); }
.ti.cur { font-weight: 600; }
.ti .n { margin-left: auto; color: var(--bb-stone); font-family: 'DM Mono', monospace; font-size: 12px; }
.ib { display: inline-flex; align-items: center; justify-content: center; border: 0; background: none; border-radius: 8px; color: var(--bb-ink); cursor: pointer; }
.ib:hover { background: var(--bb-sunk); }
/* The page's quiet text buttons (LevelsGrids.vue .qb), clay text: an action (do). */
.qb { display: inline-flex; align-items: center; gap: 4px; height: 30px; padding: 0 12px; border-radius: var(--bb-radius-pill); border: 0; background: none; font: inherit; font-size: 13.5px; color: var(--bb-clay-text); cursor: pointer; white-space: nowrap; }
.qb:hover { background: var(--bb-sunk); }
.qb:disabled { opacity: .55; cursor: default; }
.err { color: var(--bb-ink); border-left: 3px solid var(--bb-attention); padding-left: 6px; font-size: 13px; }
/* Missing in Rust: the reference's .off (color:#5C574D;opacity:.55). */
.off, .inp:disabled { color: var(--bb-stone); opacity: .55; }
</style>
