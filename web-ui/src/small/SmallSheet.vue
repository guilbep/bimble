<script setup lang="ts">
// The small-screen sheets (ADR 0064; fr/PhoneViews, PhoneSelect, PhoneProps, PhoneCut, PhoneMeasure, PhoneMore.html).
// Portrait: a sheet over the bottom of the view. A phone on its side, a tablet: a panel at the right.
// Changes apply at once (ADR 0052): a field is sent on Enter or when it loses focus, as on the desktop.
import { computed, ref, watch } from "vue";
import {
  state, actions, openViews, materials, visibility, element, rename, setMaterial, setNote, NOTE_KEY, runAction, undo, redo,
  openView, tabCall, sheets, schedules, measurements, type ElementDetail, type SheetSummary, type ScheduleDef, type Measurement,
} from "../bimble";
import { t } from "../i18n";
import { viewName } from "../shell/viewName";
import { classNoun } from "../i18n/parts/modify";
import { sheet, wide, phone } from "./screen";

const action = (name: string) => actions.value.find((a) => a.name === name);
const on = (name: string) => action(name)?.enabled ?? false;
const pressed = (name: string) => action(name)?.pressed ?? false;
const run = (name: string) => { error.value = runAction(name); };
const error = ref<string | null>(null);
watch(sheet, () => { error.value = null; props.value = false; });

// Views, sheets, quantities.
const list = ref<"views" | "sheets" | "quantities">("views");
const paper = ref<SheetSummary[]>([]);
const tables = ref<ScheduleDef[]>([]);
watch(() => [sheet.value, state.value.rev] as const, ([s]) => {
  if (s !== "views") return;
  try { paper.value = sheets(); tables.value = schedules(); } catch { paper.value = []; tables.value = []; }
}, { immediate: true });
const isCurrent = (type: string, key: string) => {
  const c = openViews.value.current;
  return !!c && c.type === type && ("id" in c ? c.id === key : "name" in c ? c.name === key : false);
};
function open(err: string | null): void {
  error.value = err;
  if (!err && !wide.value) sheet.value = null;
}

// Objects: the selection. One element: its summary, then its properties.
const sel = computed(() => state.value.selection);
const detail = ref<ElementDetail | null>(null);
const props = ref(false);
const name = ref("");
const note = ref("");
watch(() => [sel.value.length === 1 ? sel.value[0] : null, state.value.rev] as const, ([id]) => {
  if (!id) { detail.value = null; props.value = false; return; }
  try {
    const d = element(id);
    if (d.id !== detail.value?.id) error.value = null;
    detail.value = d;
    name.value = d.name;
    note.value = d.properties[NOTE_KEY] ?? "";
  } catch (e) {
    detail.value = null;
    error.value = e instanceof Error ? e.message : String(e);
  }
}, { immediate: true });
// A tap on an element opens its sheet, unless a tool's sheet is open (cut and measure also tap the view).
watch(() => sel.value.length, (n, old) => { if (n > 0 && old === 0 && sheet.value === null) sheet.value = "objects"; });
const title = computed(() => (detail.value ? `${classNoun(detail.value.class, t).noun} ${detail.value.name}` : ""));
const grouped = (n: number) => String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, " ").replace("-", "−");
const ORDER = ["width", "height", "length", "depth", "thickness"];
const sizes = computed(() =>
  Object.entries(detail.value?.generator_params ?? {})
    .filter((e): e is [string, number] => typeof e[1] === "number")
    .sort((a, b) => ((ORDER.indexOf(a[0]) + 6) % 6) - ((ORDER.indexOf(b[0]) + 6) % 6)),
);
const section = computed(() => {
  // The cross-section: not the length, and not a column's height (it has width and depth).
  const keys = sizes.value.map(([k]) => k);
  const along = keys.includes("depth") && keys.includes("width") ? ["length", "height"] : ["length"];
  const s = sizes.value.filter(([k]) => !along.includes(k)).map(([, v]) => grouped(v));
  return s.length ? `${s.join(" × ")} mm` : "";
});
function commitName(): void {
  const d = detail.value;
  if (d && name.value !== d.name) error.value = rename(d.id, name.value);
}
function commitMaterial(e: Event): void {
  const d = detail.value;
  const v = (e.target as HTMLSelectElement).value;
  if (d && v !== d.material_name) error.value = setMaterial([d.id], v);
}
function commitNote(): void {
  const d = detail.value;
  if (d && note.value.trim() !== (d.properties[NOTE_KEY] ?? "")) error.value = setNote(d.id, note.value);
}

