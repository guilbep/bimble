<script lang="ts">
export const hint = "export.hint";
</script>

<script setup lang="ts">
// Screen D4, Print and export (wireframes v01 fr/Export.html, ADR 0062). Lists the real sheets (`sheets`);
// Export downloads each checked sheet through a Blob (the web has no Rust file IO) as PDF (`sheet_pdf`, the
// C core's Sheet::to_pdf, ticket D7; named "<number> <name>.pdf" as the egui sheet export) or as SVG
// (`sheet_svg`). ADR 0052: no Cancel, one Export action; the close button closes.
// SIZE1: the PDF's font: Helvetica (the default, built into every PDF reader) or Roboto, offered only
// when the core has its files (`pdf_fonts`); otherwise shown disabled with where to put them.
import { computed, ref } from "vue";
import { type PdfFont, pdfFonts, query, sheetPdf, sheetPdfName, sheets, state } from "../bimble";
import { t } from "../i18n";
import { currentDialog } from ".";

const list = computed(() => (state.value.rev, sheets()));
// ponytail: selection is panel state; starts with every sheet checked.
const picked = ref(new Set(list.value.map((s) => s.id)));
const toggle = (id: string) => { const p = new Set(picked.value); p.has(id) ? p.delete(id) : p.add(id); picked.value = p; };
const all = () => { picked.value = new Set(list.value.map((s) => s.id)); };
const n = computed(() => list.value.filter((s) => picked.value.has(s.id)).length);
// D7: PDF (the egui export's format) or SVG.
const format = ref<"pdf" | "svg">("pdf");
// SIZE1: the font choice (asked once, when the dialog opens).
const font = ref<PdfFont>("helvetica");
const roboto = pdfFonts().roboto;
const failed = ref<string | null>(null);
const busy = ref(false);

function download(name: string, blob: Blob) {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 10000);
}

async function exportSheets() {
  failed.value = null;
  busy.value = true;
  try {
    for (const s of list.value.filter((x) => picked.value.has(x.id))) {
      if (format.value === "pdf") {
        const pdf = await sheetPdf(s.id, font.value);
        download(sheetPdfName(s), new Blob([pdf.slice().buffer as ArrayBuffer], { type: "application/pdf" }));
      } else {
        const { svg } = query<{ svg: string }>("sheet_svg", { id: s.id });
        download(`${s.number}-${s.name}.svg`, new Blob([svg], { type: "image/svg+xml" }));
      }
    }
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
    <div style="display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1.1fr);gap:20px">
      <div style="display:flex;flex-direction:column;gap:6px">
        <div class="lbl">{{ t('export.sheets', { n, total: list.length }) }}</div>
        <div class="card" style="padding:8px;display:flex;flex-direction:column;gap:2px" data-bb="export-sheets">
          <label v-for="s in list" :key="s.id" class="row" style="padding:4px 6px;border-radius:8px" :class="{ on: picked.has(s.id) }">
            <span style="display:inline-flex;gap:8px;align-items:center"><input type="checkbox" :checked="picked.has(s.id)" @change="toggle(s.id)"><span class="mono" style="font-size:12.5px">{{ s.number }}</span>{{ s.name }}</span>
            <span class="mono" style="font-size:12px;color:var(--bb-stone)">{{ [s.paper, s.fields.revision].filter(Boolean).join(' · ') }}</span>
          </label>
        </div>
        <div style="display:flex;gap:8px">
          <button class="btn s" type="button" @click="all">{{ t('export.all') }}</button>
          <button class="btn s" type="button" disabled data-missing="sheets: a revision filter (current revision per sheet)">{{ t('export.revisionOnly') }}</button>
        </div>
      </div>
      <div style="display:flex;flex-direction:column;gap:10px">
        <div class="fld"><span class="lbl">{{ t('export.format') }}</span>
          <span class="seg" style="align-self:flex-start" data-bb="export-format">
            <button type="button" :class="{ on: format === 'pdf' }" :aria-pressed="format === 'pdf'" data-bb="export-pdf" @click="format = 'pdf'">PDF</button>
            <button type="button" :class="{ on: format === 'svg' }" :aria-pressed="format === 'svg'" data-bb="export-svg" @click="format = 'svg'">SVG</button>
            <button type="button" disabled data-missing="sheet_dxf: a DXF sheet export query">DXF</button>
            <button type="button" disabled data-missing="print: no printer on the web">{{ t('export.printer') }}</button>
          </span></div>
        <div v-if="format === 'pdf'" class="fld"><span class="lbl">{{ t('export.font') }}</span>
          <span class="seg" style="align-self:flex-start" data-bb="export-font">
            <button type="button" :class="{ on: font === 'helvetica' }" :aria-pressed="font === 'helvetica'" data-bb="export-font-helvetica" @click="font = 'helvetica'">Helvetica</button>
            <button type="button" :class="{ on: font === 'roboto' }" :aria-pressed="font === 'roboto'" :disabled="!roboto" data-bb="export-font-roboto" @click="font = 'roboto'">Roboto</button>
          </span>
          <span v-if="!roboto" class="note" data-bb="export-font-note">{{ t('export.fontRobotoMissing') }}</span></div>
        <div class="fld"><span class="lbl">{{ t('export.files') }}</span>
          <span class="seg" style="align-self:flex-start">
            <button type="button" class="on">{{ t('export.perSheet') }}</button>
            <button type="button" disabled data-missing="sheet_pdf: a multi-sheet PDF export">{{ t('export.onePdf') }}</button>
          </span></div>
        <div class="fld"><label class="lbl" for="ex-names">{{ t('export.names') }}</label>
          <input id="ex-names" class="inp mono" value="{project_number}-{level}-{sheet_number}-{revision}-{sheet_name}" disabled data-missing="export_name_template: a file-name template in Rust"></div>
        <div class="note" data-missing="export_name_template: the example name">{{ t('export.example') }}<span class="mono" style="color:var(--bb-ink);font-size:12.5px">—</span></div>
        <div class="row" data-missing="standards: the pen set used by the export"><span class="k">{{ t('export.pens') }}</span><span>—</span></div>
        <div class="row" data-missing="sheet_svg: a colour option"><span class="k">{{ t('export.colour') }}</span><span>—</span></div>
        <div class="fld"><label class="lbl" for="ex-dir">{{ t('export.folder') }}</label>
          <div style="display:flex;gap:8px"><input id="ex-dir" class="inp mono" style="flex:1" value="" disabled data-missing="export folder: the web downloads, no Rust file IO"><button class="btn s" type="button" disabled data-missing="export folder: the web downloads, no Rust file IO">{{ t('export.browse') }}</button></div></div>
      </div>
    </div>
    <div style="display:flex;gap:10px;align-items:center;justify-content:flex-end">
      <span v-if="failed" class="note err" role="alert" data-bb="export-failed">{{ t('export.failed', { e: failed }) }}</span>
      <span v-else class="note">{{ t('export.background') }}</span>
      <button class="btn" :class="n ? 'p' : 'd'" type="button" :disabled="!n || busy" data-bb="export-go" @click="exportSheets">{{ t('export.go', { n }) }}</button>
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
