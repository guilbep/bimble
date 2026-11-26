<script lang="ts">
export const hint = "perf.hint";
export const label = "perf.title";
</script>

<script setup lang="ts">
// The performance panel (ticket GPU1, ADR 0083): opt-in, from the status bar's "Performance" or
// `?perf=1` (`?panel=performance`). What it shows comes from perf.ts (the one source the lab tests
// and "Copy report" read too). While it is open the viewport waits for the GPU after each frame, so
// the frame times are draw-to-pixels. Nothing is sent anywhere; the report goes to the clipboard only.
import { onMounted, onUnmounted, shallowRef } from "vue";
import { state } from "../bimble";
import { t } from "../i18n";
import { currentPanel } from ".";
import { perfRecord, perfReport, perfSnapshot, perfWait, type FrameStats, type PerfSnapshot } from "../perf";

const snap = shallowRef<PerfSnapshot>(perfSnapshot());
const names = shallowRef(false);
const note = shallowRef<string | null>(null);
const shownReport = shallowRef<string | null>(null);
let timer = 0;
onMounted(() => { perfWait(true); timer = window.setInterval(() => { snap.value = perfSnapshot(); }, 500); });
onUnmounted(() => { perfWait(false); clearInterval(timer); });

const ms = (x: number | null) => (x === null || !Number.isFinite(x) ? "–" : `${x < 10 ? x.toFixed(1) : Math.round(x)} ms`);
const mb = (x: number | null) => (x === null ? "–" : `${(x / 1048576).toFixed(1)} MB`);
const num = (x: number) => Math.round(x).toLocaleString();
const row = (s: FrameStats) => (s.n ? `${s.n} · ${ms(s.last)} · ${ms(s.median)} · ${ms(s.p95)}` : "–");
const adapterText = (s: PerfSnapshot) => {
  const a = s.adapter;
  if (!a) return t("perf.unknown");
  return [a.vendor, a.architecture, a.description].filter(Boolean).join(" · ") || t("perf.unknown");
};
async function copy() {
  const text = perfReport(names.value, state.value.file);
  shownReport.value = null;
  try {
    await navigator.clipboard.writeText(text);
    note.value = t("perf.copied", { n: text.trimEnd().split("\n").length });
  } catch {
    note.value = t("perf.copyFailed");
    shownReport.value = text;
  }
  (window as unknown as { __bbPerfCopied?: string }).__bbPerfCopied = text; // the lab test reads what was copied
}
function record() {
  perfRecord();
  snap.value = perfSnapshot();
}
</script>

