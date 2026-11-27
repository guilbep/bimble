<script setup lang="ts">
// The shell's own states (ticket UX1, shell/machine.ts) over the screen they came from: a file
// being opened (Cancel: a picker may never answer), the unsaved-changes question (Discard or
// Cancel), an error with the session's words (Back). Each button is one `send`.
import { computed, ref } from "vue";
import { t } from "../i18n";
import { here, openStep, send, shown } from "./send";
import { fileRequest } from "../files"; // FS1: the page's own file dialog has its own close
import { recentFiles, retryConnection, state } from "../bimble"; // FIXUI: the connection notices
import { ask, copyText, formatBytes, progress } from "../openurl"; // URL1: a model from a link

const s = computed(() => here.value);
// PERF2: the open's step (read, parse, scene, mesh), shown once a file is being opened
const step = computed(() => (openStep.value && openStep.value.of === here.value ? openStep.value : null));
// FIXUI: the file to reopen after a lost session: the recent entry of the same name (the host's settings).
const reopen = computed(() => {
  const v = here.value;
  if (v.at !== "sessionLost" || v.file === null) return null;
  return recentFiles().find((r) => r.name === v.file && r.exists) ?? null;
});
function retry(): void { if (send({ do: "retry" })) retryConnection(); }
// ---- URL1 (ADR 0086): the link's host, its question, its download ----
const hostOf = (u: string) => { try { return new URL(u).host; } catch { return u.slice(0, 80); } };
const linkAsk = computed(() => (s.value.at === "opening" && s.value.url !== undefined && ask.value && ask.value.url === s.value.url ? ask.value : null));
const linkBytes = computed(() => (s.value.at === "opening" && s.value.url !== undefined && progress.value && progress.value.url === s.value.url ? progress.value : null));
const copied = ref(false);
async function copyUrl(u: string): Promise<void> { copied.value = await copyText(u); }
// ---- end URL1 ----
</script>

