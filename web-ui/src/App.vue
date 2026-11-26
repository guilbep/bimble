<script setup lang="ts">
import { startError, screenRoutes, state, collab } from "./bimble";
import { t } from "./i18n";
import QuickBar from "./shell/QuickBar.vue";
import TabsBar from "./shell/TabsBar.vue";
import Ribbon from "./shell/Ribbon.vue";
import LeftPanel from "./shell/LeftPanel.vue";
import ViewTabs from "./shell/ViewTabs.vue";
import ModifierPanel from "./shell/ModifierPanel.vue";
import StatusBar from "./shell/StatusBar.vue";
import "./shell/shell.css";
import { computed, onMounted, ref, watch } from "vue";
import { useFocusTrap } from "./shell/focusTrap";
import { pages, currentPage, page } from "./pages";
// The shell's state (ticket UX1): Home, the model's view or a page, moved only by `send`
// (shell/send.ts routes the actions home, project, history, new, open, import_ifc and close).
import { here } from "./shell/send";
import ShellNotice from "./shell/ShellNotice.vue";
// ---- FAKE1: a screen the core does not serve shows NotAvailable instead (shell/capability.ts) ----
import NotAvailable from "./shell/NotAvailable.vue";
import { blocked, INSTEAD } from "./shell/capability";
const naProps = (id: string, m: Parameters<typeof blocked>[0]) => {
  const reason = blocked(m);
  return reason ? { what: t(`na.screen.${id}`), reason, ...(INSTEAD[id] ? { instead: t(INSTEAD[id]!) } : {}) } : null;
};
const pageNa = computed(() => (currentPage.value ? naProps(currentPage.value, page()) : null));
const dialogNa = computed(() => (currentDialog.value ? naProps(currentDialog.value, dialog()) : null));
const panelNa = computed(() => (currentPanel.value ? naProps(currentPanel.value, panel()) : null));
// ---- end FAKE1 ----
// The aside's name: the panel's `label` key (fr/Standards.html: aria-label="Standards graphiques").
const panelLabel = computed(() => { const l = (panel() as { label?: string } | null)?.label; return l ? t(l) : undefined; });
// A panel is not a screen of the shell: the ribbon's Standards opens it beside the view.
Object.assign(screenRoutes, { standards: () => { currentPanel.value = "standards"; } });
const shellName = computed(() => { const s = here.value; return s.at === "model" ? `model.${s.show}` : s.at; });
import { dialogs, currentDialog, dialog } from "./dialogs";
// ---- SET1: File › Settings, Home's "change", the ribbon's About: dialogs, not screens of the shell ----
Object.assign(screenRoutes, { settings: () => { currentDialog.value = "settings"; }, about: () => { currentDialog.value = "about"; } });
// ---- end SET1 ----
import { panels, currentPanel, panel } from "./panels";
import { applyDeepLink } from "./deeplink";
// Under 1 024 px wide: the small-screen frame (ADR 0064) instead of the desktop bands.
import { small, wide } from "./small/screen";
import SmallTop from "./small/SmallTop.vue";
import SmallBar from "./small/SmallBar.vue";
import SmallSheet from "./small/SmallSheet.vue";
import SmallFloat from "./small/SmallFloat.vue";
// ---- PAPER1: the active plan or section On paper, over the viewport (kept alive, its canvas hidden) ----
import PaperView from "./shell/PaperView.vue";
import { openViews } from "./bimble";
const paperOn = computed(() => openViews.value.paper.on);
// ---- end PAPER1 ----

// ---- KEY1 ---- every shortcut, Esc's layers too (dialog, sheet, question, tool, selection, page): shell/keys.ts
import { installKeys } from "./shell/keys";
// ---- end KEY1 ----
// One focus trap for every dialog and menu (#79 AD).
const backdrop = ref<HTMLElement | null>(null);
const trapKey = useFocusTrap(backdrop, currentDialog);
// A change refused because someone else borrowed the element (ADR 0038): Rust reports it in
// `collab.belongs`; the alert (C2) opens by itself, as egui's does.
watch(() => state.value.rev, () => {
  if (!currentDialog.value && collab().belongs?.length) currentDialog.value = "borrowed";
});
const full = computed(() => page()?.layout === "full");
onMounted(() => { installKeys(); void applyDeepLink(); });
</script>

