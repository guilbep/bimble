<script lang="ts">
export const capability = "sheets"; // VIEWSHEET: verb set_sheet {revision} (Rust's, dispatch.rs:105)
</script>

<script setup lang="ts">
// Sheets → Revision… (ticket VIEWSHEET, ADR 0076): the sheet's revision index, the title block's {revision}
// (Rust v0.19.0 keeps one per sheet: dispatch.rs set_sheet, template.rs:189 REVISION), set by one set_sheet
// (one undo step; empty removes it). A revision table (date, text, author) is not in Rust nor here yet: said so.
import { ref } from "vue";
import { runVerb } from "../bimble";
import { t } from "../i18n";
import { currentSheet } from "../pages/SheetsSide.vue";
import { currentDialog } from ".";

const value = ref(currentSheet.value?.fields.revision ?? "");
const error = ref<string | null>(null);
const close = () => { currentDialog.value = null; };
function apply(): void {
  const s = currentSheet.value;
  if (!s) return;
  error.value = runVerb("set_sheet", { id: s.id, revision: value.value.trim() });
  if (!error.value) close();
}
</script>

<template>
  <div class="bb-dialog rv" role="dialog" aria-modal="true" :aria-label="t('vs.revision.title')" data-bb="sheet-revision">
    <div class="head">
      <h2 class="disp">{{ t('vs.revision.title') }}<span v-if="currentSheet" class="mono sub"> · {{ currentSheet.number }} {{ currentSheet.name }}</span></h2>
      <span style="flex:1"></span>
      <button class="btn s" type="button" data-bb="sheet-revision-close" @click="close">{{ t('vs.close') }}</button>
    </div>
    <label class="k" for="rv-index">{{ t('vs.revision.label', { revision: '{revision}' }) }}</label>
    <div style="display:flex;gap:8px">
      <input id="rv-index" v-model="value" class="inp mono" maxlength="200" data-bb="sheet-revision-index" @keydown.enter="apply">
      <button class="btn p" type="button" data-bb="sheet-revision-apply" @click="apply">{{ t('vs.revision.set') }}</button>
    </div>
    <p class="note">{{ t('vs.revision.note') }}</p>
    <p v-if="error" class="err" role="alert" data-bb="sheet-revision-error">{{ error }}</p>
  </div>
</template>

<style scoped>
.rv { width: min(420px, calc(100vw - 32px)); color: var(--bb-ink); display: flex; flex-direction: column; gap: 10px; }
.head { display: flex; align-items: center; gap: 10px; }
.disp { font-family: var(--bb-font-display); margin: 0; font-size: var(--bb-size-heading); font-weight: 700; }
.sub { font-size: 13px; font-weight: 400; color: var(--bb-stone); }
.mono { font-family: var(--bb-font-data); }
.k { color: var(--bb-stone); font-size: 13px; }
.inp { height: 30px; width: 120px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font-size: 14px; color: var(--bb-ink); }
.note { margin: 0; font-size: var(--bb-size-small); color: var(--bb-stone); }
.err { margin: 0; color: var(--bb-attention); font-size: 13px; }
:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