<template>
  <div class="pf" data-bb="perf">
    <div style="display:flex;align-items:center;gap:8px">
      <span class="pn" style="margin:0" data-bb="perf-title">{{ t('perf.title') }}</span>
      <span style="flex:1"></span>
      <button type="button" class="cx" :aria-label="t('perf.closePanel')" data-bb="perf-close" @click="currentPanel = null"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button>
    </div>
    <div class="note">{{ t('perf.hint') }}</div>
    <dl class="kv">
      <dt>{{ t('perf.renderer') }}</dt>
      <dd data-bb="perf-renderer">{{ snap.renderer.using }} <span class="q">({{ t('perf.asked', { what: snap.renderer.asked === 'auto' ? t('perf.askedAuto') : snap.renderer.asked }) }})</span></dd>
      <dd v-if="snap.renderer.reason" class="q" data-bb="perf-fallback">{{ t('perf.fallback', { why: snap.renderer.reason }) }}</dd>
      <dt>{{ t('perf.adapter') }}</dt>
      <dd data-bb="perf-adapter">{{ adapterText(snap) }}</dd>
      <dd v-if="snap.adapter?.software" class="warn" data-bb="perf-software">{{ t('perf.software') }}</dd>
    </dl>
    <div class="sec">{{ t('perf.frames') }}<span v-if="snap.moving.n + snap.still.n > 0 && !snap.synced" class="q"> · {{ t('perf.notSynced') }}</span></div>
    <table class="t">
      <thead><tr><th></th><th class="r">{{ t('perf.cols') }}</th></tr></thead>
      <tbody>
        <tr><td>{{ t('perf.moving') }}</td><td class="r mono" data-bb="perf-moving">{{ row(snap.moving) }}</td></tr>
        <tr><td>{{ t('perf.still') }}</td><td class="r mono" data-bb="perf-still">{{ row(snap.still) }}</td></tr>
      </tbody>
    </table>
    <div class="sec">{{ t('perf.lastFrame') }}</div>
    <dl class="kv two">
      <dt>{{ t('perf.triangles') }}</dt><dd class="mono" data-bb="perf-tris">{{ num(snap.tris) }}</dd>
      <dt>{{ t('perf.draws') }}</dt><dd class="mono" data-bb="perf-draws">{{ num(snap.draws) }}</dd>
      <dt>{{ t('perf.culled') }}</dt><dd class="mono">{{ num(snap.culled) }} / {{ num(snap.batches) }}</dd>
      <dt>{{ t('perf.proxies') }}</dt><dd class="mono">{{ num(snap.proxiesDrawn) }} ({{ num(snap.proxyTris) }})</dd>
      <template v-if="snap.renderer.using === 'WebGPU'"><dt>{{ t('perf.gpuCull') }}</dt><dd>{{ snap.renderer.gpuCull ? t('perf.on') : t('perf.off') }}</dd></template>
    </dl>
    <div class="sec">{{ t('perf.memory') }}</div>
    <dl class="kv two">
      <dt>{{ t('perf.gpuBytes') }}</dt><dd class="mono" data-bb="perf-gpu-bytes">{{ mb(snap.gpuBytes) }}</dd>
      <dt>{{ t('perf.jsHeap') }}</dt><dd class="mono">{{ mb(snap.jsHeap) }}</dd>
      <dt>{{ t('perf.wasm') }}</dt><dd class="mono">{{ mb(snap.wasmBytes) }}</dd>
    </dl>
    <div class="sec">{{ t('perf.model') }}</div>
    <dl class="kv two">
      <dt>{{ t('perf.elements') }}</dt><dd class="mono">{{ num(snap.elements) }} · {{ num(snap.triangles) }} △</dd>
      <dt>{{ t('perf.openFirst') }}</dt><dd class="mono">{{ ms(snap.openToFirst) }}</dd>
      <dt>{{ t('perf.openComplete') }}</dt><dd class="mono">{{ ms(snap.openToComplete) }}</dd>
    </dl>
    <template v-if="snap.links.length">
      <div class="sec">{{ t('perf.links') }}</div>
      <div v-for="(l, k) in snap.links" :key="k" class="note mono" data-bb="perf-link">{{ names ? l.name : `${k + 1}` }} · {{ num(l.elements) }} · {{ num(l.triangles) }} △ · {{ l.visible ? t('perf.linkShown') : t('perf.linkHidden') }}</div>
    </template>
    <div class="sec">{{ t('perf.inputs') }}</div>
    <ol class="inputs" data-bb="perf-inputs">
      <li v-for="(i, k) in snap.inputs" :key="k" class="mono">{{ ms(i.ms) }} · {{ i.what }}</li>
      <li v-if="!snap.inputs.length" class="q" style="list-style:none">{{ t('perf.none') }}</li>
    </ol>
    <div v-if="snap.recording" class="note" data-bb="perf-recording">{{ t('perf.recording') }}</div>
    <div v-else-if="snap.record" class="note" data-bb="perf-recorded">{{ t('perf.recorded', { s: snap.record.seconds, n: snap.record.moving.n + snap.record.still.n, m: ms(snap.record.moving.median) }) }}</div>
    <label class="note" style="display:flex;gap:6px;align-items:center"><input v-model="names" type="checkbox" data-bb="perf-names">{{ t('perf.names') }}</label>
    <div style="display:flex;gap:8px;flex-wrap:wrap">
      <button class="btn s" type="button" :disabled="snap.recording" data-bb="perf-record" @click="record">{{ t('perf.record') }}</button>
      <button class="btn p" type="button" data-bb="perf-copy" @click="copy">{{ t('perf.copy') }}</button>
    </div>
    <div v-if="note" class="note" role="status" data-bb="perf-note">{{ note }}</div>
    <textarea v-if="shownReport" class="inp mono" readonly rows="10" :value="shownReport" data-bb="perf-report"></textarea>
    <div class="note q">{{ t('perf.private') }}</div>
  </div>
</template>

<style scoped>
.pf { display: flex; flex-direction: column; gap: 8px; height: 100%; overflow-y: auto; font-size: var(--bb-size-small); }
.pn { font-weight: 600; font-size: var(--bb-size-body, 15px); }
.sec { font-weight: 600; margin-top: 6px; }
.note { color: var(--bb-stone); }
.q { color: var(--bb-stone); }
.warn { color: var(--bb-clay, #C06A41); font-weight: 600; }
.mono { font-family: var(--bb-font-data); }
.kv { display: grid; grid-template-columns: 1fr; gap: 2px; margin: 0; }
.kv dt { color: var(--bb-stone); }
.kv dd { margin: 0 0 4px 0; overflow-wrap: anywhere; }
.kv.two { grid-template-columns: auto 1fr; column-gap: 12px; }
.kv.two dd { text-align: right; margin: 0; }
.t { width: 100%; border-collapse: collapse; }
.t td, .t th { padding: 2px 4px; text-align: left; font-weight: normal; }
.t .r { text-align: right; }
.inputs { margin: 0; padding-left: 20px; }
.cx { width: 22px; height: 22px; border: 0; padding: 0; background: none; color: inherit; cursor: pointer; display: inline-flex; align-items: center; justify-content: center; border-radius: var(--bb-radius-pill); }
textarea { width: 100%; font-size: 11px; }
</style>
