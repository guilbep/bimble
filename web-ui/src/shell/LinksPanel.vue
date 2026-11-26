<script setup lang="ts">
// The Navigator's Links group with its buttons (ticket LNK1; Rust: lib.rs:4402 the side panel's
// Links group, links.rs:85 Manage links). A row per linked model (IFC, .bimble, .bimbleview) and per
// reference plan (PDF, DXF, with the view it is placed in): show / hide, reload, reload from…,
// remove; a reference also moves by numbers (mm) and turns (degrees), one undo step each. The calls
// are shell/links.ts's; errors show under the rows.
import { computed, reactive, watch } from "vue";
import { t } from "../i18n";
import { state } from "../bimble";
import { enabled, linkNew, linksBusy, linksError, linksPanel, modelOp, modelReload, refOp, refReload, type PanelReference } from "./links";

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
const can = computed(() => { void state.value; return { model: enabled("link_model"), pdf: enabled("link_pdf"), dxf: enabled("link_dxf") }; });
</script>

<template>
  <div class="lp" data-bb="links-panel">
    <div class="lp-add">
      <button type="button" data-bb="link-add-model" :disabled="!can.model || linksBusy" :title="t('links.addModelTip')" @click="linkNew('link_model')">{{ t('links.addModel') }}</button>
      <button type="button" data-bb="link-add-pdf" :disabled="!can.pdf || linksBusy" :title="can.pdf ? t('links.addPdfTip') : t('links.in2d')" @click="linkNew('link_pdf')">{{ t('links.addPdf') }}</button>
      <button type="button" data-bb="link-add-dxf" :disabled="!can.dxf || linksBusy" :title="can.dxf ? t('links.addDxfTip') : t('links.in2d')" @click="linkNew('link_dxf')">{{ t('links.addDxf') }}</button>
    </div>
    <div v-if="!rows.models.length && !rows.references.length" class="lp-none">{{ t('links.none') }}</div>
    <div v-for="m in rows.models" :key="'m' + m.name" class="lp-row" data-bb="link-row" :data-name="m.name" :data-visible="m.visible ? '1' : '0'" :data-missing="m.missing ? '1' : '0'">
      <button type="button" class="eye" data-bb="link-eye" :aria-pressed="m.visible" :disabled="m.missing" :title="m.visible ? t('links.hide') : t('links.show')" @click="modelOp(m.visible ? 'hide' : 'show', m.name)">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z"/><circle v-if="m.visible" cx="12" cy="12" r="3"/><path v-else d="m3 3 18 18"/></svg>
      </button>
      <span class="nm" :title="m.path">{{ m.name }}</span>
      <span class="k">{{ m.missing ? t('links.missing') : m.kind }}</span>
      <button v-if="m.missing" type="button" data-bb="link-locate" :title="t('links.locateTip')" @click="modelReload(m, true)">{{ t('links.locate') }}</button>
      <template v-else>
        <button type="button" data-bb="link-reload" :title="t('links.reloadTip')" @click="modelReload(m)">{{ t('links.reload') }}</button>
        <button type="button" data-bb="link-reload-from" :title="t('links.reloadFromTip')" @click="modelReload(m, true)">…</button>
      </template>
      <button type="button" data-bb="link-remove" :title="t('links.remove')" @click="modelOp('remove', m.name)">×</button>
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
.lp-err { font-size: 12px; color: var(--bb-ink); background: var(--bb-ochre, #e8c26a); padding: 2px 6px; border-radius: 4px; white-space: normal; }
</style>
