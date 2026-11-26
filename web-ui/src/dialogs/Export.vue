<script lang="ts">
export const hint = "export.hint";
export const capability = "export_svg"; // FAKE1: the sheets drawn (sheet_svg)
// ---- PAPER1: File → Export view (egui "Export view… (PDF, SVG)", action on_paper) opens this dialog on "This view" ----
import { ref as vref } from "vue";
import { screenRoutes } from "../bimble";
import { currentDialog as dialogNow } from ".";
/** What the dialog exports: the sheets (Print and export) or one 2D view (Export view). */
export const exportWhat = vref<"sheets" | "view">("sheets");
screenRoutes.on_paper = () => { exportWhat.value = "view"; dialogNow.value = "export"; };
// ---- end PAPER1 ----
/** DXF1: the format the dialog opens on (File → Export this view to DXF… sets "dxf" before on_paper). */
export const exportFormat = vref<"pdf" | "svg" | "dxf" | null>(null);
</script>

<script setup lang="ts">
// Screen D4, Print and export (wireframes v01 fr/Export.html, ADR 0062). Lists the real sheets (`sheets`);
// Export downloads each checked sheet through a Blob (the web has no Rust file IO) as PDF (`sheet_pdf`, the
// C core's Sheet::to_pdf, ticket D7; named "<number> <name>.pdf" as the egui sheet export) or as SVG
// (`sheet_svg`). ADR 0052: no Cancel, one Export action; the close button closes.
// SIZE1: the PDF's font: Helvetica (the default, built into every PDF reader) or Roboto, offered only
// when the core has its files (`pdf_fonts`); otherwise shown disabled with where to put them.
import { computed, onUnmounted, ref } from "vue";
import { type PdfFont, pdfFonts, query, sheetPdf, sheetPdfName, sheets, state } from "../bimble";
import { openViews, sheetsPdf, sheetsPdfName, viewPdf, viewPdfName, viewSvg, viewSvgName } from "../bimble"; // PAPER1
import { dxfName, exportDxf } from "../bimble"; // DXF1
import { useCapability } from "../shell/capability"; // DXF1
import { viewName } from "../shell/viewName"; // PAPER1
import { t } from "../i18n";
import { currentDialog } from ".";

const list = computed(() => (state.value.rev, sheets()));
// ponytail: selection is panel state; starts with every sheet checked.
const picked = ref(new Set(list.value.map((s) => s.id)));
const toggle = (id: string) => { const p = new Set(picked.value); p.has(id) ? p.delete(id) : p.add(id); picked.value = p; };
const all = () => { picked.value = new Set(list.value.map((s) => s.id)); };
const n = computed(() => list.value.filter((s) => picked.value.has(s.id)).length);
// D7: PDF (the egui export's format) or SVG.
const format = ref<"pdf" | "svg" | "dxf">(exportFormat.value ?? "pdf");
exportFormat.value = null;
// DXF1: served by the core (`query capabilities` export_dxf: not in the viewer-only build).
const dxfServed = useCapability("export_dxf");
const allViews = ref(false); // DXF1: every 2D view, one file each
const done = ref<string | null>(null); // DXF1: the "exported" confirmation
const progress = ref<string | null>(null);
// SIZE1: the font choice (asked once, when the dialog opens).
const font = ref<PdfFont>("helvetica");
const roboto = pdfFonts().roboto;
/** DEAD1: a choice not served yet says so (its tooltip; data-not-yet for the tools). */
const ny = (name: string) => t("na.tooltip", { name });
const failed = ref<string | null>(null);
const busy = ref(false);

function download(name: string, blob: Blob) {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
}

