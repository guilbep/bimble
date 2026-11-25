<script lang="ts">
import LevelsGridsSide from "./LevelsGridsSide.vue";
export const hint = "levelsgrids.hint";
export const side = LevelsGridsSide;
</script>

<script setup lang="ts">
// A7 Levels and grids (wireframes v01 LevelsGrids.html; ADR 0056). Ticket G2: the rows are the real
// storeys (`storeys` query, elevation then id as the C core and Rust sort them; drawn highest first
// as the reference) and every edit is a verb (level_add, level_set, level_remove, grid_add,
// grid_axis_set, grid_remove: one undoable step each, the verb's error shown as it says it). A
// read-only file (an IFC, a viewer file: `state.editable` false) lists its levels and grid axes
// from `grid_axes` without edit controls. Altitude = the survey height of the level (`coordinates`).
// Not in the C core: adding or removing one axis of a grid (no verb), a level's plan view link.
import { computed, ref } from "vue";
import {
  coordinates, editableNow, gridAdd, gridAxes, gridAxisSet, gridRemove, grids, levelAdd, levelRemove, levelSet,
  state, storeys, type GridAxisSummary, type LevelSummary,
} from "../bimble";
import { t } from "../i18n";
import { picked } from "./levelsPick";

const tab = ref<"levels" | "grids">("levels");
const error = ref<string | null>(null);
/** Bumped after each edit: the inputs are drawn again from the model (a refused value goes back). */
const ver = ref(0);
const editable = computed(() => { void state.value.rev; void state.value.file; return editableNow(); });
const scene = computed(() => { void state.value.rev; void state.value.file; return gridAxes(); });

interface Row { id: string; name: string; elevation: number; count: number | null; height: number | null; isStorey: boolean }
// The query's order (elevation, then id), highest first.
const rows = computed<Row[]>(() => {
  if (editable.value) {
    return [...(storeys.value as LevelSummary[])].reverse().map((s) => ({
      id: s.id, name: s.name, elevation: s.elevation, count: s.element_count, height: s.height_mm ?? null, isStorey: s.is_storey ?? true,
    }));
  }
  return [...(scene.value?.levels ?? [])].reverse().map((l) => ({ id: l.key, name: l.name, elevation: l.elevation_mm, count: null, height: null, isStorey: true }));
});
const fmt = (mm: number) => (mm === 0 ? "±0.00" : (mm > 0 ? "+" : "−") + (Math.abs(mm) / 1000).toFixed(2));
/** Metres typed ("+3.20", "−0.5", "3,2") -> mm, to the micrometre; null when not a number. */
const parseM = (s: string): number | null => {
  const v = Number(s.trim().replace("−", "-").replace("±", "").replace(",", "."));
  return s.trim() !== "" && Number.isFinite(v) ? Math.round(v * 1e6) / 1e3 : null;
};
/** Altitude (m) of a level: its survey height (`coordinates` z: the georeference's height + level). */
const altitude = (mm: number) => {
  const c = coordinates([0, 0, mm]);
  return c ? c.survey[2].toFixed(2) : "—";
};
const done = (err: string | null) => { error.value = err; ver.value++; };
const val = (e: Event) => (e.target as HTMLInputElement).value;

const rename = (r: Row, e: Event) => { const v = val(e).trim(); if (v !== r.name) done(levelSet(r.id, { name: v })); };
const setLevel = (r: Row, e: Event) => {
  const mm = parseM(val(e));
  if (mm === null) return done(t("levelsgrids.badNumber"));
  if (mm !== r.elevation) done(levelSet(r.id, { level_mm: mm }));
};
const setStorey = (r: Row, e: Event) => done(levelSet(r.id, { storey: (e.target as HTMLInputElement).checked }));
const remove = (r: Row) => done(levelRemove(r.id));
const newName = ref("");
const newLevel = ref("");
const add = () => {
  const mm = parseM(newLevel.value);
  if (mm === null) return done(t("levelsgrids.badNumber"));
  const err = levelAdd(newName.value.trim(), mm);
  done(err);
  if (!err) newName.value = newLevel.value = "";
};

