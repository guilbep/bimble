<script setup lang="ts">
// The small-screen top bar (fr/PhoneViewer.html header.mtop): logo, file and view, saved chip, language.
import { computed } from "vue";
import { state, openViews } from "../bimble";
import { t, lang, setLang } from "../i18n";
import { viewName } from "../shell/viewName";
import { sheet, wide, phone } from "./screen";
import labIcon from "../brand/bimble-icon-lab.svg";

const view = computed(() => openViews.value.views.find((v) => v.id === openViews.value.active) ?? null);
const sub = computed(() => [view.value ? viewName(view.value) : "", state.value.schema ?? ""].filter(Boolean).join(" · "));
</script>

<template>
  <header class="mtop" :class="{ low: wide && phone }" data-bb="small-top">
    <button class="tb" type="button" :aria-label="t('small.menu')" :aria-expanded="sheet === 'more'" @click="sheet = sheet === 'more' ? null : 'more'">
      <img :src="labIcon" :alt="t('app.logoAlt')" width="30" height="30" />
    </button>
    <div class="fn">
      <b data-bb="filename">{{ state.file ?? t('app.noFile') }}</b>
      <span>{{ sub }}</span>
    </div>
    <span v-if="state.file !== null" class="mchip" :class="state.unsaved ? 'c-o' : 'c-m'" data-bb="state-chip">{{ t(state.unsaved ? 'small.notSaved' : 'small.saved') }}</span>
    <span class="seg" role="group" :aria-label="t('small.language')">
      <button v-for="l in (['fr', 'en'] as const)" :key="l" type="button" class="lg" :aria-pressed="lang === l" @click="setLang(l)">{{ l.toUpperCase() }}</button>
    </span>
  </header>
</template>

<style scoped>
/* .ph .mtop{display:flex;align-items:center;gap:8px;padding:6px 10px;background:#FFFFFF;border-bottom:1px solid #DDD6C8;min-height:52px;flex:none} */
.mtop { grid-area: top; display: flex; align-items: center; gap: 8px; padding: 6px 10px; background: var(--bb-sheet); border-bottom: 1px solid var(--bb-line); min-height: 52px; flex: none; font-size: 15px; }
/* fr/PhoneLandscape.html: style="min-height:46px;padding:1px 10px" */
.mtop.low { min-height: 46px; padding: 1px 10px; }
.tb { width: 44px; height: 44px; border-radius: var(--bb-radius-pill); border: 0; background: none; display: inline-flex; align-items: center; justify-content: center; color: var(--bb-ink); flex: none; cursor: pointer; padding: 0; }
.tb:focus-visible, .lg:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
.fn { flex: 1; min-width: 0; display: flex; flex-direction: column; line-height: 1.2; }
.fn b { font-weight: 600; font-size: 14.5px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.fn span { font-size: 12.5px; color: var(--bb-stone); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; min-height: 1lh; }
/* .ph .mchip{display:inline-flex;align-items:center;gap:6px;min-height:32px;padding:0 6px 0 12px;border-radius:999px;font-size:13.5px;white-space:nowrap}; style="padding-right:12px" */
.mchip { display: inline-flex; align-items: center; min-height: 32px; padding: 0 12px; border-radius: var(--bb-radius-pill); font-size: 13.5px; white-space: nowrap; color: var(--bb-ink); }
.c-m { background: var(--bb-moss); }
.c-o { background: var(--bb-ochre); }
/* The reference's language switch: two 44 px high halves, the current one ink. */
.seg { display: inline-flex; flex: none; border: 1px solid var(--bb-stone); border-radius: var(--bb-radius-pill); overflow: hidden; }
.lg { min-width: 40px; height: 42px; border: 0; background: var(--bb-sheet); color: var(--bb-ink); font: inherit; font-size: 13px; cursor: pointer; }
.lg[aria-pressed="true"] { background: var(--bb-ink); color: var(--bb-on-ink); }
</style>
