<script lang="ts">
export const capability = "about"; // SET1: query about (ccore/session/settings.h)
</script>

<script setup lang="ts">
// About Bimble (ticket SET1; Rust lib.rs:4081 about_ui): the version, what draws it (the page's WebGL
// renderer, as Rust names its wgpu adapter for issue #59), the system, and "Copy for a bug report":
// the clipboard is written inside the click (a browser allows it only there); refused, the text is
// shown selected for Ctrl C.
import { computed, nextTick, ref } from "vue";
import { aboutInfo, state } from "../bimble";
import { t } from "../i18n";
import type * as copy from "../i18n/parts/settings";
import { currentDialog } from ".";

const tx = (k: keyof typeof copy.fr) => t(k);

/** The WebGL renderer string (WEBGL_debug_renderer_info when the browser gives it). */
function renderer(): string | null {
  try {
    const c = document.createElement("canvas");
    const gl = (c.getContext("webgl2") ?? c.getContext("webgl")) as WebGLRenderingContext | null;
    if (!gl) return null;
    const ext = gl.getExtension("WEBGL_debug_renderer_info");
    const r = String(gl.getParameter(ext ? ext.UNMASKED_RENDERER_WEBGL : gl.RENDERER) ?? "");
    gl.getExtension("WEBGL_lose_context")?.loseContext();
    return r || null;
  } catch { return null; }
}
const commit = typeof __BB_COMMIT__ === "string" && __BB_COMMIT__ ? __BB_COMMIT__ : null;
const info = computed(() => aboutInfo(renderer(), commit));
const version = computed(() => info.value?.version ?? state.value.version ?? "");
const copied = ref<"" | "copied" | "select">("");
const area = ref<HTMLTextAreaElement | null>(null);

async function copyReport(): Promise<void> {
  const text = info.value?.text ?? `Bimble ${version.value}`;
  try {
    await navigator.clipboard.writeText(text);
    copied.value = "copied";
  } catch {
    copied.value = "select";
    await nextTick();
    area.value?.focus();
    area.value?.select();
  }
}
const close = () => { currentDialog.value = null; };
</script>

<template>
  <div class="bb-dialog ab" role="dialog" aria-modal="true" :aria-label="tx('about.title')" data-bb="about">
    <div class="head">
      <h2 class="disp" data-bb="about-heading">Bimble {{ version }}</h2>
      <span style="flex:1"></span>
      <button class="btn s" type="button" data-bb="about-close" @click="close">{{ tx('settings.close') }}</button>
    </div>
    <p class="note">{{ tx('about.tagline') }}</p>
    <dl class="kv">
      <dt>{{ tx('about.version') }}</dt><dd class="mono" data-bb="about-version">{{ version }}</dd>
      <dt>{{ tx('about.core') }}</dt><dd class="mono" data-bb="about-build">{{ info?.build ?? tx('about.unknown') }}</dd>
      <dt>{{ tx('about.commit') }}</dt><dd class="mono" data-bb="about-commit">{{ info?.commit ?? tx('about.unknown') }}</dd>
      <dt>{{ tx('about.graphics') }}</dt><dd class="mono" data-bb="about-graphics">{{ info?.graphics ?? tx('about.unknown') }}</dd>
      <dt>{{ tx('about.system') }}</dt><dd class="mono" data-bb="about-platform">{{ info?.platform ?? tx('about.unknown') }}</dd>
    </dl>
    <div class="links">
      <a href="https://guilbep.github.io/bimble-docs/" target="_blank" rel="noopener">{{ tx('about.manual') }}</a>
      <a href="https://github.com/guilbep/bimble/issues" target="_blank" rel="noopener">{{ tx('about.report') }}</a>
      <a href="https://github.com/guilbep/bimble" target="_blank" rel="noopener">{{ tx('about.source') }}</a>
    </div>
    <div style="display:flex;align-items:center;gap:10px">
      <button class="btn p" type="button" :title="tx('about.copyHint')" data-bb="about-copy" @click="copyReport">{{ tx('about.copy') }}</button>
      <span v-if="copied === 'copied'" class="note" role="status" data-bb="about-copied">{{ tx('about.copied') }}</span>
    </div>
    <template v-if="copied === 'select'">
      <p class="note" role="status" data-bb="about-select">{{ tx('about.select') }}</p>
      <textarea ref="area" class="inp mono" readonly rows="4" data-bb="about-text" :value="info?.text ?? ''"></textarea>
    </template>
  </div>
</template>

<style scoped>
.ab { width: min(460px, calc(100vw - 32px)); color: var(--bb-ink); }
.head { display: flex; align-items: center; gap: 10px; }
.disp { font-family: var(--bb-font-display); margin: 0; font-size: var(--bb-size-heading); font-weight: 700; }
.kv { display: grid; grid-template-columns: 110px 1fr; gap: 4px 10px; margin: 0; }
.kv dt { color: var(--bb-stone); }
.kv dd { margin: 0; overflow-wrap: anywhere; }
.links { display: flex; gap: 16px; flex-wrap: wrap; }
.links a { color: var(--bb-link); }
.mono { font-family: var(--bb-font-data); font-size: 12.5px; }
.note { margin: 0; font-size: var(--bb-size-small); color: var(--bb-stone); }
.inp { border: 1px solid var(--bb-stone); border-radius: var(--bb-radius-sm); background: var(--bb-sheet); color: var(--bb-ink); padding: 6px 8px; resize: vertical; }
:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
