<script setup lang="ts">
import { computed, ref, watch } from "vue";
import { t } from "./i18n";
import { viewName } from "./shell/viewName";
import { state, element, rename, runVerb, materials, openViews, storeys, type ElementDetail, type ViewSummary } from "./bimble";
import { classNoun, parseMm } from "./i18n/parts/modify";

// Rendered inside `.props` (LeftPanel): rows as fr/View3D.html (view) and fr/SelectModify.html (element).
const detail = ref<ElementDetail | null>(null);
const loadError = ref<string | null>(null);
const name = ref("");
const renameError = ref<string | null>(null);

const one = computed(() => (state.value.selection.length === 1 ? state.value.selection[0] ?? null : null));

// Reload on selection or model change (rev). The watcher runs after the frame, so the viewer is free.
watch(
  () => [one.value, state.value.rev] as const,
  ([id]) => {
    loadError.value = null;
    if (!id) { detail.value = null; return; }
    try {
      const d = element(id);
      if (d.id !== detail.value?.id) renameError.value = null;
      detail.value = d;
      name.value = d.name;
    } catch (e) {
      detail.value = null;
      loadError.value = e instanceof Error ? e.message : String(e);
    }
  },
);

function commit(): void {
  const d = detail.value;
  if (!d || name.value === d.name) return;
  renameError.value = rename(d.id, name.value);
}

// Numbers among the generator parameters: the sizes, in mm, in the reference's order (width, height, length).
const ORDER = ["width", "height", "length", "depth", "thickness"];
const rank = (k: string) => (ORDER.indexOf(k) + ORDER.length + 1) % (ORDER.length + 1);
const sizes = computed(() =>
  Object.entries(detail.value?.generator_params ?? {})
    .filter((e): e is [string, number] => typeof e[1] === "number")
    .sort((a, b) => rank(a[0]) - rank(b[0])),
);
const dimLabel = (k: string) => (ORDER.includes(k) ? t(`modify.dim.${k}`) : k);
const title = computed(() => (detail.value ? `${classNoun(detail.value.class, t).noun} ${detail.value.name}` : ""));

// Origin Z: one `move` verb by the difference (ADR 0052: applies at once).
const originZ = computed(() => detail.value?.placement.origin[2] ?? 0);
const zError = ref<string | null>(null);
function commitZ(e: Event): void {
  const d = detail.value;
  const z = parseMm((e.target as HTMLInputElement).value);
  zError.value = null;
  if (!d || z === null || z === originZ.value) return;
  zError.value = runVerb("move", { ids: [d.id], d: [0, 0, z - originZ.value] });
}

// `open_views` is `{views, active, tabs}` (bridge.rs); bimble.ts types it loosely until then.
const view = computed(() => {
  const ov = openViews.value as unknown as { views?: ViewSummary[]; active?: string | null };
  return ov.views?.find((v) => v.id === ov.active) ?? null;
});
const viewType = computed(() => {
  const v = view.value;
  if (!v) return "";
  if (v.type === "plan") return t(v.look === "up" ? "view.ceiling" : "view.plan");
  return t(v.type === "section" ? "view.section" : "view.three_d");
});

// A plan on paper (fr/PlanOnPaper.html): its range planes as stored (`{level, offset_mm}`).
const onPaper = computed(() => view.value?.type === "plan" && openViews.value.paper.on);
type Plane = { level: string; offset_mm: number };
const PLANES = ["top", "cut", "bottom", "depth"] as const;
const planes = computed(() => {
  const r = (view.value?.range ?? {}) as Partial<Record<(typeof PLANES)[number], Plane>>;
  const key = view.value?.storey as string | undefined;
  const own = storeys.value.find((s) => s.id === key || s.name === key)?.name ?? key ?? "";
  // ponytail: only "associated" names a storey here; above/below need Rust to resolve the level.
  return PLANES.map((k) => ({ k, level: r[k]?.level === "associated" ? own : "", offset: r[k]?.offset_mm ?? 0 }));
});

// The reference's four families; the other categories count as "other".
const FAMILIES = [
  { key: "concrete", swatch: "var(--bb-sheet)" },
  { key: "steel", swatch: "var(--bb-sunk)" },
  { key: "timber", swatch: "var(--bb-line)" },
  { key: "other", swatch: "var(--bb-cream)" },
] as const;
const families = computed(() =>
  FAMILIES.map((f) => ({
    ...f,
    count: materials.value
      .filter((m) => (f.key === "other" ? !["concrete", "steel", "timber"].includes(m.category) : m.category === f.key))
      .reduce((n, m) => n + m.element_count, 0),
  })),
);
// The reference writes thousands with a space and minus as U+2212 ("−250").
const grouped = (n: number) => String(n).replace(/\B(?=(\d{3})+(?!\d))/g, " ").replace("-", "\u2212");
</script>