// ---- PAPER1: "This view" (lib.rs plan_panel: a 2D view, the active one first) and "One PDF" ----
const views2d = computed(() => openViews.value.views.filter((v) => v.type === "plan" || v.type === "section"));
const viewId = ref<string | null>(null);
const theView = computed(() => views2d.value.find((v) => v.id === viewId.value) ?? views2d.value.find((v) => v.id === openViews.value.active) ?? views2d.value[0] ?? null);
const onePdf = ref(false);
const what = exportWhat;
onUnmounted(() => { exportWhat.value = "sheets"; }); // the next Print and export opens on the sheets
const ready = computed(() => (what.value === "view" ? !!theView.value : n.value > 0));
/** DXF1: the click's feedback is painted before the work starts (ADR 0074: within 200 ms). */
const painted = () => new Promise<void>((r) => requestAnimationFrame(() => setTimeout(r, 0)));
async function exportView() {
  const v = theView.value;
  if (!v) return;
  if (format.value === "dxf") {
    const list = allViews.value ? views2d.value : [v];
    let k = 0;
    for (const x of list) {
      progress.value = t("export.progress", { i: ++k, n: list.length });
      await painted();
      download(dxfName(viewName(x)), new Blob([(await exportDxf("view", x.id)).slice().buffer as ArrayBuffer], { type: "image/vnd.dxf" }));
    }
    done.value = t("export.done", { n: list.length });
    return;
  }
  if (format.value === "pdf") download(viewPdfName(v), new Blob([(await viewPdf(v.id, font.value)).slice().buffer as ArrayBuffer], { type: "application/pdf" }));
  else download(viewSvgName(v), new Blob([viewSvg(v.id).svg], { type: "image/svg+xml" }));
}
async function exportAll() {
  failed.value = null;
  done.value = null;
  progress.value = t("export.busy");
  busy.value = true;
  await painted();
  try {
    if (what.value === "view") await exportView();
    else if (onePdf.value && format.value === "pdf") {
      const chosen = list.value.filter((x) => picked.value.has(x.id));
      download(sheetsPdfName(chosen), new Blob([(await sheetsPdf(chosen.map((x) => x.id), font.value)).slice().buffer as ArrayBuffer], { type: "application/pdf" }));
    } else await exportSheets();
  } catch (e) {
    failed.value = e instanceof Error ? e.message : String(e);
  } finally {
    busy.value = false;
    progress.value = null;
  }
}
// ---- end PAPER1 ----

async function exportSheets() {
  failed.value = null;
  busy.value = true;
  try {
    for (const s of list.value.filter((x) => picked.value.has(x.id))) {
      if (format.value === "dxf") { // DXF1: the sheet in paper mm
        download(dxfName(`${s.number} ${s.name}`), new Blob([(await exportDxf("sheet", s.id)).slice().buffer as ArrayBuffer], { type: "image/vnd.dxf" }));
      } else if (format.value === "pdf") {
        const pdf = await sheetPdf(s.id, font.value);
        download(sheetPdfName(s), new Blob([pdf.slice().buffer as ArrayBuffer], { type: "application/pdf" }));
      } else {
        const { svg } = query<{ svg: string }>("sheet_svg", { id: s.id });
        download(`${s.number}-${s.name}.svg`, new Blob([svg], { type: "image/svg+xml" }));
      }
    }
    if (format.value === "dxf") done.value = t("export.done", { n: n.value });
  } catch (e) {
    failed.value = e instanceof Error ? e.message : String(e);
  } finally {
    busy.value = false;
  }
}
</script>

