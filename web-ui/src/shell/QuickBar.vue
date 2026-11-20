<script setup lang="ts">
import { computed, onMounted, onUnmounted } from "vue";
import { state, actions, historyLabels, runAction, undo, redo } from "../bimble";
import { t } from "../i18n";
import { currentDialog } from "../dialogs";
import { page } from "../pages";
import labIcon from "../brand/bimble-icon-lab.svg";

/** Whether the ribbon action `name` is enabled now (`query("actions")`). */
const on = (name: string) => actions.value.some((a) => a.name === name && a.enabled);
const undoLabel = computed(() => {
  const what = historyLabels.value.undo[0];
  return what ? t("quick.undo", { what }) : t("app.undo");
});
// A full-window page (Home) has the reference's short header: logo, File, command field, user (fr/Home.html).
const bare = computed(() => page()?.layout === "full");
// Ctrl+K / Cmd+K opens the command palette anywhere (D8). Capture phase, and the event stops here:
// egui binds Ctrl K to its own palette when the canvas has focus.
function onKey(e: KeyboardEvent) {
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k") { e.preventDefault(); e.stopImmediatePropagation(); currentDialog.value = "palette"; }
}
onMounted(() => window.addEventListener("keydown", onKey, true));
onUnmounted(() => window.removeEventListener("keydown", onKey, true));
</script>

<template>
  <!-- Reference: header.top (fr/View3D.html). -->
  <header class="top" data-bb="quickbar">
    <!-- Lab mark instead of the logo disc (ADR 0049); same 30 × 32 box as the reference a.ib. -->
    <a href="#" class="ib" :aria-label="t('quick.home')" style="width:auto;padding:0 2px" data-bb="quickbar-logo"
      @click.prevent="runAction('home')"><img :src="labIcon" :alt="t('app.logoAlt')" width="26" height="26" /></a>
    <button v-if="!bare" class="ib a" type="button" :aria-label="t('quick.home')" :title="t('quick.home')" data-bb="quickbar-home"
      @click="runAction('home')"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M4 11 12 4l8 7"/><path d="M6 10v10h12V10"/><path d="M10 20v-6h4v6"/></svg></button>
    <button class="qb" type="button" style="font-weight:600" data-bb="quickbar-file" aria-haspopup="menu" :aria-expanded="currentDialog === 'file-menu'"
      @click="currentDialog = 'file-menu'">{{ t('quick.file') }} <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></button>
    <span v-if="!bare" class="tabsep" data-bb="quickbar-sep"></span>
    <button v-if="!bare" class="ib" type="button" :disabled="!on('open')" :aria-label="t('quick.open')" :title="t('quick.open')" data-bb="quickbar-open"
      @click="runAction('open')"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M3 7h6l2 2h10v10H3z"/><path d="M3 7V5h6"/></svg></button>
    <button v-if="!bare" class="ib" type="button" :disabled="!on('save')" :aria-label="t('quick.save')" :title="t('quick.save')" data-bb="quickbar-save"
      @click="runAction('save')"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5 4h11l3 3v13H5z"/><path d="M8 4v5h7V4"/><rect x="8" y="13" width="8" height="5"/></svg></button>
    <button v-if="!bare" class="ib" type="button" :disabled="!on('undo')" :aria-label="undoLabel" :title="undoLabel" data-bb="quickbar-undo"
      @click="undo()"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 7 5 11l4 4"/><path d="M5 11h9a5 5 0 0 1 0 10h-3"/></svg></button>
    <button v-if="!bare" class="ib" type="button" :disabled="!on('redo')" :aria-label="t('quick.redo')" :title="t('quick.redo')" data-bb="quickbar-redo"
      @click="redo()"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m15 7 4 4-4 4"/><path d="M19 11h-9a5 5 0 0 0 0 10h3"/></svg></button>
    <!-- Print and export opens screen D4 (dialogs/Export.vue). -->
    <button v-if="!bare" class="ib a" type="button" :aria-label="t('quick.export')" :title="t('quick.export')" data-bb="quickbar-export"
      aria-haspopup="dialog" @click="currentDialog = 'export'"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M7 9V4h10v5"/><rect x="4" y="9" width="16" height="7" rx="1"/><path d="M7 14h10v6H7z"/></svg></button>
    <button v-if="!bare" class="ib a" type="button" :disabled="!on('synchronize')" :aria-label="t('quick.sync')" :title="t('quick.sync')" data-bb="quickbar-sync"
      @click="runAction('synchronize')"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20 11a8 8 0 0 0-14.5-4.5L4 8"/><path d="M4 4v4h4"/><path d="M4 13a8 8 0 0 0 14.5 4.5L20 16"/><path d="M20 20v-4h-4"/></svg></button>
    <div v-if="bare" style="flex:1"></div>
    <div v-else class="fname">
      <span data-bb="filename">{{ state.file ?? t('app.noFile') }}</span>
      <!-- `query("state").schema`: ".bimble", "IFC4"… -->
      <span class="mono schema" style="font-size:12px" data-bb="quickbar-schema">{{ state.schema ?? '' }}</span>
      <span class="chip nx" :class="state.unsaved ? 'c-o' : 'c-m'" data-bb="state-chip">{{ t(state.unsaved ? 'quick.notSaved' : 'app.saved') }}</span>
    </div>
    <!-- Opens the command palette (src/dialogs/Palette.vue, D8). -->
    <button class="cmdk" type="button" data-bb="command-field" aria-haspopup="dialog" @click="currentDialog = 'palette'"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><circle cx="11" cy="11" r="6"/><path d="m20 20-4.5-4.5"/></svg><span>{{ t('quick.search') }}</span><span class="kbd">Ctrl K</span></button>
    <!-- Settings → You: an empty disc when no name is set. -->
    <span class="user" data-bb="quickbar-user" :title="state.user?.name" :data-missing="state.user ? undefined : 'user-name-unset'">{{ state.user?.trigram }}</span>
    <button v-if="!bare" class="btn" :class="on('save') ? 'p' : 'd'" type="button" :disabled="!on('save')" data-bb="quickbar-primary"
      @click="runAction('save')">{{ t('quick.saveButton') }}</button>
  </header>
