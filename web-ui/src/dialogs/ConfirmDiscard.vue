<script lang="ts">
export const hint = "files.discard.hint";
</script>

<script setup lang="ts">
// Ticket FS1: the one question before a route drops unsaved changes (files.ts confirmDiscard: new, open, import,
// close, a recent file, a drop). Two answers: discard them, or keep editing (the close button and Esc keep them too).
import { confirmRequest } from "../files";
import { state } from "../bimble";
import { t } from "../i18n";

const req = confirmRequest.value;
</script>

<template>
  <div class="bb-dialog cd" role="alertdialog" :aria-label="t('files.discard.title')" data-bb="confirm-discard">
    <div style="display:flex;align-items:center">
      <h2 style="font-size:22px">{{ t('files.discard.title') }}</h2><span style="flex:1"></span>
      <button class="ib" type="button" :aria-label="t('files.close')" data-bb="confirm-discard-close" @click="req?.resolve(false)"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg></button>
    </div>
    <p style="margin:0">{{ t('files.discard.body', { file: state.file ?? '' }) }}</p>
    <div style="display:flex;gap:10px;justify-content:flex-end">
      <button class="btn" type="button" data-bb="confirm-discard-no" @click="req?.resolve(false)">{{ t('files.discard.no') }}</button>
      <button class="btn p" type="button" data-bb="confirm-discard-yes" @click="req?.resolve(true)">{{ t('files.discard.yes') }}</button>
    </div>
  </div>
</template>

<style scoped>
.cd { width: 440px; max-width: 100%; box-sizing: border-box; }
h2 { font-family: 'Sora', sans-serif; margin: 0; }
.btn { display: inline-flex; align-items: center; justify-content: center; gap: 6px; min-height: 32px; padding: 5px 16px; border-radius: var(--bb-radius-pill); font: inherit; font-weight: 600; color: var(--bb-ink); cursor: pointer; white-space: nowrap; border: 1px solid var(--bb-stone); background: none; }
.btn.p { background: var(--bb-clay); border: 0; }
button:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
