<script setup lang="ts">
// The viewport's right-click menu (ticket ISS71, issue #71). glviewport.ts sends `bb-viewport-menu`
// ({x, y}: client px) after a right click without a drag (the element under the pointer selected
// first when it was not); the items are shell/contextItems.ts's rows, each a C core action: greyed
// out when `query actions` says it is not enabled now, run on a click. Esc, a click elsewhere, a
// wheel or a resize close it. Style: the navigator's view menu (.vmenu).
import { computed, nextTick, onBeforeUnmount, onMounted, ref } from "vue";
import { actions, query, runAction, type ActionState } from "../bimble";
import { t } from "../i18n";
import { BOX_SELECTION_MARGIN_MM, CONTEXT_ITEMS, type ContextItem } from "./contextItems";

const at = ref<{ x: number; y: number } | null>(null);
const menu = ref<HTMLElement | null>(null);
const error = ref<string | null>(null);
const enabled = (a: string) => actions.value.find((s) => s.name === a)?.enabled === true;
const items = computed(() => CONTEXT_ITEMS.map((i) => ({ ...i, on: enabled(i.action) })));
const vars = { m: BOX_SELECTION_MARGIN_MM / 1000 };
const tip = (i: ContextItem) => (i.tip ? t(i.tip, vars) : undefined);

function close(): void { at.value = null; error.value = null; }
function run(i: ContextItem & { on: boolean }): void {
  if (!i.on) return;
  const err = runAction(i.action);
  if (err) { error.value = err; return; }
  close();
}
async function open(e: Event): Promise<void> {
  const d = (e as CustomEvent<{ x: number; y: number }>).detail;
  error.value = null;
  try { actions.value = query<ActionState[]>("actions"); } catch { /* no viewer: the last list */ } // the selection just made
  at.value = { x: d.x, y: d.y };
  await nextTick();
  const m = menu.value;
  if (!m) return;
  // Kept on screen: flipped left or up when it would leave the window.
  const r = m.getBoundingClientRect();
  at.value = { x: d.x + r.width > innerWidth ? Math.max(0, d.x - r.width) : d.x, y: d.y + r.height > innerHeight ? Math.max(0, d.y - r.height) : d.y };
  (m.querySelector("button:not([disabled])") as HTMLElement | null)?.focus();
}
function outside(e: PointerEvent): void { if (at.value && !menu.value?.contains(e.target as Node)) close(); }
// On the menu itself (keys.ts keeps the one window keydown listener: KEY1).
function key(e: KeyboardEvent): void {
  if (e.key === "Escape") { e.preventDefault(); close(); return; }
  if (e.key !== "ArrowDown" && e.key !== "ArrowUp") return;
  e.preventDefault();
  const b = [...(menu.value?.querySelectorAll<HTMLButtonElement>("button:not([disabled])") ?? [])];
  const k = b.indexOf(document.activeElement as HTMLButtonElement);
  b[(k + (e.key === "ArrowDown" ? 1 : b.length - 1)) % b.length]?.focus();
}
onMounted(() => {
  addEventListener("bb-viewport-menu", open);
  addEventListener("pointerdown", outside, true);
  addEventListener("resize", close);
  addEventListener("wheel", close, { passive: true });
});
onBeforeUnmount(() => {
  removeEventListener("bb-viewport-menu", open);
  removeEventListener("pointerdown", outside, true);
  removeEventListener("resize", close);
  removeEventListener("wheel", close);
});
</script>

<template>
  <div v-if="at" ref="menu" class="ctx" role="menu" :aria-label="t('ctx.menu')" data-bb="viewport-menu" :style="{ left: `${at.x}px`, top: `${at.y}px` }" @contextmenu.prevent @keydown="key">
    <template v-for="i in items" :key="i.action">
      <hr v-if="i.sep">
      <button type="button" role="menuitem" :disabled="!i.on" :title="tip(i)" :data-bb="`ctx-${i.action}`" @click="run(i)">{{ t(i.key) }}</button>
    </template>
    <p v-if="error" class="ctx-err" role="alert">{{ error }}</p>
  </div>
</template>

<style scoped>
.ctx { position: fixed; z-index: 60; display: flex; flex-direction: column; min-width: 180px; border: 1px solid var(--bb-line); border-radius: 8px; background: var(--bb-sheet); padding: 4px 0; box-shadow: 0 4px 14px rgb(0 0 0 / 0.14); }
.ctx button { text-align: left; background: none; border: 0; padding: 5px 12px; font: inherit; font-size: 13px; color: var(--bb-ink); cursor: pointer; }
.ctx button:hover:not([disabled]), .ctx button:focus-visible { background: var(--bb-sunk); outline: none; }
.ctx button[disabled] { color: var(--bb-stone); opacity: 0.55; cursor: default; }
.ctx hr { border: 0; border-top: 1px solid var(--bb-line); margin: 4px 0; width: 100%; }
.ctx-err { margin: 2px 8px; font-size: 12px; color: var(--bb-ink); background: var(--bb-error); padding: 2px 6px; border-radius: 4px; max-width: 260px; overflow-wrap: anywhere; }
</style>
