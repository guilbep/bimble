<script lang="ts">
import SheetsSide from "./SheetsSide.vue";
export const hint = "sheets.hint";
export const side = SheetsSide;
</script>

<script setup lang="ts">
// D3 Sheets (wireframes v01 SheetLayout.html, `.vp` with the paper). Two sources, the first that answers:
//  - `sheet_layout` (the C core, ticket SH1): the paper drawn here, white at its proportion, each placed view as a
//    labelled outline (dashed while its box is an estimate). Drag an outline: `sheet_move`.
//  - `sheet_svg` (the sheet's drawing, the egui sheet export; the C core draws it since D5) in an SVG <image>: it
//    holds user text, so it is never put in the DOM (an image runs no script and loads nothing). With both, the
//    outlines are handles over it.
// The view box is paper mm (the sheet's top-left corner at 0, 0), with a margin around the paper.
import { computed, ref } from "vue";
import { currentSheet, selectedPlaced, sheetsError } from "./SheetsSide.vue";
import { query, sheetLayout, sheetMove, state, type PlacedLayout, type SheetLayout } from "../bimble";
import { t } from "../i18n";

const layout = computed<SheetLayout | { error: string } | null>(() => {
  void state.value.rev;
  if (!currentSheet.value) return null;
  try { return sheetLayout(currentSheet.value.id); } catch (e) { return { error: (e as Error).message }; }
});
/** The sheet's drawing (Rust's, or the C core's since D5), asked again at each rev. */
const svg = computed<{ src: string } | { error: string } | null>(() => {
  void state.value.rev;
  if (!currentSheet.value) return null;
  try {
    const { svg } = query<{ svg: string }>("sheet_svg", { id: currentSheet.value.id });
    return { src: `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}` };
  } catch (e) {
    return { error: (e as Error).message };
  }
});
const lay = computed(() => (layout.value && "placed" in layout.value ? layout.value : null));
const image = computed(() => (svg.value && "src" in svg.value ? svg.value.src : null));
/** Neither answered: the layout's error (the C core's), else the drawing's. */
const failed = computed(() => (lay.value || image.value ? null : (layout.value && "error" in layout.value ? layout.value.error : null) ?? (svg.value && "error" in svg.value ? svg.value.error : null)));

const W = computed(() => lay.value?.width_mm ?? 841);
const H = computed(() => lay.value?.height_mm ?? 594);
const M = computed(() => Math.max(W.value, H.value) * 0.04);
const viewBox = computed(() => `${-M.value} ${-M.value} ${W.value + 2 * M.value} ${H.value + 2 * M.value}`);
/** Text height on the paper, mm: readable at the page's size whatever the paper. */
const font = computed(() => Math.max(W.value, H.value) / 70);

// ---- drag a placed view (paper mm) ----
const svgEl = ref<SVGSVGElement | null>(null);
const drag = ref<{ id: string; from: [number, number]; at: [number, number]; d: [number, number]; moved: boolean; pointer: number } | null>(null);
function toPaper(e: PointerEvent): [number, number] {
  const el = svgEl.value, m = el?.getScreenCTM();
  if (!el || !m) return [0, 0];
  const p = new DOMPoint(e.clientX, e.clientY).matrixTransform(m.inverse());
  return [p.x, p.y];
}
const clamp = (v: number, hi: number) => Math.min(Math.max(v, 0), hi);
const round = (v: number) => Math.round(v * 10) / 10;
function down(e: PointerEvent, p: PlacedLayout) {
  if (e.button !== 0) return;
  selectedPlaced.value = p.id;
  (e.currentTarget as Element).setPointerCapture?.(e.pointerId);
  drag.value = { id: p.id, from: toPaper(e), at: p.at_mm, d: [0, 0], moved: false, pointer: e.pointerId };
}
function move(e: PointerEvent) {
  const d = drag.value;
  if (!d || e.pointerId !== d.pointer) return;
  const q = toPaper(e);
  const dx = q[0] - d.from[0], dy = q[1] - d.from[1];
  // The paper bounds the centre (sheet.rs validate: placed on the paper).
  d.d = [clamp(d.at[0] + dx, W.value) - d.at[0], clamp(d.at[1] + dy, H.value) - d.at[1]];
  d.moved ||= Math.hypot(dx, dy) > font.value / 4;
  drag.value = { ...d };
}
function up(e: PointerEvent) {
  const d = drag.value;
  if (!d || e.pointerId !== d.pointer) return;
  drag.value = null;
  if (!d.moved || !currentSheet.value) return;
  sheetsError.value = sheetMove(currentSheet.value.id, d.id, [round(d.at[0] + d.d[0]), round(d.at[1] + d.d[1])]);
}
/** Arrow keys move the focused view by 1 mm (Shift: 10 mm). */
function key(e: KeyboardEvent, p: PlacedLayout) {
  const step = e.shiftKey ? 10 : 1;
  const by: Record<string, [number, number]> = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] };
  const v = by[e.key];
  if (!v || !currentSheet.value) return;
  e.preventDefault();
  selectedPlaced.value = p.id;
  sheetsError.value = sheetMove(currentSheet.value.id, p.id, [round(clamp(p.at_mm[0] + v[0], W.value)), round(clamp(p.at_mm[1] + v[1], H.value))]);
}

