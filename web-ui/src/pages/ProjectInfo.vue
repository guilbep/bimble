<script lang="ts">
import ProjectInfoSide from "./ProjectInfoSide.vue";
export const hint = "projectinfo.hint";
export const side = ProjectInfoSide;
export const capability = "project_info", editable = true; // FAKE1: query project refuses a read-only file
</script>

<script setup lang="ts">
import { stateRev } from "../bimble"; // VIS2
// A3 Project information (wireframes v01 ProjectInfo.html; ADR 0030, 0052). Leaving a field or Enter
// (the native `change` event) sends one SetProject with the whole map; undo restores it.
import { computed, nextTick, ref } from "vue";
import { project, state, setProject, setProjectField, northInfo, coordinates } from "../bimble";
import { georefSet, type GeorefPatch } from "../bimble"; // WAVE1B (A5)
import { t } from "../i18n";
import { currentDialog } from "../dialogs";
import NotAvailable from "../shell/NotAvailable.vue"; // FAKE1
import { has } from "../shell/capability"; // FAKE1

// The reference's "Basic data" rows, by Rust key (bimble-core project::KNOWN).
const BASIC = ["project_number", "project_title", "project_address", "project_city", "phase", "phase_description"] as const;
const info = computed(() => project.value?.info ?? {});
// end_date (as the reference), then every other key the project holds.
const other = computed(() => ["end_date", ...Object.keys(info.value).filter((k) => k !== "end_date" && !(BASIC as readonly string[]).includes(k))]);
const label = (k: string) => (k === "end_date" || (BASIC as readonly string[]).includes(k) ? t(`projectinfo.${k}` as "projectinfo.end_date") : k.replace(/_/g, " "));
const error = ref<string | null>(null);
// ---- G2: the georeference's system and base point (query north, coordinates of the world origin) ----
const georefText = computed(() => {
  void stateRev.value;
  const n = northInfo.value, c = n ? coordinates([0, 0, 0]) : null;
  return n && c ? `${n.crs ?? "—"} · ${c.text} · ${n.rotation_deg}°` : "—";
});
// ---- end G2 ----
// ---- WAVE1B (A5): the georeference's fields here too (verb georef_set, one undoable step each), as
// the Setting out panel edits them: system, the base point's east, north and height, the angles ----
type GNum = "east_m" | "north_m" | "height_m" | "rotation_deg" | "map_rotation_deg";
const geoVer = ref(0);
const geoError = ref<string | null>(null);
const geoFields = computed((): { k: GNum; unit: string; v: number | null }[] => {
  void stateRev.value; void geoVer.value;
  const n = northInfo.value, b = n ? coordinates([0, 0, 0])?.survey ?? null : null;
  return [
    { k: "east_m", unit: "m", v: b ? b[0] : null }, { k: "north_m", unit: "m", v: b ? b[1] : null }, { k: "height_m", unit: "m", v: b ? b[2] : null },
    { k: "rotation_deg", unit: "°", v: n ? n.rotation_deg : null }, { k: "map_rotation_deg", unit: "°", v: n ? n.map_rotation_deg : null },
  ];
});
const geoShow = (v: number | null) => (v === null ? "" : String(Math.round(v * 1e6) / 1e6));
const geoApply = (patch: GeorefPatch) => { geoError.value = georefSet(patch); geoVer.value++; };
function geoNum(k: GNum, e: Event) {
  const txt = (e.target as HTMLInputElement).value.trim().replace(",", ".");
  const v = Number(txt);
  if (txt === "" || !Number.isFinite(v)) { geoError.value = t("settingout.badNumber"); geoVer.value++; return; }
  geoApply({ [k]: v });
}
const GEO_LABEL: Record<GNum, Parameters<typeof t>[0]> = { east_m: "settingout.east", north_m: "settingout.north", height_m: "settingout.elevation", rotation_deg: "settingout.angle", map_rotation_deg: "settingout.mapAngle" };
// ---- end WAVE1B ----

function commit(key: string, e: Event) {
  const v = (e.target as HTMLInputElement).value;
  const next = { ...info.value };
  // ponytail: an emptied entry is removed, as the egui form does (ADR 0030).
  if (v.trim() === "") delete next[key];
  else next[key] = v;
  error.value = setProject(next);
}

// "Add field": a label and a value; Add (or Enter in either field) sends one `set_project_field` (the
// key is made in Rust). DEAD1: the row has its own Add and Cancel buttons and says why Add is off
// (no name yet), and the core's refusal shows in the row; "Add field" again goes back to the name.
const adding = ref(false);
const newLabel = ref("");
const newValue = ref("");
const addError = ref<string | null>(null);
const labelInput = ref<HTMLInputElement | null>(null);
async function startAdding() {
  adding.value = true;
  await nextTick();
  labelInput.value?.focus();
}
function addField() {
  if (newLabel.value.trim() === "") { addError.value = null; return; }
  addError.value = setProjectField(newLabel.value, newValue.value);
  if (addError.value) return;
  adding.value = false;
  newLabel.value = newValue.value = "";
}
function cancelAdd() {
  adding.value = false;
  addError.value = null;
  newLabel.value = newValue.value = "";
}
</script>