// ---- grids ----
const gridList = computed(() => { void state.value.rev; return editable.value ? grids() : []; });
type Ends = { start_mm: number[]; end_mm: number[] };
/** An axis at a constant x (it runs along y), at a constant y, or oblique (null). */
const along = (a: Ends): "x" | "y" | null => (a.start_mm[0] === a.end_mm[0] ? "x" : a.start_mm[1] === a.end_mm[1] ? "y" : null);
const angle = (a: Ends) => {
  const d = (Math.atan2(a.end_mm[1]! - a.start_mm[1]!, a.end_mm[0]! - a.start_mm[0]!) * 180) / Math.PI;
  return `${(Math.round(d * 10) / 10 + 0).toFixed(1)}°`;
};
const position = (a: Ends) => { const k = along(a); return k === "x" ? a.start_mm[0]! : k === "y" ? a.start_mm[1]! : null; };
const mmText = (v: number) => String(Math.round(v * 1000) / 1000);
const retag = (g: string, a: GridAxisSummary, e: Event) => { const v = val(e).trim(); if (v !== a.tag) done(gridAxisSet(g, a.id, { tag: v })); };
/** Moves an axis to the position typed (mm): both ends by the same step, across the axis. */
const move = (g: string, a: GridAxisSummary, e: Event) => {
  const k = along(a), txt = val(e).trim(), v = Number(txt.replace(",", "."));
  if (!k || txt === "" || !Number.isFinite(v)) return done(t("levelsgrids.badNumber"));
  const i = k === "x" ? 0 : 1;
  const d = v - a.start_mm[i]!;
  if (d === 0) return;
  const s: [number, number] = [a.start_mm[0], a.start_mm[1]], en: [number, number] = [a.end_mm[0], a.end_mm[1]];
  s[i] = v;
  en[i] = en[i] + d;
  done(gridAxisSet(g, a.id, { start: s, end: en }));
};
const removeGrid = (id: string) => done(gridRemove(id));
const gName = ref("Grid");
const gx = ref("0 6000 12000");
const gy = ref("0 5000");
const list = (s: string) => s.split(/[\s;,]+/).filter(Boolean).map(Number);
const addGrid = () => {
  const x = list(gx.value), y = list(gy.value);
  if ([...x, ...y].some((v) => !Number.isFinite(v))) return done(t("levelsgrids.badNumber"));
  done(gridAdd(gName.value.trim(), x, y));
};
// Read-only: the scene's axes (no grid ids).
const sceneAxes = computed(() => scene.value?.axes ?? []);

// Preview: the reference's band y 60..358 spread over the real elevations (drawing only).
const lines = computed(() => {
  const r = rows.value;
  const hi = r[0]?.elevation ?? 0, lo = r[r.length - 1]?.elevation ?? 0;
  return r.map((s) => ({ s, y: hi === lo ? 200 : 60 + ((hi - s.elevation) / (hi - lo)) * 298 }));
});
// Grids preview: every axis of the model (or the scene) fitted in 300 x 300 at (40, 80), y up.
const planLines = computed(() => {
  const ax: (Ends & { tag: string })[] = editable.value ? gridList.value.flatMap((g) => g.axes) : sceneAxes.value;
  if (!ax.length) return [];
  const xs = ax.flatMap((a) => [a.start_mm[0]!, a.end_mm[0]!]), ys = ax.flatMap((a) => [a.start_mm[1]!, a.end_mm[1]!]);
  const x0 = Math.min(...xs), y0 = Math.min(...ys), k = 300 / Math.max(1, Math.max(...xs) - x0, Math.max(...ys) - y0);
  const P = (p: number[]): [number, number] => [40 + (p[0]! - x0) * k, 380 - (p[1]! - y0) * k];
  return ax.map((a, i) => ({ i, tag: a.tag, a: P(a.start_mm), b: P(a.end_mm) }));
});
</script>

