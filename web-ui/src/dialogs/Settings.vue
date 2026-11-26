<script lang="ts">
export const capability = "settings"; // SET1: query settings, action set_settings (ccore/session/settings.h)
</script>

<script setup lang="ts">
// Settings (ticket SET1): Rust's window (crates/bimble-viewer/src/lib.rs:4127 settings_ui): You (name,
// e-mail: lib.rs:4023), Coordinates (coords.rs:57), Appearance (theme, font, text size: lib.rs:4041).
// The same file as the Rust app (settings.rs): every change is one `set_settings`, applied at once
// (ADR 0052: no OK/Cancel). The memory share and the three performance fields are kept in the file
// but not shown: the lab's viewport has no use for them (`query settings` applies, DEVIATIONS 23).
import { computed, ref, watch } from "vue";
import { setSettings, userSettings, type SettingsValues } from "../bimble";
import { t } from "../i18n";
import type * as copy from "../i18n/parts/settings";
import { currentDialog } from ".";
import { TEXT_SIZE, fontInstalled } from "../shell/theme";

type K = keyof typeof copy.fr;
const tx = (k: K, v: Record<string, string | number> = {}) => t(k, v);
const s = computed(() => userSettings.value);
const v = computed(() => s.value?.values ?? null);
const error = ref<string | null>(null);

function set(patch: Partial<SettingsValues>): void {
  error.value = setSettings(patch);
}

// Name and e-mail: written when the field is left or Enter pressed (one change, one write).
const name = ref(v.value?.user_name ?? "");
const email = ref(v.value?.user_email ?? "");
watch(v, (x) => { if (x) { name.value = x.user_name; email.value = x.user_email; } });
const commitName = () => { if (v.value && name.value !== v.value.user_name) set({ user_name: name.value }); };
const commitEmail = () => { if (v.value && email.value !== v.value.user_email) set({ user_email: email.value }); };
// settings.rs author(): "Name <e-mail>", either one alone.
const author = computed(() => {
  const n = (v.value?.user_name ?? "").trim(), e = (v.value?.user_email ?? "").trim();
  return n && e ? `${n} <${e}>` : n || e || tx("settings.authorNone");
});

// Coordinates: survey, wgs84, utm, or "epsg:<n>".
const COORDS = ["survey", "wgs84", "utm"] as const;
const isEpsg = computed(() => (v.value?.coordinates ?? "").startsWith("epsg:"));
const epsg = ref((v.value?.coordinates ?? "").startsWith("epsg:") ? v.value!.coordinates.slice(5) : "21781");
const commitEpsg = () => { const c = epsg.value.trim(); if (/^\d{1,5}$/.test(c)) set({ coordinates: `epsg:${c}` }); else error.value = tx("settings.coord.epsgHint"); };

// Appearance.
const themes = computed(() => s.value?.themes ?? []);
const fonts = computed(() => s.value?.fonts ?? []);
/** DEAD1: a font this computer lacks cannot be chosen (it would change nothing): disabled with where its files go. */
const installed = computed(() => new Map(fonts.value.map((f) => [f, fontInstalled(f)])));
const sizes = computed<[number, number]>(() => s.value?.text_sizes ?? [10, 16]);
const isDefaultLook = computed(() => !v.value || (v.value.theme === "grey" && v.value.ui_font === "roboto" && v.value.text_size === TEXT_SIZE));
const close = () => { currentDialog.value = null; };
</script>