<template>
  <!-- Window frame of the wireframes v01 (DESIGN-SPEC.md § Window). Each band owns its file. -->
  <div class="bb-app" :class="{ small, wide }" :data-bb-shell="shellName">
    <SmallTop v-if="small" />
    <QuickBar v-if="!small" />
    <!-- A full-window page (`layout = "full"`) hides the bands it has no use for; #bimble stays in the DOM. -->
    <TabsBar v-if="!small" v-show="!full" />
    <Ribbon v-if="!small" v-show="!full" />
    <div class="bb-work">
      <LeftPanel v-if="!small" v-show="!full" />
      <div class="bb-main">
        <ViewTabs v-if="!small" v-show="!full" />
        <!-- Reference (D5): the right panel sits beside the viewport, under the view tabs. -->
        <div class="bb-row">
          <section v-if="currentPage && pages[currentPage]" class="bb-page" :class="{ full }" data-bb="page">
            <NotAvailable v-if="pageNa" v-bind="pageNa" /><!-- FAKE1 -->
            <component :is="pages[currentPage]" v-else />
          </section>
          <!-- Hidden, never removed, under a page. -->
          <div v-show="!(currentPage && pages[currentPage])" class="bb-vp" data-bb="viewport">
            <!-- Never re-created (no v-if): the viewer binds to it at init(). -->
            <canvas id="bimble" :style="paperOn ? { visibility: 'hidden' } : undefined" />
            <PaperView v-if="paperOn" /><!-- PAPER1 -->
            <SmallFloat v-if="small" />
            <p v-if="startError" class="start-error" role="alert">{{ t('app.startError', { msg: startError }) }}</p>
          </div>
          <aside v-if="currentPanel && panels[currentPanel]" class="bb-right" data-bb="right-panel" :aria-label="panelLabel">
            <NotAvailable v-if="panelNa" v-bind="panelNa" compact /><!-- FAKE1 -->
            <component :is="panels[currentPanel]" v-else />
          </aside>
          <SmallSheet v-if="small" />
        </div>
        <ModifierPanel v-if="!small" />
      </div>
    </div>
    <div v-if="currentDialog && dialogs[currentDialog]" ref="backdrop" class="bb-backdrop" tabindex="-1" @click.self="currentDialog = null" @keydown="trapKey">
      <div v-if="dialogNa" class="bb-dialog" role="dialog" :aria-label="dialogNa.what"><NotAvailable v-bind="dialogNa" compact /></div><!-- FAKE1 -->
      <component :is="dialogs[currentDialog]" v-else />
    </div>
    <ShellNotice />
    <StatusBar v-if="!small" />
    <SmallBar v-if="small" />
  </div>
</template>

<style>
html, body { margin: 0; height: 100%; }
body { background: var(--bb-cream); }
/* Dialog and menu framing (DESIGN-SPEC § Components): a dialog component's root takes one of these. */
.bb-dialog { display: flex; flex-direction: column; gap: 14px; padding: 24px; border-radius: 16px; background: var(--bb-cream); }
/* Menus have no dimmed backdrop (the reference draws none): the backdrop stays for click-outside, clear. */
.bb-app > .bb-backdrop:has(> .bb-menu) { background: none; }
.bb-menu { padding: 6px; border-radius: 16px; border: 1px solid var(--bb-stone); background: var(--bb-sheet); }
</style>

<style scoped>
.bb-page { flex: 1; min-height: 0; overflow: auto; padding: 24px 32px; background: var(--bb-sheet); }
/* The reference's body row for a full-window page: cream ground, the page owns its padding. */
.bb-page.full { padding: 0; background: var(--bb-cream); overflow: hidden; }
/* <div style="flex:1;display:flex;min-height:0"> */
.bb-row { flex: 1; display: flex; min-height: 0; }
/* aside style="width:400px;flex:none;border-left:1px solid #5C574D;background:#F5F2EB;padding:14px;...;overflow:hidden" */
.bb-right { width: 400px; flex: none; box-sizing: border-box; padding: 14px; border-left: 1px solid var(--bb-stone); background: var(--bb-cream); overflow: hidden; }
.bb-backdrop { position: fixed; inset: 0; z-index: 100; display: grid; place-items: center; background: color-mix(in srgb, var(--bb-ink) 30%, transparent); }
/* Small screens (ADR 0064): top bar, view, tools; a phone on its side or a tablet has the tools on a rail at the left. */
.bb-app.small { display: grid; grid-template: "top" auto "work" 1fr "bar" auto / 1fr; height: 100dvh; }
.bb-app.small.wide { grid-template: "bar top" auto "bar work" 1fr / auto 1fr; }
.bb-app.small .bb-work { grid-area: work; min-width: 0; }
.bb-app.small .bb-page { padding: 12px; }
#bimble { position: absolute; inset: 0; width: 100%; height: 100%; display: block; }
/* Error: ink text on an ochre band, with the word "Error". */
.start-error { position: absolute; inset: auto var(--bb-space-4) var(--bb-space-4); margin: 0; padding: var(--bb-space-2) var(--bb-space-3); background: var(--bb-error); color: var(--bb-ink); }
</style>