// Cut: the section box (the viewer's own; its faces are dragged in the view).
const box = computed(() => {
  const b = visibility.value?.section_box;
  if (!b?.on || !b.box) return null;
  const z0 = b.origin_mm?.[2] ?? 0;
  const m = (mm: number) => `${mm < 0 ? "−" : "+"}${(Math.abs(mm) / 1000).toFixed(2)} m`;
  return { top: m((b.box.max[2] ?? 0) + z0), bottom: m((b.box.min[2] ?? 0) + z0) };
});

// Measure: the kept measurements, read again after each change in the viewer.
const kept = computed<Measurement[]>(() => { void state.value; return sheet.value === "measure" ? measurements() : []; });
</script>

<template>
  <section v-if="sheet" class="sheet" :class="{ side: wide, narrow: wide && phone, tall: props }" :aria-label="t(`small.${sheet}`)" data-bb="small-sheet">
    <span v-if="!wide" class="grab" aria-hidden="true" />

    <!-- Views -->
    <template v-if="sheet === 'views'">
      <div class="sh"><h2>{{ t('small.views') }}</h2><button class="tb" type="button" :aria-label="t('small.close')" data-bb="small-close" @click="sheet = null"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button></div>
      <span class="mseg" role="group">
        <button type="button" :aria-pressed="list === 'views'" @click="list = 'views'">{{ t('small.views') }}</button>
        <button type="button" :aria-pressed="list === 'sheets'" @click="list = 'sheets'">{{ t('small.sheets') }} · {{ paper.length }}</button>
        <button type="button" :aria-pressed="list === 'quantities'" @click="list = 'quantities'">{{ t('small.quantities') }} · {{ tables.length }}</button>
      </span>
      <div class="rows">
        <template v-if="list === 'views'">
          <button v-for="v in openViews.views" :key="v.id" type="button" class="lrow" :class="{ sel: v.id === openViews.active && isCurrent('view', v.id) }" @click="open(openView(v.id))">
            <span>{{ viewName(v) }}</span><span class="r">{{ v.type === 'three_d' ? '' : `1:${v.scale}` }}</span>
          </button>
        </template>
        <template v-else-if="list === 'sheets'">
          <button v-for="s in paper" :key="s.id" type="button" class="lrow" :class="{ sel: isCurrent('sheet', s.id) }" @click="open(tabCall('open', { type: 'sheet', id: s.id }))">
            <span>{{ s.number }} {{ s.name }}</span><span class="r">{{ s.paper }}</span>
          </button>
          <p v-if="!paper.length" class="quiet">{{ t('small.none') }}</p>
        </template>
        <template v-else>
          <button v-for="s in tables" :key="s.name" type="button" class="lrow" :class="{ sel: isCurrent('schedule', s.name) }" @click="open(tabCall('open', { type: 'schedule', name: s.name }))">
            <span>{{ s.name }}</span>
          </button>
          <p v-if="!tables.length" class="quiet">{{ t('small.none') }}</p>
        </template>
      </div>
    </template>

    <!-- Objects: properties of one element -->
    <template v-else-if="sheet === 'objects' && detail && props">
      <div class="sh">
        <button class="tb" type="button" :aria-label="t('small.back')" @click="props = false"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m14 6-6 6 6 6"/></svg></button>
        <h2>{{ title }}</h2>
        <button class="tb" type="button" :aria-label="t('small.close')" data-bb="small-close" @click="sheet = null"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button>
      </div>
      <div class="sech">{{ t('small.editable') }}</div>
      <label class="fld"><span class="k">{{ t('small.name') }}</span>
        <input v-model="name" class="minp" data-bb="small-name" @keydown.enter="commitName" @blur="commitName"></label>
      <label class="fld"><span class="k">{{ t('small.material') }}</span>
        <select class="minp" :value="detail.material_name ?? ''" data-bb="small-material" @change="commitMaterial">
          <option v-for="x in materials" :key="x.id" :value="x.name">{{ x.name }}</option>
        </select></label>
      <label class="fld"><span class="k">{{ t('small.note') }}</span>
        <input v-model="note" class="minp" maxlength="2000" :placeholder="t('small.notePlaceholder')" data-bb="small-note" @keydown.enter="commitNote" @blur="commitNote"></label>
      <div class="sech">{{ t('small.readOnly') }}</div>
      <div class="kv"><span class="k">{{ t('small.class') }}</span><span class="mono">{{ detail.class }}</span></div>
      <div class="kv"><span class="k">{{ t('small.storey') }}</span><span>{{ detail.storey_name ?? '—' }}</span></div>
      <div v-for="[k, v] in sizes" :key="k" class="kv"><span class="k">{{ ORDER.includes(k) ? t(`modify.dim.${k}`) : k }}</span><span class="mono">{{ grouped(v) }} mm</span></div>
      <div class="kv"><span class="k">GlobalId</span><span class="mono" style="font-size:11.5px">{{ detail.id }}</span></div>
      <p class="card2">{{ t('small.geometry') }}</p>
    </template>

    <!-- Objects: the selection -->
    <template v-else-if="sheet === 'objects'">
      <div class="sh">
        <span v-if="sel.length" class="dot" aria-hidden="true" />
        <h2>{{ detail ? title : sel.length > 1 ? t('small.selected', { n: sel.length }) : t('small.objects') }}</h2>
        <span v-if="detail" class="mono quiet">{{ detail.class }}</span>
        <button class="tb" type="button" :aria-label="t('small.close')" data-bb="small-close" @click="sheet = null"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button>
      </div>
      <p v-if="!sel.length" class="quiet">{{ t('small.tapElement') }}</p>
      <template v-if="detail">
        <div class="kv"><span class="k">{{ t('small.storey') }}</span><span>{{ detail.storey_name ?? '—' }}</span></div>
        <div v-if="section" class="kv"><span class="k">{{ t('small.section') }}</span><span class="mono">{{ section }}</span></div>
        <div class="kv" style="border:0"><span class="k">{{ t('small.material') }}</span><span>{{ detail.material_name ?? '—' }}</span></div>
      </template>
      <div class="pills">
        <button v-if="detail" class="pill q" type="button" data-bb="small-props" @click="props = true">{{ t('small.properties') }}</button>
        <button v-if="sel.length" class="pill" type="button" :disabled="!on('hide_now')" @click="run('hide_now')">{{ t('small.hide') }}</button>
        <button v-if="sel.length" class="pill" type="button" :disabled="!on('isolate_now')" @click="run('isolate_now')">{{ t('small.isolate') }}</button>
        <button class="pill" type="button" :disabled="!on('clear_temporary')" @click="run('clear_temporary')">{{ t('small.showAll') }}</button>
      </div>
    </template>

    <!-- Cut -->
    <template v-else-if="sheet === 'cut'">
      <div class="sh"><h2>{{ t('small.cut') }}</h2><button class="tb" type="button" :aria-label="t('small.close')" data-bb="small-close" @click="sheet = null"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button></div>
      <div class="kv"><span class="k">{{ t('small.sectionBox') }}</span>
        <button class="pill" :class="{ ink: pressed('section_box') }" type="button" :aria-pressed="pressed('section_box')" :disabled="!on('section_box')" data-bb="small-box" @click="run('section_box')">{{ t(pressed('section_box') ? 'small.boxOn' : 'small.boxOff') }}</button></div>
      <template v-if="box">
        <div class="kv"><span class="k">{{ t('small.top') }}</span><span class="mono">{{ box.top }}</span></div>
        <div class="kv"><span class="k">{{ t('small.bottom') }}</span><span class="mono">{{ box.bottom }}</span></div>
        <p class="quiet">{{ t('small.boxHint') }}</p>
      </template>
      <div class="pills"><button class="pill" type="button" :disabled="!on('box_to_selection')" @click="run('box_to_selection')">{{ t('small.boxToSelection') }}</button></div>
    </template>

    <!-- Measure -->
    <template v-else-if="sheet === 'measure'">
      <div class="sh"><h2>{{ t('small.measure') }}</h2><button class="tb" type="button" :aria-label="t('small.close')" data-bb="small-close" @click="sheet = null"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button></div>
      <p v-if="pressed('measure')" class="quiet">{{ t('small.measureOn') }}</p>
      <div v-for="m in kept" :key="m.id" class="kv"><span class="mono">{{ m.label }}</span></div>
      <p v-if="!kept.length && !pressed('measure')" class="quiet">{{ t('small.noMeasure') }}</p>
      <div class="pills"><button class="pill q" type="button" :disabled="!on('measure')" :aria-pressed="pressed('measure')" data-bb="small-measure" @click="run('measure')">{{ t(pressed('measure') ? 'small.measureStop' : 'small.measureStart') }}</button></div>
    </template>

    <!-- More -->
    <template v-else>
      <div class="sh"><h2>{{ t('small.more') }}</h2><button class="tb" type="button" :aria-label="t('small.close')" data-bb="small-close" @click="sheet = null"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button></div>
      <div class="sech">{{ t('small.file') }}</div>
      <div class="pills">
        <button class="pill" :class="{ p: on('save') }" type="button" :disabled="!on('save')" data-bb="small-save" @click="run('save')">{{ t('small.save') }}</button>
        <button class="pill" type="button" :disabled="!on('synchronize')" data-bb="small-sync" @click="run('synchronize')">{{ t('small.sync') }}</button>
        <button class="pill" type="button" :disabled="!on('open')" @click="run('open')">{{ t('small.open') }}</button>
        <button class="pill" type="button" :disabled="!on('undo')" @click="error = undo()">{{ t('small.undo') }}</button>
        <button class="pill" type="button" :disabled="!on('redo')" @click="error = redo()">{{ t('small.redo') }}</button>
      </div>
      <p class="quiet">{{ t(on('synchronize') ? 'small.syncConflict' : 'small.syncDesktop') }}</p>
      <div class="sech">{{ t('small.display') }}</div>
      <div class="pills">
        <button v-for="a in (['edges', 'hatching'] as const)" :key="a" class="pill" :class="{ ink: pressed(a) }" type="button" :aria-pressed="pressed(a)" :disabled="!on(a)" @click="run(a)">{{ t(`small.${a}`) }}</button>
      </div>
      <p v-if="state.version" class="quiet mono">{{ t('small.version', { v: state.version }) }}</p>
    </template>

    <p v-if="error" class="err" role="alert">{{ t('small.error', { msg: error }) }}</p>
  </section>
