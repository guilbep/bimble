<script lang="ts">
export const layout = "full";
import { state as st } from "../bimble";
// A1: "Aucun modèle ouvert" with no model; with a model open, Home shows no hint (a string = already translated).
export const hint = () => (st.value.file === null ? { key: "home.noModel" } : "");
</script>

<script setup lang="ts">
// A1 Home (fr/Home.html). Layout only; every button is one runAction (ADR 0048).
import { computed, ref } from "vue";
import { state, actions, recentFiles } from "../bimble";
import { t } from "../i18n";
import { act as run, send, shown as control } from "../shell/send";
import NotAvailable from "../shell/NotAvailable.vue"; // FAKE1
import { useCapability, INSTEAD } from "../shell/capability"; // FAKE1

/** Whether the action `name` is enabled now (`query("actions")`). */
const on = (name: string) => actions.value.some((a) => a.name === name && a.enabled);
// New, Open and Import go to the shell (shell/send.ts screenRoutes); a refusal is shown, not dropped.
const act = (name: string) => { if (on(name)) run(name); };
// Re-read when the state changes (a file opened adds itself to the list).
const recent = computed(() => (state.value, hasRecent.value ? recentFiles() : [])); // FAKE1: not asked when not served
const filter = ref("");
const shown = computed(() => recent.value.filter((f) => f.name.toLowerCase().includes(filter.value.trim().toLowerCase())));
// ---- FAKE1: the user line and the recent files only where the core serves them (capabilities
// `user`, `recent_files`: FS1 and SET1 turn them on in the core, and these light up by themselves) ----
const hasUser = useCapability("user"), hasRecent = useCapability("recent_files");
// ---- end FAKE1 ----
</script>

