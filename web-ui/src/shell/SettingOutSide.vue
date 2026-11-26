<script setup lang="ts">
// Setting out tab's left panel (fr/Implantation.html aside.left > .props). Ticket G2: the project
// base point is the model's georeference (ADR 0045, ccore/src/georef.c): read with `query north`
// (system, turns) and `query coordinates` (the survey coordinates of the world origin: east, north,
// height), changed with the verb `georef_set` (one undoable step per field, the verb's error shown).
// Orientation = the active view's true_north. The selection's survey coordinates through
// `coordinates`; the pointer's readout in the status bar is ticket KEY1's.
// SETOUT (ADR 0075): the survey point (`query setting_out`, verb georef_set {survey_point_mm}: X,
// Y, Z in world mm, its survey coordinates read back; at the base point until placed). "Move the
// base point" is a capability the core does not serve (base_point_move): drawn "not yet".
import { computed, ref } from "vue";
import { coordinates, editableNow, element, georefSet, northInfo, openViews, setView, settingOut, state, surveyPointSet, type GeorefPatch } from "../bimble";
import { placing, referencePlaneAdd, referencePlaneDelete, referencePlaneRename, refPlanes, storeys, workPlaneSet } from "../bimble"; // WAVE1B
import { t } from "../i18n";
import { has } from "./capability";

const view = computed(() => openViews.value.views.find((v) => v.id === openViews.value.active) ?? null);
const orient = (e: Event) => view.value && setView(view.value.id, { true_north: (e.target as HTMLSelectElement).value === "true" });
const editable = computed(() => { void state.value.rev; void state.value.file; return editableNow(); });
/** The survey coordinates of the world origin = the georeference's east, north, height. */
const base = computed(() => { void state.value.rev; void state.value.file; return editable.value ? coordinates([0, 0, 0]) : null; });
const error = ref<string | null>(null);
const ver = ref(0);
type Num = "east_m" | "north_m" | "height_m" | "rotation_deg" | "map_rotation_deg";
const fields = computed((): { k: Num; label: Parameters<typeof t>[0]; unit: string; v: number | null }[] => {
  const n = northInfo.value, b = base.value?.survey ?? null;
  return [
    { k: "east_m", label: "settingout.east", unit: "m", v: b ? b[0] : null },
    { k: "north_m", label: "settingout.north", unit: "m", v: b ? b[1] : null },
    { k: "height_m", label: "settingout.elevation", unit: "m", v: b ? b[2] : null },
    { k: "rotation_deg", label: "settingout.angle", unit: "°", v: n ? n.rotation_deg : null },
    { k: "map_rotation_deg", label: "settingout.mapAngle", unit: "°", v: n ? n.map_rotation_deg : null },
  ];
});
const show = (v: number | null) => (v === null ? "" : String(Math.round(v * 1e6) / 1e6));
const apply = (patch: GeorefPatch) => { error.value = georefSet(patch); ver.value++; };
const setNum = (k: Num, e: Event) => {
  const s = (e.target as HTMLInputElement).value.trim().replace(",", ".");
  const v = Number(s);
  if (s === "" || !Number.isFinite(v)) { error.value = t("settingout.badNumber"); ver.value++; return; }
  apply({ [k]: v });
};
const setCrs = (e: Event) => apply({ crs: (e.target as HTMLInputElement).value.trim() });
// ---- SETOUT: the survey point ----
const sp = computed(() => settingOut.value?.survey_point ?? null);
const AXES = ["X", "Y", "Z"] as const;
const spShow = (i: number) => (sp.value ? String(Math.round(sp.value.at_mm[i]! * 1000) / 1000) : "");
const setSp = (i: number, e: Event) => {
  const txt = (e.target as HTMLInputElement).value.trim().replace(",", ".");
  const v = Number(txt);
  if (!sp.value || txt === "" || !Number.isFinite(v)) { error.value = t("settingout.badNumber"); ver.value++; return; }
  const at = [...sp.value.at_mm] as [number, number, number];
  at[i] = v;
  error.value = surveyPointSet(at);
  ver.value++;
};
const spText = computed(() => {
  const s = sp.value?.survey_m;
  return s ? `E ${s[0].toFixed(3)} N ${s[1].toFixed(3)} · ${s[2].toFixed(3)} m` : "—";
});
const moveServed = computed(() => has("base_point_move"));
// ---- end SETOUT ----
// ---- WAVE1B: reference planes (ADR 0075, ccore/session/refplanes.h): the work plane chosen among the
// stored planes and the implicit ones (levels, grid axes); stored planes renamed and deleted (verbs
// rename, delete); a new one by origin + normal (verb reference_plane_add); by three points through
// the ribbon's Reference planes tool. ----
const rpServed = computed(() => has("reference_planes"));
const rp = computed(() => refPlanes.value);
const wpValue = computed(() => (rp.value?.active ? `${rp.value.active.kind}:${rp.value.active.id}` : ""));
const levels = computed(() => (rp.value?.implicit ?? []).filter((x) => x.kind === "level").sort((a, b) => (a.kind === "level" && b.kind === "level" ? a.elevation - b.elevation : 0)));
const axes = computed(() => (rp.value?.implicit ?? []).filter((x) => x.kind === "axis"));
const setWork = (e: Event) => {
  const v = (e.target as HTMLSelectElement).value, i = v.indexOf(":");
  error.value = i < 0 ? workPlaneSet(null) : workPlaneSet(v.slice(0, i) as "plane" | "level" | "axis", v.slice(i + 1));
  ver.value++;
};
const rename = (id: string, e: Event) => { error.value = referencePlaneRename(id, (e.target as HTMLInputElement).value.trim()); ver.value++; };
const ORIGIN = ref<[number, number, number]>([0, 0, 0]);
const NORMAL = ref<[number, number, number]>([0, 0, 1]);
const setVec = (which: "o" | "n", i: number, e: Event) => {
  const v = which === "o" ? ORIGIN : NORMAL;
  const x = Number((e.target as HTMLInputElement).value.trim().replace(",", "."));
  if (!Number.isFinite(x)) { error.value = t("settingout.badNumber"); ver.value++; return; }
  const a = [...v.value] as [number, number, number];
  a[i] = x;
  v.value = a;
};
const addPlane = () => {
  const storey = placing.value?.storey ?? storeys.value[0]?.id;
  if (!storey) return;
  error.value = referencePlaneAdd({ storey, origin: ORIGIN.value, normal: NORMAL.value });
  ver.value++;
};
// ---- end WAVE1B ----
/** The first selected element's origin in the survey system (the status bar's text). */
const selText = computed(() => {
  void northInfo.value;
  const id = state.value.selection[0];
  if (!id || !editable.value) return null;
  try { return coordinates(element(id).placement.origin as [number, number, number])?.text ?? null; } catch { return null; }
});
</script>

