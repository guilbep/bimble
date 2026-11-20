<script lang="ts">
import LevelsGridsSide from "./LevelsGridsSide.vue";
export const hint = "levelsgrids.hint";
export const side = LevelsGridsSide;
</script>

<script setup lang="ts">
// A7 Levels and grids (wireframes v01 LevelsGrids.html; ADR 0056, Proposed). Rows are the real storeys
// (`storeys` query). Editing, project zero, storey height, the storey flag, plan links and grids have no
// Rust call yet: drawn as the reference, disabled, data-missing.
import { computed, ref } from "vue";
import { storeys } from "../bimble";
import { t } from "../i18n";
import { picked } from "./levelsPick";

const tab = ref<"levels" | "grids">("levels");
// Highest first, as the reference.
const rows = computed(() => [...storeys.value].sort((a, b) => b.elevation - a.elevation));
const fmt = (mm: number) => (mm === 0 ? "±0.00" : (mm > 0 ? "+" : "−") + (Math.abs(mm) / 1000).toFixed(2));
// Preview: the reference's band y 60..358 spread over the real elevations (drawing only).
const lines = computed(() => {
  const r = rows.value;
  const hi = r[0]?.elevation ?? 0, lo = r[r.length - 1]?.elevation ?? 0;
  return r.map((s) => ({ s, y: hi === lo ? 200 : 60 + ((hi - s.elevation) / (hi - lo)) * 298 }));
});
</script>

<template>
  <div class="lg-page" data-bb="levels-grids">
    <div class="col">
      <div class="head">
        <h2 style="font-size:20px">{{ t('levelsgrids.title') }}</h2>
        <span class="seg" role="tablist">
          <button type="button" role="tab" :aria-selected="tab === 'levels'" :class="{ on: tab === 'levels' }" data-bb="levels-tab" @click="tab = 'levels'">{{ t('levelsgrids.levels') }}</button>
          <button type="button" role="tab" :aria-selected="tab === 'grids'" :class="{ on: tab === 'grids' }" data-missing="query: grids (ADR 0056)" @click="tab = 'grids'">{{ t('levelsgrids.grids') }}</button>
        </span>
        <span style="flex:1" />
        <span class="note">{{ t('levelsgrids.altNote') }}</span>
      </div>
      <template v-if="tab === 'levels'">
        <table class="t" data-bb="levels-table">
          <thead><tr><th>{{ t('levelsgrids.name') }}</th><th class="r">{{ t('levelsgrids.level') }}</th><th class="r">{{ t('levelsgrids.altitude') }}</th><th class="r">{{ t('levelsgrids.height') }}</th><th>{{ t('levelsgrids.storey') }}</th><th>{{ t('levelsgrids.plan') }}</th></tr></thead>
          <tbody>
            <tr v-for="s in rows" :key="s.id" :class="{ sl: s.id === picked }" @click="picked = s.id">
              <td><input class="inp" style="width:150px" :value="s.name" :aria-label="t('levelsgrids.name')" disabled data-missing="verb: level_set {id, name} (ADR 0056)"></td>
              <td class="r"><input class="inp mono" style="width:90px;text-align:right" :value="fmt(s.elevation)" :aria-label="t('levelsgrids.level')" disabled data-missing="verb: level_set {id, level_mm} (ADR 0056)"></td>
              <td class="r mono" data-missing="query: project zero (ADR 0056)">—</td>
              <td class="r mono" data-missing="query: storeys.height_mm (ADR 0056)">—</td>
              <td style="text-align:center"><input type="checkbox" :aria-label="t('levelsgrids.storeyAria')" checked disabled data-missing="query: storeys.is_storey; verb: level_set {storey} (ADR 0056)"></td>
              <td data-missing="query: storeys.plan_view (ADR 0056)" />
            </tr>
          </tbody>
        </table>
        <button type="button" class="qb" style="color:var(--bb-clay-text);margin-top:6px" disabled data-missing="verb: level_add {name, level_mm, storey} (ADR 0056)">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 5v14M5 12h14" /></svg> {{ t('levelsgrids.add') }}
        </button>
        <p class="note" style="margin:0">{{ t('levelsgrids.storeyNote') }}</p>
      </template>
    </div>
    <div class="card" style="padding:10px;display:flex;flex-direction:column">
      <div class="eyebrow">{{ t('levelsgrids.preview') }}</div>
      <svg width="100%" height="100%" viewBox="0 0 380 460" role="img" :aria-label="t('levelsgrids.previewAria')" data-bb="levels-preview">
        <rect v-if="lines.length > 1" x="70" y="60" width="200" height="282" fill="none" stroke="var(--bb-ink)" stroke-width="1" />
        <g v-for="l in lines" :key="l.s.id">
          <path :d="`M40 ${l.y} H300`" stroke="var(--bb-ink)" :stroke-width="l.s.id === picked ? 1.4 : 0.7" stroke-dasharray="14 3 2 3" />
          <path :d="`M300 ${l.y} l8 -8 h-16z`" :fill="l.s.id === picked ? 'var(--bb-pink)' : 'var(--bb-sheet)'" stroke="var(--bb-ink)" stroke-width="1" />
          <text x="314" :y="l.y - 2" font-family="DM Mono" font-size="11" fill="var(--bb-ink)">{{ l.s.name }} {{ fmt(l.s.elevation) }}</text>
        </g>
      </svg>
    </div>
  </div>
</template>

<style scoped>
/* .page{...padding:24px 32px} + style="display:grid;grid-template-columns:minmax(0,1fr) 380px;gap:28px" */
.lg-page { flex: 1; background: var(--bb-sheet); overflow: hidden; min-height: 0; padding: 24px 32px; display: grid; grid-template-columns: minmax(0, 1fr) 380px; gap: 28px; height: 100%; box-sizing: border-box; }
.col { display: flex; flex-direction: column; gap: 12px; min-width: 0; }
.head { display: flex; align-items: center; gap: 14px; flex-wrap: wrap; }
h2 { font-family: 'Sora', sans-serif; margin: 0; }
.mono { font-family: 'DM Mono', monospace; }
.note { font-size: 13px; color: var(--bb-stone); }
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
.card { background: var(--bb-sheet); border: 1px solid var(--bb-line); border-radius: 16px; box-sizing: border-box; min-height: 0; }
</style>
