<script lang="ts">
export const hint = "materials.hint";
export const capability = "materials"; // FAKE1: query materials_page, verbs material_* (ccore/session/capabilities.c)
</script>

<script setup lang="ts">
import { stateRev } from "../bimble"; // VIS2
// MAT1: the material browser (crates/bimble-viewer/src/materials_page.rs, ADR 0031). Left: the model's
// materials with how many elements are made of each, then the library. Right: the selected one's
// identity, properties and graphics. In an editable model every change is one verb (material_add,
// material_set, material_remove, set_material for the selection): one undo step each. A file read as
// it is (an IFC, a .bimbleview) shows the list read only, its values from the library grade.
import { computed, ref, watch } from "vue";
import { query, runVerb, state } from "../bimble";
import { t } from "../i18n";

interface Props { density_kg_m3?: number; e_mpa?: number; strength_mpa?: number; source?: string }
interface Hatch { lines: { angle_deg: number; spacing_px: number }[]; stipple: boolean }
interface Row {
  id: string | null; name: string; category: string; ifc_category: string; elements: number; props: Props;
  from_library: boolean; grade: string | null; strength_symbol: string; own_props: boolean; grade_differs: boolean;
  colour: string; hatch: Hatch | null; paper_cut: string | null; paper_seen: string | null;
}
interface LibRow { name: string; category: string; props: Props; strength_symbol: string; have: boolean }
interface Page { editable: boolean; rows: Row[]; library: LibRow[] }

const CATS = ["concrete", "steel", "timber", "masonry", "insulation", "earth", "glass", "other"] as const;
const catLabel = (c: string) => t(`materials.cat.${c}` as "materials.cat.concrete");

// Re-asked after every change of the model (rev), undo and redo included.
const data = computed<Page>(() => {
  void stateRev.value;
  void state.value.file;
  try { return query<Page>("materials_page"); } catch { return { editable: false, rows: [], library: [] }; }
});
const editable = computed(() => data.value.editable);
const q = ref("");
const shown = computed(() => data.value.rows.filter((r) => r.name.toLowerCase().includes(q.value.toLowerCase())));
const libShown = computed(() => data.value.library.filter((m) => !q.value || m.name.toLowerCase().includes(q.value.toLowerCase())));
// The selected material by name (materials_page.rs:16); the first one when it is gone.
const selected = ref<string | null>(null);
const row = computed(() => data.value.rows.find((r) => r.name === selected.value) ?? null);
watch(data, (d) => {
  if (!d.rows.some((r) => r.name === selected.value)) selected.value = d.rows[0]?.name ?? null;
}, { immediate: true });
const error = ref<string | null>(null);
const nSel = computed(() => state.value.selection.length);

