<script setup lang="ts">
import { state, visibility, actions, runAction, pointer, status, placing } from "../bimble";
import { lang, setLang, t, type Lang } from "../i18n";
import { page } from "../pages";
import { dialog } from "../dialogs";

const langs: Lang[] = ["fr", "en"];
// Each chip: a view mode read from `query("visibility")`; `undo` is the action its × runs, enabled as `query("actions")` says.
type Chip = { key: Parameters<typeof t>[0]; n: number; undo: string };
const enabled = (name: string) => actions.value.some((a) => a.name === name && a.enabled);
// The reference's "1 284 éléments · 0.4 s · 312 MB · au repos": visible elements, load time, memory, idle or fps.
const grouped = (n: number) => String(n).replace(/\B(?=(\d{3})+(?!\d))/g, " ");
const counts = (): string => {
  const s = status.value;
  if (!s) return "";
  const mb = Math.round((s.app_bytes || s.model_bytes) / 1048576);
  const rate = s.idle ? t("status.idle") : t("status.fps", { n: s.fps ?? 0 });
  return t("status.counts", { n: grouped(s.visible), s: (s.load_ms / 1000).toFixed(1), mb, rate });
};
// While placing: the hint keyed on kind and click step (Rust's `hint` is English only).
const placingHint = (): Parameters<typeof t>[0] | undefined => {
  const p = placing.value;
  return p?.active ? `placing.${p.kind}.${p.points.length ? 1 : 0}` : undefined;
};
const chips = (): Chip[] => {
  const v = visibility.value;
  if (!v) return [];
  const all: Chip[] = [
    { key: "status.sectionBox", n: v.section_box.on ? 1 : 0, undo: "section_box" },
    { key: "status.hiddenNow", n: v.hidden_now.count, undo: "clear_temporary" },
    { key: "status.isolatedNow", n: v.isolated_now.count, undo: "clear_temporary" },
    { key: "status.hiddenInView", n: v.hidden_in_view.count, undo: "show_all_in_view" },
    { key: "status.isolatedInView", n: v.isolated_in_view.count, undo: "show_all_in_view" },
  ];
  return all.filter((c) => c.n > 0);
};
</script>

<template>
  <!-- Reference: footer.status (D1). -->
  <footer class="status" data-bb="statusbar">
    <span class="hint" data-bb="status-hint">{{ t(dialog()?.hint ?? page()?.hint ?? placingHint() ?? "status.hint") }}</span>
    <span class="mono q coords" style="font-size:12px" data-bb="status-coords">{{ pointer?.text ?? "" }}</span>
    <span style="flex:1"></span>
    <!-- Reference: `.status > .chip`, a set of direct children; each carries the landmark, the check reads the first. -->
    <!-- Reference (B1): `.chip.c-n` "Placement : dalle" with ×; × runs `place_<kind>`, which toggles placing off. -->
    <span v-if="placing?.active" data-bb="status-chip" class="chip c-n">{{ t("placing.chip", { kind: t(`placing.kind.${placing.kind}`) }) }}<button
        type="button" class="cx" :aria-label="t('placing.stop')" :disabled="!enabled(`place_${placing.kind}`)"
        @click="runAction(`place_${placing.kind}`)"><svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button></span>
    <span v-if="state.selection.length" data-bb="status-chip" class="chip c-p nx">{{ t(state.selection.length === 1 ? "status.selectedOne" : "status.selected", { n: state.selection.length }) }}</span>
    <span v-for="c in chips()" :key="c.key" data-bb="status-chip" class="chip c-n">{{ t(c.key, { n: c.n }) }}<button
        type="button" class="cx" :aria-label="t('status.undoChip')" :disabled="!enabled(c.undo)"
        @click="runAction(c.undo)"><svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button></span>
    <!-- ponytail: linked models have no query yet; no chip is drawn rather than a fake one. -->
    <span hidden data-missing="links"></span>
    <span class="mono q" style="font-size:12px" data-bb="status-counts">{{ counts() }}</span>
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
/* .chip .cx{width:18px;height:18px;border-radius:999px;display:inline-flex;align-items:center;justify-content:center} (a span there; a real button here, reset) */
.chip .cx { width: 18px; height: 18px; border-radius: var(--bb-radius-pill); display: inline-flex; align-items: center; justify-content: center; border: 0; padding: 0; background: none; color: inherit; font: inherit; cursor: pointer; }
/* disabled: .tool.off{color:#5C574D;opacity:.55} */
.chip .cx:disabled { color: var(--bb-stone); opacity: .55; cursor: default; }
.c-p { background: var(--bb-pink); }
/* .seg{display:inline-flex;gap:2px;padding:2px;border-radius:999px;background:#EDE7DB} */
.seg { display: inline-flex; gap: 2px; padding: 2px; border-radius: var(--bb-radius-pill); background: var(--bb-sunk); }
/* .seg button{padding:2px 12px;border-radius:999px;color:#5C574D;text-decoration:none;font:inherit;font-size:13px;border:0;background:none;cursor:pointer;white-space:nowrap} */
.seg button { padding: 2px 12px; border-radius: var(--bb-radius-pill); color: var(--bb-stone); text-decoration: none; font: inherit; font-size: var(--bb-size-small); border: 0; background: none; cursor: pointer; white-space: nowrap; }
/* .lg{border:0;cursor:pointer;font-family:'DM Mono',monospace;font-size:12px;padding:2px 9px;border-radius:999px} (lower specificity than .seg button in the reference: kept for the same cascade) */
.lg { border: 0; cursor: pointer; font-family: var(--bb-font-data); font-size: 12px; padding: 2px 9px; border-radius: var(--bb-radius-pill); }
/* active: inline style="background:#141414;color:#F5F2EB" in the reference (= .seg .on) */
.seg .on { background: var(--bb-ink); color: var(--bb-on-ink); }
</style>