<template>
  <div class="bb-dialog ex" role="dialog" :aria-label="t('export.title')" data-bb="export">
    <div style="display:flex;align-items:center">
      <h2 style="font-size:22px">{{ t('export.title') }}</h2><span style="flex:1"></span>
      <button class="ib" type="button" :aria-label="t('export.close')" data-bb="export-close" @click="currentDialog = null"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button>
    </div>
    <!-- PAPER1: the sheets, or one view (File → Export view) -->
    <span class="seg" style="align-self:flex-start" data-bb="export-what">
      <button type="button" :class="{ on: what === 'sheets' }" :aria-pressed="what === 'sheets'" data-bb="export-what-sheets" @click="what = 'sheets'">{{ t('export.whatSheets') }}</button>
      <button type="button" :class="{ on: what === 'view' }" :aria-pressed="what === 'view'" data-bb="export-what-view" @click="what = 'view'">{{ t('export.whatView') }}</button>
    </span>
    <div style="display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1.1fr);gap:20px">
      <div v-if="what === 'view'" class="fld" data-bb="export-view">
        <label class="lbl" for="ex-view">{{ t('export.view') }}</label>
        <select id="ex-view" class="inp" :value="theView?.id ?? ''" data-bb="export-view-select" @change="viewId = ($event.target as HTMLSelectElement).value">
          <option v-for="v in views2d" :key="v.id" :value="v.id">{{ viewName(v) }} · 1:{{ v.scale }}</option>
        </select>
        <span v-if="!views2d.length" class="note" data-bb="export-view-none">{{ t('export.noView') }}</span>
        <span v-else class="note">{{ t('export.viewNote') }}</span>
        <span class="note" data-missing="plan_panel: the storey plan without a view, the paper and template choices">{{ t('export.storeyPlan') }}</span>
        <label v-if="format === 'dxf'" class="note" style="display:inline-flex;gap:6px;align-items:center" data-bb="export-all-views"><input type="checkbox" :checked="allViews" @change="allViews = !allViews">{{ t('export.allViews') }} · {{ t('export.dxfViews') }}</label>
      </div>
      <div v-else style="display:flex;flex-direction:column;gap:6px">
        <div class="lbl">{{ t('export.sheets', { n, total: list.length }) }}</div>
        <div class="card" style="padding:8px;display:flex;flex-direction:column;gap:2px" data-bb="export-sheets">
          <label v-for="s in list" :key="s.id" class="row" style="padding:4px 6px;border-radius:8px" :class="{ on: picked.has(s.id) }">
            <span style="display:inline-flex;gap:8px;align-items:center"><input type="checkbox" :checked="picked.has(s.id)" @change="toggle(s.id)"><span class="mono" style="font-size:12.5px">{{ s.number }}</span>{{ s.name }}</span>
            <span class="mono" style="font-size:12px;color:var(--bb-stone)">{{ [s.paper, s.fields.revision].filter(Boolean).join(' · ') }}</span>
          </label>
        </div>
        <div style="display:flex;gap:8px">
          <button class="btn s" type="button" @click="all">{{ t('export.all') }}</button>
          <button class="btn s" type="button" disabled data-not-yet :title="ny(t('export.revisionOnly'))" data-missing="sheets: a revision filter (current revision per sheet)">{{ t('export.revisionOnly') }}</button>
        </div>
      </div>
      <div style="display:flex;flex-direction:column;gap:10px">
        <div class="fld"><span class="lbl">{{ t('export.format') }}</span>
          <span class="seg" style="align-self:flex-start" data-bb="export-format">
            <button type="button" :class="{ on: format === 'pdf' }" :aria-pressed="format === 'pdf'" data-bb="export-pdf" @click="format = 'pdf'">PDF</button>
            <button type="button" :class="{ on: format === 'svg' }" :aria-pressed="format === 'svg'" data-bb="export-svg" @click="format = 'svg'">SVG</button>
            <button v-if="dxfServed" type="button" :class="{ on: format === 'dxf' }" :aria-pressed="format === 'dxf'" data-bb="export-dxf" @click="format = 'dxf'">DXF</button>
            <button v-else type="button" disabled data-not-yet :title="ny('DXF')" data-missing="view_dxf, sheet_dxf: not in this build">DXF</button>
            <button type="button" disabled data-not-yet :title="ny(t('export.printer'))" data-missing="print: no printer on the web">{{ t('export.printer') }}</button>
          </span></div>
        <div v-if="format === 'pdf'" class="fld"><span class="lbl">{{ t('export.font') }}</span>
          <span class="seg" style="align-self:flex-start" data-bb="export-font">
            <button type="button" :class="{ on: font === 'helvetica' }" :aria-pressed="font === 'helvetica'" data-bb="export-font-helvetica" @click="font = 'helvetica'">Helvetica</button>
            <button type="button" :class="{ on: font === 'roboto' }" :aria-pressed="font === 'roboto'" :disabled="!roboto" :title="roboto ? undefined : t('export.fontRobotoMissing')" data-bb="export-font-roboto" @click="font = 'roboto'">Roboto</button>
          </span>
          <span v-if="!roboto" class="note" data-bb="export-font-note">{{ t('export.fontRobotoMissing') }}</span></div>
        <div v-if="what === 'sheets'" class="fld"><span class="lbl">{{ t('export.files') }}</span>
          <span class="seg" style="align-self:flex-start" data-bb="export-files"><!-- PAPER1: One PDF (sheets_pdf) -->
            <button type="button" :class="{ on: !onePdf || format !== 'pdf' }" :aria-pressed="!onePdf || format !== 'pdf'" data-bb="export-per-sheet" @click="onePdf = false">{{ t('export.perSheet') }}</button>
            <button type="button" :class="{ on: onePdf && format === 'pdf' }" :aria-pressed="onePdf && format === 'pdf'" :disabled="format !== 'pdf'" data-bb="export-one-pdf" @click="onePdf = true">{{ t('export.onePdf') }}</button>
          </span></div>
        <div class="fld"><label class="lbl" for="ex-names">{{ t('export.names') }}</label>
          <input id="ex-names" class="inp mono" value="{project_number}-{level}-{sheet_number}-{revision}-{sheet_name}" disabled data-not-yet :title="ny(t('export.names'))" data-missing="export_name_template: a file-name template in Rust"></div>
        <div class="note" data-missing="export_name_template: the example name">{{ t('export.example') }}<span class="mono" style="color:var(--bb-ink);font-size:12.5px">—</span></div>
        <div class="row" data-missing="standards: the pen set used by the export"><span class="k">{{ t('export.pens') }}</span><span>—</span></div>
        <div class="row" data-missing="sheet_svg: a colour option"><span class="k">{{ t('export.colour') }}</span><span>—</span></div>
        <div class="fld"><label class="lbl" for="ex-dir">{{ t('export.folder') }}</label>
          <div style="display:flex;gap:8px"><input id="ex-dir" class="inp mono" style="flex:1" value="" disabled data-not-yet :title="ny(t('export.folder'))" data-missing="export folder: the web downloads, no Rust file IO"><button class="btn s" type="button" disabled data-not-yet :title="ny(t('export.browse'))" data-missing="export folder: the web downloads, no Rust file IO">{{ t('export.browse') }}</button></div></div>
      </div>
    </div>
    <div style="display:flex;gap:10px;align-items:center;justify-content:flex-end">
      <span v-if="failed" class="note err" role="alert" data-bb="export-failed">{{ t('export.failed', { e: failed }) }}</span>
      <span v-else-if="progress" class="note" role="status" data-bb="export-progress">{{ progress }}</span>
      <span v-else-if="done" class="note" role="status" data-bb="export-done">{{ done }}</span>
      <span v-else-if="format === 'dxf'" class="note">{{ t('export.dxfNote') }}</span>
      <span v-else class="note">{{ t('export.background') }}</span>
      <button class="btn" :class="ready ? 'p' : 'd'" type="button" :disabled="!ready || busy" :aria-busy="busy" data-bb="export-go" @click="exportAll">{{ what === 'view' ? (format === 'dxf' && allViews ? t('export.goViews', { n: views2d.length }) : t('export.goView')) : t('export.go', { n }) }}</button>
    </div>
  </div>