<template>
  <div class="pi" data-bb="project-info">
    <section class="col" style="gap:9px">
      <h2 style="font-size:20px" data-bb="project-basic">{{ t('projectinfo.basic') }}</h2>
      <p class="note" style="margin:0 0 4px">{{ t('projectinfo.basicNote') }}</p>
      <p v-if="error" class="banner b-o" role="alert" data-bb="project-error">{{ t('projectinfo.error', { msg: error }) }}</p>
      <div v-for="k in BASIC" :key="k" class="fld">
        <label class="lbl" :for="`pi-${k}`">{{ label(k) }}</label>
        <input :id="`pi-${k}`" class="inp" :value="info[k] ?? ''" :data-bb="`project-field-${k}`" @change="commit(k, $event)">
        <span class="mono key">{{ '{' + k + '}' }}</span>
      </div>
      <h2 style="font-size:20px;margin-top:14px">{{ t('projectinfo.ifc') }}</h2>
      <div class="ifc">
        <span class="k">IfcProject</span><span class="mono" data-bb="project-id">{{ project?.id ?? '' }}</span>
        <span class="k">IfcSite</span><span class="mono" data-bb="project-site-id">{{ project?.site_id ?? '' }}</span>
        <span class="k">IfcBuilding</span><span class="mono" data-bb="project-building-id">{{ project?.building_id ?? '' }}</span>
        <span class="k">{{ t('projectinfo.schema') }}</span><span class="mono">{{ state.schema ?? '' }}</span>
        <!-- G2: the georeference (IfcMapConversion on export), edited in the Setting out tab's panel -->
        <span class="k">IfcMapConversion</span><span class="mono" data-bb="project-georef">{{ georefText }}</span>
        <!-- end G2 -->
      </div>
      <!-- WAVE1B (A5): the georeference, editable here as in the Setting out panel -->
      <h2 style="font-size:20px;margin-top:14px" data-bb="project-georef-title">{{ t('projectinfo.georef') }}</h2>
      <p v-if="geoError" class="banner b-o" role="alert" data-bb="project-georef-error">{{ t('projectinfo.error', { msg: geoError }) }}</p>
      <div :key="geoVer" class="ifc">
        <label class="k" for="pi-geo-crs">{{ t('settingout.system') }}</label>
        <input id="pi-geo-crs" class="inp mono" :value="northInfo?.crs ?? ''" placeholder="EPSG:2056" data-bb="project-georef-crs" @change="geoApply({ crs: ($event.target as HTMLInputElement).value.trim() })">
        <template v-for="f in geoFields" :key="f.k">
          <label class="k" :for="`pi-geo-${f.k}`">{{ t(GEO_LABEL[f.k]) }} ({{ f.unit }})</label>
          <input :id="`pi-geo-${f.k}`" class="inp mono" style="text-align:right" :value="geoShow(f.v)" :data-bb="`project-georef-${f.k}`" @change="geoNum(f.k, $event)">
        </template>
      </div>
    </section>
    <section class="col" style="gap:10px">
      <div style="display:flex;align-items:center">
        <h2 style="font-size:20px">{{ t('projectinfo.team') }}</h2>
        <span style="flex:1" />
        <button v-if="has('users')" class="btn s" type="button" data-bb="project-add-member" @click="currentDialog = 'team-member'">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg>{{ t('projectinfo.addMember') }}
        </button>
      </div>
      <!-- No Rust source for team members yet: header only, no rows. -->
      <NotAvailable v-if="!has('users')" compact :what="t('na.screen.team')" /><!-- FAKE1 -->
      <table v-else class="t" data-bb="project-team" data-missing="query: team members (ADR 0055)">
        <thead><tr><th>{{ t('projectinfo.role') }}</th><th>{{ t('projectinfo.company') }}</th><th>{{ t('projectinfo.contact') }}</th><th>{{ t('projectinfo.mine') }}</th><th /></tr></thead>
        <tbody />
      </table>
      <p class="note" style="margin:0">{{ t('projectinfo.mineNote') }}</p>
      <h2 style="font-size:20px;margin-top:10px">{{ t('projectinfo.other') }}</h2>
      <div v-for="k in other" :key="k" class="fld">
        <label class="lbl" :for="`pi-${k}`">{{ label(k) }}</label>
        <input :id="`pi-${k}`" class="inp" :value="info[k] ?? ''" :data-bb="`project-field-${k}`" @change="commit(k, $event)">
        <span class="mono key">{{ '{' + k + '}' }}</span>
      </div>
      <form v-if="adding" class="fld newf" data-bb="project-new-field" @submit.prevent="addField" @keydown.esc.prevent="cancelAdd">
        <input ref="labelInput" v-model="newLabel" class="inp" :aria-label="t('projectinfo.newLabel')" :placeholder="t('projectinfo.newLabel')" data-bb="project-new-label">
        <input v-model="newValue" class="inp" :aria-label="t('projectinfo.newValue')" :placeholder="t('projectinfo.newValue')" data-bb="project-new-value">
        <button type="submit" class="btn s" :disabled="newLabel.trim() === ''" :title="newLabel.trim() === '' ? t('projectinfo.needLabel') : undefined" data-bb="project-new-add">{{ t('projectinfo.addIt') }}</button>
        <button type="button" class="btn s" data-bb="project-new-cancel" @click="cancelAdd">{{ t('projectinfo.cancelAdd') }}</button>
        <p v-if="addError" class="err" role="alert" data-bb="project-new-error">{{ t('projectinfo.error', { msg: addError }) }}</p>
      </form>
      <button type="button" class="qb add" data-bb="project-add-field" @click="startAdding">
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg> {{ t('projectinfo.addField') }}
      </button>
    </section>
  </div>
