<script setup lang="ts">
// The shell's own states (ticket UX1, shell/machine.ts) over the screen they came from: a file
// being opened (Cancel: a picker may never answer), the unsaved-changes question (Discard or
// Cancel), an error with the session's words (Back). Each button is one `send`.
import { computed } from "vue";
import { t } from "../i18n";
import { here, send, shown } from "./send";
import { fileRequest } from "../files"; // FS1: the page's own file dialog has its own close

const s = computed(() => here.value);
</script>

<template>
  <div v-if="(s.at === 'opening' && !fileRequest) || s.at === 'confirmClose' || s.at === 'error'" class="shell-backdrop">
    <div v-if="s.at === 'opening'" class="bb-dialog shell-card" role="status" data-bb="shell-opening">
      <p class="msg">{{ t(`shellstate.opening.${s.what}`) }}</p>
      <div class="acts">
        <button v-if="shown('shell-opening-cancel')" class="btn s" type="button" data-bb="shell-opening-cancel" @click="send({ do: 'cancel' })">{{ t('shellstate.cancel') }}</button>
      </div>
    </div>
    <div v-else-if="s.at === 'confirmClose'" class="bb-dialog shell-card" role="alertdialog" :aria-label="t('shellstate.confirm.title')" data-bb="shell-confirm">
      <h2 class="title">{{ t('shellstate.confirm.title') }}</h2>
      <p class="msg">{{ t('shellstate.confirm.text') }}</p>
      <div class="acts">
        <button v-if="shown('shell-confirm-cancel')" class="btn s" type="button" data-bb="shell-confirm-cancel" @click="send({ do: 'cancel' })">{{ t('shellstate.cancel') }}</button>
        <button v-if="shown('shell-confirm-discard')" class="btn p" type="button" data-bb="shell-confirm-discard" @click="send({ do: 'discard' })">{{ t('shellstate.confirm.discard') }}</button>
      </div>
    </div>
    <div v-else class="bb-dialog shell-card" role="alertdialog" :aria-label="t('shellstate.error.title')" data-bb="shell-error">
      <h2 class="title">{{ t('shellstate.error.title') }}</h2>
      <p class="msg mono">{{ s.message }}</p>
      <div class="acts">
        <button v-if="shown('shell-error-back')" class="btn p" type="button" data-bb="shell-error-back" @click="send({ do: 'back' })">{{ t('shellstate.error.back') }}</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.shell-backdrop { position: fixed; inset: 0; z-index: 90; display: grid; place-items: center; background: color-mix(in srgb, var(--bb-ink) 30%, transparent); }
.shell-card { max-width: 480px; }
.title { margin: 0; font-family: 'Sora', 'Source Sans 3', sans-serif; font-size: 20px; font-weight: 700; }
.msg { margin: 0; font-size: 14px; overflow-wrap: anywhere; }
.mono { font-family: 'DM Mono', ui-monospace, Menlo, monospace; font-size: 13px; }
.acts { display: flex; gap: 10px; justify-content: flex-end; }
</style>
