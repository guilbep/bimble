<script setup lang="ts">
import { stateRev } from "../bimble"; // VIS2
// A view On paper (ticket PAPER1, Rust paper.rs paper_ui): the active plan or section drawn as its paper
// (`query view_svg {view, bare: true}`: the drawing its PDF and sheets are made of), over the model's
// viewport while `open_views.paper.on`. The viewport stays alive under it (App.vue hides the canvas only).
// As Sheets.vue, the SVG goes in an <image> (it holds user text: an image runs no script). Navigation as
// Rust's: the wheel zooms at the pointer, any drag pans, the paper fits when first shown; double-click
// goes back to the model (paper.rs:357).
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import { openViews, runAction, state, viewSvg } from "../bimble";
import { t } from "../i18n";

/** The drawing of the active view, asked again at each rev and view; an error is shown, not thrown. */
const drawn = computed<{ src: string; w: number; h: number; title: string } | { error: string } | null>(() => {
  void stateRev.value;
  const id = openViews.value.active;
  if (!id || !openViews.value.paper.on) return null;
  try {
    const d = viewSvg(id, true);
    const title = /<title>([^<]*)<\/title>/.exec(d.svg)?.[1] ?? "";
    return { src: `data:image/svg+xml;charset=utf-8,${encodeURIComponent(d.svg)}`, w: d.width_mm, h: d.height_mm, title };
  } catch (e) {
    return { error: (e as Error).message };
  }
});
const pic = computed(() => (drawn.value && "src" in drawn.value ? drawn.value : null));
const failed = computed(() => (drawn.value && "error" in drawn.value ? drawn.value.error : null));

// The camera on the paper: its centre (paper mm) and zoom (px per mm), fitted when the picture's size or
// the view changes (paper.rs `fit`: the paper and 5 % around it).
const el = ref<HTMLElement | null>(null);
const size = ref({ w: 1, h: 1 });
const center = ref<[number, number]>([0, 0]);
const zoom = ref(1);
function fit() {
  const p = pic.value;
  if (!p || p.w <= 0) return;
  center.value = [p.w / 2, p.h / 2];
  zoom.value = Math.min(size.value.w / (p.w * 1.05), size.value.h / (p.h * 1.05));
}
watch(() => [openViews.value.active, pic.value?.w, pic.value?.h], fit);
const viewBox = computed(() => {
  const z = zoom.value || 1, w = size.value.w / z, h = size.value.h / z;
  return `${center.value[0] - w / 2} ${center.value[1] - h / 2} ${w} ${h}`;
});
let ro: ResizeObserver | null = null;
onMounted(() => {
  const e = el.value;
  if (!e) return;
  const measure = () => { size.value = { w: Math.max(e.clientWidth, 1), h: Math.max(e.clientHeight, 1) }; };
  measure();
  fit();
  ro = new ResizeObserver(() => { const first = size.value.w <= 1; measure(); if (first) fit(); });
  ro.observe(e);
});
onUnmounted(() => ro?.disconnect());

/** The paper point under a screen point. */
function toPaper(x: number, y: number): [number, number] {
  const r = el.value?.getBoundingClientRect();
  if (!r) return center.value;
  return [center.value[0] + (x - r.left - r.width / 2) / zoom.value, center.value[1] + (y - r.top - r.height / 2) / zoom.value];
}
function wheel(e: WheelEvent) {
  e.preventDefault();
  const before = toPaper(e.clientX, e.clientY);
  zoom.value = Math.min(Math.max(zoom.value * Math.exp(-e.deltaY * 0.0015), 0.01), 1000);
  const after = toPaper(e.clientX, e.clientY);
  center.value = [center.value[0] + before[0] - after[0], center.value[1] + before[1] - after[1]];
}
const drag = ref<{ x: number; y: number; pointer: number } | null>(null);
function down(e: PointerEvent) {
  (e.currentTarget as Element).setPointerCapture?.(e.pointerId);
  drag.value = { x: e.clientX, y: e.clientY, pointer: e.pointerId };
}
function move(e: PointerEvent) {
  const d = drag.value;
  if (!d || d.pointer !== e.pointerId) return;
  center.value = [center.value[0] - (e.clientX - d.x) / zoom.value, center.value[1] - (e.clientY - d.y) / zoom.value];
  drag.value = { ...d, x: e.clientX, y: e.clientY };
}
const up = () => { drag.value = null; };
</script>

<template>
  <div ref="el" class="paper" data-bb="paper-view" :data-title="pic?.title" @wheel="wheel" @dblclick="runAction('paper_view')">
    <svg width="100%" height="100%" :viewBox="viewBox" preserveAspectRatio="xMidYMid meet" role="img"
         :aria-label="pic ? t('paperview.label', { title: pic.title }) : t('viewtabs.onPaper')"
         @pointerdown="down" @pointermove="move" @pointerup="up" @pointercancel="up">
      <template v-if="pic">
        <rect x="0" y="0" :width="pic.w" :height="pic.h" fill="var(--bb-sheet)" />
        <image :href="pic.src" x="0" y="0" :width="pic.w" :height="pic.h" preserveAspectRatio="none" data-bb="paper-view-image" />
      </template>
    </svg>
    <p v-if="failed" class="err" role="alert" data-bb="paper-view-error">{{ t('paperview.failed', { e: failed }) }}</p>
    <button class="fit" type="button" data-bb="paper-view-fit" @click="fit">{{ t('paperview.fit') }}</button>
  </div>
</template>

<style scoped>
/* paper.rs: the paper on a grey ground (Color32::from_gray(200)): the sunk tone of the palette. */
.paper { position: absolute; inset: 0; background: var(--bb-sunk); overflow: hidden; touch-action: none; cursor: grab; }
.paper svg { position: absolute; inset: 0; display: block; }
.err { position: absolute; left: 10px; top: 10px; margin: 0; padding: 4px 8px; background: var(--bb-error); color: var(--bb-ink); }
.fit { position: absolute; right: 12px; bottom: 12px; padding: 2px 12px; border-radius: var(--bb-radius-pill); border: 1px solid var(--bb-line); background: var(--bb-sheet); color: var(--bb-ink); font: inherit; font-size: var(--bb-size-small); cursor: pointer; }
.fit:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