function run(name: string, args: unknown): boolean {
  error.value = runVerb(name, args);
  return error.value === null;
}
function addFromLibrary(name: string) {
  if (run("material_add", { library: name })) selected.value = name;
}
const blank = ref("");
function addBlank() {
  const name = blank.value.trim();
  if (!name) return;
  if (run("material_add", { name })) { selected.value = name; blank.value = ""; }
}
function rename(e: Event) {
  const r = row.value;
  const v = (e.target as HTMLInputElement).value.trim();
  if (!r?.id || v === r.name) return;
  // A rename refused stays on the material under its old name (materials_page.rs:461).
  if (run("material_set", { id: r.id, name: v })) selected.value = v;
  else (e.target as HTMLInputElement).value = r.name;
}
function setFamily(e: Event) {
  const r = row.value;
  if (r?.id) run("material_set", { id: r.id, category: (e.target as HTMLSelectElement).value });
}
// The values held by the page: the material's, or the grade's (from_library). Changing one sends all
// four; values first taken from the grade get "(changed)" after their source (materials_page.rs:333).
function propsOf(r: Row): Props { return { ...r.props }; }
function sendProps(r: Row, p: Props) {
  if (!r.id) return;
  if (r.from_library && p.source) p.source = `${p.source} (changed)`;
  run("material_set", { id: r.id, props: p });
}
function setValue(field: "density_kg_m3" | "e_mpa" | "strength_mpa", e: Event) {
  const r = row.value;
  if (!r) return;
  const v = Number((e.target as HTMLInputElement).value);
  if (!Number.isFinite(v) || v === r.props[field]) return;
  sendProps(r, { ...propsOf(r), [field]: v });
}
function setOne(field: "density_kg_m3" | "e_mpa" | "strength_mpa") {
  const r = row.value;
  if (r) sendProps(r, { ...propsOf(r), [field]: 1 });
}
function useGrade() {
  const r = row.value, g = data.value.library.find((m) => m.name === r?.grade);
  if (r?.id && g) run("material_set", { id: r.id, props: g.props });
}
function clearValues() {
  const r = row.value;
  if (r?.id) run("material_set", { id: r.id, props: {} });
}
function assign() {
  const r = row.value;
  if (r && nSel.value) run("set_material", { ids: state.value.selection, material: r.name });
}
function remove() {
  const r = row.value;
  if (r?.id) run("material_remove", { id: r.id });
}
const fmt = (v: number | undefined, unit: string) => (v === undefined ? t("materials.unknown") : `${v} ${unit}`);
// The family's screen pattern as SVG lines (style.rs Lines: angle and spacing in screen px).
function hatchLines(h: Hatch | null, w: number, hgt: number): string[] {
  if (!h) return [];
  const out: string[] = [];
  for (const l of h.lines) {
    const a = (l.angle_deg * Math.PI) / 180, s = Math.max(2, l.spacing_px);
    const dx = Math.cos(a), dy = -Math.sin(a), nx = -dy, ny = dx, r = Math.hypot(w, hgt);
    for (let k = -r; k <= r; k += s) {
      const cx = w / 2 + nx * k, cy = hgt / 2 + ny * k;
      out.push(`M${(cx - dx * r).toFixed(1)} ${(cy - dy * r).toFixed(1)}L${(cx + dx * r).toFixed(1)} ${(cy + dy * r).toFixed(1)}`);
    }
  }
  return out;
}
</script>

