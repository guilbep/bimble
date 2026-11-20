<script setup lang="ts">
import { computed } from "vue";
import { actions, runAction } from "../bimble";
import { t } from "../i18n";
import { currentPage } from "../pages";
import { currentPanel } from "../panels";
import type { RibbonTool } from "./ribbon-tabs";

const props = defineProps<{ tool: RibbonTool; bb: string }>();
// Real state from Rust; an action Rust does not list (or no action) is unavailable.
const state = computed(() => actions.value.find((a) => a.name === props.tool.action));
// A tool that opens a Vue page or panel: always available, pressed while it is open.
const vue = computed(() => props.tool.page ?? props.tool.panel);
const on = computed(() => vue.value
  ? (props.tool.page ? currentPage.value === props.tool.page : currentPanel.value === props.tool.panel)
  : state.value?.pressed === true);
const off = computed(() => !vue.value && !state.value?.enabled);
// Before the first `actions` refresh the list is empty: unavailable, not missing.
const missing = computed(() => vue.value ? undefined :
  props.tool.action === null ? "action" : state.value || !actions.value.length ? undefined : `action ${props.tool.action}`);

function run(): void {
  if (props.tool.page) currentPage.value = on.value ? null : props.tool.page;
  else if (props.tool.panel) currentPanel.value = on.value ? null : props.tool.panel;
  else if (props.tool.action && !off.value) runAction(props.tool.action);
}
</script>

<template>
  <!-- Reference: .tool (button, or a link to another screen). .off is a class only, as the reference. -->
  <component :is="tool.tag" class="tool" :class="{ on, off }" :type="tool.tag === 'button' ? 'button' : undefined"
             :href="tool.tag === 'a' ? '#' : undefined" :role="tool.tag === 'a' ? 'button' : undefined" :aria-pressed="on ? 'true' : undefined"
             :aria-disabled="off ? 'true' : undefined" :data-missing="missing" :data-bb="bb" @click.prevent="run" @keydown.space.prevent="run">
    <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"
         stroke-linejoin="round" aria-hidden="true" v-html="tool.icon" />
    <span>{{ t(tool.key) }}</span>
  </component>
</template>

<style scoped>
/* .tool{display:flex;flex-direction:column;align-items:center;gap:3px;min-width:58px;max-width:92px;padding:6px 6px 5px;border-radius:12px;color:#141414;text-decoration:none;font-size:12.5px;line-height:1.15;text-align:center;border:1px solid transparent;background:none;font-family:inherit;cursor:pointer} */
.tool { display: flex; flex-direction: column; align-items: center; gap: 3px; min-width: 58px; max-width: 92px; padding: 6px 6px 5px; border-radius: 12px; color: var(--bb-ink); text-decoration: none; font-size: 12.5px; line-height: 1.15; text-align: center; border: 1px solid transparent; background: none; font-family: inherit; cursor: pointer; }
/* .tool:hover{background:#EDE7DB} */
.tool:hover { background: var(--bb-sunk); }
.tool:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* .tool.on{background:#EDE7DB;border-color:#141414} */
.tool.on { background: var(--bb-sunk); border-color: var(--bb-ink); }
/* .tool.off{color:#5C574D;opacity:.55} */
.tool.off { color: var(--bb-stone); opacity: 0.55; }
</style>