</template>

<style scoped>
/* .ph .sheet{position:absolute;left:0;right:0;bottom:0;background:#F5F2EB;border-top:1px solid #5C574D;border-radius:16px 16px 0 0;padding:6px 16px 14px;display:flex;flex-direction:column;gap:10px} */
.sheet { position: absolute; left: 0; right: 0; bottom: 0; z-index: 3; max-height: 78%; overscroll-behavior: contain; touch-action: pan-y; overflow-y: auto; background: var(--bb-cream); border-top: 1px solid var(--bb-stone); border-radius: 16px 16px 0 0; padding: 6px 16px 14px; display: flex; flex-direction: column; gap: 10px; font-size: 15px; }
/* fr/PhoneProps.html: the properties sheet covers the view (style="top:64px" from the window's top). */
.sheet.tall { top: 12px; max-height: none; }
/* fr/PhoneLandscape.html aside: width:286px;flex:none;border-left:1px solid #5C574D;background:#F5F2EB;padding:8px 14px */
.sheet.side { position: static; padding-right: calc(14px + env(safe-area-inset-right)); flex: none; width: 320px; max-height: none; border-top: 0; border-left: 1px solid var(--bb-stone); border-radius: 0; padding: 8px 14px; gap: 6px; }
.sheet.side.narrow { width: 286px; }
/* .ph .grab{width:40px;height:4px;border-radius:999px;background:#5C574D;align-self:center;margin:4px 0 2px;flex:none} */
.grab { width: 40px; height: 4px; border-radius: var(--bb-radius-pill); background: var(--bb-stone); align-self: center; margin: 4px 0 2px; flex: none; }
/* .ph .sh{display:flex;align-items:center;gap:8px;min-height:44px} */
.sh { display: flex; align-items: center; gap: 8px; min-height: 44px; flex: none; }
.sh h2 { font-family: var(--bb-font-display); font-weight: 500; font-size: 17px; margin: 0; flex: 1; min-width: 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.tb { width: 44px; height: 44px; border-radius: var(--bb-radius-pill); border: 0; background: none; display: inline-flex; align-items: center; justify-content: center; color: var(--bb-ink); flex: none; cursor: pointer; padding: 0; }
.dot { width: 12px; height: 12px; border-radius: 50%; border: 1px solid var(--bb-ink); background: var(--bb-selection); flex: none; }
.rows { display: flex; flex-direction: column; gap: 2px; }
/* .ph .lrow{display:flex;align-items:center;gap:10px;min-height:48px;padding:0 10px;border-radius:12px;color:#141414;text-decoration:none;font-size:15.5px} */
.lrow { display: flex; align-items: center; gap: 10px; min-height: 48px; padding: 0 10px; border-radius: 12px; border: 0; background: none; color: var(--bb-ink); font: inherit; font-size: 15.5px; text-align: left; cursor: pointer; flex: none; }
.lrow.sel { background: var(--bb-selection); }
.lrow .r { margin-left: auto; color: var(--bb-stone); font-family: var(--bb-font-data); font-size: 12.5px; }
/* .ph .kv{display:flex;justify-content:space-between;align-items:center;gap:12px;min-height:44px;border-bottom:1px solid #EDE7DB;font-size:15px} */
.kv { display: flex; justify-content: space-between; align-items: center; gap: 12px; min-height: 44px; border-bottom: 1px solid var(--bb-sunk); font-size: 15px; flex: none; }
.k { color: var(--bb-stone); }
.mono { font-family: var(--bb-font-data); font-size: 13.5px; }
.quiet { color: var(--bb-stone); margin: 0; font-size: 14px; }
.fld { display: flex; flex-direction: column; gap: 4px; flex: none; }
.fld .k { font-size: 13.5px; }
/* .ph .minp{height:44px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 10px;font:inherit;font-size:15px;color:#141414} — 16 px: iOS zooms the page on a smaller field. */
.minp { height: 44px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 10px; font: inherit; font-size: 16px; color: var(--bb-ink); min-width: 0; width: 100%; }
.sech { font-family: var(--bb-font-data); font-size: 12px; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 4px; }
.pills { display: flex; gap: 8px; flex-wrap: wrap; flex: none; }
/* .ph .pill{display:inline-flex;align-items:center;gap:6px;min-height:44px;padding:0 14px;border-radius:999px;background:#FFFFFF;border:1px solid #5C574D;color:#141414;font:inherit;font-size:15px;white-space:nowrap;cursor:pointer} */
.pill { display: inline-flex; align-items: center; gap: 6px; min-height: 44px; padding: 0 14px; border-radius: var(--bb-radius-pill); background: var(--bb-sheet); border: 1px solid var(--bb-stone); color: var(--bb-ink); font: inherit; font-size: 15px; white-space: nowrap; cursor: pointer; }
.pill.p { background: var(--bb-action); border-color: var(--bb-action); font-weight: 600; }
.pill.q { border-color: var(--bb-ink); }
.pill.ink { background: var(--bb-ink); border-color: var(--bb-ink); color: var(--bb-on-ink); }
.pill:disabled { background: var(--bb-sunk); border-color: var(--bb-sunk); color: var(--bb-stone); cursor: default; }
/* .ph .mseg: one pill split in parts, the current one ink. */
.mseg { display: flex; border: 1px solid var(--bb-stone); border-radius: var(--bb-radius-pill); overflow: hidden; flex: none; }
.mseg button { flex: 1; min-height: 44px; border: 0; background: var(--bb-sheet); color: var(--bb-ink); font: inherit; font-size: 14px; cursor: pointer; padding: 0 6px; white-space: nowrap; }
.mseg button[aria-pressed="true"] { background: var(--bb-ink); color: var(--bb-on-ink); }
.card2 { margin: 0; padding: 12px 14px; border-radius: 16px; background: var(--bb-sheet); font-size: 14px; }
/* Error: ink text on an ochre band, with the word "Error". */
.err { margin: 0; padding: 6px 10px; background: var(--bb-error); color: var(--bb-ink); font-size: 14px; overflow-wrap: anywhere; }
button:focus-visible, .minp:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