/** A placed view's outline, the drag applied; a box of no size (an empty model) gets a small frame. */
function rect(p: PlacedLayout) {
  const d: [number, number] = drag.value?.id === p.id ? drag.value.d : [0, 0];
  const min = font.value * 4;
  let [x0, y0, x1, y1] = p.box_mm;
  if (x1 - x0 < min) [x0, x1] = [p.at_mm[0] - min / 2, p.at_mm[0] + min / 2];
  if (y1 - y0 < min) [y0, y1] = [p.at_mm[1] - min / 2, p.at_mm[1] + min / 2];
  return { x: x0 + d[0], y: y0 + d[1], w: x1 - x0, h: y1 - y0 };
}
const name = (p: PlacedLayout) => p.view_name ?? p.view;
</script>

<template>
  <div class="vp" data-bb="sheet">
    <svg
      ref="svgEl" width="100%" height="100%" :viewBox="viewBox" preserveAspectRatio="xMidYMid meet" role="group"
      :aria-label="currentSheet ? t('sheets.paperLabel', { n: currentSheet.number, name: currentSheet.name, paper: currentSheet.paper }) : t('page.sheets')"
      @pointermove="move" @pointerup="up" @pointercancel="up"
    >
      <rect :x="-M" :y="-M" :width="W + 2 * M" :height="H + 2 * M" fill="var(--bb-sunk)" />
      <template v-if="lay || image">
        <rect x="0" y="0" :width="W" :height="H" fill="var(--bb-sheet)" stroke="var(--bb-line)" :stroke-width="font / 8" data-bb="sheet-paper-rect" />
        <image v-if="image" :href="image" x="0" y="0" :width="W" :height="H" preserveAspectRatio="xMidYMid meet" data-bb="sheet-paper" />
        <g v-for="p in lay?.placed ?? []" :key="p.id" class="placed" :class="{ drag: drag?.id === p.id }"
           role="button" tabindex="0" :aria-label="t('sheets.placedLabel', { name: name(p), scale: p.scale })" :aria-pressed="p.id === selectedPlaced"
           data-bb="sheet-placed" :data-id="p.id" @pointerdown="down($event, p)" @keydown="key($event, p)" @focus="selectedPlaced = p.id"
        >
          <rect :x="rect(p).x" :y="rect(p).y" :width="rect(p).w" :height="rect(p).h"
                :fill="p.id === selectedPlaced ? 'var(--bb-selection)' : 'var(--bb-sheet)'" :fill-opacity="image ? (p.id === selectedPlaced ? 0.25 : 0) : (p.id === selectedPlaced ? 0.35 : 1)"
                stroke="var(--bb-ink)" :stroke-width="font / 10" :stroke-dasharray="p.box === 'estimate' ? `${font / 2} ${font / 3}` : undefined" />
          <text :x="rect(p).x + font / 2" :y="rect(p).y + font * 1.3" :font-size="font" font-family="Source Sans 3" fill="var(--bb-ink)" data-bb="sheet-placed-name">{{ name(p) }}</text>
          <text :x="rect(p).x + font / 2" :y="rect(p).y + font * 2.5" :font-size="font * 0.85" font-family="DM Mono" fill="var(--bb-stone)">1:{{ p.scale }}</text>
        </g>
      </template>
      <text v-else-if="failed" :x="W / 2" :y="H / 2" text-anchor="middle" font-family="Source Sans 3" :font-size="font" fill="var(--bb-error)" role="alert" data-bb="sheet-error">{{ failed }}</text>
      <text v-else-if="!currentSheet" :x="W / 2" :y="H / 2" text-anchor="middle" font-family="Source Sans 3" :font-size="font" fill="var(--bb-stone)" data-bb="sheet-empty">{{ t('sheets.none') }}</text>
    </svg>
  </div>
</template>

<style scoped>
/* .vp{flex:1;background:#FFFFFF;position:relative;overflow:hidden;min-height:0}: fills the page box, over its 24 32 padding. */
.vp { margin: -24px -32px; height: calc(100% + 48px); background: var(--bb-sheet); position: relative; overflow: hidden; min-height: 0; }
/* .vp > svg:not(.cube){position:absolute;left:0;top:0;width:100%;height:100%} */
.vp > svg { position: absolute; left: 0; top: 0; width: 100%; height: 100%; touch-action: none; }
.placed { cursor: grab; outline: none; }
.placed.drag { cursor: grabbing; }
.placed:focus-visible rect { stroke: var(--bb-focus); }
</style>
