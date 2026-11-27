<script lang="ts">
export const capability = "settings"; // SET1: query settings, action set_settings (ccore/session/settings.h)
</script>

<script setup lang="ts">
// Settings (ticket SET1): Rust's window (crates/bimble-viewer/src/lib.rs:4127 settings_ui): You (name,
// e-mail: lib.rs:4023), Coordinates (coords.rs:57), Appearance (theme, font, text size: lib.rs:4041).
// The same file as the Rust app (settings.rs): every change is one `set_settings`, applied at once
// (ADR 0052: no OK/Cancel). The memory share and two performance fields are kept in the file but not
// shown: the lab's viewport has no use for them (`query settings` applies, DEVIATIONS 23); "While the
// view moves" (moving_fps) is shown since it applies (ticket 3D1, ADR 0078), and "Far parts simplified"
// (moving_proxies, HLOD proxies) since ticket 3D2. GPU1 (ADR 0083): the 3D renderer (renderer: not one of Rust's
// fields) and, with WebGPU, "Cull copies on the graphics card" (gpu_cull) apply.
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
/** SIZE3: the links' GPU budget's choices (MB), the stored value among them. */
const linkGpuChoices = computed(() => [...new Set([256, 512, 768, 1024, 1536, 2048, 4096, v.value?.link_gpu_mb ?? 768])].sort((a, b) => a - b));
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
        <!-- 3D1 (ADR 0078): the moving budget follows moving_fps (Rust's choices: lib.rs settings_ui) -->
        <label v-if="s?.applies.moving_fps" class="row"><span>{{ tx('settings.moving') }}</span>
          <select class="inp" :value="v.moving_fps" data-bb="settings-moving-fps" data-setting="moving_fps" @change="set({ moving_fps: Number(($event.target as HTMLSelectElement).value) })">
            <option v-for="f in [0, 15, 30, 60, 120, 240]" :key="f" :value="f">{{ f === 0 ? tx('settings.moving.0') : tx('settings.moving.fps', { fps: f }) }}</option>
          </select></label>
        <p v-if="s?.applies.moving_fps" class="note" data-bb="settings-moving-note">{{ v.moving_fps === 0 ? tx('settings.moving.0Note') : tx('settings.movingNote') }}</p>
        <!-- 3D2 (ADR 0078): HLOD proxies (moving_proxies, Rust's setting): far cells drawn as stand-ins -->
        <label v-if="s?.applies.moving_proxies" class="row"><span>{{ tx('settings.proxies') }}</span>
          <input type="checkbox" :checked="v.moving_proxies" data-bb="settings-moving-proxies" data-setting="moving_proxies" @change="set({ moving_proxies: ($event.target as HTMLInputElement).checked })"></label>
        <p v-if="s?.applies.moving_proxies" class="note" data-bb="settings-proxies-note">{{ tx('settings.proxiesNote') }}</p>
        <!-- TESS1 (ADR 0085): the reader's detail for the links' conversions -->
        <label v-if="s?.applies.link_detail" class="row"><span>{{ tx('settings.linkDetail') }}</span>
          <select class="inp" :value="v.link_detail ?? 'standard'" data-bb="settings-link-detail" data-setting="link_detail" @change="set({ link_detail: ($event.target as HTMLSelectElement).value as 'exact' | 'standard' | 'light' | 'custom' })">
            <option value="exact">{{ tx('settings.linkDetail.exact') }}</option>
            <option value="standard">{{ tx('settings.linkDetail.standard') }}</option>
            <option value="light">{{ tx('settings.linkDetail.light') }}</option>
            <option value="custom">{{ tx('settings.linkDetail.custom') }}</option>
          </select></label>
        <label v-if="s?.applies.link_chord_mm && v.link_detail === 'custom'" class="row"><span>{{ tx('settings.linkChord') }}</span>
          <input class="inp" type="number" min="0.01" max="1000" step="0.5" :value="v.link_chord_mm ?? 2" data-bb="settings-link-chord" data-setting="link_chord_mm" @change="set({ link_chord_mm: Number(($event.target as HTMLInputElement).value) })"></label>
        <label v-if="s?.applies.link_max_deg && v.link_detail === 'custom'" class="row"><span>{{ tx('settings.linkMaxDeg') }}</span>
          <input class="inp" type="number" min="1" max="90" step="1" :value="v.link_max_deg ?? 18" data-bb="settings-link-max-deg" data-setting="link_max_deg" @change="set({ link_max_deg: Number(($event.target as HTMLInputElement).value) })"></label>
        <p v-if="s?.applies.link_detail" class="note" data-bb="settings-link-detail-note">{{ tx('settings.linkDetailNote') }}</p>
        <!-- SIZE3 (ADR 0078 amendment 5): the links' GPU budget, MB -->
        <label v-if="s?.applies.link_gpu_mb" class="row"><span>{{ tx('settings.linkGpu') }}</span>
          <select class="inp" :value="v.link_gpu_mb ?? 768" data-bb="settings-link-gpu" data-setting="link_gpu_mb" @change="set({ link_gpu_mb: Number(($event.target as HTMLSelectElement).value) })">
            <option v-for="mb in linkGpuChoices" :key="mb" :value="mb">{{ tx('settings.linkGpu.mb', { mb }) }}</option>
          </select></label>
        <p v-if="s?.applies.link_gpu_mb" class="note" data-bb="settings-link-gpu-note">{{ tx('settings.linkGpuNote') }}</p>
        <!-- VIS1 (ADR 0078 amendment 7): draw only what can be seen (draw_visible, not Rust's) -->
        <label v-if="s?.applies.draw_visible" class="row"><span>{{ tx('settings.visible') }}</span>
          <input type="checkbox" :checked="v.draw_visible !== false" data-bb="settings-draw-visible" data-setting="draw_visible" @change="set({ draw_visible: ($event.target as HTMLInputElement).checked })"></label>
        <p v-if="s?.applies.draw_visible" class="note" data-bb="settings-visible-note">{{ tx('settings.visibleNote') }}</p>
        <!-- GPU1 (ADR 0083): the 3D renderer (automatic is WebGL2 for now); GPU culling of copies applies with WebGPU -->
        <label v-if="s?.applies.renderer" class="row"><span>{{ tx('settings.renderer') }}</span>
          <select class="inp" :value="v.renderer ?? 'auto'" data-bb="settings-renderer" data-setting="renderer" @change="set({ renderer: ($event.target as HTMLSelectElement).value as 'auto' | 'webgl2' | 'webgpu' })">
            <option v-for="r in ['auto', 'webgl2', 'webgpu']" :key="r" :value="r">{{ tx(`settings.renderer.${r}` as K) }}</option>
          </select></label>
        <p v-if="s?.applies.renderer" class="note" data-bb="settings-renderer-note">{{ tx('settings.rendererNote') }}</p>
        <!-- GPU2: `bimble-ccore open` in an app window of Chromium with the flags that give WebGPU (the native host's) -->
        <label v-if="s?.applies.open_app" class="row"><span>{{ tx('settings.openApp') }}</span>
          <input type="checkbox" :checked="v.open_app ?? false" data-bb="settings-open-app" data-setting="open_app" @change="set({ open_app: ($event.target as HTMLInputElement).checked })"></label>
        <p v-if="s?.applies.open_app" class="note" data-bb="settings-open-app-note">{{ tx('settings.openAppNote') }}</p>
        <label v-if="s?.applies.gpu_cull" class="row"><span>{{ tx('settings.gpuCullOn') }}</span>
          <input type="checkbox" :checked="v.gpu_cull" data-bb="settings-gpu-cull-on" data-setting="gpu_cull" @change="set({ gpu_cull: ($event.target as HTMLInputElement).checked })"></label>
        <p v-if="s && !s.applies.gpu_cull" class="note" data-bb="settings-gpu-cull">{{ tx('settings.gpuCull') }}</p>
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