</template>

<style scoped>
/* .page style="display:grid;grid-template-columns:minmax(0,1.15fr) minmax(0,1fr);gap:28px;align-content:start" (padding from .bb-page) */
.fld.newf { display: flex; flex-wrap: wrap; gap: 8px; }
.fld.newf .inp { flex: 1 1 150px; }
.newf .err { flex-basis: 100%; margin: 0; color: var(--bb-red, #b42318); font-size: 12px; }
.pi { display: grid; grid-template-columns: minmax(0, 1.15fr) minmax(0, 1fr); gap: 28px; align-content: start; }
.col { display: flex; flex-direction: column; }
/* .page h2{font-family:'Sora',sans-serif;margin:0} */
h2 { font-family: 'Sora', sans-serif; margin: 0; }
.mono { font-family: 'DM Mono', ui-monospace, Menlo, monospace; }
/* .note{font-size:13px;color:#5C574D} */
.note { font-size: 13px; color: var(--bb-stone); }
/* .banner{padding:10px 16px;font-size:14px;...} .b-o{background:#D2B347} — the error band: ochre with the word "Error". */
.banner { margin: 0; padding: 10px 16px; font-size: 14px; display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }
.b-o { background: var(--bb-error); color: var(--bb-ink); }
/* .fld{display:flex;flex-direction:column;gap:3px} + style="display:grid;grid-template-columns:180px minmax(0,1fr) 150px;gap:10px;align-items:center" */
.fld { display: grid; grid-template-columns: 180px minmax(0, 1fr) 150px; gap: 10px; align-items: center; }
/* .lbl{font-size:13px;color:#5C574D} + style="font-size:13.5px;color:#141414" */
.lbl { font-size: 13.5px; color: var(--bb-ink); }
/* .inp{height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 8px;font:inherit;font-size:13.5px;color:#141414;min-width:0} */
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
.inp:focus-visible, button:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
.key { font-size: 12px; color: var(--bb-stone); }
/* style="display:grid;grid-template-columns:180px minmax(0,1fr);gap:8px 10px;font-size:13.5px" */
.ifc { display: grid; grid-template-columns: 180px minmax(0, 1fr); gap: 8px 10px; font-size: 13.5px; }
.ifc .k { color: var(--bb-stone); }
.ifc .mono { font-size: 12.5px; }
/* .btn{...} .btn.s{background:#FFFFFF;border:1px solid #141414;font-weight:400} */
/* .qb{display:inline-flex;align-items:center;gap:4px;height:32px;padding:0 12px;border-radius:999px;border:0;background:none;color:#141414;font:inherit;cursor:pointer;...} style="align-self:flex-start;color:#A35A37" */
.qb { display: inline-flex; align-items: center; gap: 4px; height: 32px; padding: 0 12px; border-radius: var(--bb-radius-pill); border: 0; background: none; font: inherit; cursor: pointer; white-space: nowrap; }
.add { align-self: flex-start; color: var(--bb-clay-text); }
/* Missing in Rust: the reference's .off (color:#5C574D;opacity:.55). */
.off, button:disabled { color: var(--bb-stone); opacity: .55; cursor: default; }
/* table.t ... */
table.t { border-collapse: collapse; width: 100%; font-size: 13.5px; }
table.t th { text-align: left; font-weight: 600; color: var(--bb-stone); border-bottom: 1px solid var(--bb-stone); padding: 6px 8px; white-space: nowrap; font-size: 13px; }
</style>
