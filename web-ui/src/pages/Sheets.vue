<script lang="ts">
import SheetsSide from "./SheetsSide.vue";
export const hint = "sheets.hint";
export const side = SheetsSide;
</script>

<script setup lang="ts">
// D3 Sheets (wireframes v01 SheetLayout.html, `.vp` with the paper). Rust draws sheets to SVG
// (bimble-drawing Sheet::to_svg) only for export: no query returns it, so the paper frame is drawn as the
// reference and the drawing inside it (placed views, title block template) is data-missing `sheet_svg`.
// Real: the current sheet's number and name in the title block.
import { currentSheet } from "./SheetsSide.vue";
import { t } from "../i18n";
</script>

<template>
  <div class="vp" data-bb="sheet">
    <svg
      width="100%" height="100%" viewBox="0 0 1100 640" preserveAspectRatio="xMidYMid meet" role="img"
      :aria-label="currentSheet ? t('sheets.paperLabel', { n: currentSheet.number, name: currentSheet.name, paper: currentSheet.paper }) : t('page.sheets')"
    >
      <rect x="0" y="0" width="1100" height="640" fill="var(--bb-sunk)" />
      <rect x="100.0" y="26.0" width="883.1" height="573.8" fill="var(--bb-sheet)" stroke="var(--bb-ink)" stroke-width="1" data-bb="sheet-paper" />
      <rect x="114.0" y="40.0" width="855.1" height="545.8" fill="none" stroke="var(--bb-ink)" stroke-width="1.6" />
      <g data-missing="query: sheet_svg (bimble-drawing Sheet::to_svg for the current sheet)" />
      <g v-if="currentSheet" font-family="Source Sans 3" font-size="11" fill="var(--bb-ink)" data-bb="sheet-title-block">
        <rect x="669.1" y="465.8" width="300" height="120" fill="var(--bb-sheet)" stroke="var(--bb-ink)" stroke-width="1.2" />
        <text x="679.1" y="502.8" font-weight="600">{{ currentSheet.name }}</text>
        <text x="959.1" y="487.8" font-family="DM Mono" font-size="16" text-anchor="end">{{ currentSheet.number }}</text>
      </g>
    </svg>
  </div>
</template>

<style scoped>
/* .vp{flex:1;background:#FFFFFF;position:relative;overflow:hidden;min-height:0}: fills the page box, over its 24 32 padding. */
.vp { margin: -24px -32px; height: calc(100% + 48px); background: var(--bb-sheet); position: relative; overflow: hidden; min-height: 0; }
/* .vp > svg:not(.cube){position:absolute;left:0;top:0;width:100%;height:100%} */
.vp > svg { position: absolute; left: 0; top: 0; width: 100%; height: 100%; }
</style>