<template>
  <div class="mt" data-bb="materials" :data-bb-editable="editable ? '1' : '0'">
    <header class="hd">
      <h2>{{ t('materials.title') }}</h2>
      <p class="note">{{ editable ? t('materials.noteEdit') : t('materials.noteRead') }}</p>
      <p v-if="error" class="banner b-o" role="alert" data-bb="material-error">{{ t('materials.error', { msg: error }) }}</p>
    </header>
    <div class="cols">
      <!-- left: the list, then the library -->
      <section class="left">
        <input v-model="q" class="inp" :placeholder="t('materials.find')" :aria-label="t('materials.find')" data-bb="material-find">
        <ul class="list" data-bb="material-list">
          <li v-for="r in shown" :key="r.id ?? r.name">
            <button type="button" class="it" :class="{ on: r.name === selected }" :aria-pressed="r.name === selected"
                    data-bb="material-row" :data-name="r.name" :data-count="r.elements" @click="selected = r.name">
              <span class="sw" :style="{ background: r.colour }" aria-hidden="true" />
              <span class="nm">{{ r.name }}</span><span class="ct">({{ r.elements }})</span>
            </button>
          </li>
        </ul>
        <div v-if="editable" class="blank">
          <input v-model="blank" class="inp" :placeholder="t('materials.blankName')" :aria-label="t('materials.blankName')" data-bb="material-blank-name" @keydown.enter="addBlank">
          <button type="button" class="btn s" :disabled="!blank.trim()" data-bb="material-add-blank" @click="addBlank">{{ t('materials.addBlank') }}</button>
        </div>
        <h3>{{ t('materials.library') }}</h3>
        <p class="note">{{ t('materials.libraryNote') }}</p>
        <ul class="list lib" data-bb="material-library">
          <li v-for="m in libShown" :key="m.name" class="lr">
            <span class="nm">{{ m.name }}</span><span class="ct">{{ catLabel(m.category) }}</span>
            <button v-if="editable" type="button" class="btn s" :disabled="m.have" :title="m.have ? t('materials.have') : t('materials.addTitle')"
                    data-bb="material-library-add" :data-name="m.name" @click="addFromLibrary(m.name)">{{ t('materials.add') }}</button>
          </li>
        </ul>
      </section>
      <!-- right: the selected material -->
      <section v-if="row" class="right" data-bb="material-detail" :data-name="row.name">
        <h2 data-bb="material-title">{{ row.name }}</h2>
        <p class="note" data-bb="material-count">{{ row.elements === 1 ? t('materials.element') : t('materials.elements', { n: row.elements }) }}</p>
        <h3>{{ t('materials.identity') }}</h3>
        <div class="grid">
          <label class="k" for="mat-name">{{ t('materials.name') }}</label>
          <input v-if="editable" id="mat-name" :key="row.name" class="inp" :value="row.name" data-bb="material-name" @change="rename">
          <span v-else>{{ row.name }}</span>
          <label class="k" for="mat-family">{{ t('materials.family') }}</label>
          <select v-if="editable" id="mat-family" class="inp" :value="row.category" data-bb="material-family" @change="setFamily">
            <option v-for="c in CATS" :key="c" :value="c">{{ catLabel(c) }}</option>
          </select>
          <span v-else>{{ catLabel(row.category) }}</span>
          <span class="k">{{ t('materials.ifcCategory') }}</span>
          <span class="mono">{{ row.ifc_category || t('materials.noCategory') }}</span>
        </div>
        <h3>{{ t('materials.properties') }}</h3>
        <div class="grid" data-bb="material-props">
          <template v-for="f in ([['density_kg_m3', 'materials.density', 'kg/m³'], ['e_mpa', 'materials.modulus', 'MPa'], ['strength_mpa', 'materials.strength', 'MPa']] as const)" :key="f[0]">
            <label class="k" :for="`mat-${f[0]}`">{{ t(f[1], { symbol: row.strength_symbol }) }}</label>
            <span v-if="editable && row.props[f[0]] !== undefined" class="num">
              <input :id="`mat-${f[0]}`" :key="`${row.name}-${row.props[f[0]]}`" class="inp" type="number" min="0.001" step="any" :value="row.props[f[0]]"
                     :data-bb="`material-${f[0]}`" @change="setValue(f[0], $event)"><span class="u">{{ f[2] }}</span>
            </span>
            <button v-else-if="editable" type="button" class="btn s sm" :data-bb="`material-set-${f[0]}`" @click="setOne(f[0])">{{ t('materials.set') }}</button>
            <span v-else>{{ fmt(row.props[f[0]], f[2]) }}</span>
          </template>
          <span class="k">{{ t('materials.source') }}</span>
          <span data-bb="material-source">{{ row.props.source || '—' }}</span>
        </div>
        <p v-if="row.from_library" class="note">{{ t('materials.fromLibrary', { grade: row.grade ?? '' }) }}</p>
        <div v-if="editable" class="row">
          <button v-if="row.grade" type="button" class="btn s" :disabled="!row.grade_differs" data-bb="material-use-grade" @click="useGrade">{{ t('materials.useGrade', { grade: row.grade }) }}</button>
          <button type="button" class="btn s" :disabled="!row.own_props" data-bb="material-clear" @click="clearValues">{{ t('materials.clear') }}</button>
        </div>
        <h3>{{ t('materials.graphics') }}</h3>
        <p class="note">{{ t('materials.graphicsNote', { family: catLabel(row.category).toLowerCase() }) }}</p>
        <div class="grid">
          <span class="k">{{ t('materials.onScreen') }}</span>
          <svg width="72" height="22" viewBox="0 0 72 22" role="img" :aria-label="row.colour" data-bb="material-swatch" :data-colour="row.colour">
            <rect width="72" height="22" rx="2" :fill="row.colour" />
            <path v-for="(d, i) in hatchLines(row.hatch, 72, 22)" :key="i" :d="d" stroke="#141414" stroke-opacity=".35" stroke-width=".8" />
            <g v-if="row.hatch?.stipple" fill="#141414" fill-opacity=".3"><circle v-for="i in 24" :key="i" :cx="(i * 13) % 72" :cy="(i * 7) % 22" r=".8" /></g>
          </svg>
          <span class="k">{{ t('materials.cut') }}</span><span>{{ row.paper_cut ?? t('materials.ownPattern') }}</span>
          <span class="k">{{ t('materials.seen') }}</span><span>{{ row.paper_seen ?? t('materials.ownPattern') }}</span>
        </div>
        <p class="note">{{ t('materials.hatchLater') }}</p>
        <div v-if="editable" class="row">
          <button type="button" class="btn s" :disabled="nSel === 0" data-bb="material-assign" @click="assign">{{ t('materials.assign', { n: nSel }) }}</button>
          <button type="button" class="btn s danger" data-bb="material-remove" @click="remove">{{ t('materials.remove') }}</button>
        </div>
      </section>
      <section v-else class="right"><p class="note">{{ t('materials.none') }}</p></section>
    </div>
  </div>
