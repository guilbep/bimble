<script setup lang="ts">
import { stateRev } from "./bimble"; // VIS2
import { computed, ref, watch } from "vue";
import { groupDigits } from "./format"; // QTY1
import { t } from "./i18n";
import { viewName } from "./shell/viewName";
import { state, element, query, rename, runVerb, setGenerator, setMaterial, setView, materials, openViews, storeys, type ElementDetail, type ViewSummary } from "./bimble";
import { classNoun, parseMm } from "./i18n/parts/modify";
// ---- SEC1 ---- view properties made real (verbs view_edit, set_view; query view_visibility)
import { viewClip, viewEdit, viewVisibility, visibility, type SectionClip, type ViewVisibility } from "./bimble";
import { editableModel, has } from "./shell/capability";
// ---- end SEC1 ----
// ---- WAVE1B (B2): the host's openings, one selected, its sizes changed (verb opening_set) ----
import { openingSet, openingsOf, selectedOpening, type OpeningInfo, type OpeningPatch } from "./bimble";
import { noteAdd, type PropertySet } from "./bimble"; // M1LINKS

// Rendered inside `.props` (LeftPanel): rows as fr/View3D.html (view) and fr/SelectModify.html (element).
const detail = ref<ElementDetail | null>(null);
const loadError = ref<string | null>(null);
const name = ref("");
const renameError = ref<string | null>(null);

const one = computed(() => (state.value.selection.length === 1 ? state.value.selection[0] ?? null : null));

// Read-only: the selected element is in a linked model (on_change `linked`), or the file is open as it is
// (an IFC, a .bimbleview: `query("state").editable` false, and every model query answers "read only").
const linkedName = computed(() => {
  const lk = state.value.linked;
  return lk && lk.id === one.value ? lk.link.replace(/\.[^.]+$/, "") : null;
});
const fileReadOnly = computed(() => {
  if (!state.value.file) return false;
  try { return query<{ editable?: boolean }>("state").editable === false; } catch { return false; }
});
const readOnly = computed(() =>
  linkedName.value !== null ? { kind: "linked", text: t("props.linkedReadOnly", { name: linkedName.value }) }
  : fileReadOnly.value ? { kind: "file", text: t("props.fileReadOnly") } : null);

// Reload on selection or model change (rev), after the next frame is presented (VIS2: a press's feedback is the selection's
// highlight in the viewport first; the element's query and this panel's render came before that frame, in the click's
// task: 100 to 200 ms with the CPU throttled 6 times). A load asked again before it runs is the one that counts.
let loadSeq = 0;
watch(
  () => [one.value, state.value.rev] as const,
  ([id]) => {
    loadError.value = null;
    if (!id) { detail.value = null; loadSeq++; return; }
    if (detail.value && detail.value.id !== id) detail.value = null; // (another element: nothing stale shown meanwhile)
    const seq = ++loadSeq;
    requestAnimationFrame(() => setTimeout(() => { if (seq === loadSeq && one.value === id) load(id); }, 0));
  },
);
function load(id: string): void {
    try {
      const d = element(id);
      if (d.id !== detail.value?.id) renameError.value = materialError.value = null;
      detail.value = d;
      name.value = d.name;
    } catch (e) {
      detail.value = null;
      loadError.value = e instanceof Error ? e.message : String(e);
    }
}