</template>

<style scoped>
/* .dialog{...} style="width:860px" */
.ex { width: 860px; max-width: 100%; box-sizing: border-box; }
h2 { font-family: 'Sora', sans-serif; margin: 0; }
.lbl { font-size: 13px; color: var(--bb-stone); }
.card { background: var(--bb-sheet); border: 1px solid var(--bb-line); border-radius: 16px; }
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.row.on { background: var(--bb-pink); }
.k { color: var(--bb-stone); white-space: nowrap; }
.seg { display: inline-flex; gap: 2px; padding: 2px; border-radius: 999px; background: var(--bb-sunk); }
.seg button { padding: 2px 12px; border-radius: 999px; color: var(--bb-stone); font: inherit; font-size: 13px; border: 0; background: none; cursor: pointer; white-space: nowrap; }
.seg button:disabled { cursor: default; }
.seg .on { background: var(--bb-ink); color: var(--bb-cream); }
.fld { display: flex; flex-direction: column; gap: 3px; }
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; }
/* Text on the selected (pink) row is ink, not stone (ADR 0049); !important beats the inline quiet colour. */
.on > .mono { color: var(--bb-ink) !important; }
.inp.mono { font-family: 'DM Mono', monospace; font-size: 13px; }
.note { font-size: 13px; color: var(--bb-stone); }
.note.err { color: var(--bb-error); }
button:focus-visible, input:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