<template>
  <div class="lg-page" data-bb="levels-grids" :data-bb-editable="editable ? '1' : '0'">
    <div class="col">
      <div class="head">
        <h2 style="font-size:20px">{{ t('levelsgrids.title') }}</h2>
        <span class="seg" role="tablist">
          <button type="button" role="tab" :aria-selected="tab === 'levels'" :class="{ on: tab === 'levels' }" data-bb="levels-tab" @click="tab = 'levels'">{{ t('levelsgrids.levels') }}</button>
          <button type="button" role="tab" :aria-selected="tab === 'grids'" :class="{ on: tab === 'grids' }" data-bb="grids-tab" @click="tab = 'grids'">{{ t('levelsgrids.grids') }}</button>
        </span>
        <span style="flex:1" />
        <span class="note">{{ editable ? t('levelsgrids.altNote') : t('levelsgrids.readOnly') }}</span>
      </div>
      <p v-if="error" class="err" role="alert" data-bb="levels-error">{{ error }}</p>
      <template v-if="tab === 'levels'">
        <table :key="ver" class="t" data-bb="levels-table">
          <thead><tr><th>{{ t('levelsgrids.name') }}</th><th class="r">{{ t('levelsgrids.level') }}</th><th class="r">{{ t('levelsgrids.altitude') }}</th><th class="r">{{ t('levelsgrids.height') }}</th><th>{{ t('levelsgrids.storey') }}</th><th>{{ t('levelsgrids.plan') }}</th><th v-if="editable" /></tr></thead>
          <tbody>
            <tr v-for="s in rows" :key="s.id" :class="{ sl: s.id === picked }" :data-bb-level="s.id" @click="picked = s.id">
              <td v-if="editable"><input class="inp" style="width:150px" :value="s.name" :aria-label="t('levelsgrids.name')" data-bb="level-name" @change="rename(s, $event)"></td>
              <td v-else data-bb="level-name">{{ s.name }}</td>
              <td v-if="editable" class="r"><input class="inp mono" style="width:90px;text-align:right" :value="fmt(s.elevation)" :aria-label="t('levelsgrids.level')" data-bb="level-mm" @change="setLevel(s, $event)"></td>
              <td v-else class="r mono" data-bb="level-mm">{{ fmt(s.elevation) }}</td>
              <td class="r mono">{{ altitude(s.elevation) }}</td>
              <td class="r mono">{{ s.height === null ? '—' : (s.height / 1000).toFixed(2) }}</td>
              <td style="text-align:center"><input type="checkbox" :aria-label="t('levelsgrids.storeyAria')" :checked="s.isStorey" :disabled="!editable" data-bb="level-storey" @change="setStorey(s, $event)"></td>
              <td data-missing="query: storeys.plan_view (ADR 0056)" />
              <td v-if="editable"><button type="button" class="x" :aria-label="t('levelsgrids.remove', { name: s.name })" :title="t('levelsgrids.remove', { name: s.name })" data-bb="level-remove" @click.stop="remove(s)">×</button></td>
            </tr>
          </tbody>
        </table>
        <form v-if="editable" class="addrow" data-bb="level-add" @submit.prevent="add">
          <input v-model="newName" class="inp" style="width:150px" :placeholder="t('levelsgrids.name')" :aria-label="t('levelsgrids.newName')" data-bb="level-add-name">
          <input v-model="newLevel" class="inp mono" style="width:90px;text-align:right" placeholder="+0.00" :aria-label="t('levelsgrids.newLevel')" data-bb="level-add-mm">
          <button type="submit" class="qb" style="color:var(--bb-clay-text)" data-bb="level-add-go">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg> {{ t('levelsgrids.add') }}
          </button>
        </form>
        <p class="note" style="margin:0">{{ t('levelsgrids.storeyNote') }}</p>
      </template>
      <template v-else>
        <template v-if="editable">
          <div v-for="g in gridList" :key="g.id + ver" class="grid" :data-bb-grid="g.id">
            <div class="ghead">
              <span class="gname">{{ g.name }}</span><span class="note mono">IfcGrid</span><span style="flex:1" />
              <button type="button" class="qb" data-bb="grid-remove" @click="removeGrid(g.id)">{{ t('levelsgrids.removeGrid') }}</button>
            </div>
            <table class="t" data-bb="grid-axes">
              <thead><tr><th>{{ t('levelsgrids.tag') }}</th><th>{{ t('levelsgrids.direction') }}</th><th class="r">{{ t('levelsgrids.position') }}</th></tr></thead>
              <tbody>
                <tr v-for="a in g.axes" :key="a.id" :data-bb-axis="a.id">
                  <td><input class="inp mono" style="width:70px" :value="a.tag" :aria-label="t('levelsgrids.tag')" data-bb="axis-tag" @change="retag(g.id, a, $event)"></td>
                  <td class="mono">{{ angle(a) }}</td>
                  <td class="r">
                    <template v-if="position(a) !== null"><span class="mono note">{{ along(a) }}</span> <input class="inp mono" style="width:100px;text-align:right" :value="mmText(position(a)!)" :aria-label="t('levelsgrids.position')" data-bb="axis-pos" @change="move(g.id, a, $event)"></template>
                    <span v-else class="mono">—</span>
                  </td>
                </tr>
              </tbody>
            </table>
            <div class="addrow">
              <button type="button" class="qb" disabled data-missing="verb: grid_axis_add (the C core has grid_axis_set only)">{{ t('levelsgrids.addAxis') }}</button>
              <button type="button" class="qb" disabled data-missing="verb: grid_axis_remove (the C core has grid_axis_set only)">{{ t('levelsgrids.removeAxis') }}</button>
            </div>
          </div>
          <p v-if="!gridList.length" class="note" data-bb="grids-empty">{{ t('levelsgrids.noGrid') }}</p>
          <form class="gadd" data-bb="grid-add" @submit.prevent="addGrid">
            <label class="k" for="g2-name">{{ t('levelsgrids.gridName') }}</label><input id="g2-name" v-model="gName" class="inp" style="width:150px" data-bb="grid-add-name">
            <label class="k" for="g2-x">{{ t('levelsgrids.axesX') }}</label><input id="g2-x" v-model="gx" class="inp mono" style="width:220px" data-bb="grid-add-x">
            <label class="k" for="g2-y">{{ t('levelsgrids.axesY') }}</label><input id="g2-y" v-model="gy" class="inp mono" style="width:220px" data-bb="grid-add-y">
            <span /><button type="submit" class="qb" style="color:var(--bb-clay-text)" data-bb="grid-add-go">{{ t('levelsgrids.addGrid') }}</button>
          </form>
        </template>
        <table v-else class="t" data-bb="grid-axes">
          <thead><tr><th>{{ t('levelsgrids.tag') }}</th><th>{{ t('levelsgrids.direction') }}</th><th class="r">{{ t('levelsgrids.position') }}</th></tr></thead>
          <tbody>
            <tr v-for="(a, i) in sceneAxes" :key="i">
              <td class="mono">{{ a.tag }}</td><td class="mono">{{ angle(a) }}</td>
              <td class="r mono">{{ position(a) === null ? '—' : `${along(a)} ${mmText(position(a)!)}` }}</td>
            </tr>
          </tbody>
        </table>
      </template>
    </div>
    <div class="card" style="padding:10px;display:flex;flex-direction:column">
      <div class="eyebrow">{{ tab === 'levels' ? t('levelsgrids.preview') : t('levelsgrids.planPreview') }}</div>
      <svg v-if="tab === 'levels'" width="100%" height="100%" viewBox="0 0 380 460" role="img" :aria-label="t('levelsgrids.previewAria')" data-bb="levels-preview">
        <rect v-if="lines.length > 1" x="70" y="60" width="200" height="282" fill="none" stroke="var(--bb-ink)" stroke-width="1" />
        <g v-for="l in lines" :key="l.s.id">
          <path :d="`M40 ${l.y} H300`" stroke="var(--bb-ink)" :stroke-width="l.s.id === picked ? 1.4 : 0.7" stroke-dasharray="14 3 2 3" />
          <path :d="`M300 ${l.y} l8 -8 h-16z`" :fill="l.s.id === picked ? 'var(--bb-pink)' : 'var(--bb-sheet)'" stroke="var(--bb-ink)" stroke-width="1" />
          <text x="314" :y="l.y - 2" font-family="DM Mono" font-size="11" fill="var(--bb-ink)">{{ l.s.name }} {{ fmt(l.s.elevation) }}</text>
        </g>
      </svg>
      <svg v-else width="100%" height="100%" viewBox="0 0 380 460" role="img" :aria-label="t('levelsgrids.planPreview')" data-bb="grids-preview">
        <g v-for="l in planLines" :key="l.i">
          <path :d="`M${l.a[0]} ${l.a[1]} L${l.b[0]} ${l.b[1]}`" stroke="var(--bb-ink)" stroke-width="0.7" stroke-dasharray="14 3 2 3" />
          <circle :cx="l.a[0]" :cy="l.a[1]" r="9" fill="var(--bb-sheet)" stroke="var(--bb-ink)" stroke-width="1" />
          <text :x="l.a[0]" :y="l.a[1] + 4" text-anchor="middle" font-family="DM Mono" font-size="10" fill="var(--bb-ink)">{{ l.tag }}</text>
        </g>
      </svg>
    </div>
  </div>
