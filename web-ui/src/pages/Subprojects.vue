<script lang="ts">
import SubprojectsSide from "./SubprojectsSide.vue";
// The active sub-project's name: no Rust source yet (query: subprojects → active), shown empty.
export const hint = () => ({ key: "subprojects.hint", params: { name: "" } });
export const side = SubprojectsSide;
</script>

<script setup lang="ts">
// C5 Sub-projects (wireframes v01 Subprojects.html; ADR 0053, Proposed). Sub-projects never lock: borrowing stays
// per element (the draft replaces ADR 0038's layer borrow). No sub-project data in Rust yet: the buttons and the
// table header are drawn as the reference, disabled; the rows (the `subprojects` query) are absent.
import { t } from "../i18n";
</script>

<template>
  <div class="sp" data-bb="subprojects">
    <div class="head">
      <h2 style="font-size:20px" data-bb="subprojects-title">{{ t('subprojects.title') }}</h2>
      <span style="flex:1" />
      <button class="btn s" type="button" disabled data-missing="verb: subproject_new {name}" data-bb="subprojects-new">
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg>{{ t('subprojects.new') }}
      </button>
      <button class="btn s" type="button" disabled data-missing="verb: subproject_set {id, name}">{{ t('subprojects.rename') }}</button>
      <button class="btn s" type="button" disabled data-missing="verb: subproject_move {ids, subproject}">{{ t('subprojects.move') }}</button>
    </div>
    <!-- The reference's rows (name, elements, visible by default, open on load, borrows, active; linked models with a
         dashed line and Active disabled): no Rust source yet. -->
    <table class="t" data-bb="subprojects-table"
           data-missing="query: subprojects [{id, name, elements, classes, visible_by_default, open_on_load, borrowed_by, active}]; verbs: subproject_set, subproject_activate">
      <thead><tr><th>{{ t('subprojects.name') }}</th><th class="r">{{ t('subprojects.elements') }}</th><th>{{ t('subprojects.visible') }}</th><th>{{ t('subprojects.open') }}</th><th>{{ t('subprojects.borrows') }}</th><th>{{ t('subprojects.active') }}</th></tr></thead>
      <tbody />
    </table>
    <div class="cards" data-bb="subprojects-cards">
      <div class="card"><div class="eyebrow">{{ t('subprojects.do') }}</div><p>{{ t('subprojects.doText') }}</p></div>
      <div class="card"><div class="eyebrow">{{ t('subprojects.dont') }}</div><p>{{ t('subprojects.dontText') }}</p></div>
    </div>
  </div>
</template>

<style scoped>
/* .page style="display:flex;flex-direction:column;gap:14px" (padding from .bb-page) */
.sp { display: flex; flex-direction: column; gap: 14px; }
/* style="display:flex;align-items:center;gap:10px;flex-wrap:wrap" */
.head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
/* .page h2{font-family:'Sora',sans-serif;margin:0} */
h2 { font-family: 'Sora', sans-serif; margin: 0; }
/* .btn{…} .btn.s{background:#FFFFFF;border:1px solid #141414;font-weight:400} */
.btn { display: inline-flex; align-items: center; justify-content: center; gap: 6px; min-height: 32px; padding: 5px 16px; border-radius: 999px; font: inherit; font-weight: 600; color: var(--bb-ink); cursor: pointer; white-space: nowrap; }
.btn.s { background: var(--bb-sheet); border: 1px solid var(--bb-ink); font-weight: 400; }
/* table.t … */
table.t { border-collapse: collapse; width: 100%; font-size: 13.5px; }
table.t th { text-align: left; font-weight: 600; color: var(--bb-stone); border-bottom: 1px solid var(--bb-stone); padding: 6px 8px; white-space: nowrap; font-size: 13px; }
table.t th.r { text-align: right; }
/* style="display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px" */
.cards { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 14px; }
/* .card{background:#FFFFFF;border:1px solid #DDD6C8;border-radius:16px;padding:18px} */
.card { background: var(--bb-sheet); border: 1px solid var(--bb-line); border-radius: 16px; padding: 18px; }
/* .eyebrow{font-family:'DM Mono',monospace;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#5C574D} */
.eyebrow { font-family: 'DM Mono', monospace; font-size: 12px; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); }
.card p { margin: 6px 0 0; }
button:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* Missing in Rust: the reference's .off (color:#5C574D;opacity:.55). */
button:disabled { color: var(--bb-stone); opacity: .55; cursor: default; }
</style>
