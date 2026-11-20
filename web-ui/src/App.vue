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
import { computed, onMounted, onUnmounted, ref, watch } from "vue";
import { useFocusTrap } from "./shell/focusTrap";
import { pages, currentPage, page } from "./pages";
// Every caller (quick bar, File menu, palette, ribbon, Home) runs actions through runAction:
// these open the Vue screen of the same name (a ribbon tool with `page`/`panel` toggles it itself).
const openPage = (id: string) => () => { currentPage.value = id; };
// The aside's name: the panel's `label` key (fr/Standards.html: aria-label="Standards graphiques").
const panelLabel = computed(() => { const l = (panel() as { label?: string } | null)?.label; return l ? t(l) : undefined; });
Object.assign(screenRoutes, {
  home: openPage("home"),
  project: openPage("project-info"),
  history: openPage("history"),
  standards: () => { currentPanel.value = "standards"; },
});
import { dialogs, currentDialog } from "./dialogs";
import { panels, currentPanel, panel } from "./panels";
import { applyDeepLink } from "./deeplink";

// Esc closes the dialog first, then the page.
function onKey(e: KeyboardEvent) {
  if (e.key !== "Escape") return;
  if (currentDialog.value) currentDialog.value = null;
  else if (currentPage.value) currentPage.value = null;
}
// One focus trap for every dialog and menu (#79 AD).
const backdrop = ref<HTMLElement | null>(null);
const trapKey = useFocusTrap(backdrop, currentDialog);
// A change refused because someone else borrowed the element (ADR 0038): Rust reports it in
// `collab.belongs`; the alert (C2) opens by itself, as egui's does.
watch(() => state.value.rev, () => {
  if (!currentDialog.value && collab().belongs?.length) currentDialog.value = "borrowed";
});
const full = computed(() => page()?.layout === "full");
onMounted(() => { window.addEventListener("keydown", onKey); void applyDeepLink(); });
onUnmounted(() => window.removeEventListener("keydown", onKey));
</script>

<template>
  <!-- Window frame of the wireframes v01 (DESIGN-SPEC.md § Window). Each band owns its file. -->
  <div class="bb-app">
    <QuickBar />
    <!-- A full-window page (`layout = "full"`) hides the bands it has no use for; #bimble stays in the DOM. -->
    <TabsBar v-show="!full" />
    <Ribbon v-show="!full" />
    <div class="bb-work">
      <LeftPanel v-show="!full" />
      <div class="bb-main">
        <ViewTabs v-show="!full" />
        <!-- Reference (D5): the right panel sits beside the viewport, under the view tabs. -->
        <div class="bb-row">
          <section v-if="currentPage && pages[currentPage]" class="bb-page" :class="{ full }" data-bb="page">
            <component :is="pages[currentPage]" />
          </section>
          <!-- Hidden, never removed, under a page. -->
          <div v-show="!(currentPage && pages[currentPage])" class="bb-vp" data-bb="viewport">
            <!-- Never re-created (no v-if): the viewer binds to it at init(). -->
            <canvas id="bimble" />
            <p v-if="startError" class="start-error" role="alert">{{ t('app.startError', { msg: startError }) }}</p>
          </div>
          <aside v-if="currentPanel && panels[currentPanel]" class="bb-right" data-bb="right-panel" :aria-label="panelLabel">
            <component :is="panels[currentPanel]" />
          </aside>
            </div>
        <ModifierPanel />
      </div>
    </div>
    <div v-if="currentDialog && dialogs[currentDialog]" ref="backdrop" class="bb-backdrop" tabindex="-1" @click.self="currentDialog = null" @keydown="trapKey">
      <component :is="dialogs[currentDialog]" />
    </div>
    <StatusBar />
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
#bimble { position: absolute; inset: 0; width: 100%; height: 100%; display: block; }
/* Error: ink text on an ochre band, with the word "Error". */
.start-error { position: absolute; inset: auto var(--bb-space-4) var(--bb-space-4); margin: 0; padding: var(--bb-space-2) var(--bb-space-3); background: var(--bb-error); color: var(--bb-ink); }
</style>