</template>

<style scoped>
.mt { display: flex; flex-direction: column; gap: 14px; }
h2 { font-family: 'Sora', sans-serif; margin: 0; font-size: 20px; }
h3 { font-family: 'Sora', sans-serif; margin: 12px 0 4px; font-size: 15px; }
.note { font-size: 13px; color: var(--bb-stone); margin: 2px 0; }
.mono { font-family: 'DM Mono', ui-monospace, Menlo, monospace; font-size: 12.5px; }
.banner { margin: 6px 0 0; padding: 10px 16px; font-size: 14px; }
.b-o { background: var(--bb-error); color: var(--bb-ink); }
.cols { display: grid; grid-template-columns: minmax(0, 300px) minmax(0, 1fr); gap: 28px; align-items: start; }
.left, .right { display: flex; flex-direction: column; gap: 6px; min-width: 0; }
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.inp:focus-visible, button:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
.list { list-style: none; margin: 0; padding: 0; max-height: 320px; overflow: auto; }
.list.lib { max-height: 260px; }
.it { display: flex; align-items: center; gap: 8px; width: 100%; padding: 4px 6px; border: 0; border-radius: 6px; background: none; font: inherit; font-size: 13.5px; color: var(--bb-ink); cursor: pointer; text-align: left; }
.it.on { background: var(--bb-sheet); box-shadow: inset 0 0 0 1px var(--bb-ink); }
.sw { width: 14px; height: 14px; border-radius: 2px; flex: none; border: 1px solid rgba(0, 0, 0, .2); }
.nm { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.ct { color: var(--bb-stone); font-size: 12.5px; }
.lr { display: grid; grid-template-columns: minmax(0, 1fr) auto auto; gap: 8px; align-items: center; padding: 2px 6px; font-size: 13.5px; }
.blank { display: grid; grid-template-columns: minmax(0, 1fr) auto; gap: 6px; margin-top: 6px; }
.grid { display: grid; grid-template-columns: 160px minmax(0, 1fr); gap: 6px 12px; align-items: center; font-size: 13.5px; }
.k { color: var(--bb-stone); }
.num { display: inline-flex; align-items: center; gap: 6px; }
.num .inp { width: 130px; }
.u { color: var(--bb-stone); font-size: 12.5px; }
.row { display: flex; gap: 8px; flex-wrap: wrap; margin-top: 6px; }
.btn { display: inline-flex; align-items: center; gap: 4px; height: 30px; padding: 0 12px; border-radius: var(--bb-radius-pill); font: inherit; font-size: 13.5px; cursor: pointer; white-space: nowrap; }
.btn.s { background: var(--bb-sheet); border: 1px solid var(--bb-ink); color: var(--bb-ink); }
.btn.sm { height: 24px; padding: 0 8px; justify-self: start; }
.btn.danger { color: var(--bb-clay-text); border-color: var(--bb-clay-text); }
button:disabled { color: var(--bb-stone); opacity: .55; cursor: default; }
</style>
