<script lang="ts">
export const capability = "sheets"; // VIEWSHEET: verb sheet_place (SH1), query sheet_layout
</script>

<script setup lang="ts">
import { stateRev } from "../bimble"; // VIS2
// Sheets → Place view… (ticket VIEWSHEET, ADR 0076): on the sheet shown, the views of the model; a click places
// that view at the paper's centre (one sheet_place, one undo step) and selects it on the sheet, to be moved with
// the Select tool. A view already on a sheet is placed again (the core keeps one view on several sheets, as Rust's
// layout does; the list says where it already is). The ribbon tool is off without a sheet shown (action
// sheet_place_view).
import { computed, ref } from "vue";
import { openViews, sheetLayout, sheetPlace, sheets, state, type SheetSummary } from "../bimble";
import { t } from "../i18n";
import { viewName } from "../shell/viewName";
import { send } from "../shell/send";
import { currentSheet, selectedPlaced } from "../pages/SheetsSide.vue";
import { currentDialog } from ".";

/** sheet.rs size_mm: landscape width and height, mm. */
const SIZE: Record<string, [number, number]> = { A4: [297, 210], A3: [420, 297], A2: [594, 420], A1: [841, 594], A0: [1189, 841] };
const error = ref<string | null>(null);
const close = () => { currentDialog.value = null; };
const all = computed<SheetSummary[]>(() => { void stateRev.value; try { return sheets(); } catch { return []; } });
/** How many sheets already show each view. */
const onSheets = computed(() => {
  const n = new Map<string, number>();
  for (const s of all.value) {
    try { for (const p of sheetLayout(s.id).placed) n.set(p.view, (n.get(p.view) ?? 0) + 1); } catch { /* not served */ }
  }
  return n;
});
const list = computed(() => openViews.value.views.map((v) => ({ id: v.id, name: viewName(v), on: onSheets.value.get(v.id) ?? 0 })));

function place(view: string): void {
  const s = currentSheet.value;
  if (!s) return;
  const [w, h] = SIZE[s.paper] ?? [841, 594];
  const at: [number, number] = s.portrait ? [h / 2, w / 2] : [w / 2, h / 2];
  let before = new Set<string>();
  try { before = new Set(sheetLayout(s.id).placed.map((p) => p.id)); } catch { /* idem */ }
  error.value = sheetPlace(s.id, view, at);
  if (error.value) return;
  try { selectedPlaced.value = sheetLayout(s.id).placed.find((p) => !before.has(p.id))?.id ?? null; } catch { /* idem */ }
  close();
  send({ do: "showSheet", sheet: s.id }); // shell_graph: control placeview-pick
}
</script>

<template>
  <div class="bb-dialog pv" role="dialog" aria-modal="true" :aria-label="t('vs.placeView.title')" data-bb="place-view">
    <div class="head">
      <h2 class="disp">{{ t('vs.placeView.title') }}<span v-if="currentSheet" class="mono sub"> · {{ currentSheet.number }} {{ currentSheet.name }}</span></h2>
      <span style="flex:1"></span>
      <button class="btn s" type="button" data-bb="place-view-close" @click="close">{{ t('vs.close') }}</button>
    </div>
    <p class="note">{{ t('vs.placeView.pick') }}</p>
    <ul v-if="list.length" class="views" data-bb="place-view-list">
      <li v-for="v in list" :key="v.id">
        <button class="row" type="button" :data-bb="`place-view-${v.id}`" @click="place(v.id)">
          <span>{{ v.name }}</span><span v-if="v.on" class="on">{{ t('vs.placeView.placed', { n: v.on }) }}</span>
        </button>
      </li>
    </ul>
    <p v-else class="note">{{ t('vs.placeView.none') }}</p>
    <p v-if="error" class="err" role="alert" data-bb="place-view-error">{{ error }}</p>
  </div>
</template>

<style scoped>
.pv { width: min(440px, calc(100vw - 32px)); color: var(--bb-ink); display: flex; flex-direction: column; gap: 10px; }
.head { display: flex; align-items: center; gap: 10px; }
.disp { font-family: var(--bb-font-display); margin: 0; font-size: var(--bb-size-heading); font-weight: 700; }
.sub { font-size: 13px; font-weight: 400; color: var(--bb-stone); }
.mono { font-family: var(--bb-font-data); }
.note { margin: 0; font-size: var(--bb-size-small); color: var(--bb-stone); }
.views { list-style: none; margin: 0; padding: 0; max-height: 50vh; overflow: auto; border: 1px solid var(--bb-line); border-radius: 10px; }
.row { display: flex; width: 100%; justify-content: space-between; gap: 10px; padding: 7px 10px; border: 0; border-bottom: 1px solid var(--bb-sunk); background: none; font: inherit; color: var(--bb-ink); text-align: left; cursor: pointer; }
.row:hover { background: var(--bb-sunk); }
.on { color: var(--bb-stone); font-size: 12px; white-space: nowrap; }
.err { margin: 0; color: var(--bb-attention); font-size: 13px; }
:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
