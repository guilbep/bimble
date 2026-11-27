<script lang="ts">
export const hint = "palette.hint";
</script>

<script setup lang="ts">
// Screen D8, Command palette (wireframes v01 fr/Palette.html). Lists the ribbon actions (`query("actions")`),
// the views (`open_views`), the sheets and the schedules; each entry runs its Rust call.
// Recent commands (`recent_actions`, Rust's last 5) come first when the query is empty, without a
// heading: the reference draws no recent section, so its layout holds.
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { actions, openView, openViews, paletteMatch, recentActions, runAction, scheduleList, sheets, state, tabCall, type ScheduleDef } from "../bimble";
import { t } from "../i18n";
import { RIBBON, type RibbonTool } from "../shell/ribbon-tabs";
import { viewName } from "../shell/viewName";
import { shortcutFor } from "../shell/keys"; // KEY1: the shortcut shown is the one the key table runs
import { notYetAction } from "../shell/capability"; // DEAD1
import { currentDialog } from ".";
import { send } from "../shell/send";
import { pickedSchedule } from "../pages/quantitiesPick";
import { currentPanel } from "../panels"; // SIZE2: the performance panel

interface Item { /** The Rust action name, for actions. */ name?: string; label: string; /** Also searched: the Rust (English) label. */ also?: string; note: string; kbd: string; icon: string; enabled: boolean; go: () => string | null;
  /** DEAD1: a disabled row says why: "not available yet" (an action of a capability this core does not serve), or the ribbon tool's reason it is off now. */
  notYet?: boolean; why?: string | undefined }

// Icons copied from the reference (and the navigator's, for plans).
const CUBE = '<path d="M12 3 20 7v10l-8 4-8-4V7z"/><path d="M4 7l8 4 8-4M12 11v10"/>';
const PLAN = '<rect x="4" y="4" width="16" height="16"/><path d="M4 12h9v8M13 4v4"/>';
const SECTION = '<path d="M3 12h18"/><path d="m7 8-4 4 4 4"/><rect x="9" y="5" width="6" height="14" stroke-dasharray="2 2"/>';
const SHEET = '<path d="M5 3h10l4 4v14H5z"/><path d="M14 3v5h5"/><rect x="8" y="11" width="8" height="6"/>';
const LIST = '<path d="M9 6h11M9 12h11M9 18h11"/><circle cx="5" cy="6" r="1"/><circle cx="5" cy="12" r="1"/><circle cx="5" cy="18" r="1"/>';
const GAUGE = '<path d="M4 18a8 8 0 1 1 16 0"/><path d="m12 18 4-6"/>'; // SIZE2

// The ribbon tool of each action: its icon. Labels: `action.<name>` (i18n/parts/actions.ts), the Rust label if a new action has none.
const tools = new Map<string, RibbonTool>();
for (const groups of Object.values(RIBBON)) for (const g of groups) for (const tool of g.tools) if (tool.action && !tools.has(tool.action)) tools.set(tool.action, tool);
// Rust tab (`ActionState.tab`) → the tab bar's copy, else `actionTab.<tab>` (i18n/parts/actions.ts).
const TAB: Record<string, string> = { File: "quick.file", Project: "tab.project", Collaborate: "tab.collaborate", Annotate: "tab.annotate", View: "tab.view", Sheets: "tab.sheets", Concrete: "tab.concrete", "Setting out": "tab.settingOut" /* SETOUT */, Quantities: "tab.quantities" /* QTY1 */ };
const copy = (key: string, rust: string) => { const s = t(key); return s === key ? rust : s; };