<template>
  <!-- The reference's body under the header and above the status bar: the whole window, not the viewport slot. -->
  <div class="home" data-bb="home">
    <div class="disc" aria-hidden="true" data-bb="home-disc"></div>
    <div class="left">
      <!-- Pierre (DECISIONS #6): the product's name in the wordmark font, out of flow above the eyebrow. -->
      <div class="wordmark" data-bb="home-wordmark">bimble</div>
      <!-- major.minor, as the reference ("Bimble 0.15 · home"). -->
      <div class="eyebrow" data-bb="home-eyebrow">{{ t('home.eyebrow', { version: (state.version ?? '').split('.').slice(0, 2).join('.') }) }}</div>
      <h1 class="disp" data-bb="home-title">{{ t('home.hello', { name: state.user?.name ?? '' }) }}</h1>
      <p class="lead" data-bb="home-lead">{{ t('home.lead') }}</p>
      <div class="acts">
        <a class="btn p" href="#" data-bb="home-open" :aria-disabled="!on('open') || undefined" @click.prevent="act('open')"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M3 7h6l2 2h10v10H3z"/><path d="M3 7V5h6"/></svg> {{ t('home.open') }}</a>
        <button class="btn s" type="button" data-bb="home-import" :disabled="!on('import_ifc')" @click="act('import_ifc')"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3v12"/><path d="m7 10 5 5 5-5"/><path d="M5 13v8h14v-8"/></svg>{{ t('home.importIfc') }}</button>
        <button v-if="control('home-new')" class="btn s" type="button" data-bb="home-new" :disabled="!on('new')" @click="act('new')"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5 3h10l4 4v14H5z"/><path d="M12 10v6M9 13h6"/></svg>{{ t('home.new') }}</button>
        <!-- Rust home.rs "Back to the model": shown while a model is loaded. -->
        <button v-if="control('home-back')" class="btn s" type="button" data-bb="home-back" @click="send({ do: 'showView' })">{{ t('home.backToModel') }}</button>
      </div>
      <NotAvailable v-if="!hasUser" class="you" compact :what="t('na.screen.you')" :instead="t(INSTEAD.you!)" /><!-- FAKE1 -->
      <div v-else class="card you" data-bb="home-you">
        <div class="eyebrow">{{ t('home.you') }}</div>
        <!-- Settings → You gives the name only: e-mail and shared folder have no Rust source. -->
        <div class="row"><span>{{ state.user?.name }}</span><span class="mono" style="font-size:12px" data-bb="home-email">{{ state.user?.email ?? '' }}</span></div>
        <div class="row"><span class="k">{{ t('home.sharedFolder') }}</span><span class="mono" style="font-size:12.5px" data-missing="shared-folder"></span></div>
        <a href="#" class="change" data-bb="home-settings" :aria-disabled="!on('settings') || undefined" @click.prevent="act('settings')">{{ t('home.changeYou') }}</a>
      </div>
      <p class="note" data-bb="home-drop">{{ t('home.drop') }}</p>
    </div>
    <div class="right">
      <div class="head">
        <h2 class="disp" data-bb="home-recent-title">{{ t('home.recent') }}</h2>
        <span style="flex:1"></span>
        <input v-if="hasRecent" v-model="filter" class="inp" :placeholder="t('home.filter')" :aria-label="t('home.filter')" data-bb="home-filter">
      </div>
      <NotAvailable v-if="!hasRecent" compact :what="t('na.screen.recent')" :instead="t(INSTEAD.recent!)" /><!-- FAKE1 -->
      <div v-else class="grid" data-bb="home-recent">
        <!-- ponytail: the web bridge cannot open a file by its path (no file system), so each card is drawn but disabled. -->
        <a v-for="f in shown" :key="f.path" href="#" class="card rc" :class="{ gone: !f.exists }" aria-disabled="true" data-missing="open-by-path" :title="f.path" @click.prevent>
          <!-- No preview from Rust: the reference's tile, empty. -->
          <div class="tile" aria-hidden="true"></div>
          <div class="body">
            <div class="name">{{ f.name }}</div>
            <!-- opened_unix is null today: the folder only. -->
            <div class="mono where">{{ f.folder }}<template v-if="f.opened_unix !== null"> · {{ new Date(f.opened_unix * 1000).toLocaleDateString() }}</template></div>
            <span class="note" style="font-size:12.5px">{{ f.exists ? (f.schema ?? '') : t('home.notFound') }}</span>
          </div>
        </a>
      </div>
    </div>
  </div>
</template>

<style scoped>
/* .app's body row: flex:1;position:relative;overflow:hidden;display:flex;gap:40px;padding:48px 56px. A full-window page (layout = "full"). */
.home { position: relative; height: 100%; box-sizing: border-box; overflow: hidden; display: flex; gap: 40px; padding: 48px 56px; }
/* DESIGN-SPEC § Components: one decorative disc (clay, 420) bled off the bottom-left corner. */
.disc { position: absolute; left: -160px; bottom: -200px; width: 420px; height: 420px; border-radius: 50%; background: var(--bb-clay); }
.left { position: relative; width: 360px; flex: none; display: flex; flex-direction: column; gap: 18px; }
/* Not in the reference: out of flow, so no reference box moves. */
.wordmark { position: absolute; left: 0; bottom: 100%; margin-bottom: 10px; font-family: var(--bb-font-wordmark); font-size: 28px; line-height: 1; }
.right { position: relative; flex: 1; display: flex; flex-direction: column; gap: 14px; min-width: 0; }
.eyebrow { font-family: 'DM Mono', monospace; font-size: 12px; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); }
.mono { font-family: 'DM Mono', ui-monospace, Menlo, monospace; }
.disp { font-family: 'Sora', 'Source Sans 3', sans-serif; margin: 0; font-weight: 700; }
h1.disp { font-size: 52px; line-height: 1.02; }
h2.disp { font-size: 20px; }
.lead { margin: 0; font-size: 18px; }
.acts { display: flex; flex-direction: column; gap: 10px; align-items: flex-start; }
/* .btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;min-height:32px;padding:5px 16px;border-radius:999px;font:inherit;font-weight:600;color:#141414;cursor:pointer;text-decoration:none;white-space:nowrap} */
.btn { display: inline-flex; align-items: center; justify-content: center; gap: 6px; min-height: 32px; padding: 5px 16px; border-radius: var(--bb-radius-pill); font: inherit; font-weight: 600; color: var(--bb-ink); cursor: pointer; text-decoration: none; white-space: nowrap; } /* kept scoped: the page's link colour rule would beat the global .btn colour */
.btn.p { background: var(--bb-clay); border: 0; min-height: 40px; padding: 8px 20px; }
.btn[disabled], .btn[aria-disabled] { cursor: default; opacity: .55; }
.change[aria-disabled] { cursor: default; opacity: .55; }
.card { background: var(--bb-sheet); border: 1px solid var(--bb-line); border-radius: 16px; }
.you { padding: 14px; display: flex; flex-direction: column; gap: 6px; margin-top: 8px; }
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.row .k { color: var(--bb-stone); white-space: nowrap; }
.change { font-size: 13.5px; color: var(--bb-clay-text); }
.change:hover { color: var(--bb-ink); }
.note { margin: 0; font-size: 13px; color: var(--bb-stone); }
.head { display: flex; align-items: center; gap: 12px; }
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: 8px; background: var(--bb-sheet); padding: 0 8px; font: inherit; font-size: 13.5px; color: var(--bb-ink); min-width: 0; width: 240px; }
.grid { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 16px; }
.rc { padding: 0; overflow: hidden; text-decoration: none; color: var(--bb-ink); display: flex; flex-direction: column; cursor: default; }
.rc.gone { opacity: .6; }
.tile { height: 120px; background: var(--bb-sheet); border-bottom: 1px solid var(--bb-line); }
.body { padding: 10px 12px; display: flex; flex-direction: column; gap: 4px; }
.name { font-weight: 600; font-size: 14px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.where { font-size: 11.5px; color: var(--bb-stone); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
