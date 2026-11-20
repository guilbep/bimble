<script lang="ts">
import HistorySide from "./HistorySide.vue";
export const hint = "history.hint";
export const side = HistorySide;
</script>

<script setup lang="ts">
// C6 History (wireframes v01 History.html; ADR 0038). Versions, variants, authors, dates and compare are not bridged
// (the egui history panel reads bimble_core::repo directly): drawn as the reference, disabled, data-missing.
// Real data: the session's undo steps (`query("history")`), shown as the changes not yet in a version.
// ponytail: entries are display only; there is no Rust "undo up to step N" call.
import { historyLabels } from "../bimble";
import { t } from "../i18n";
</script>

<template>
  <div class="hi" data-bb="history">
    <section class="col">
      <div class="head">
        <h2 style="font-size:20px" data-bb="history-title">{{ t('history.title') }}</h2>
        <span style="flex:1" />
        <span class="sel-inp off" style="width:170px" data-missing="query: variants (branches) and switch verb" data-bb="history-branch">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="6" cy="5" r="2" /><circle cx="6" cy="19" r="2" /><circle cx="18" cy="8" r="2" /><path d="M6 7v10M18 10c0 5-12 3-12 7" /></svg> —<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6" /></svg>
        </span>
        <button class="btn s" type="button" disabled data-missing="verb: new variant (branch)" data-bb="history-new-variant">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg>{{ t('history.newVariant') }}
        </button>
      </div>
      <!-- The reference's version rows (hash, message, author, time, graph): no Rust source yet, so no rows. -->
      <div class="versions" data-missing="query: versions (Repo log: hash, message, author, time, variant)" />
      <div class="card" data-bb="history-new-version">
        <div class="eyebrow">{{ t('history.newVersion') }}</div>
        <div class="row"><span>{{ t('history.pending') }}</span><span class="chip c-o nx off" data-missing="query: elements changed since the last version">{{ t('history.elements', { n: '—' }) }}</span></div>
        <input class="inp" :placeholder="t('history.messagePlaceholder')" :aria-label="t('history.message')" disabled data-missing="verb: create a version (Repo commit)">
        <div style="display:flex;gap:10px;justify-content:flex-end">
          <button class="btn s" type="button" disabled data-missing="query: diff since the last version">{{ t('history.showChanges') }}</button>
          <button class="btn p" type="button" disabled data-missing="verb: create a version (Repo commit)" data-bb="history-commit">{{ t('history.createVersion') }}</button>
        </div>
      </div>
    </section>
    <section class="col">
      <h2 style="font-size:20px" data-bb="history-version">{{ t('history.version') }} <span class="mono off" style="font-size:14px" data-missing="query: selected version hash">—</span></h2>
      <p class="note" style="margin:0" data-bb="history-meta">{{ t('history.changes', { n: historyLabels.undo.length }) }}</p>
      <!-- Real rows: the session's undo steps, newest first. Element / field / before → after need a per-element diff. -->
      <table class="t" data-bb="history-changes" data-missing="query: per-element changes (element, field, before → after)">
        <thead><tr><th>{{ t('history.element') }}</th><th>{{ t('history.field') }}</th><th>{{ t('history.change') }}</th></tr></thead>
        <tbody>
          <tr v-for="(label, i) in historyLabels.undo" :key="i"><td colspan="3">{{ label }}</td></tr>
        </tbody>
      </table>
      <div style="display:flex;gap:10px">
        <button class="btn s" type="button" disabled data-missing="verb: compare two versions in 3D" data-bb="history-compare">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="5" width="7" height="14" /><rect x="14" y="5" width="7" height="14" /><path d="M10 12h4" /></svg>{{ t('history.compare3d') }}
        </button>
        <button class="btn s" type="button" disabled data-missing="verb: merge a variant" data-bb="history-merge">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="6" cy="5" r="2" /><circle cx="6" cy="19" r="2" /><circle cx="18" cy="8" r="2" /><path d="M6 7v10M18 10c0 5-12 3-12 7" /></svg>{{ t('history.merge', { variant: '—' }) }}
        </button>
      </div>
    </section>
  </div>
</template>

<style scoped>
/* .page style="display:grid;grid-template-columns:minmax(0,1.2fr) minmax(0,1fr);gap:24px" (padding from .bb-page) */
.hi { display: grid; grid-template-columns: minmax(0, 1.2fr) minmax(0, 1fr); gap: 24px; align-content: start; }
/* style="display:flex;flex-direction:column;gap:10px;min-width:0" */
.col { display: flex; flex-direction: column; gap: 10px; min-width: 0; }
/* style="display:flex;align-items:center;gap:10px;flex-wrap:wrap" */
.head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
/* .page h2{font-family:'Sora',sans-serif;margin:0} */
h2 { font-family: 'Sora', sans-serif; margin: 0; }
.mono { font-family: 'DM Mono', monospace; }
/* .note{font-size:13px;color:#5C574D} */
.note { font-size: 13px; color: var(--bb-stone); }
.versions { display: flex; flex-direction: column; gap: 10px; }
.versions:empty { display: none; }
/* .card{background:#FFFFFF;border:1px solid #DDD6C8;border-radius:16px;padding:18px} + style="padding:14px;display:flex;flex-direction:column;gap:8px;margin-top:6px" */
.card { background: var(--bb-sheet); border: 1px solid var(--bb-line); border-radius: 16px; padding: 14px; display: flex; flex-direction: column; gap: 8px; margin-top: 6px; }
/* .eyebrow{font-family:'DM Mono',monospace;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#5C574D} */
.eyebrow { font-family: 'DM Mono', monospace; font-size: 12px; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
/* .chip{padding:2px 4px 2px 10px;border-radius:999px;font-size:13px;display:inline-flex;gap:4px;align-items:center;white-space:nowrap;color:#141414} .chip.nx{padding-right:10px} .c-o{background:#D2B347} */
.chip { padding: 2px 4px 2px 10px; border-radius: var(--bb-radius-pill); font-size: 13px; display: inline-flex; gap: 4px; align-items: center; white-space: nowrap; color: var(--bb-ink); }
/* Text on a fill is ink (ADR 0049): a disabled chip keeps ink at full opacity (stone or .55 on ochre is under 4.5:1). */
.chip.off { color: var(--bb-ink); opacity: 1; }
/* .sel-inp{display:inline-flex;align-items:center;justify-content:space-between;gap:6px;height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 6px 0 8px;font-size:13.5px;white-space:nowrap} */
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
/* .btn{...} .btn.p{background:#C06A41;border:0} .btn.s{background:#FFFFFF;border:1px solid #141414;font-weight:400} */
.inp:focus-visible, button:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* Missing in Rust: the reference's .off (color:#5C574D;opacity:.55). */
.off, button:disabled, input:disabled { color: var(--bb-stone); opacity: .55; cursor: default; }
/* table.t ... */
table.t { border-collapse: collapse; width: 100%; font-size: 13.5px; }
table.t th { text-align: left; font-weight: 600; color: var(--bb-stone); border-bottom: 1px solid var(--bb-stone); padding: 6px 8px; white-space: nowrap; font-size: 13px; }
table.t td { border-bottom: 1px solid var(--bb-sunk); padding: 5px 8px; vertical-align: middle; }
</style>
