<script setup lang="ts">
// The Navigator's Links group with its buttons (ticket LNK1; Rust: lib.rs:4402 the side panel's
// Links group, links.rs:85 Manage links). A row per linked model (IFC, .bimble, .bimbleview) and per
// reference plan (PDF, DXF, with the view it is placed in): show / hide, reload, reload from…,
// remove; a reference also moves by numbers (mm) and turns (degrees), one undo step each. The calls
// are shell/links.ts's; errors show under the rows.
import { computed, onUnmounted, reactive, ref, watch } from "vue";
import { t } from "../i18n";
import { state } from "../bimble";
import { send } from "./send";
import { enabled, linkFolder, linkLog, linksTick, linkNew, linksBusy, linksError, linksPanel, modelOp, modelReload, modelSet, refOp, refReload, reloadAll, type PanelModel, type PanelReference } from "./links";

const rows = computed(() => { void state.value; return linksPanel(); });
// The move and rotate fields of each reference, by id (filled from its placement).
const edit = reactive<Record<string, { dx: number; dy: number; angle: number }>>({});
watch(rows, (v) => {
  for (const r of v.references) {
    const f = edit[r.id];
    if (!f) edit[r.id] = { dx: 0, dy: 0, angle: r.rotation_deg };
    else f.angle = r.rotation_deg;
  }
}, { immediate: true });
const fields = (r: PanelReference) => edit[r.id] ?? (edit[r.id] = { dx: 0, dy: 0, angle: r.rotation_deg });
function move(r: PanelReference): void {
  const f = fields(r);
  void refOp(r.id, { op: "move", offset_mm: [r.offset_mm[0] + Number(f.dx || 0), r.offset_mm[1] + Number(f.dy || 0)] }).then((e) => { if (!e) { f.dx = 0; f.dy = 0; } });
}
function rotate(r: PanelReference): void {
  const f = fields(r);
  void refOp(r.id, { op: "rotate", rotation_deg: Number(f.angle || 0) });
}
const can = computed(() => { void state.value; return { model: enabled("link_model"), folder: enabled("link_folder"), pdf: enabled("link_pdf"), dxf: enabled("link_dxf") }; });
// LINKF: a linked model's state, its numbers and its settings (ccore/session/links_panel.h)
const PALETTE = ["#c0504d", "#4f81bd", "#9bbb59", "#8064a2", "#4bacc6", "#f79646", "#2c4d75", "#7f7f7f", "#d99694", "#95b3d7", "#c3d69b", "#b2a2c7"];
const mb = (n = 0) => (n < 1e6 ? `${(n / 1e3).toFixed(0)} kB` : `${(n / 1e6).toFixed(n < 1e8 ? 1 : 0)} MB`);
const num = (n = 0) => n.toLocaleString();
const busy = (m: PanelModel) => m.state === "queued" || m.state === "converting";
function stateText(m: PanelModel): string {
  if (m.missing) return t("links.missing");
  if (m.state === "converting") return t("links.state.converting", { n: m.progress ?? 0 });
  return m.state && m.state !== "ready" ? t(`links.state.${m.state}`) : m.kind;
}
const detail = (m: PanelModel) => m.state === "ready"
  ? t("links.detail", { size: mb(m.bytes), elements: num(m.elements), triangles: num(m.triangles) }) +
    (m.cache ? ` · ${t(m.from_cache ? "links.fromCache" : "links.cacheWritten", { date: new Date((m.cache_mtime_s ?? 0) * 1000).toLocaleString() })}` : "")
  : m.message ?? "";
// LINKF: the report of one link, shown under its row; "Open as editable…" asks first (it converts)
const logOf = ref<{ name: string; text: string } | null>(null);
const showLog = (m: PanelModel) => { logOf.value = logOf.value?.name === m.name ? null : { name: m.name, text: linkLog(m.name) }; };
const confirmEdit = ref<string | null>(null);
const openEditable = (m: PanelModel) => { confirmEdit.value = null; send({ do: "import", path: m.path }); };
// LINKF: while a link is queued or converting, its row is polled (query links_panel, 250 ms): the
// core publishes no change for the progress (one full refresh per finished link, not per 5 %).
let poll: number | null = null;
watch(() => rows.value.models.some(busy), (b) => {
  if (b && poll === null) poll = window.setInterval(() => linksTick.value++, 250);
  if (!b && poll !== null) window.clearInterval(poll), (poll = null);
}, { immediate: true });
onUnmounted(() => { if (poll !== null) window.clearInterval(poll); });
const anyReady = computed(() => rows.value.models.some((m) => m.state === "ready" || m.state === "unloaded" || m.state === "failed"));
</script>