<template>
  <p v-if="state.selection.length > 1" class="pn" style="display:flex;align-items:center;gap:8px">
    <span class="dot" style="background:var(--bb-selection)" aria-hidden="true" />{{ t('props.selected', { n: state.selection.length }) }}
  </p>
  <p v-else-if="loadError" class="error" role="alert">{{ t('props.error', { msg: loadError }) }}</p>
  <template v-else-if="detail">
    <div class="pn" style="display:flex;align-items:center;gap:8px"><span class="dot" style="background:var(--bb-selection)" />{{ title }}</div>
    <div class="row">
      <label class="k" for="el-name">{{ t('props.name') }}</label>
      <input
        id="el-name"
        v-model="name"
        class="inp"
        style="width:150px"
        :aria-invalid="renameError ? 'true' : 'false'"
        aria-describedby="el-name-error"
        @keydown.enter="commit"
        @blur="commit"
      />
    </div>
    <p v-if="renameError" id="el-name-error" role="alert" class="error">{{ t('props.error', { msg: renameError }) }}</p>
    <div class="row"><span class="k">{{ t('props.class') }}</span><span class="mono">{{ detail.class }}</span></div>
    <div class="row"><span class="k">GlobalId</span><span class="mono"><span style="font-size:11.5px">{{ detail.id }}</span></span></div>
    <div class="row"><span class="k">{{ t('props.storey') }}</span><span>{{ detail.storey_name ?? "—" }}</span></div>
    <div class="row">
      <span class="k">{{ t('props.material') }}</span>
      <span class="sel-inp" style="width:150px" data-missing="set element material (no Command in bimble-core)"><span class="ell">{{ detail.material_name ?? "—" }}</span><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span>
    </div>
    <div class="row">
      <span class="k">{{ t('modify.phase') }}</span>
      <span class="sel-inp" style="width:150px" data-missing="element phase (no phase in the model)"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span>
    </div>
    <div class="sec">{{ t('el.sizes') }}</div>
    <template v-if="sizes.length">
      <div v-for="[k, v] in sizes" :key="k" class="row">
        <label class="k" :for="`el-${k}`">{{ dimLabel(k) }}</label>
        <span style="display:inline-flex;align-items:center;gap:6px">
          <!-- ponytail: read-only until a verb sets one generator parameter. -->
          <input :id="`el-${k}`" class="inp mono" :value="grouped(v)" readonly data-missing="set_generator (Command::SetGenerator has no bimble.ts call)" style="width:96px;text-align:right" />
          <span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">mm</span>
        </span>
      </div>
    </template>
    <div class="row">
      <label class="k" for="el-origin-z">{{ t('modify.originZ') }}</label>
      <span style="display:inline-flex;align-items:center;gap:6px">
        <input id="el-origin-z" :key="`${detail.id}:${originZ}`" class="inp mono" :value="grouped(originZ)" style="width:96px;text-align:right"
               :aria-invalid="zError ? 'true' : 'false'" @keydown.enter="commitZ" @blur="commitZ" />
        <span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">mm</span>
      </span>
    </div>
    <p v-if="zError" role="alert" class="error">{{ t('modify.error', { msg: zError }) }}</p>
  </template>
  <template v-else-if="view && onPaper">
    <div class="pn">{{ t('view.props', { name: viewName(view) }) }}</div>
    <div class="row"><span class="k">{{ t('view.scale') }}</span><span class="sel-inp" style="width:100px" data-missing="set view scale (Command::SetView needs the whole View: no bimble.ts call)">1:{{ view.scale }}<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span></div>
    <div class="row"><span class="k">{{ t('onpaper.orientation') }}</span><span class="sel-inp" style="width:150px" data-missing="set view true_north (no bimble.ts call)">{{ t(view.true_north ? 'onpaper.trueNorth' : 'onpaper.projectNorth') }}<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span></div>
    <div class="row"><span class="k">{{ t('view.phase') }}</span><span class="sel-inp" style="width:150px" data-missing="view phase filter"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span></div>
    <div class="row"><span class="k">{{ t('onpaper.crop') }}</span><span class="sel-inp" style="width:110px" data-missing="set view crop (no bimble.ts call)">{{ t(view.crop ? 'onpaper.on' : 'onpaper.off') }}<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span></div>
    <div class="sec">{{ t('onpaper.range') }}</div>
    <div v-for="p in planes" :key="p.k" class="row">
      <label class="k" :for="`range-${p.k}`">{{ t(`onpaper.${p.k}`) }}</label>
      <span style="display:inline-flex;gap:6px">
        <span class="sel-inp" style="width:78px" :data-missing="p.level ? 'set view range level (no bimble.ts call)' : 'range plane storey name for above/below/unlimited (resolve in Rust)'"><span class="mono ell">{{ p.level }}</span><svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span>
        <input :id="`range-${p.k}`" class="inp mono" :value="grouped(p.offset)" readonly data-missing="set view range offset (no bimble.ts call)" style="width:70px;text-align:right">
      </span>
    </div>
    <div class="sec">{{ t('onpaper.refs') }}</div>
    <div v-if="view.references" class="row" data-missing="reference names in query('views') (only a count today)"><span class="k">{{ t('nav.refs', { n: view.references }) }}</span></div>
    <div class="row"><span class="k">{{ t('onpaper.display') }}</span><span class="sel-inp" style="width:110px" data-missing="reference display style (no field in View)"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span></div>
  </template>
  <template v-else-if="view">
    <div class="pn">{{ t('view.props', { name: viewName(view) }) }}</div>
    <div class="row"><span class="k">{{ t('view.type') }}</span><span>{{ viewType }}</span></div>
    <div class="row"><span class="k">{{ t('view.scale') }}</span><span class="mono">1:{{ view.scale }}</span></div>
    <div class="row">
      <span class="k">{{ t('view.phase') }}</span>
      <span class="sel-inp" style="width:150px" data-missing="view phase filter"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span>
    </div>
    <div class="row">
      <span class="k">{{ t('view.subprojects') }}</span>
      <span class="sel-inp" style="width:160px" data-missing="view sub-projects filter"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span>
    </div>
    <div class="row"><span class="k">{{ t('view.detail') }}</span><span data-missing="view detail level" /></div>
    <div class="row"><span class="k">{{ t('view.hidden') }}</span><span class="mono" data-missing="hidden classes in view" /></div>
    <div class="sec">{{ t('view.families') }}</div>
    <label v-for="f in families" :key="f.key" class="row" style="padding-left:0px">
      <span style="display:inline-flex;gap:8px;align-items:center">
        <input type="checkbox" checked disabled data-missing="family visibility in view"> <span :style="`width:14px;height:14px;border:1px solid var(--bb-ink);background:${f.swatch};display:inline-block`" />{{ t(`fam.${f.key}`) }}</span>
      <span class="mono" style="font-size:12px;color:var(--bb-stone)">{{ grouped(f.count) }}</span>
    </label>
    <div class="sec" data-missing="class counts and class visibility in view">{{ t('view.classes') }}</div>
  </template>
  <p v-else class="row k">{{ t('props.empty') }}</p>
