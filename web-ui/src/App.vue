<script setup lang="ts">
import { startError } from "./bimble";
import { t } from "./i18n";
import QuickBar from "./shell/QuickBar.vue";
import TabsBar from "./shell/TabsBar.vue";
import Ribbon from "./shell/Ribbon.vue";
import LeftPanel from "./shell/LeftPanel.vue";
import ViewTabs from "./shell/ViewTabs.vue";
import ModifierPanel from "./shell/ModifierPanel.vue";
import StatusBar from "./shell/StatusBar.vue";
import "./shell/shell.css";
import { onMounted, onUnmounted } from "vue";
import { pages, currentPage } from "./pages";
import { dialogs, currentDialog } from "./dialogs";
import { panels, currentPanel } from "./panels";
import { applyDeepLink } from "./deeplink";

// Esc closes the dialog first, then the page.
function onKey(e: KeyboardEvent) {
  if (e.key !== "Escape") return;
  if (currentDialog.value) currentDialog.value = null;
  else if (currentPage.value) currentPage.value = null;
}
onMounted(() => { window.addEventListener("keydown", onKey); void applyDeepLink(); });
onUnmounted(() => window.removeEventListener("keydown", onKey));
</script>

<template>
  <!-- Window frame of the wireframes v01 (DESIGN-SPEC.md § Window). Each band owns its file. -->
  <div class="bb-app">
    <QuickBar />
    <TabsBar />
    <Ribbon />
    <div class="bb-work">
      <LeftPanel />
      <div class="bb-main">
        <ViewTabs />
        <section v-if="currentPage && pages[currentPage]" class="bb-page" data-bb="page">
          <component :is="pages[currentPage]" />
        </section>
        <!-- Hidden, never removed, under a page. -->
        <div v-show="!(currentPage && pages[currentPage])" class="bb-vp" data-bb="viewport">
          <!-- Never re-created (no v-if): the viewer binds to it at init(). -->
          <canvas id="bimble" />
          <p v-if="startError" class="start-error" role="alert">{{ t('app.startError', { msg: startError }) }}</p>
        </div>
        <ModifierPanel />
      </div>
      <aside v-if="currentPanel && panels[currentPanel]" class="bb-right" data-bb="right-panel">
        <component :is="panels[currentPanel]" />
      </aside>
    </div>
    <div v-if="currentDialog && dialogs[currentDialog]" class="bb-backdrop" @click.self="currentDialog = null">
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
.bb-menu { padding: 6px; border-radius: 16px; border: 1px solid var(--bb-stone); background: var(--bb-sheet); }
</style>

<style scoped>
.bb-page { flex: 1; min-height: 0; overflow: auto; padding: 24px 32px; background: var(--bb-sheet); }
.bb-right { width: 400px; flex: none; box-sizing: border-box; padding: 14px; border-left: 1px solid var(--bb-stone); background: var(--bb-cream); overflow: auto; }
.bb-backdrop { position: fixed; inset: 0; z-index: 100; display: grid; place-items: center; background: color-mix(in srgb, var(--bb-ink) 30%, transparent); }
#bimble { position: absolute; inset: 0; width: 100%; height: 100%; display: block; }
/* Error: ink text on an ochre band, with the word "Error". */
.start-error { position: absolute; inset: auto var(--bb-space-4) var(--bb-space-4); margin: 0; padding: var(--bb-space-2) var(--bb-space-3); background: var(--bb-error); color: var(--bb-ink); }
</style>