<template>
  <div class="lp" data-bb="links-panel">
    <div class="lp-add">
      <button type="button" data-bb="link-add-model" :disabled="!can.model || linksBusy" :title="t('links.addModelTip')" @click="linkNew('link_model')">{{ t('links.addModel') }}</button>
      <button type="button" data-bb="link-add-folder" :disabled="!can.folder || linksBusy" :title="t('links.addFolderTip')" @click="linkFolder()">{{ t('links.addFolder') }}</button>
      <button v-if="anyReady" type="button" data-bb="link-reload-all" :title="t('links.reloadAllTip')" @click="reloadAll()">{{ t('links.reloadAll') }}</button>
      <button type="button" data-bb="link-add-pdf" :disabled="!can.pdf || linksBusy" :title="can.pdf ? t('links.addPdfTip') : t('links.in2d')" @click="linkNew('link_pdf')">{{ t('links.addPdf') }}</button>
      <button type="button" data-bb="link-add-dxf" :disabled="!can.dxf || linksBusy" :title="can.dxf ? t('links.addDxfTip') : t('links.in2d')" @click="linkNew('link_dxf')">{{ t('links.addDxf') }}</button>
    </div>
    <div v-if="!rows.models.length && !rows.references.length" class="lp-none">{{ t('links.none') }}</div>
    <div v-for="m in rows.models" :key="'m' + m.name" class="lp-ref" data-bb="link-model" :data-name="m.name" :data-state="m.state ?? 'ready'">
    <div class="lp-row" data-bb="link-row" :data-name="m.name" :data-visible="m.visible ? '1' : '0'" :data-missing="m.missing ? '1' : '0'" :data-state="m.state ?? 'ready'">
      <span v-if="m.colour != null" class="sw" :style="{ background: PALETTE[m.colour] }" aria-hidden="true"></span>
      <button type="button" class="eye" data-bb="link-eye" :aria-pressed="m.visible" :disabled="m.missing" :title="m.visible ? t('links.hide') : t('links.show')" @click="modelOp(m.visible ? 'hide' : 'show', m.name)">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z"/><circle v-if="m.visible" cx="12" cy="12" r="3"/><path v-else d="m3 3 18 18"/></svg>
      </button>
      <span class="nm" :title="m.path">{{ m.name }}</span>
      <span class="k" data-bb="link-state">{{ stateText(m) }}</span>
      <button v-if="m.missing" type="button" data-bb="link-locate" :title="t('links.locateTip')" @click="modelReload(m, true)">{{ t('links.locate') }}</button>
      <button v-else-if="busy(m)" type="button" data-bb="link-cancel" :title="t('links.cancelTip')" @click="modelSet('cancel', m.name)">{{ t('links.cancel') }}</button>
      <template v-else>
        <button type="button" data-bb="link-reload" :title="t('links.reloadTip')" @click="modelReload(m)">{{ t('links.reload') }}</button>
        <button type="button" data-bb="link-reload-from" :title="t('links.reloadFromTip')" @click="modelReload(m, true)">…</button>
      </template>
      <button type="button" data-bb="link-remove" :title="t('links.remove')" @click="modelOp('remove', m.name)">×</button>
    </div>
    <div v-if="!m.missing && m.state" class="lp-place" data-bb="link-detail">
      <span class="d" :title="m.log ? t('links.logTip', { path: m.log }) : m.path">{{ detail(m) }}</span>
      <button v-if="m.log" type="button" data-bb="link-log" :title="t('links.logTip', { path: m.log })" @click="showLog(m)">{{ t('links.log') }}</button>
      <button v-if="m.kind === 'ifc' && !m.missing" type="button" data-bb="link-open-editable" :title="t('links.openEditableTip')" @click="confirmEdit = confirmEdit === m.name ? null : m.name">{{ t('links.openEditable') }}</button>
      <button v-if="m.state === 'ready'" type="button" data-bb="link-unload" :title="t('links.unloadTip')" @click="modelSet('unload', m.name)">{{ t('links.unload') }}</button>
      <button v-if="m.state === 'unloaded'" type="button" data-bb="link-load" :title="t('links.loadTip')" @click="modelSet('load', m.name)">{{ t('links.load') }}</button>
      <button type="button" data-bb="link-pin" :aria-pressed="m.pinned" :title="t(m.pinned ? 'links.unpinTip' : 'links.pinTip')" @click="modelSet(m.pinned ? 'unpin' : 'pin', m.name)">{{ t(m.pinned ? 'links.pinned' : 'links.unpinned') }}</button>
      <select data-bb="link-colour" :value="m.colour ?? ''" :aria-label="t('links.colour')" @change="modelSet('colour', m.name, { colour: ($event.target as HTMLSelectElement).value === '' ? null : Number(($event.target as HTMLSelectElement).value) })">
        <option value="">{{ t('links.ownColours') }}</option>
        <option v-for="(c, i) in PALETTE" :key="c" :value="i">{{ t('links.colourN', { n: i + 1 }) }}</option>
      </select>
      <select data-bb="link-origin" :value="m.origin ?? 'file'" :disabled="m.pinned" :aria-label="t('links.origin')" :title="m.pinned ? t('links.pinnedTip') : t('links.originTip')" @change="modelSet('place', m.name, { origin: ($event.target as HTMLSelectElement).value })">
        <option value="file">{{ t('links.origin.file') }}</option>
        <option value="origin">{{ t('links.origin.origin') }}</option>
        <option value="base_point">{{ t('links.origin.base_point') }}</option>
        <option value="survey_point">{{ t('links.origin.survey_point') }}</option>
      </select>
    </div>
    <div v-if="confirmEdit === m.name" class="lp-place" role="alert" data-bb="link-open-editable-warning">
      <span class="d" style="white-space:normal">{{ t('links.openEditableWarn') }}</span>
      <button type="button" data-bb="link-open-editable-go" @click="openEditable(m)">{{ t('links.openEditableGo') }}</button>
      <button type="button" @click="confirmEdit = null">{{ t('links.cancel') }}</button>
    </div>
    <pre v-if="logOf?.name === m.name" class="lp-log" data-bb="link-log-text">{{ logOf.text }}</pre>
    </div>
    <div v-for="r in rows.references" :key="'r' + r.id" class="lp-ref" data-bb="ref-row" :data-id="r.id" :data-name="r.name" :data-visible="r.visible ? '1' : '0'">
      <div class="lp-row">
        <button type="button" class="eye" data-bb="ref-eye" :aria-pressed="r.visible" :title="r.visible ? t('links.hide') : t('links.show')" @click="refOp(r.id, { op: r.visible ? 'hide' : 'show' })">
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z"/><circle v-if="r.visible" cx="12" cy="12" r="3"/><path v-else d="m3 3 18 18"/></svg>
        </button>
        <span class="nm" :title="r.source ?? ''">{{ r.name }}</span>
        <span class="k" :title="r.view_name">{{ r.kind }} · {{ r.view_name }}</span>
        <button type="button" data-bb="ref-reload" :title="t('links.reloadTip')" @click="refReload(r)">{{ t('links.reload') }}</button>
        <button type="button" data-bb="ref-reload-from" :title="t('links.reloadFromTip')" @click="refReload(r, true)">…</button>
        <button type="button" data-bb="ref-remove" :title="t('links.remove')" @click="refOp(r.id, { op: 'remove' })">×</button>
      </div>
      <div class="lp-place">
        <label>{{ t('links.dx') }} <input v-model.number="fields(r).dx" type="number" step="10" data-bb="ref-dx"></label>
        <label>{{ t('links.dy') }} <input v-model.number="fields(r).dy" type="number" step="10" data-bb="ref-dy"></label>
        <button type="button" data-bb="ref-move" :title="t('links.moveTip')" @click="move(r)">{{ t('links.move') }}</button>
        <label>{{ t('links.angle') }} <input v-model.number="fields(r).angle" type="number" step="1" data-bb="ref-angle"></label>
        <button type="button" data-bb="ref-rotate" :title="t('links.rotateTip')" @click="rotate(r)">{{ t('links.rotate') }}</button>
      </div>
    </div>
    <div v-if="linksError" class="lp-err" role="alert" data-bb="links-error">{{ linksError }}</div>
  </div>
