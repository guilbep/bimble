<script setup lang="ts">
// The five tools (fr/PhoneViewer.html nav.mbar; fr/PhoneLandscape.html nav.rail): each opens its sheet.
import { t } from "../i18n";
import { sheet, wide, type SheetId } from "./screen";

// Stroke icons, 1.5 px, on a 24 px grid.
const TOOLS: { id: SheetId; d: string[] }[] = [
  { id: "views", d: ["M4 5h7v6H4z", "M13 5h7v6h-7z", "M4 13h7v6H4z", "M13 13h7v6h-7z"] },
  { id: "objects", d: ["m12 3 8 4.5v9L12 21l-8-4.5v-9z", "m4 7.5 8 4.5 8-4.5", "M12 12v9"] },
  { id: "cut", d: ["M4 4v16", "M4 12h16", "m15 7 5 5-5 5"] },
  { id: "measure", d: ["M3 16 16 3l5 5L8 21z", "m7 12 2 2", "m10 9 2 2", "m13 6 2 2"] },
  { id: "more", d: ["M5 12h.01", "M12 12h.01", "M19 12h.01"] },
];
</script>

<template>
  <nav :class="wide ? 'rail' : 'mbar'" :aria-label="t('small.tools')" data-bb="small-bar">
    <button v-for="x in TOOLS" :key="x.id" type="button" :class="{ on: sheet === x.id }" :aria-pressed="sheet === x.id" :data-bb="`small-tool-${x.id}`"
      @click="sheet = sheet === x.id ? null : x.id">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" :stroke-width="x.id === 'more' ? 2.5 : 1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path v-for="p in x.d" :key="p" :d="p" /></svg>
      <span>{{ t(`small.${x.id}`) }}</span>
    </button>
  </nav>
</template>

<style scoped>
/* .ph .mbar{display:flex;justify-content:space-around;gap:2px;padding:6px 6px 10px;background:#FFFFFF;border-top:1px solid #DDD6C8;flex:none} */
.mbar { grid-area: bar; display: flex; justify-content: space-around; gap: 2px; padding: 6px 6px calc(10px + env(safe-area-inset-bottom)); background: var(--bb-sheet); border-top: 1px solid var(--bb-line); flex: none; }
/* .ph .mbar a{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:2px;min-height:50px;border-radius:16px;color:#141414;text-decoration:none;font-size:12px;line-height:1.1} */
.mbar button { flex: 1; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 2px; min-height: 50px; border-radius: 16px; border: 0; background: none; color: var(--bb-ink); font: inherit; font-size: 12px; line-height: 1.1; cursor: pointer; padding: 0; }
/* .ph .rail{display:flex;flex-direction:column;gap:2px;padding:6px;background:#FFFFFF;border-right:1px solid #DDD6C8;flex:none;justify-content:space-around} */
.rail { grid-area: bar; display: flex; flex-direction: column; gap: 2px; padding: 6px; padding-left: calc(6px + env(safe-area-inset-left)); background: var(--bb-sheet); border-right: 1px solid var(--bb-line); flex: none; justify-content: space-around; }
/* .ph .rail a{...;width:56px;min-height:48px;border-radius:14px;...;font-size:11px;line-height:1.1} */
.rail button { display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 1px; width: 56px; min-height: 48px; border-radius: 14px; border: 0; background: none; color: var(--bb-ink); font: inherit; font-size: 11px; line-height: 1.1; cursor: pointer; padding: 0; }
/* .ph .mbar a.on{background:#141414;color:#F5F2EB} */
button.on { background: var(--bb-ink); color: var(--bb-on-ink); }
/* A tablet is tall: the tools stay together at the top instead of spreading over the height. */
@media (min-height: 500px) { .rail { justify-content: flex-start; gap: 6px; } }
button:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