const items = computed<Item[]>(() => {
  void state.value.rev;
  const list: Item[] = actions.value.map((a) => {
    const tool = tools.get(a.name);
    return {
      name: a.name,
      label: copy(`action.${a.name}`, a.label),
      also: a.label,
      note: a.tab in TAB ? t(TAB[a.tab]!) : copy(`actionTab.${a.tab}`, a.tab),
      kbd: shortcutFor(a.name) || "—",
      icon: tool?.icon ?? "",
      enabled: a.enabled,
      go: () => runAction(a.name),
      // DEAD1: why a row is off: not served by this core (capability), else the ribbon's reason, if any
      notYet: !a.enabled && notYetAction(a.name),
      why: a.enabled ? undefined : notYetAction(a.name) ? t("na.tooltip", { name: copy(`action.${a.name}`, a.label) }) : tool?.offTip ? `${copy(`action.${a.name}`, a.label)}: ${t(tool.offTip)}` : undefined,
    };
  });
  for (const v of openViews.value.views) {
    list.push({ label: t("palette.goto", { name: viewName(v) }), note: t("tab.view"), kbd: "—", icon: v.type === "section" ? SECTION : v.type === "plan" ? PLAN : CUBE, enabled: true, go: () => openView(v.id) });
  }
  // SIZE2: the performance panel (not an action: a panel of the lab, as the status bar's button), always there
  list.push({ label: t("perf.paletteOpen"), also: "performance", note: t("perf.paletteNote"), kbd: "—", icon: GAUGE, enabled: true,
    go: () => { currentPanel.value = "performance"; return null; } });
  // ponytail: sheets throw without a model; no model, no entries. Schedules: the effective ones (`schedule_list`), as egui.
  try {
    for (const s of sheets()) list.push({ label: t("palette.goto", { name: `${s.number} ${s.name}` }), note: t("tab.sheets"), kbd: "—", icon: SHEET, enabled: true, go: () => tabCall("open", { type: "sheet", id: s.id }) });
    for (const s of scheduleList() as ScheduleDef[]) list.push({ label: t("palette.goto", { name: s.name }), note: t("tab.quantities"), kbd: "—", icon: LIST, enabled: true, go: () => goSchedule(s.name) });
  } catch { /* no model */ }
  return list;
});

/** DEAD1: a schedule is a page, not a view tab (the core's tab "open" takes views only: it answered
 *  400): the Quantities page on that schedule, through the shell. */
function goSchedule(name: string): string | null {
  pickedSchedule.value = name;
  return send({ do: "showPage", page: "quantities" }) ? null : `the Quantities page cannot be shown now`;
}

// Filtering is Rust's (`palette_match`, egui's word-start matcher, accent-insensitive), on the label
// joined with the Rust label: "sec" finds "Zone de coupe" through "Section box".
const text = ref("");
const shown = computed(() => {
  const all = items.value;
  const q = text.value.trim();
  if (q) {
    // Q2: a client whose palette_match fails (an old C core answered "not in the C core") must not
    // throw out of this computed: a plain case-insensitive part match instead, and one warning.
    const texts = all.map((i) => `${i.label} ${i.also ?? ""}`);
    try { return paletteMatch(q, texts).map((k) => all[k]!).filter(Boolean); } catch (e) {
      console.warn(`palette: palette_match: ${e instanceof Error ? e.message : String(e)}`);
      const lq = q.toLowerCase();
      return all.filter((_, k) => texts[k]!.toLowerCase().includes(lq));
    }
  }
  let recent: string[] = [];
  try { recent = recentActions(); } catch { /* no viewer yet */ }
  const first = recent.map((n) => all.find((i) => i.name === n)).filter((i): i is Item => !!i);
  return [...first, ...all.filter((i) => !first.includes(i))];
});
const index = ref(0);
watch(text, () => (index.value = 0));

const list = ref<HTMLElement | null>(null);
watch(index, async (i) => {
  await nextTick();
  list.value?.children[i]?.scrollIntoView({ block: "nearest" });
});

function run(i: number) {
  const item = shown.value[i];
  if (!item?.enabled) return;
  currentDialog.value = null;
  const err = item.go();
  if (err) console.warn(`palette ${item.label}: ${err}`);
}

function onKey(e: KeyboardEvent) {
  const n = shown.value.length;
  if (e.key === "ArrowDown" && n) index.value = (index.value + 1) % n;
  else if (e.key === "ArrowUp" && n) index.value = (index.value - 1 + n) % n;
  else if (e.key === "Enter") run(index.value);
  else return;
  e.preventDefault();
}

// The reference dims the viewport only (its .overlay sits in .vp): cover the viewport's box, as SharedFolder.
const input = ref<HTMLInputElement | null>(null);
const box = ref<Record<string, string>>({ inset: "0" });
let ro: ResizeObserver | null = null;
onMounted(() => {
  input.value?.focus();
  const vp = document.querySelector<HTMLElement>('[data-bb="viewport"]');
  if (!vp) return;
  const place = () => {
    const r = vp.getBoundingClientRect();
    if (r.width) box.value = { left: `${r.left}px`, top: `${r.top}px`, width: `${r.width}px`, height: `${r.height}px` };
  };
  place();
  ro = new ResizeObserver(place);
  ro.observe(vp);
});
onBeforeUnmount(() => ro?.disconnect());
</script>