<template>
  <div class="pn">{{ t("settingout.title") }}</div>
  <template v-if="editable">
    <div :key="ver" style="display:contents">
      <div class="row">
        <label class="k" for="so-crs">{{ t("settingout.system") }}</label>
        <input id="so-crs" class="inp mono" style="width:140px" :value="northInfo?.crs ?? ''" placeholder="EPSG:2056" data-bb="settingout-crs" @change="setCrs">
      </div>
      <div v-for="f in fields" :key="f.k" class="row">
        <label class="k" :for="`so-${f.k}`">{{ t(f.label) }}</label>
        <span style="display:inline-flex;align-items:center;gap:6px"><input :id="`so-${f.k}`" class="inp mono" :value="show(f.v)" :data-bb="`settingout-${f.k}`" style="width:140px;text-align:right" @change="setNum(f.k, $event)"><span class="mono" style="font-size:12px;color:var(--bb-stone);width:22px">{{ f.unit }}</span></span>
      </div>
    </div>
    <p v-if="error" class="err" role="alert" data-bb="settingout-error">{{ error }}</p>
    <div class="row"><span class="k">{{ t("settingout.basePoint") }}</span><span class="mono small" data-bb="settingout-base">{{ base?.text ?? "—" }}</span></div>
    <!-- SETOUT: the survey point -->
    <div class="sec">{{ t("settingout.survey") }}</div>
    <div :key="`sp${ver}`" style="display:contents">
      <div v-for="(a, i) in AXES" :key="a" class="row">
        <label class="k" :for="`so-sp-${a}`">{{ t("settingout.surveyAt", { axis: a }) }}</label>
        <input :id="`so-sp-${a}`" class="inp mono" style="width:140px;text-align:right" :value="spShow(i)" :data-bb="`settingout-survey-${a}`" :disabled="!sp" @change="setSp(i, $event)">
      </div>
    </div>
    <div class="row"><span class="k">{{ t("settingout.surveyCoords") }}</span><span class="mono small" data-bb="settingout-survey-coords">{{ spText }}</span></div>
    <div class="row">
      <span v-if="sp && !sp.set" class="note">{{ t("settingout.surveyAtBase") }}</span>
      <button v-else type="button" class="btn s" data-bb="settingout-survey-remove" :disabled="!sp?.set" @click="error = surveyPointSet(null); ver++">{{ t("settingout.surveyRemove") }}</button>
    </div>
    <p class="note">{{ t("settingout.markers") }}</p>
    <!-- WAVE1B: reference planes and the work plane -->
    <template v-if="rpServed && rp">
      <div class="sec">{{ t("settingout.rp.title") }}</div>
      <div class="row"><label class="k" for="so-wp">{{ t("settingout.rp.work") }}</label>
        <select id="so-wp" class="sel-inp" style="width:170px" data-bb="refplane-work" :value="wpValue" @change="setWork">
          <option value="">{{ t("settingout.rp.storeyLevel") }}</option>
          <optgroup :label="t('settingout.rp.stored')"><option v-for="p in rp.planes" :key="p.id" :value="`plane:${p.id}`">{{ p.name }}</option></optgroup>
          <optgroup :label="t('settingout.rp.levels')"><option v-for="l in levels" :key="l.id" :value="`level:${l.id}`">{{ t("settingout.rp.level", { name: l.name }) }}</option></optgroup>
          <optgroup v-if="axes.length" :label="t('settingout.rp.axes')"><option v-for="a in axes" :key="a.id" :value="`axis:${a.id}`">{{ t("settingout.rp.axis", { name: a.name }) }}</option></optgroup>
        </select></div>
      <div :key="`rp${ver}`" style="display:contents">
        <p v-if="!rp.planes.length" class="note">{{ t("settingout.rp.none") }}</p>
        <div v-for="(p, i) in rp.planes" :key="p.id" class="row" data-bb="refplane-row" :data-id="p.id">
          <input class="inp" style="width:150px" :value="p.name" :aria-label="t('settingout.rp.name', { n: i + 1 })" data-bb="refplane-name" @change="rename(p.id, $event)">
          <button type="button" class="btn s" data-bb="refplane-delete" @click="error = referencePlaneDelete(p.id); ver++">{{ t("settingout.rp.delete") }}</button>
        </div>
        <div v-for="(a, i) in (['X', 'Y', 'Z'] as const)" :key="`o${a}`" class="row">
          <label class="k" :for="`so-rpo-${a}`">{{ t("settingout.rp.origin", { axis: a }) }}</label>
          <input :id="`so-rpo-${a}`" class="inp mono" style="width:90px;text-align:right" :value="ORIGIN[i]" :data-bb="`refplane-origin-${a}`" @change="setVec('o', i, $event)">
        </div>
        <div v-for="(a, i) in (['X', 'Y', 'Z'] as const)" :key="`n${a}`" class="row">
          <label class="k" :for="`so-rpn-${a}`">{{ t("settingout.rp.normal", { axis: a }) }}</label>
          <input :id="`so-rpn-${a}`" class="inp mono" style="width:90px;text-align:right" :value="NORMAL[i]" :data-bb="`refplane-normal-${a}`" @change="setVec('n', i, $event)">
        </div>
      </div>
      <button type="button" class="btn s" style="align-self:flex-start" data-bb="refplane-add" @click="addPlane">{{ t("settingout.rp.add") }}</button>
      <p class="note" data-bb="refplane-pick">{{ rp.pick.on ? t("settingout.rp.picking", { n: rp.pick.points.length }) : t("settingout.rp.pick") }}</p>
    </template>
    <!-- end WAVE1B -->
  </template>
  <p v-else class="note" data-bb="settingout-readonly">{{ t("settingout.noModel") }}</p>
  <div class="row"><span class="k">{{ t("settingout.readFrom") }}</span><span data-missing="where the georeference was read from (IfcMapConversion of an imported IFC)">—</span></div>
  <div class="sec">{{ t("settingout.view") }}</div>
  <div class="row"><label class="k" for="so-orient">{{ t("settingout.orientation") }}</label>
    <select id="so-orient" class="sel-inp" style="width:150px" data-bb="settingout-orientation" :disabled="!view" :value="String(view?.true_north ?? false)" @change="orient">
      <option value="false">{{ t("settingout.projectNorth") }}</option><option value="true">{{ t("settingout.trueNorth") }}</option>
    </select></div>
  <div class="row"><span class="k">{{ t("settingout.shown") }}</span><span class="mono small" data-bb="settingout-selection">{{ selText ?? "—" }}</span></div>
  <!-- SETOUT: not served by the core (capability base_point_move): "not yet", with the tooltip -->
  <button type="button" class="btn s" :class="{ ny: !moveServed }" style="align-self:flex-start;margin-top:4px" :disabled="!moveServed"
          :data-not-yet="moveServed ? undefined : ''" :title="moveServed ? undefined : t('na.tooltip', { name: t('settingout.move') })"
          data-bb="settingout-move">{{ t("settingout.move") }}</button>
</template>

<style scoped>
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.k { color: var(--bb-stone); white-space: nowrap; }
.mono { font-family: 'DM Mono', monospace; }
.small { font-size: 11.5px; text-align: right; }
.note { font-size: 13px; color: var(--bb-stone); margin: 0; }
.err { color: var(--bb-ink); border-left: 3px solid var(--bb-attention); padding-left: 6px; font-size: 13px; margin: 0; }
.btn.ny { border-style: dashed; cursor: help; }
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
</style>