// ---- WAVE1B (B2): openings of the selected host; the selected one's fields ----
const openings = computed<OpeningInfo[]>(() => { void stateRev.value; const d = detail.value; return d && d.openings > 0 ? openingsOf(d.id) : []; });
const selOpening = computed(() => {
  const so = selectedOpening.value, d = detail.value;
  return so && d && so.host === d.id ? openings.value.find((o) => o.id === so.id) ?? null : null;
});
// The element selection moved to another element: the opening is let go.
watch(one, (id) => { if (selectedOpening.value && selectedOpening.value.host !== id) selectedOpening.value = null; });
const openingError = ref<string | null>(null);
const openingFields = computed(() => {
  const o = selOpening.value;
  if (!o || o.kind === "other") return [] as { k: keyof OpeningPatch; v: number }[];
  const ks: (keyof OpeningPatch)[] = o.kind === "sleeve" ? (o.wall ? ["diameter", "sill"] : ["diameter"]) : o.wall ? ["width", "height", "sill"] : ["width", "length"];
  return ks.map((k) => ({ k, v: (o[k as keyof OpeningInfo] as number | undefined) ?? 0 }));
});
function commitOpening(k: keyof OpeningPatch, old: number, e: Event): void {
  const o = selOpening.value, d = detail.value;
  const v = parseMm((e.target as HTMLInputElement).value);
  openingError.value = null;
  if (!o || !d || v === null || v === old) return;
  openingError.value = openingSet(d.id, o.id, { [k]: v });
}
/** The centre moved: x, y typed (world mm). */
function commitOpeningAt(i: 0 | 1, e: Event): void {
  const o = selOpening.value, d = detail.value;
  const v = parseMm((e.target as HTMLInputElement).value);
  openingError.value = null;
  if (!o || !d || v === null || v === o.centre[i]) return;
  const a: [number, number] = [o.centre[0], o.centre[1]];
  a[i] = v;
  openingError.value = openingSet(d.id, o.id, { a });
}
const openingLabel = (o: OpeningInfo, i: number) => o.kind === "sleeve" ? t("props.op.sleeve", { n: i + 1, d: grouped(o.diameter ?? 0) })
  : o.kind === "opening" ? t("props.op.box", { n: i + 1, w: grouped(o.width ?? 0), h: grouped((o.wall ? o.height : o.length) ?? 0) }) : t("props.op.other", { n: i + 1 });
// ---- end WAVE1B ----

// ---- M1LINKS ---- a linked element's property and quantity sets (query element {id, link}), a note on it
const psets = computed(() => (detail.value as unknown as { property_sets?: PropertySet[] } | null)?.property_sets ?? null);
const psetsNote = computed(() => (detail.value as unknown as { property_sets_note?: string | null } | null)?.property_sets_note ?? null);
const noteText = ref("");
const noteError = ref<string | null>(null);
function addNote(): void {
  const lk = state.value.linked;
  if (!lk || !noteText.value.trim()) return;
  noteError.value = noteAdd(lk.link, lk.id, noteText.value.trim());
  if (!noteError.value) noteText.value = "";
}
// ---- end M1LINKS ----
// FIXUI: the read-only rows' extra fields (scene_element, a linked element's detail).
const roExtra = computed(() => {
  const d = detail.value as (ElementDetail & { family?: string; triangles?: number; volume_m3?: number }) | null;
  return { family: d?.family ?? "", triangles: typeof d?.triangles === "number" ? d.triangles : null, volume: typeof d?.volume_m3 === "number" ? d.volume_m3 : null };
});

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

// Sizes: one `set_generator` verb per changed field (Enter or blur).
const sizeError = ref<string | null>(null);
function commitSize(k: string, old: number, e: Event): void {
  const d = detail.value;
  const v = parseMm((e.target as HTMLInputElement).value);
  sizeError.value = null;
  if (!d || v === null || v === old) return;
  sizeError.value = setGenerator(d.id, { [k]: v });
}

// Material: one `set_material` verb on change.
const materialError = ref<string | null>(null);
function commitMaterial(e: Event): void {
  const d = detail.value;
  const v = (e.target as HTMLSelectElement).value;
  materialError.value = d && v !== d.material_name ? setMaterial([d.id], v) : null;
}

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

// On paper: one `set_view` verb per changed field (change, Enter or blur).
const SCALES = [1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000];
const scales = computed(() => [...new Set([...SCALES, view.value?.scale ?? 100])].sort((a, b) => a - b));
const viewError = ref<string | null>(null);
function patchView(patch: Parameters<typeof setView>[1]): void {
  viewError.value = view.value ? setView(view.value.id, patch) : null;
}
function commitOffset(k: (typeof PLANES)[number], old: number, e: Event): void {
  const v = parseMm((e.target as HTMLInputElement).value);
  if (v === null || v === old) return;
  patchView({ range: { [k]: { offset_mm: v } } });
}