<template>
  <div class="overlay pal-ov" :style="box" @click.self="currentDialog = null">
    <div class="pal" role="dialog" :aria-label="t('palette.title')" data-bb="palette">
      <div style="display:flex;align-items:center;gap:8px;padding:4px 6px">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="11" cy="11" r="6"/><path d="m20 20-4.5-4.5"/></svg>
        <input ref="input" v-model="text" class="inp" style="flex:1;border:0;font-size:16px" :aria-label="t('palette.query')" data-bb="palette-query" @keydown="onKey">
        <span class="kbd">Esc</span>
      </div>
      <div style="border-top:1px solid var(--bb-sunk)"></div>
      <div ref="list" class="rows" data-bb="palette-list">
        <a v-for="(item, i) in shown" :key="i" href="#" class="ti" :class="{ sel: i === index, off: !item.enabled }" style="padding:8px 12px;gap:12px"
          :aria-disabled="!item.enabled || undefined" :title="item.why" :data-not-yet="item.notYet ? '' : undefined" data-bb="palette-item" @click.prevent="run(i)">
          <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" v-html="item.icon"></svg>
          <span class="lab">{{ item.label }}</span><span v-if="item.notYet" class="note ny" data-bb="palette-not-yet">{{ t('palette.notYet') }}</span><span class="note">{{ item.note }}</span><span class="kbd">{{ item.kbd }}</span>
        </a>
      </div>
      <div class="note" style="padding:4px 12px" data-bb="palette-help">{{ t('palette.help') }}</div>
    </div>
  </div>
</template>

<style>
/* The overlay below dims the viewport only, as the reference: keep the window backdrop clear (it still closes on a click). */
.bb-app > .bb-backdrop:has(> .pal-ov) { background: none; }
</style>

<style scoped>
/* .overlay{...;background:rgba(20,20,20,.30);display:flex;align-items:center;justify-content:center;padding:24px} + style="align-items:flex-start;padding-top:60px" (fixed here: over the viewport's box) */
.overlay { position: fixed; box-sizing: border-box; background: color-mix(in srgb, var(--bb-ink) 30%, transparent); display: flex; align-items: flex-start; justify-content: center; padding: 24px; padding-top: 60px; }
/* role=dialog style="width:640px;background:#FFFFFF;border:1px solid #5C574D;border-radius:16px;padding:10px;display:flex;flex-direction:column;gap:6px" */
.pal { width: 640px; max-width: 100%; max-height: 100%; background: var(--bb-sheet); border: 1px solid var(--bb-stone); border-radius: 16px; padding: 10px; display: flex; flex-direction: column; gap: 6px; font-size: 14px; line-height: 1.35; color: var(--bb-ink); }
.pal * { box-sizing: border-box; }
/* The rows scroll when they do not fit (the reference shows a filtered few); same gap as the dialog. */
.rows { display: flex; flex-direction: column; gap: 6px; overflow: auto; min-height: 0; }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
/* ponytail: the caret shows the focus; the field is borderless in the reference, so no ring. */
.inp:focus { outline: none; }
/* .kbd{font-family:'DM Mono',monospace;font-size:11.5px;padding:1px 6px;border-radius:6px;background:#EDE7DB;color:#141414} */
.kbd { font-family: 'DM Mono', monospace; font-size: 11.5px; padding: 1px 6px; border-radius: 6px; background: var(--bb-sunk); color: var(--bb-ink); }
/* .ti{display:flex;gap:6px;align-items:center;padding:2px 6px;border-radius:8px;color:#141414;text-decoration:none;font-size:13.5px;line-height:20px;white-space:nowrap} .ti:hover{background:#EDE7DB} */
.ti { display: flex; gap: 6px; align-items: center; padding: 2px 6px; border-radius: 8px; color: var(--bb-ink); text-decoration: none; font-size: 13.5px; line-height: 20px; white-space: nowrap; flex: none; }
.ti:hover { background: var(--bb-sunk); }
/* .ti.sel{background:#D7ACB6} */
.ti.sel { background: var(--bb-pink); }
/* Unavailable: .tool.off{color:#5C574D;opacity:.55} */
.ti.off { color: var(--bb-stone); opacity: .55; cursor: default; }
.ti:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* <span style="flex:1"> */
.lab { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; }
/* .note{font-size:13px;color:#5C574D} */
.note { font-size: 13px; color: var(--bb-stone); }
/* Text on a fill is ink (ADR 0049): stone on pink is under 4.5:1. */
.ti.sel .note { color: var(--bb-ink); }
</style>
