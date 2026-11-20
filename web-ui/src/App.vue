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
        <div class="bb-vp" data-bb="viewport">
          <!-- Never re-created (no v-if): the viewer binds to it at init(). -->
          <canvas id="bimble" />
          <p v-if="startError" class="start-error" role="alert">{{ t('app.startError', { msg: startError }) }}</p>
        </div>
        <ModifierPanel />
      </div>
    </div>
    <StatusBar />
  </div>
</template>

<style>
html, body { margin: 0; height: 100%; }
body { background: var(--bb-cream); }
</style>

<style scoped>
#bimble { position: absolute; inset: 0; width: 100%; height: 100%; display: block; }
/* Error: ink text on an ochre band, with the word "Error". */
.start-error { position: absolute; inset: auto var(--bb-space-4) var(--bb-space-4); margin: 0; padding: var(--bb-space-2) var(--bb-space-3); background: var(--bb-error); color: var(--bb-ink); }
</style>