</template>

<style scoped>
.lp { display: flex; flex-direction: column; gap: 2px; padding: 2px 0 4px 16px; font-size: 12.5px; }
.lp-add { display: flex; flex-wrap: wrap; gap: 4px; margin-bottom: 2px; }
.lp button { font: inherit; font-size: 12px; border: 1px solid var(--bb-line); background: var(--bb-sheet); color: var(--bb-ink); border-radius: 6px; padding: 1px 6px; cursor: pointer; }
.lp button:hover { background: var(--bb-sunk); }
.lp button:disabled { opacity: .5; cursor: default; background: var(--bb-sheet); }
.lp button:focus-visible, .lp input:focus-visible { outline: var(--bb-focus); outline-offset: 1px; }
.lp-row { display: flex; align-items: center; gap: 4px; min-width: 0; }
.lp-row .eye { border: 0; padding: 1px 2px; background: none; display: inline-flex; }
.lp-row .nm { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; flex: 1 1 auto; min-width: 0; }
.lp-row .k { color: var(--bb-stone); font-family: 'DM Mono', monospace; font-size: 11px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 40%; }
.lp-place { display: flex; flex-wrap: wrap; align-items: center; gap: 4px; padding-left: 22px; }
.lp-place label { display: inline-flex; align-items: center; gap: 2px; color: var(--bb-stone); }
.lp-place input { width: 58px; font: inherit; font-size: 12px; border: 1px solid var(--bb-line); border-radius: 4px; padding: 0 3px; background: var(--bb-sheet); color: var(--bb-ink); }
.lp-none { color: var(--bb-stone); }
.lp-place .d { color: var(--bb-stone); font-size: 11.5px; flex: 1 1 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.lp-place select { font: inherit; font-size: 11.5px; border: 1px solid var(--bb-line); border-radius: 4px; background: var(--bb-sheet); color: var(--bb-ink); max-width: 9em; }
.lp-log { max-height: 220px; overflow: auto; font-family: 'DM Mono', monospace; font-size: 11px; background: var(--bb-sunk); border-radius: 6px; padding: 4px 6px; margin: 2px 0 2px 22px; white-space: pre-wrap; }
.sw { width: 10px; height: 10px; border-radius: 2px; flex: none; }
.lp-err { font-size: 12px; color: var(--bb-ink); background: var(--bb-ochre, #e8c26a); padding: 2px 6px; border-radius: 4px; white-space: normal; }
</style>