<template>
  <div class="bb-dialog st" role="dialog" aria-modal="true" :aria-label="tx('settings.title')" data-bb="settings">
    <div class="head">
      <h2 class="disp">{{ tx('settings.title') }}</h2>
      <span style="flex:1"></span>
      <button class="btn s" type="button" data-bb="settings-close" @click="close">{{ tx('settings.close') }}</button>
    </div>
    <p v-if="!v" class="note">{{ tx('settings.unavailable') }}</p>
    <template v-else>
      <p v-if="error" class="error" role="alert" data-bb="settings-error">{{ tx('settings.error', { msg: error }) }}</p>
      <p v-if="s?.note" class="note" data-bb="settings-note">{{ s.note }}</p>

      <section>
        <h3 class="eyebrow">{{ tx('settings.you') }}</h3>
        <label class="row"><span>{{ tx('settings.name') }}</span>
          <input v-model="name" class="inp" type="text" maxlength="120" autocomplete="name" data-bb="settings-name" data-setting="user_name" @change="commitName" @keydown.enter="commitName"></label>
        <label class="row"><span>{{ tx('settings.email') }}</span>
          <input v-model="email" class="inp" type="email" maxlength="120" autocomplete="email" data-bb="settings-email" data-setting="user_email" @change="commitEmail" @keydown.enter="commitEmail"></label>
        <p class="note" data-bb="settings-author">{{ tx('settings.author', { who: author }) }}</p>
      </section>

      <section role="radiogroup" :aria-label="tx('settings.coordinates')" data-setting="coordinates">
        <h3 class="eyebrow">{{ tx('settings.coordinates') }}</h3>
        <label v-for="c in COORDS" :key="c" class="radio">
          <input type="radio" name="bb-coords" :checked="v.coordinates === c" :data-bb="`settings-coord-${c}`" @change="set({ coordinates: c })">
          <span>{{ tx(`settings.coord.${c}`) }}</span>
        </label>
        <label class="radio">
          <input type="radio" name="bb-coords" :checked="isEpsg" data-bb="settings-coord-epsg" @change="commitEpsg">
          <span>{{ tx('settings.coord.epsg') }}</span>
          <input v-model="epsg" class="inp mono" style="width:90px" inputmode="numeric" :aria-label="tx('settings.coord.epsgHint')" :title="tx('settings.coord.epsgHint')" data-bb="settings-epsg" @change="commitEpsg">
        </label>
      </section>

      <section>
        <h3 class="eyebrow">{{ tx('settings.appearance') }}</h3>
        <label class="row"><span>{{ tx('settings.theme') }}</span>
          <select class="inp" :value="v.theme" data-bb="settings-theme" data-setting="theme" @change="set({ theme: ($event.target as HTMLSelectElement).value })">
            <option v-for="id in themes" :key="id" :value="id">{{ tx(`settings.theme.${id}` as K) }}</option>
          </select></label>
        <div class="row" role="radiogroup" :aria-label="tx('settings.font')" data-setting="ui_font"><span>{{ tx('settings.font') }}</span>
          <span style="display:flex;gap:14px">
            <label v-for="f in fonts" :key="f" class="radio" :title="installed.get(f) === false ? tx('settings.fontMissing') : undefined"><input type="radio" name="bb-font" :checked="v.ui_font === f" :disabled="installed.get(f) === false" :data-bb="`settings-font-${f}`" @change="set({ ui_font: f })"><span>{{ tx(`settings.font.${f}` as K) }}</span><span v-if="installed.get(f) === false" class="note" :data-bb="`settings-font-missing-${f}`">{{ tx('settings.fontNotInstalled') }}</span></label>
          </span></div>
        <label class="row"><span>{{ tx('settings.textSize') }}</span>
          <span style="display:flex;gap:8px;align-items:center">
            <input type="range" :min="sizes[0]" :max="sizes[1]" step="0.5" :value="v.text_size" data-bb="settings-text-size" data-setting="text_size" @change="set({ text_size: Number(($event.target as HTMLInputElement).value) })">
            <span class="mono" data-bb="settings-text-size-value">{{ v.text_size }} pt</span>
          </span></label>
        <button v-if="!isDefaultLook" class="btn s" type="button" style="align-self:flex-start" data-bb="settings-default-look" @click="set({ theme: 'grey', ui_font: 'roboto', text_size: TEXT_SIZE })">{{ tx('settings.defaultLook') }}</button>
      </section>

      <p class="note" data-bb="settings-kept">{{ tx('settings.kept') }}</p>
      <p class="note mono" data-bb="settings-file">{{ s?.file ? tx('settings.file', { file: s.file }) : tx('settings.browser') }}</p>
    </template>
  </div>
</template>

<style scoped>
.st { width: min(520px, calc(100vw - 32px)); max-height: calc(100vh - 32px); overflow: auto; color: var(--bb-ink); }
.head { display: flex; align-items: center; gap: 10px; }
.disp { font-family: var(--bb-font-display); margin: 0; font-size: var(--bb-size-heading); font-weight: 700; }
section { display: flex; flex-direction: column; gap: 8px; }
.eyebrow { margin: 0; font-family: var(--bb-font-data); font-size: var(--bb-size-eyebrow); font-weight: 400; letter-spacing: var(--bb-tracking-eyebrow); text-transform: uppercase; color: var(--bb-stone); }
.row { display: grid; grid-template-columns: 120px 1fr; align-items: center; gap: 10px; }
.radio { display: flex; align-items: center; gap: 8px; }
.inp { height: 30px; border: 1px solid var(--bb-stone); border-radius: var(--bb-radius-sm); background: var(--bb-sheet); color: var(--bb-ink); padding: 0 8px; font: inherit; min-width: 0; }
.mono { font-family: var(--bb-font-data); }
.note { margin: 0; font-size: var(--bb-size-small); color: var(--bb-stone); overflow-wrap: anywhere; }
input[type="radio"], input[type="range"] { accent-color: var(--bb-clay); }
:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
@media (max-width: 420px) { .row { grid-template-columns: 1fr; gap: 4px; } }
</style>
