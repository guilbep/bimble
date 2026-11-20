<script lang="ts">
import { computed } from "vue";
import { openViews, sheets, state, type SheetSummary } from "../bimble";

/** The sheet the page shows: the current sheet tab, else the first sheet; null without sheets. */
export const currentSheet = computed<SheetSummary | null>(() => {
  void state.value.rev;
  let all: SheetSummary[] = [];
  try { all = sheets(); } catch { /* no model yet */ }
  const cur = openViews.value.current;
  return (cur?.type === "sheet" && all.find((s) => s.id === cur.id)) || all[0] || null;
});
</script>

<script setup lang="ts">
// D3's left panel properties (fr/SheetLayout.html `.props`). Real: number, name, paper and orientation, revision
// (the sheet's `revision` field). No sheet Command is bridged (bimble.ts has no SetSheet), so fields are read only.
import { t } from "../i18n";

const caret = "m6 9 6 6 6-6";
</script>

<template>
  <div class="pn" data-bb="sheet-props-title">{{ t('sheets.sheet', { n: currentSheet?.number ?? '' }) }}</div>
  <div class="row">
    <label class="k" for="sheet-number">{{ t('sheets.number') }}</label>
    <span style="display:inline-flex;align-items:center;gap:6px">
      <input id="sheet-number" class="inp mono" :value="currentSheet?.number ?? ''" style="width:90px;text-align:right" disabled data-missing="bimble.ts: SetSheet command (apply op set_sheet)" data-bb="sheet-number">
      <span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px" />
    </span>
  </div>
  <div class="row"><span class="k">{{ t('sheets.name') }}</span><span data-bb="sheet-name">{{ currentSheet?.name ?? '' }}</span></div>
  <div class="row">
    <span class="k">{{ t('sheets.size') }}</span>
    <span class="sel-inp off" style="width:140px" data-missing="bimble.ts: SetSheet command (paper, portrait)" data-bb="sheet-size">
      {{ currentSheet ? `${currentSheet.paper} ${t(currentSheet.portrait ? 'sheets.portrait' : 'sheets.landscape')}` : '' }}<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="caret" /></svg>
    </span>
  </div>
  <div class="row">
    <span class="k">{{ t('sheets.template') }}</span>
    <span class="sel-inp off" style="width:140px" data-missing="query: title block templates" data-bb="sheet-template">—<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="caret" /></svg></span>
  </div>
  <div class="row">
    <span class="k">{{ t('sheets.revision') }}</span>
    <span class="sel-inp off" style="width:80px" data-missing="query: revisions; SetSheet command" data-bb="sheet-revision">{{ currentSheet?.fields.revision ?? '—' }}<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path :d="caret" /></svg></span>
  </div>
  <div class="row"><span class="k">{{ t('sheets.drawnChecked') }}</span><span class="mono off" data-missing="query: drawn / checked (team members, ADR 0055)">—</span></div>
  <div class="sec">{{ t('sheets.selectedView') }}</div>
  <div class="row"><span class="k">{{ t('sheets.view') }}</span><span class="off" data-missing="query: the placed view selected on the sheet">—</span></div>
  <div class="row"><span class="k">{{ t('sheets.viewScale') }}</span><span class="mono off" data-missing="query: the placed view selected on the sheet">—</span></div>
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
.sel-inp { display: inline-flex; align-items: center; justify-content: space-between; gap: 6px; height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 6px 0 8px; font-size: 13.5px; white-space: nowrap; }
/* .sec{font-size:12px;font-family:'DM Mono',monospace;letter-spacing:.08em;text-transform:uppercase;color:#5C574D;margin-top:6px} */
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
/* Missing in Rust: the reference's .off (color:#5C574D;opacity:.55). */
.off, .inp:disabled { color: var(--bb-stone); opacity: .55; }
</style>
