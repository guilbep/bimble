<script lang="ts">
export const hint = "lca.hint";
export const capability = "lca"; // FAKE1
</script>

<script setup lang="ts">
// D7 Carbon balance (wireframes v01 LCA.html; ADR 0063, Proposed). Rust has no carbon factors and no
// per-material quantity sum yet: the material names are real (`materials`), every number is data-missing.
// Never invent factors here (ADR 0063: KBOB only, from Rust).
import { materials } from "../bimble";
import { t } from "../i18n";
const M = "lca_balance: KBOB factors per material, amount per material (m³ or kg), total, per m² ERA";
</script>

<template>
  <div class="lca" data-bb="lca">
    <div class="row">
      <h2 style="font-size:20px" data-bb="lca-title">{{ t('page.lca') }}</h2>
      <span class="chip c-n nx" style="font-size:12px">{{ t('lca.sample') }}</span>
      <span style="flex:1" />
      <button class="btn p" type="button" disabled data-missing="lca_report: PDF and CSV with KBOB version" data-bb="lca-export">{{ t('lca.export') }}</button>
    </div>
    <div class="kpis" data-bb="lca-kpis">
      <div class="card" style="padding:14px"><div class="eyebrow">{{ t('lca.total') }}</div><div class="disp" style="font-size:32px;font-weight:700" :data-missing="M">—</div><div class="note">{{ t('lca.totalNote') }}</div></div>
      <div class="card" style="padding:14px"><div class="eyebrow">{{ t('lca.perM2') }}</div><div class="disp" style="font-size:32px;font-weight:700" :data-missing="M">—</div><div class="note">{{ t('lca.perM2Note') }}</div></div>
      <div class="card" style="padding:14px"><div class="eyebrow">{{ t('lca.variant') }}</div><div class="disp" style="font-size:32px;font-weight:700" data-missing="variants (ADR 0038) and lca_balance per variant">—</div><div class="note">{{ t('lca.variantNote') }}</div></div>
    </div>
    <div class="cols">
      <div class="card" style="padding:14px" data-bb="lca-chart">
        <div style="display:flex;align-items:center;gap:10px">
          <span class="pn" style="margin:0">{{ t('lca.byMaterial') }}</span><span style="flex:1" />
          <div style="display:flex;gap:16px;font-size:13px"><span class="note">{{ t('lca.unitT') }}</span></div>
        </div>
        <svg width="100%" height="270" viewBox="0 0 840 270" role="img" :aria-label="t('lca.chartAria')" :data-missing="M">
          <template v-for="(m, i) in materials" :key="m.name">
            <text v-if="i < 4" x="0" :y="42 + 56 * i" font-family="Source Sans 3" font-size="13" fill="var(--bb-ink)">{{ m.name }}</text>
          </template>
          <path d="M210.0 20V254" stroke="var(--bb-sunk)" />
        </svg>
      </div>
      <div class="card" style="padding:14px" data-bb="lca-table">
        <span class="pn">{{ t('lca.table') }}</span>
        <table class="t">
          <thead><tr><th>{{ t('lca.col.material') }}</th><th class="r">{{ t('lca.col.quantity') }}</th><th></th><th class="r">{{ t('lca.col.factor') }}</th><th class="r">t</th></tr></thead>
          <tbody>
            <tr v-for="m in materials" :key="m.name">
              <td>{{ m.name }}</td>
              <td class="r mono" :data-missing="M">—</td><td class="mono"></td>
              <td class="r mono" :data-missing="M">—</td><td class="r mono" :data-missing="M">—</td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>
  </div>
</template>

<style scoped>
/* .page style="display:flex;flex-direction:column;gap:14px" */
.lca { display: flex; flex-direction: column; gap: 14px; }
.row { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.kpis { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 14px; }
.cols { display: grid; grid-template-columns: minmax(0, 1.3fr) minmax(0, 1fr); gap: 18px; }
h2 { font-family: 'Sora', sans-serif; margin: 0; }
.card { background: var(--bb-sheet); border: 1px solid var(--bb-line); border-radius: 16px; }
.eyebrow { font-family: 'DM Mono', monospace; font-size: 12px; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); }
.disp { font-family: 'Sora', 'Source Sans 3', sans-serif; }
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; display: block; }
.note { font-size: 13px; color: var(--bb-stone); }
.mono { font-family: 'DM Mono', ui-monospace, Menlo, monospace; }
.t th { text-align: left; font-weight: 600; color: var(--bb-stone); border-bottom: 1px solid var(--bb-stone); padding: 6px 8px; white-space: nowrap; font-size: 13px; }
.t td { border-bottom: 1px solid var(--bb-sunk); padding: 5px 8px; vertical-align: middle; }
.t td.r, .t th.r { text-align: right; }
</style>