// ---- SEC1 ---- the view's own fields (viewing.rs:976-1440): name, a section's cut, the crop by
// numbers, a plan's range levels, what the view hides by family and IFC class. Each change is one
// verb (view_edit or set_view), one undo step; the viewport and the paper drawings follow.
const canEdit = computed(() => { void stateRev.value; return has("views") && editableModel(); });
const vvis = computed<ViewVisibility | null>(() => { void stateRev.value; void openViews.value; return viewVisibility(); });
const secError = ref<string | null>(null);
function edit(patch: Parameters<typeof viewEdit>[1]): void {
  secError.value = view.value ? viewEdit(view.value.id, patch) : null;
}
function commitName(e: Event): void {
  const v = (e.target as HTMLInputElement).value.trim();
  if (v && view.value && v !== view.value.name) edit({ name: v });
}
const origin = computed(() => (view.value?.origin as number[] | undefined) ?? [0, 0]);
function commitOrigin(k: 0 | 1, e: Event): void {
  const v = parseMm((e.target as HTMLInputElement).value);
  if (v === null || v === origin.value[k]) return;
  const o = [...origin.value];
  o[k] = v;
  edit({ origin: o });
}
function commitDepth(e: Event): void {
  const v = parseMm((e.target as HTMLInputElement).value);
  if (v !== null && v !== view.value?.depth_mm) edit({ depth_mm: v });
}
// The crop (ADR 0046), in the view's plane, world mm; shown and typed in metres as Rust's form.
type Crop = { min: number[]; max: number[] };
const crop = computed(() => (view.value?.crop as Crop | null | undefined) ?? null);
/** A new crop: what the view shows of the model (view_clip's box, back in world mm, in its plane). */
function cropModel(): Crop | null {
  const c = viewClip() as SectionClip | null, o = visibility.value?.section_box.origin_mm;
  if (!c?.clip || !o) return null;
  const w = (k: number, b: number[]) => (b[k] ?? 0) + (o[k] ?? 0);
  if (c.type === "section") {
    const d = c.dir ?? [0, 1], r = [d[1] ?? 0, -(d[0] ?? 0)];
    const along = [c.clip.min, c.clip.max].flatMap((p) => [c.clip!.min, c.clip!.max].map((q) => (r[0] ?? 0) * w(0, p) + (r[1] ?? 0) * w(1, q)));
    return { min: [Math.round(Math.min(...along)), Math.round(w(2, c.clip.min))], max: [Math.round(Math.max(...along)), Math.round(w(2, c.clip.max))] };
  }
  return { min: [Math.round(w(0, c.clip.min)), Math.round(w(1, c.clip.min))], max: [Math.round(w(0, c.clip.max)), Math.round(w(1, c.clip.max))] };
}
function toggleCrop(on: boolean): void {
  if (!view.value) return;
  viewError.value = setView(view.value.id, { crop: on ? cropModel() : null } as Parameters<typeof setView>[1]);
}
function commitCrop(side: "min" | "max", k: 0 | 1, e: Event): void {
  const c = crop.value;
  const m = Number((e.target as HTMLInputElement).value.replace(",", "."));
  if (!c || !view.value || !Number.isFinite(m) || Math.round(m * 1000) === c[side][k]) return;
  const next = { min: [...c.min], max: [...c.max] };
  next[side][k] = Math.round(m * 1000);
  viewError.value = setView(view.value.id, { crop: next } as Parameters<typeof setView>[1]);
}
const LEVELS = ["associated", "above", "below", "unlimited"] as const;
const rangePlanes = computed(() => {
  const r = (view.value?.range ?? {}) as Partial<Record<(typeof PLANES)[number], Plane>>;
  return PLANES.map((k) => ({ k, level: r[k]?.level ?? "associated", offset: r[k]?.offset_mm ?? 0 }));
});
// Families as Rust's labels (MaterialCategory::label); "other" is every family but the three.
const FAMILY_LABELS: Record<string, string[]> = { concrete: ["Concrete"], steel: ["Steel"], timber: ["Timber"], other: ["Masonry", "Insulation", "Earth", "Glass", "Other"] };
const familyShown = (key: string) => !(FAMILY_LABELS[key] ?? []).some((l) => vvis.value?.hidden_families.includes(l));
function toggleFamily(key: string, on: boolean): void {
  const cur = new Set(vvis.value?.hidden_families ?? []);
  for (const l of FAMILY_LABELS[key] ?? []) on ? cur.delete(l) : cur.add(l);
  edit({ visibility: { hidden_families: [...cur] } });
}
// The model's IFC classes and their counts (query elements, as the navigator's object tree).
const classes = computed(() => {
  void stateRev.value;
  if (!canEdit.value) return [] as [string, number][];
  const n = new Map<string, number>();
  try { for (const e of query<{ class: string }[]>("elements", { limit: 100000 })) n.set(e.class, (n.get(e.class) ?? 0) + 1); } catch { /* none */ }
  return [...n].sort((a, b) => a[0].localeCompare(b[0]));
});
function toggleClass(cls: string, on: boolean): void {
  const cur = new Set(vvis.value?.hidden_classes ?? []);
  if (on) cur.delete(cls); else cur.add(cls);
  edit({ visibility: { hidden_classes: [...cur] } });
}
const hiddenText = computed(() => {
  const v = vvis.value;
  if (!v) return "";
  const all = [...v.hidden_classes, ...v.hidden_families, ...v.hidden_storeys, ...v.hidden_materials];
  const n = v.hidden_elements.length + v.isolated_elements.length;
  return all.length || n ? [...all, ...(n ? [`${n} el.`] : [])].join(", ") : t("sec1.hiddenNone");
});
// ---- end SEC1 ----

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
const grouped = (n: number) => groupDigits(n); // QTY1: format.ts (the integer part only: "3 101.6124838541646")
</script>