</template>

<style scoped>
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.row .k, .k { color: var(--bb-stone); white-space: nowrap; }
p.row { margin: 0; }
.mono { font-family: var(--bb-font-data); }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.inp.mono { font-family: 'DM Mono', monospace; font-size: 13px; }
.inp:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* .sel-inp{display:inline-flex;align-items:center;justify-content:space-between;gap:6px;height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 6px 0 8px;font-size:13.5px;white-space:nowrap} */
.sel-inp { display: inline-flex; align-items: center; justify-content: space-between; gap: 6px; height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 6px 0 8px; font-size: 13.5px; white-space: nowrap; }
.ell { overflow: hidden; text-overflow: ellipsis; }
/* .sec{font-size:12px;font-family:'DM Mono',monospace;letter-spacing:.08em;text-transform:uppercase;color:#5C574D;margin-top:6px} */
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
/* .dot{width:12px;height:12px;border-radius:50%;border:1px solid #141414;display:inline-block;flex:none} */
.dot { width: 12px; height: 12px; border-radius: 50%; border: 1px solid var(--bb-ink); display: inline-block; flex: none; }
/* Error: ink text on an ochre band, with the word "Error". */
.error { color: var(--bb-ink); background: var(--bb-error); font-size: 13.5px; margin: 0; padding: 2px 8px; overflow-wrap: anywhere; }
</style>
