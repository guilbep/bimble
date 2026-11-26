<script setup lang="ts">
import { computed, onUnmounted, ref } from "vue";
import { actions, runAction } from "../bimble";
import { t } from "../i18n";
import { currentPage } from "../pages";
import { send } from "./send";
import { currentPanel } from "../panels";
import { currentDialog } from "../dialogs";
import type { RibbonTool } from "./ribbon-tabs";
import { HIDE_NOT_AVAILABLE, toolNotYet } from "./capability";
import { shortcutFor } from "./keys"; // KEY1
import { BOX_SELECTION_MARGIN_MM } from "./contextItems"; // ISS71
import type { PartKey } from "../i18n/keys.gen"; // ISS71

const props = defineProps<{ tool: RibbonTool; bb: string }>();
// Real state from Rust; an action Rust does not list (or no action) is unavailable.
const state = computed(() => actions.value.find((a) => a.name === props.tool.action));
// A tool that opens a Vue page, panel or dialog: available, pressed while it is open.
const vue = computed(() => props.tool.page ?? props.tool.panel ?? props.tool.dialog);
// ---- FAKE1: a tool that cannot work in this core (shell/capability.ts): drawn "not yet", never acts ----
const notYet = computed(() => toolNotYet(props.tool));
const tip = ref(false);
let tipTimer = 0;
onUnmounted(() => clearTimeout(tipTimer));
// ---- end FAKE1 ----
const on = computed(() => !notYet.value && (vue.value
  ? (props.tool.page ? currentPage.value === props.tool.page : props.tool.panel ? currentPanel.value === props.tool.panel : currentDialog.value === props.tool.dialog)
  : state.value?.pressed === true));
// VIEWSHEET: a dialog tool with an action is as enabled as its action (the core says when: a sheet shown, a plan shown).
const gated = computed(() => !!props.tool.dialog && props.tool.action !== null);
const off = computed(() => !notYet.value && (!vue.value || gated.value) && !state.value?.enabled);
// Before the first `actions` refresh the list is empty: unavailable, not missing.
const missing = computed(() => vue.value ? undefined :
  props.tool.action === null ? "action" : state.value || !actions.value.length ? undefined : `action ${props.tool.action}`);
const title = computed(() => (notYet.value ? t("na.tooltip", { name: t(props.tool.key) })
  : off.value && props.tool.offTip ? `${t(props.tool.key)}: ${t(props.tool.offTip)}` : undefined)); // VIEWSHEET: why it is off

// ---- KEY1 ---- the tooltip: the tool's name and its key (the key table's, Rust's), when it has one.
const keyTip = computed(() => { const k = shortcutFor(props.tool.action); return k ? `${t(props.tool.key)} (${k})` : undefined; });
// ISS71: an action's longer tooltip, after the tool's name and key ({m}: Box to selection's margin, m).
const TIPS: Partial<Record<string, PartKey>> = { box_to_selection: "ctx.boxToSelectionTip" };
const longTip = computed(() => {
  const k = props.tool.action ? TIPS[props.tool.action] : undefined;
  return k ? `${keyTip.value ?? t(props.tool.key)}: ${t(k, { m: BOX_SELECTION_MARGIN_MM / 1000 })}` : undefined;
});
// ---- end KEY1 ----

function run(): void {
  if (notYet.value || (off.value && props.tool.offTip)) { // the tooltip, shown on a click too (a touch screen has no hover); VIEWSHEET: the reason it is off
    tip.value = true;
    clearTimeout(tipTimer);
    tipTimer = window.setTimeout(() => (tip.value = false), 2500);
    return;
  }
  if (props.tool.page) send(on.value ? { do: "back" } : { do: "showPage", page: props.tool.page });
  else if (props.tool.panel) currentPanel.value = on.value ? null : props.tool.panel;
  else if (props.tool.dialog) { // VIEWSHEET: the action first (the palette's recent list), then the dialog
    if (gated.value && props.tool.action) runAction(props.tool.action);
    currentDialog.value = on.value ? null : props.tool.dialog;
  }
  else if (props.tool.action && !off.value) runAction(props.tool.action);
}
</script>

<template>
  <!-- Reference: .tool (button, or a link to another screen). .off is a class only, as the reference. FAKE1: .ny "not yet". -->
  <component :is="tool.tag" v-if="!(notYet && HIDE_NOT_AVAILABLE)" class="tool" :class="{ on, off, ny: notYet }" :type="tool.tag === 'button' ? 'button' : undefined"
             :href="tool.tag === 'a' ? '#' : undefined" :role="tool.tag === 'a' ? 'button' : undefined" :aria-pressed="on ? 'true' : undefined"
             :aria-disabled="off || notYet ? 'true' : undefined" :data-missing="missing" :data-bb="bb" :data-not-yet="notYet ? '' : undefined"
             :title="title ?? longTip ?? keyTip" @click.prevent="run" @keydown.space.prevent="run">
    <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"
         stroke-linejoin="round" aria-hidden="true" v-html="tool.icon" />
    <span>{{ t(tool.key) }}</span>
    <span v-if="tip" class="tip" role="tooltip" :data-bb="notYet ? 'not-yet-tip' : 'off-tip'">{{ title }}</span>
  </component>
</template>

<style scoped>
/* .tool{display:flex;flex-direction:column;align-items:center;gap:3px;min-width:58px;max-width:92px;padding:6px 6px 5px;border-radius:12px;color:#141414;text-decoration:none;font-size:12.5px;line-height:1.15;text-align:center;border:1px solid transparent;background:none;font-family:inherit;cursor:pointer} */
.tool { position: relative; display: flex; flex-direction: column; align-items: center; gap: 3px; min-width: 58px; max-width: 92px; padding: 6px 6px 5px; border-radius: 12px; color: var(--bb-ink); text-decoration: none; font-size: 12.5px; line-height: 1.15; text-align: center; border: 1px solid transparent; background: none; font-family: inherit; cursor: pointer; }
/* .tool:hover{background:#EDE7DB} */
.tool:hover { background: var(--bb-sunk); }
.tool:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* .tool.on{background:#EDE7DB;border-color:#141414} */
.tool.on { background: var(--bb-sunk); border-color: var(--bb-ink); }
/* .tool.off{color:#5C574D;opacity:.55} */
.tool.off { color: var(--bb-stone); opacity: 0.55; }
/* FAKE1: not in this preview: a dashed outline, quiet text, the help cursor (not the greyed "not now"). */
.tool.ny { color: var(--bb-stone); opacity: 0.8; border: 1px dashed var(--bb-line); cursor: help; }
.tool.ny:hover { background: none; }
.tip { position: absolute; top: calc(100% + 4px); left: 50%; transform: translateX(-50%); z-index: 50; width: max-content; max-width: 220px;
  padding: 6px 10px; border-radius: var(--bb-radius-sm); background: var(--bb-ink); color: var(--bb-on-ink); font-size: var(--bb-size-small); white-space: normal; }
</style>
