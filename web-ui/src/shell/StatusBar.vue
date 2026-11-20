<script setup lang="ts">
import { state } from "../bimble";
import { lang, setLang, t, type Lang } from "../i18n";

const langs: Lang[] = ["fr", "en"];
</script>

<template>
  <!-- Reference: footer.status (D1). -->
  <footer class="status" data-bb="statusbar">
    <span class="hint" data-bb="status-hint">{{ t("status.hint") }}</span>
    <!-- ponytail: the pointer position has no Rust query yet; the slot keeps the reference layout, empty. -->
    <span class="mono q coords" style="font-size:12px" data-bb="status-coords" data-missing="pointer-coordinates"></span>
    <span style="flex:1"></span>
    <!-- The chips are a set: each carries the landmark, the check reads the first. None can be undone yet: no × disc. -->
    <span v-if="state.file" data-bb="status-chips" class="chip nx" :class="state.unsaved ? 'c-o' : 'c-m'">{{ t(state.unsaved ? "status.unsaved" : "status.saved") }}</span>
    <span v-if="state.selection.length" data-bb="status-chips" class="chip nx c-n">{{ t("status.selected", { n: state.selection.length }) }}</span>
    <!-- ponytail: no element-count query in bimble.ts yet; counts show what State holds. -->
    <span class="mono q" style="font-size:12px" data-bb="status-counts">{{ t("status.counts", { rev: state.rev, n: state.selection.length }) }}</span>
    <span class="seg" role="group" :aria-label="t('status.langGroup')" style="margin-left:4px" data-bb="status-lang">
      <button v-for="l in langs" :key="l" type="button" class="lg" :class="{ on: lang === l }"
        :aria-pressed="lang === l" :aria-label="t(l === 'fr' ? 'status.langFr' : 'status.langEn')"
        @click="setLang(l)">{{ l.toUpperCase() }}</button>
    </span>
  </footer>
</template>

<style scoped>
/* .status{display:flex;align-items:center;gap:8px;padding:4px 12px;background:#FFFFFF;border-top:1px solid #DDD6C8;font-size:13px;min-height:34px;flex-wrap:nowrap;white-space:nowrap} */
.status { display: flex; align-items: center; gap: var(--bb-space-2); padding: 4px 12px; background: var(--bb-sheet); border-top: var(--bb-border-hairline); font-size: var(--bb-size-small); min-height: 34px; flex-wrap: nowrap; white-space: nowrap; }
/* .status .hint{color:#141414;overflow:hidden;text-overflow:ellipsis;min-width:0;flex:0 1 auto} .status > *{flex-shrink:0} .status > .hint{flex-shrink:1} */
.status .hint { color: var(--bb-ink); overflow: hidden; text-overflow: ellipsis; min-width: 0; flex: 0 1 auto; }
.status > * { flex-shrink: 0; }
.status > .hint { flex-shrink: 1; }
/* .mono{font-family:'DM Mono',...} .status .q{color:#5C574D} */
.mono { font-family: var(--bb-font-data); }
.status .q { color: var(--bb-stone); }
/* Empty slot keeps the reference box: 47 DM Mono cells = "E 2'538'412.50 N 1'152'210.00 · 432.80 m (LV95)", one line high. */
.coords { min-width: 47ch; min-height: 1lh; }
/* .chip{padding:2px 4px 2px 10px;border-radius:999px;font-size:13px;display:inline-flex;gap:4px;align-items:center;white-space:nowrap;color:#141414;text-decoration:none} .chip.nx{padding-right:10px} */
.chip { padding: 2px 4px 2px 10px; border-radius: var(--bb-radius-pill); font-size: var(--bb-size-small); display: inline-flex; gap: var(--bb-space-1); align-items: center; white-space: nowrap; color: var(--bb-ink); text-decoration: none; }
.chip.nx { padding-right: 10px; }
.c-n { background: var(--bb-sunk); }
.c-o { background: var(--bb-ochre); }
.c-m { background: var(--bb-moss); }
/* .seg{display:inline-flex;gap:2px;padding:2px;border-radius:999px;background:#EDE7DB} */
.seg { display: inline-flex; gap: 2px; padding: 2px; border-radius: var(--bb-radius-pill); background: var(--bb-sunk); }
/* .seg button{padding:2px 12px;border-radius:999px;color:#5C574D;text-decoration:none;font:inherit;font-size:13px;border:0;background:none;cursor:pointer;white-space:nowrap} */
.seg button { padding: 2px 12px; border-radius: var(--bb-radius-pill); color: var(--bb-stone); text-decoration: none; font: inherit; font-size: var(--bb-size-small); border: 0; background: none; cursor: pointer; white-space: nowrap; }
/* .lg{border:0;cursor:pointer;font-family:'DM Mono',monospace;font-size:12px;padding:2px 9px;border-radius:999px} (lower specificity than .seg button in the reference: kept for the same cascade) */
.lg { border: 0; cursor: pointer; font-family: var(--bb-font-data); font-size: 12px; padding: 2px 9px; border-radius: var(--bb-radius-pill); }
/* active: inline style="background:#141414;color:#F5F2EB" in the reference (= .seg .on) */
.seg .on { background: var(--bb-ink); color: var(--bb-on-ink); }
</style>
