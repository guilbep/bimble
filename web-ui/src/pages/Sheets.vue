<script lang="ts">
import SheetsSide from "./SheetsSide.vue";
export const hint = "sheets.hint";
export const side = SheetsSide;
</script>

<script setup lang="ts">
// D3 Sheets (wireframes v01 SheetLayout.html, `.vp` with the paper). The paper is Rust's drawing of the
// current sheet (`sheet_svg`, the egui sheet export), in an SVG <image>: it holds user text, so it is never
// put in the DOM (an image runs no script and loads nothing). Placed where the reference draws the paper.
import { computed } from "vue";
import { currentSheet } from "./SheetsSide.vue";
import { query, state } from "../bimble";
import { t } from "../i18n";

const paper = computed<{ src: string } | { error: string } | null>(() => {
  void state.value.rev;
  if (!currentSheet.value) return null;
  try {
    const { svg } = query<{ svg: string }>("sheet_svg", { id: currentSheet.value.id });
    return { src: `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}` };
  } catch (e) {
    return { error: (e as Error).message };
  }
});
</script>

<template>
  <div class="vp" data-bb="sheet">
    <svg
      width="100%" height="100%" viewBox="0 0 1100 640" preserveAspectRatio="xMidYMid meet" role="img"
      :aria-label="currentSheet ? t('sheets.paperLabel', { n: currentSheet.number, name: currentSheet.name, paper: currentSheet.paper }) : t('page.sheets')"
    >
      <rect x="0" y="0" width="1100" height="640" fill="var(--bb-sunk)" />
      <image v-if="paper && 'src' in paper" :href="paper.src" x="100.0" y="26.0" width="883.1" height="573.8" preserveAspectRatio="xMidYMid meet" data-bb="sheet-paper" />
      <text v-else-if="paper" x="550" y="320" text-anchor="middle" font-family="Source Sans 3" font-size="13.5" fill="var(--bb-error)" role="alert" data-bb="sheet-error">{{ paper.error }}</text>
    </svg>
  </div>
</template>

<style scoped>
/* .vp{flex:1;background:#FFFFFF;position:relative;overflow:hidden;min-height:0}: fills the page box, over its 24 32 padding. */
.vp { margin: -24px -32px; height: calc(100% + 48px); background: var(--bb-sheet); position: relative; overflow: hidden; min-height: 0; }
/* .vp > svg:not(.cube){position:absolute;left:0;top:0;width:100%;height:100%} */
.vp > svg { position: absolute; left: 0; top: 0; width: 100%; height: 100%; }
</style>