<template>
  <div v-if="(s.at === 'opening' && !fileRequest) || s.at === 'confirmClose' || s.at === 'error' || s.at === 'disconnected' || s.at === 'sessionLost'" class="shell-backdrop">
    <!-- FIXUI: the native host does not answer: blocking, with what is known, and Retry. -->
    <div v-if="s.at === 'disconnected'" class="bb-dialog shell-card" role="alertdialog" :aria-label="t('shellstate.lost.title')" data-bb="shell-disconnected">
      <h2 class="title">{{ t('shellstate.lost.title') }}</h2>
      <p class="msg">{{ s.file === null ? t('shellstate.lost.noFile') : s.unsaved ? t('shellstate.lost.unsaved', { file: s.file }) : t('shellstate.lost.saved', { file: s.file }) }}</p>
      <p class="msg step">{{ t('shellstate.lost.trying') }}</p>
      <div class="acts">
        <button v-if="shown('shell-disconnected-retry')" class="btn p" type="button" data-bb="shell-disconnected-retry" @click="retry">{{ t('shellstate.lost.retry') }}</button>
      </div>
    </div>
    <div v-else-if="s.at === 'sessionLost'" class="bb-dialog shell-card" role="alertdialog" :aria-label="t('shellstate.gone.title')" data-bb="shell-session-lost">
      <h2 class="title">{{ t('shellstate.gone.title') }}</h2>
      <p class="msg">{{ s.file === null ? t('shellstate.gone.noFile') : s.unsaved ? t('shellstate.gone.unsaved', { file: s.file }) : t('shellstate.gone.saved', { file: s.file }) }}</p>
      <p v-if="state.file !== null" class="msg step">{{ t('shellstate.gone.hostHas', { file: state.file }) }}</p>
      <div class="acts">
        <button v-if="shown('shell-lost-home')" class="btn s" type="button" data-bb="shell-lost-home" @click="send({ do: 'home' })">{{ t('shellstate.gone.home') }}</button>
        <button v-if="shown('shell-lost-show')" class="btn s" type="button" data-bb="shell-lost-show" @click="send({ do: 'showView' })">{{ t('shellstate.gone.show', { file: state.file ?? '' }) }}</button>
        <button v-if="reopen && shown('shell-lost-reopen')" class="btn p" type="button" data-bb="shell-lost-reopen" @click="send({ do: 'open', path: reopen.path })">{{ t('shellstate.gone.reopen', { file: reopen.name }) }}</button>
      </div>
    </div>
    <!-- URL1: another site's link asks first (openurl.ts ask); Cancel is the shell's -->
    <div v-else-if="s.at === 'opening' && linkAsk" class="bb-dialog shell-card" role="alertdialog" :aria-label="t('openurl.ask.title', { host: linkAsk.host })" data-bb="shell-openurl-ask">
      <h2 class="title">{{ t('openurl.ask.title', { host: linkAsk.host }) }}</h2>
      <p class="msg mono" data-bb="shell-openurl-address">{{ linkAsk.url }}</p>
      <p class="msg step">{{ t('openurl.ask.text') }}</p>
      <div class="acts">
        <button v-if="shown('shell-opening-cancel')" class="btn s" type="button" data-bb="shell-opening-cancel" @click="send({ do: 'cancel' })">{{ t('shellstate.cancel') }}</button>
        <button class="btn p" type="button" data-bb="shell-openurl-allow" @click="linkAsk.answer(true)">{{ t('openurl.ask.open') }}</button>
      </div>
    </div>
    <div v-else-if="s.at === 'opening'" class="bb-dialog shell-card" role="status" data-bb="shell-opening" :data-url="s.url">
      <p class="msg">{{ s.url !== undefined ? t('openurl.opening', { host: hostOf(s.url) }) : t(`shellstate.opening.${s.what}`) }}</p>
      <template v-if="linkBytes && !step"><!-- URL1: the download, before the open's steps -->
        <progress class="bar" :value="linkBytes.total ? linkBytes.loaded : undefined" :max="linkBytes.total ?? undefined" data-bb="shell-openurl-progress" />
        <p class="msg step" data-bb="shell-openurl-bytes">{{ linkBytes.total && linkBytes.loaded <= linkBytes.total ? t('openurl.progress.of', { loaded: formatBytes(linkBytes.loaded), total: formatBytes(linkBytes.total) }) : t('openurl.progress.sofar', { loaded: formatBytes(linkBytes.loaded) }) }}</p>
      </template>
      <template v-if="step"><!-- PERF2 -->
        <progress class="bar" :value="step.n" :max="step.total" data-bb="shell-opening-progress" :aria-label="t('shellstate.step.of', { n: step.n, of: step.total })" />
        <p class="msg step" data-bb="shell-opening-step">{{ t(`shellstate.step.${step.step}` as 'shellstate.step.read') }}</p>
      </template>
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
    <div v-else class="bb-dialog shell-card" role="alertdialog" :aria-label="t(s.url !== undefined ? 'openurl.error.title' : 'shellstate.error.title')" data-bb="shell-error">
      <h2 class="title">{{ t(s.url !== undefined ? 'openurl.error.title' : 'shellstate.error.title') }}</h2>
      <p class="msg" :class="{ mono: s.url === undefined }" data-bb="shell-error-message">{{ s.message }}</p>
      <p v-if="s.url !== undefined" class="msg mono step" data-bb="shell-error-url">{{ s.url }}</p>
      <div class="acts">
        <button v-if="shown('shell-error-back')" class="btn p" type="button" data-bb="shell-error-back" @click="send({ do: 'back' })">{{ t('shellstate.error.back') }}</button>
        <!-- URL1: a link's error -->
        <button v-if="s.url !== undefined" class="btn s" type="button" data-bb="shell-error-copy" @click="copyUrl(s.url)">{{ copied ? t('openurl.error.copied') : t('openurl.error.copy') }}</button>
        <button v-if="shown('shell-error-home')" class="btn s" type="button" data-bb="shell-error-home" @click="send({ do: 'home' })">{{ t('openurl.error.home') }}</button>
        <button v-if="s.url !== undefined && shown('shell-error-retry')" class="btn p" type="button" data-bb="shell-error-retry" @click="copied = false, send({ do: 'open', url: s.url })">{{ t('openurl.error.retry') }}</button>
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
.bar { width: 100%; height: 6px; accent-color: var(--bb-ink); }
.step { font-size: 13px; color: var(--bb-stone); }
.acts { display: flex; gap: 10px; justify-content: flex-end; flex-wrap: wrap; }
</style>