</template>

<style scoped>
/* .page{...padding:24px 32px} + style="display:grid;grid-template-columns:minmax(0,1fr) 380px;gap:28px" */
.lg-page { flex: 1; background: var(--bb-sheet); overflow: auto; min-height: 0; padding: 24px 32px; display: grid; grid-template-columns: minmax(0, 1fr) 380px; gap: 28px; height: 100%; box-sizing: border-box; }
.col { display: flex; flex-direction: column; gap: 12px; min-width: 0; }
.head { display: flex; align-items: center; gap: 14px; flex-wrap: wrap; }
h2 { font-family: 'Sora', sans-serif; margin: 0; }
.mono { font-family: 'DM Mono', monospace; }
.note { font-size: 13px; color: var(--bb-stone); }
.err { color: var(--bb-ink); border-left: 3px solid var(--bb-attention); padding-left: 6px; font-size: 13px; margin: 0; }
.eyebrow { font-family: 'DM Mono', monospace; font-size: 12px; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); }
.seg { display: inline-flex; gap: 2px; padding: 2px; border-radius: 999px; background: var(--bb-sunk); }
.seg button { padding: 2px 12px; border-radius: 999px; color: var(--bb-stone); font: inherit; font-size: 13px; border: 0; background: none; cursor: pointer; white-space: nowrap; }
.seg .on { background: var(--bb-ink); color: var(--bb-on-ink); }
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; box-sizing: border-box; }
.inp.mono { font-family: 'DM Mono', monospace; font-size: 13px; }
table.t { border-collapse: collapse; width: 100%; font-size: 13.5px; }
table.t th { text-align: left; font-weight: 600; color: var(--bb-stone); border-bottom: 1px solid var(--bb-stone); padding: 6px 8px; white-space: nowrap; font-size: 13px; }
table.t td { border-bottom: 1px solid var(--bb-sunk); padding: 5px 8px; vertical-align: middle; }
table.t tr.sl td { background: var(--bb-pink); }
table.t td.r, table.t th.r { text-align: right; }
.qb { display: inline-flex; align-items: center; gap: 4px; height: 32px; padding: 0 12px; border-radius: 999px; border: 0; background: none; font: inherit; cursor: pointer; white-space: nowrap; align-self: flex-start; }
.qb:disabled { opacity: .55; cursor: default; }
.x { border: 0; background: none; font: inherit; font-size: 18px; line-height: 1; color: var(--bb-stone); cursor: pointer; padding: 2px 6px; border-radius: 6px; }
.x:hover { color: var(--bb-ink); background: var(--bb-sunk); }
.addrow { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
.grid { display: flex; flex-direction: column; gap: 6px; }
.ghead { display: flex; align-items: center; gap: 10px; }
.gname { font-family: 'Sora', sans-serif; font-weight: 500; }
.gadd { display: grid; grid-template-columns: max-content max-content; gap: 6px 12px; align-items: center; margin-top: 8px; }
.k { color: var(--bb-stone); font-size: 13.5px; white-space: nowrap; }
.card { background: var(--bb-sheet); border: 1px solid var(--bb-line); border-radius: 16px; box-sizing: border-box; min-height: 0; }
</style>