<template>
  <p v-if="state.selection.length > 1" class="pn" style="display:flex;align-items:center;gap:8px">
    <span class="dot" style="background:var(--bb-selection)" aria-hidden="true" />{{ t('props.selected', { n: state.selection.length }) }}
  </p>
  <!-- A file open as it is: scene_element answers its elements (FIXUI); an error stays an error. -->
  <p v-else-if="loadError" class="error" role="alert">{{ t('props.error', { msg: loadError }) }}</p>
  <template v-else-if="detail">
    <div class="pn" style="display:flex;align-items:center;gap:8px"><span class="dot" style="background:var(--bb-selection)" />{{ title }}</div>
    <!-- Slate is "other": a linked model (bimble-design colour jobs). -->
    <p v-if="readOnly" class="ro" data-bb="props-readonly"><span class="dot" :class="{ linked: readOnly.kind === 'linked' }" aria-hidden="true" />{{ readOnly.text }}</p>
    <div class="row">
      <label class="k" for="el-name">{{ t('props.name') }}</label>
      <input
        id="el-name"
        v-model="name"
        class="inp"
        style="width:150px"
        :disabled="readOnly !== null"
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
    <!-- FIXUI: read only (a file open as it is, a linked model): the rows Rust's viewer shows (lib.rs:4570). -->
    <template v-if="readOnly">
      <div class="row" data-bb="props-material"><span class="k">{{ t('props.material') }}</span><span>{{ detail.material_name ?? "—" }}</span></div>
      <div v-if="roExtra.family" class="row"><span class="k">{{ t('props.family') }}</span><span>{{ roExtra.family }}</span></div>
      <div v-if="roExtra.triangles !== null" class="row"><span class="k">{{ t('props.triangles') }}</span><span class="mono">{{ grouped(roExtra.triangles) }}</span></div>
      <div v-if="roExtra.volume !== null" class="row"><span class="k">{{ t('props.volume') }}</span><span class="mono">{{ roExtra.volume.toFixed(3) }} m³</span></div>
      <!-- M1LINKS: the linked element's property and quantity sets, then a note on it -->
      <template v-if="psets !== null">
        <div class="sec">{{ t('m1.propertySets') }}</div>
        <p v-if="!psets.length" class="row" data-bb="props-no-sets">{{ psetsNote ? t('m1.setsNote', { why: psetsNote }) : t('m1.noSets') }}</p>
        <template v-for="ps in psets" :key="ps.name">
          <div class="row" data-bb="props-pset" :data-set="ps.name"><span class="k" style="font-weight:600">{{ ps.name }}</span><span class="mono" style="font-size:11px">{{ ps.type }}</span></div>
          <div v-for="p in ps.properties" :key="`${ps.name}/${p.name}`" class="row" data-bb="props-prop" :data-prop="p.name"><span class="k">{{ p.name }}</span><span class="mono">{{ p.value }}</span></div>
        </template>
      </template>
      <template v-if="linkedName !== null">
        <div class="sec">{{ t('m1.addNote') }}</div>
        <div class="row">
          <input v-model="noteText" class="inp" style="width:170px" data-bb="note-text" :placeholder="t('m1.notePlaceholder')" :aria-label="t('m1.notePlaceholder')" @keydown.enter="addNote" />
          <button type="button" class="btn" data-bb="note-add" :disabled="!noteText.trim()" @click="addNote">{{ t('m1.noteAdd') }}</button>
        </div>
        <p v-if="noteError" role="alert" class="error">{{ noteError }}</p>
      </template>
    </template>
    <template v-else>
    <div class="row">
      <label class="k" for="el-material">{{ t('props.material') }}</label>
      <select id="el-material" class="sel-inp" style="width:150px" :value="detail.material_name ?? ''" :disabled="readOnly !== null"
              :aria-invalid="materialError ? 'true' : 'false'" @change="commitMaterial">
        <option v-for="x in materials" :key="x.id" :value="x.name">{{ x.name }}</option>
      </select>
    </div>
    <p v-if="materialError" role="alert" class="error">{{ t('modify.error', { msg: materialError }) }}</p>
    <div class="row">
      <span class="k">{{ t('modify.phase') }}</span>
      <span class="sel-inp" style="width:150px" data-missing="element phase (no phase in the model)"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span>
    </div>
    <div class="sec">{{ t('el.sizes') }}</div>
    <template v-if="sizes.length">
      <div v-for="[k, v] in sizes" :key="k" class="row">
        <label class="k" :for="`el-${k}`">{{ dimLabel(k) }}</label>
        <span style="display:inline-flex;align-items:center;gap:6px">
          <input :id="`el-${k}`" :key="`${detail.id}:${k}:${v}`" class="inp mono" :value="grouped(v)" style="width:96px;text-align:right" :disabled="readOnly !== null"
                 :aria-invalid="sizeError ? 'true' : 'false'" @keydown.enter="commitSize(k, v, $event)" @blur="commitSize(k, v, $event)" />
          <span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">mm</span>
        </span>
      </div>
      <p v-if="sizeError" role="alert" class="error">{{ t('modify.error', { msg: sizeError }) }}</p>
    </template>
    <div class="row">
      <label class="k" for="el-origin-z">{{ t('modify.originZ') }}</label>
      <span style="display:inline-flex;align-items:center;gap:6px">
        <input id="el-origin-z" :key="`${detail.id}:${originZ}`" class="inp mono" :value="grouped(originZ)" style="width:96px;text-align:right" :disabled="readOnly !== null"
               :aria-invalid="zError ? 'true' : 'false'" @keydown.enter="commitZ" @blur="commitZ" />
        <span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">mm</span>
      </span>
    </div>
    <p v-if="zError" role="alert" class="error">{{ t('modify.error', { msg: zError }) }}</p>
    <!-- WAVE1B (B2): the openings of this host; one selected (here, or a click on it in a view) -->
    <template v-if="openings.length">
      <div class="sec">{{ t('props.op.title') }}</div>
      <button v-for="(o, i) in openings" :key="o.id" type="button" class="oprow" :class="{ sel: selOpening?.id === o.id }" data-bb="props-opening" :data-id="o.id"
              :aria-pressed="selOpening?.id === o.id" @click="selectedOpening = { host: detail.id, id: o.id }">{{ openingLabel(o, i) }}</button>
      <template v-if="selOpening">
        <div v-for="f in openingFields" :key="f.k" class="row">
          <label class="k" :for="`op-${f.k}`">{{ t(`props.op.${f.k}`) }}</label>
          <span style="display:inline-flex;align-items:center;gap:6px">
            <input :id="`op-${f.k}`" :key="`${selOpening.id}:${f.k}:${f.v}`" class="inp mono" :value="grouped(f.v)" style="width:96px;text-align:right" :data-bb="`props-opening-${f.k}`"
                   @keydown.enter="commitOpening(f.k, f.v, $event)" @blur="commitOpening(f.k, f.v, $event)" />
            <span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">mm</span>
          </span>
        </div>
        <div v-for="(ax, i) in (['x', 'y'] as const)" v-show="selOpening.kind !== 'other'" :key="ax" class="row">
          <label class="k" :for="`op-at-${ax}`">{{ t('props.op.at', { axis: ax.toUpperCase() }) }}</label>
          <span style="display:inline-flex;align-items:center;gap:6px">
            <input :id="`op-at-${ax}`" :key="`${selOpening.id}:${ax}:${selOpening.centre[i]}`" class="inp mono" :value="grouped(Math.round(selOpening.centre[i] ?? 0))" style="width:96px;text-align:right"
                   :data-bb="`props-opening-at-${ax}`" @keydown.enter="commitOpeningAt(i as 0 | 1, $event)" @blur="commitOpeningAt(i as 0 | 1, $event)" />
            <span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">mm</span>
          </span>
        </div>
        <p v-if="selOpening.kind === 'other'" class="note">{{ t('props.op.notEditable') }}</p>
        <p v-if="openingError" role="alert" class="error" data-bb="props-opening-error">{{ t('modify.error', { msg: openingError }) }}</p>
      </template>
    </template>
    <!-- end WAVE1B -->
    </template>
  </template>
  <template v-else-if="view && onPaper">
    <div class="pn">{{ t('view.props', { name: viewName(view) }) }}</div>
    <div class="row"><label class="k" for="view-scale">{{ t('view.scale') }}</label><select id="view-scale" class="sel-inp" style="width:100px" :value="view.scale" @change="patchView({ scale: Number(($event.target as HTMLSelectElement).value) })"><option v-for="n in scales" :key="n" :value="n">1:{{ n }}</option></select></div>
    <div class="row"><label class="k" for="view-north">{{ t('onpaper.orientation') }}</label><select id="view-north" class="sel-inp" style="width:150px" :value="view.true_north ? 'true' : 'project'" @change="patchView({ true_north: ($event.target as HTMLSelectElement).value === 'true' })"><option value="project">{{ t('onpaper.projectNorth') }}</option><option value="true">{{ t('onpaper.trueNorth') }}</option></select></div>
    <div class="row"><span class="k">{{ t('view.phase') }}</span><span class="sel-inp" style="width:150px" data-missing="view phase filter"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span></div>
    <div class="row"><label class="k" for="view-crop">{{ t('onpaper.crop') }}</label><select id="view-crop" class="sel-inp" style="width:110px" :value="view.crop ? 'on' : 'off'" data-missing="draw a crop rectangle (set_view takes one; no tool draws it)" @change="patchView({ crop: null })"><option value="on" :disabled="!view.crop">{{ t('onpaper.on') }}</option><option value="off">{{ t('onpaper.off') }}</option></select></div>
    <div class="sec">{{ t('onpaper.range') }}</div>
    <div v-for="p in planes" :key="p.k" class="row">
      <label class="k" :for="`range-${p.k}`">{{ t(`onpaper.${p.k}`) }}</label>
      <span style="display:inline-flex;gap:6px">
        <span class="sel-inp" style="width:78px" :data-missing="p.level ? 'set view range level (set_view takes offsets only)' : 'range plane storey name for above/below/unlimited (resolve in Rust)'"><span class="mono ell">{{ p.level }}</span><svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span>
        <input :id="`range-${p.k}`" class="inp mono" :value="grouped(p.offset)" style="width:70px;text-align:right" @keydown.enter="commitOffset(p.k, p.offset, $event)" @blur="commitOffset(p.k, p.offset, $event)">
      </span>
    </div>
    <p v-if="viewError" role="alert" class="error">{{ t('modify.error', { msg: viewError }) }}</p>
    <div class="sec">{{ t('onpaper.refs') }}</div>
    <div v-if="view.references" class="row" data-missing="reference names in query('views') (only a count today)"><span class="k">{{ t('nav.refs', { n: view.references }) }}</span></div>
    <div class="row"><span class="k">{{ t('onpaper.display') }}</span><span class="sel-inp" style="width:110px" data-missing="reference display style (no field in View)"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span></div>
  </template>
  <template v-else-if="view">
    <div class="pn">{{ t('view.props', { name: viewName(view) }) }}</div>
    <!-- SEC1: name, scale, a section's cut, the crop, a plan's range, what the view hides (one verb each) -->
    <div class="row"><label class="k" for="view-name">{{ t('sec1.name') }}</label><input id="view-name" :key="`${view.id}:${view.name}`" class="inp" :value="view.name" style="width:150px" :disabled="!canEdit" data-bb="view-name" @keydown.enter="commitName" @blur="commitName"></div>
    <div class="row"><span class="k">{{ t('view.type') }}</span><span>{{ viewType }}</span></div>
    <div class="row"><label class="k" for="view-scale3">{{ t('view.scale') }}</label><select id="view-scale3" class="sel-inp" style="width:100px" :value="view.scale" :disabled="!canEdit" @change="patchView({ scale: Number(($event.target as HTMLSelectElement).value) })"><option v-for="n in scales" :key="n" :value="n">1:{{ n }}</option></select></div>
    <template v-if="view.type === 'section'">
      <div class="row"><label class="k" for="view-facing">{{ t('sec1.looking') }}</label><select id="view-facing" class="sel-inp" style="width:100px" :value="view.facing" :disabled="!canEdit" data-bb="view-facing" @change="edit({ facing: ($event.target as HTMLSelectElement).value })"><option v-for="f in ['north', 'east', 'south', 'west']" :key="f" :value="f">{{ t(`sec1.${f}`) }}</option></select></div>
      <div class="row"><span class="k">{{ t('sec1.through') }}</span><span style="display:inline-flex;gap:4px">
        <input :key="`ox${origin[0]}`" class="inp mono" :value="grouped(origin[0] ?? 0)" style="width:76px;text-align:right" :disabled="!canEdit" aria-label="X" data-bb="view-origin-x" @keydown.enter="commitOrigin(0, $event)" @blur="commitOrigin(0, $event)">
        <input :key="`oy${origin[1]}`" class="inp mono" :value="grouped(origin[1] ?? 0)" style="width:76px;text-align:right" :disabled="!canEdit" aria-label="Y" data-bb="view-origin-y" @keydown.enter="commitOrigin(1, $event)" @blur="commitOrigin(1, $event)"></span></div>
      <div class="row"><label class="k" for="view-depth">{{ t('sec1.depth') }}</label><input id="view-depth" :key="`d${view.depth_mm}`" class="inp mono" :value="grouped(Number(view.depth_mm ?? 0))" style="width:96px;text-align:right" :disabled="!canEdit" data-bb="view-depth" @keydown.enter="commitDepth" @blur="commitDepth"></div>
    </template>
    <template v-if="view.type !== 'three_d'">
      <div class="row"><label class="k" for="view-crop3">{{ t('sec1.crop') }}</label><input id="view-crop3" type="checkbox" :checked="crop !== null" :disabled="!canEdit" data-bb="view-crop" @change="toggleCrop(($event.target as HTMLInputElement).checked)"></div>
      <template v-if="crop">
        <div v-for="(lab, k) in ['sec1.cropX', 'sec1.cropY']" :key="lab" class="row"><span class="k">{{ t(lab) }}</span><span style="display:inline-flex;gap:4px">
          <input :key="`c${k}${crop.min[k]}`" class="inp mono" :value="((crop.min[k] ?? 0) / 1000).toFixed(2)" style="width:70px;text-align:right" :disabled="!canEdit" @keydown.enter="commitCrop('min', k as 0 | 1, $event)" @blur="commitCrop('min', k as 0 | 1, $event)">
          <input :key="`C${k}${crop.max[k]}`" class="inp mono" :value="((crop.max[k] ?? 0) / 1000).toFixed(2)" style="width:70px;text-align:right" :disabled="!canEdit" @keydown.enter="commitCrop('max', k as 0 | 1, $event)" @blur="commitCrop('max', k as 0 | 1, $event)"></span></div>
      </template>
    </template>
    <template v-if="view.type === 'plan'">
      <div class="sec">{{ t('sec1.range') }}</div>
      <div v-for="p in rangePlanes" :key="p.k" class="row">
        <span class="k">{{ t(`sec1.${p.k === 'depth' ? 'viewDepth' : p.k}`) }}</span>
        <span style="display:inline-flex;gap:4px">
          <select class="sel-inp" style="width:120px" :value="p.level" :disabled="!canEdit" :aria-label="t(`sec1.${p.k === 'depth' ? 'viewDepth' : p.k}`)" @change="edit({ levels: { [p.k]: ($event.target as HTMLSelectElement).value } })"><option v-for="l in LEVELS" :key="l" :value="l">{{ t(`sec1.${l}`) }}</option></select>
          <input :key="`${p.k}${p.offset}`" class="inp mono" :value="grouped(p.offset)" style="width:64px;text-align:right" :disabled="!canEdit || p.level === 'unlimited'" @keydown.enter="commitOffset(p.k, p.offset, $event)" @blur="commitOffset(p.k, p.offset, $event)">
        </span>
      </div>
    </template>
    <p v-if="secError || viewError" role="alert" class="error">{{ t('modify.error', { msg: secError ?? viewError ?? '' }) }}</p>
    <div class="row">
      <span class="k">{{ t('view.phase') }}</span>
      <span class="sel-inp" style="width:150px" data-missing="view phase filter"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span>
    </div>
    <div class="row">
      <span class="k">{{ t('view.subprojects') }}</span>
      <span class="sel-inp" style="width:160px" data-missing="view sub-projects filter"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span>
    </div>
    <div class="row"><span class="k">{{ t('view.detail') }}</span><span data-missing="view detail level" /></div>
    <div class="row"><span class="k">{{ t('view.hidden') }}</span><span class="mono ell" style="max-width:170px" data-bb="view-hidden">{{ hiddenText }}</span></div>
    <div class="sec">{{ t('view.families') }}</div>
    <label v-for="f in families" :key="f.key" class="row" style="padding-left:0px">
      <span style="display:inline-flex;gap:8px;align-items:center">
        <input type="checkbox" :checked="familyShown(f.key)" :disabled="!canEdit" @change="toggleFamily(f.key, ($event.target as HTMLInputElement).checked)"> <span :style="`width:14px;height:14px;border:1px solid var(--bb-ink);background:${f.swatch};display:inline-block`" />{{ t(`fam.${f.key}`) }}</span>
      <span class="mono" style="font-size:12px;color:var(--bb-stone)">{{ grouped(f.count) }}</span>
    </label>
    <div class="sec">{{ t('view.classes') }}</div>
    <label v-for="[c, n] in classes" :key="c" class="row" style="padding-left:0px">
      <span style="display:inline-flex;gap:8px;align-items:center"><input type="checkbox" :checked="!vvis?.hidden_classes.includes(c)" :disabled="!canEdit" :data-class="c" @change="toggleClass(c, ($event.target as HTMLInputElement).checked)">{{ c }}</span>
      <span class="mono" style="font-size:12px;color:var(--bb-stone)">{{ grouped(n) }}</span>
    </label>
    <!-- end SEC1 -->
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
.ell { overflow: hidden; text-overflow: ellipsis; }
/* .sec{font-size:12px;font-family:'DM Mono',monospace;letter-spacing:.08em;text-transform:uppercase;color:#5C574D;margin-top:6px} */
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
/* .dot{width:12px;height:12px;border-radius:50%;border:1px solid #141414;display:inline-block;flex:none} */
.dot { width: 12px; height: 12px; border-radius: 50%; border: 1px solid var(--bb-ink); display: inline-block; flex: none; }
/* The read-only line: ink text, a disc with a 1px ink outline (slate for a linked model, sunk for a file). */
.ro { display: flex; align-items: center; gap: 8px; margin: 0 0 4px; font-size: 13.5px; color: var(--bb-ink); }
.ro .dot { background: var(--bb-sunk); }
.ro .dot.linked { background: var(--bb-slate); }
.inp:disabled, .sel-inp:disabled { color: var(--bb-stone); background: var(--bb-sunk); cursor: default; }
/* Error: ink text on an ochre band, with the word "Error". */
/* WAVE1B (B2): an opening's row; the selected one in the selection colour (ADR 0049: pink is selection) */
.note { font-size: 13px; color: var(--bb-stone); margin: 0; }
/* M1LINKS: the note's Add button */
.btn { height: 30px; border: 1px solid var(--bb-ink); border-radius: 999px; background: var(--bb-sheet); padding: 0 12px; font: inherit; font-size: 13px; color: var(--bb-ink); cursor: pointer; }
.btn:disabled { color: var(--bb-stone); border-color: var(--bb-stone); cursor: default; }
.btn:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
.oprow { display: block; width: 100%; text-align: left; background: none; border: 0; border-radius: 8px; padding: 2px 6px; font: inherit; font-size: 13.5px; color: var(--bb-ink); cursor: pointer; }
.oprow:hover { background: var(--bb-sunk); }
.oprow.sel { background: var(--bb-selection); }
.oprow:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
