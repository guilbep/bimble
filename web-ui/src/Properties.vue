<script setup lang="ts">
import { computed, ref, watch } from "vue";
import { t } from "./i18n";
import { viewName } from "./shell/viewName";
import { state, element, rename, materials, openViews, type ElementDetail, type ViewSummary } from "./bimble";

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

// Numbers among the generator parameters: the sizes, in mm.
const sizes = computed(() =>
  Object.entries(detail.value?.generator_params ?? {}).filter(
    (e): e is [string, number] => typeof e[1] === "number",
  ),
);

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
const grouped = (n: number) => String(n).replace(/\B(?=(\d{3})+(?!\d))/g, " ");
</script>

<template>
  <p v-if="state.selection.length > 1" class="pn" style="display:flex;align-items:center;gap:8px">
    <span class="dot" style="background:var(--bb-selection)" aria-hidden="true" />{{ t('props.selected', { n: state.selection.length }) }}
  </p>
  <p v-else-if="loadError" class="error" role="alert">{{ t('props.error', { msg: loadError }) }}</p>
  <template v-else-if="detail">
    <div class="pn" style="display:flex;align-items:center;gap:8px"><span class="dot" style="background:var(--bb-selection)" />{{ detail.name }}</div>
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
      <span class="sel-inp" style="width:150px" data-missing="set-material"><span class="ell">{{ detail.material_name ?? "—" }}</span><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span>
    </div>
    <div class="row"><span class="k">{{ t('props.generator') }}</span><span class="mono">{{ detail.generator }}</span></div>
    <template v-if="sizes.length">
      <div class="sec">{{ t('el.sizes') }}</div>
      <div v-for="[k, v] in sizes" :key="k" class="row">
        <label class="k" :for="`el-${k}`">{{ k }}</label>
        <span style="display:inline-flex;align-items:center;gap:6px">
          <!-- ponytail: read-only until a verb sets one generator parameter. -->
          <input :id="`el-${k}`" class="inp mono" :value="grouped(v)" readonly data-missing="set-generator-param" style="width:96px;text-align:right" />
          <span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">mm</span>
        </span>
      </div>
    </template>
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
.pn { font-family: var(--bb-font-display); font-weight: 500; font-size: 14px; margin: 0 0 4px; }
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