</template>

<style scoped>
/* .top{display:flex;align-items:center;gap:6px;padding:6px 12px;background:#FFFFFF;min-height:46px;flex-wrap:wrap} */
.top { display: flex; align-items: center; gap: 6px; padding: 6px 12px; background: var(--bb-sheet); min-height: 46px; flex-wrap: wrap; }
/* .ib{width:32px;height:32px;border-radius:999px;border:0;background:none;display:inline-flex;align-items:center;justify-content:center;color:#141414;cursor:pointer;text-decoration:none;padding:0;flex:none} */
.ib { width: var(--bb-target); height: var(--bb-target); border-radius: var(--bb-radius-pill); border: 0; background: none; display: inline-flex; align-items: center; justify-content: center; color: var(--bb-ink); cursor: pointer; text-decoration: none; padding: 0; flex: none; }
/* .ib:hover{background:#EDE7DB} */
.ib:hover { background: var(--bb-sunk); }
/* The reference draws these as <a> (inherited font); the <button> takes the same font. */
.ib.a { font: inherit; }
.ib img { display: block; }
/* Unavailable: .tool.off{color:#5C574D;opacity:.55} */
.ib:disabled { color: var(--bb-stone); opacity: .55; cursor: default; background: none; }
/* .qb{display:inline-flex;align-items:center;gap:4px;height:32px;padding:0 12px;border-radius:999px;border:0;background:none;color:#141414;font:inherit;cursor:pointer;text-decoration:none;white-space:nowrap} */
.qb { display: inline-flex; align-items: center; gap: var(--bb-space-1); height: var(--bb-target); padding: 0 12px; border-radius: var(--bb-radius-pill); border: 0; background: none; color: var(--bb-ink); font: inherit; cursor: pointer; text-decoration: none; white-space: nowrap; }
.qb:hover { background: var(--bb-sunk); }
/* .tabsep{width:1px;height:18px;background:#DDD6C8;margin:0 6px} */
.tabsep { width: 1px; height: 18px; background: var(--bb-line); margin: 0 6px; }
/* .fname{display:flex;align-items:center;gap:8px;margin:0 auto;font-size:14px;min-width:0} */
.fname { display: flex; align-items: center; gap: var(--bb-space-2); margin: 0 auto; font-size: var(--bb-size-ui); min-width: 0; }
/* .mono{font-family:'DM Mono',...}; schema: style="font-size:12px;color:#5C574D" */
.mono { font-family: var(--bb-font-data); }
.schema { color: var(--bb-stone); min-width: 4ch; min-height: 1lh; }
/* .chip{padding:2px 4px 2px 10px;border-radius:999px;font-size:13px;display:inline-flex;gap:4px;align-items:center;white-space:nowrap;color:#141414;text-decoration:none} .chip.nx{padding-right:10px} */
.c-m { background: var(--bb-moss); }
/* .cmdk{display:inline-flex;align-items:center;gap:8px;height:32px;padding:0 12px;border:1px solid #5C574D;border-radius:999px;color:#5C574D;text-decoration:none;font-size:13.5px;background:#FFFFFF;white-space:nowrap} (an <a> there: font inherited) */
.cmdk { font: inherit; display: inline-flex; align-items: center; gap: var(--bb-space-2); height: var(--bb-target); padding: 0 12px; border: var(--bb-border-control); border-radius: var(--bb-radius-pill); color: var(--bb-stone); text-decoration: none; font-size: 13.5px; background: var(--bb-sheet); white-space: nowrap; cursor: pointer; }
/* .kbd{font-family:'DM Mono',monospace;font-size:11.5px;padding:1px 6px;border-radius:6px;background:#EDE7DB;color:#141414} */
.kbd { font-family: var(--bb-font-data); font-size: 11.5px; padding: 1px 6px; border-radius: 6px; background: var(--bb-sunk); color: var(--bb-ink); }
/* style="width:30px;height:30px;border-radius:999px;background:#EDE7DB;display:inline-flex;align-items:center;justify-content:center;font-family:'DM Mono',monospace;font-size:11px" */
.user { width: 30px; height: 30px; border-radius: var(--bb-radius-pill); background: var(--bb-sunk); display: inline-flex; align-items: center; justify-content: center; font-family: 'DM Mono', monospace; font-size: 11px; }
/* .btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;min-height:32px;padding:5px 16px;border-radius:999px;font:inherit;font-weight:600;color:#141414;cursor:pointer;text-decoration:none;white-space:nowrap} */
.btn { display: inline-flex; align-items: center; justify-content: center; gap: 6px; min-height: var(--bb-target); padding: 5px 16px; border-radius: var(--bb-radius-pill); font: inherit; font-weight: 600; color: var(--bb-ink); cursor: pointer; text-decoration: none; white-space: nowrap; }
/* .btn.p{background:#C06A41;border:0} */
/* .btn.d{background:#EDE7DB;border:0;color:#5C574D;font-weight:400;cursor:default} */
</style>
