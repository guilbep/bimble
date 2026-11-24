<script setup lang="ts">
// Over the viewport (fr/PhoneViewer.html div.float): the view's name (opens Views).
// The reference's "3D home" and "Fit all" buttons are the viewer's own, drawn in the canvas under its view cube.
import { computed } from "vue";
import { openViews, state } from "../bimble";
import { viewName } from "../shell/viewName";
import { sheet } from "./screen";

const view = computed(() => openViews.value.views.find((v) => v.id === openViews.value.active) ?? null);
</script>

<template>
  <template v-if="state.file !== null">
    <div class="float" style="left:10px;top:10px">
      <button v-if="view" class="pill" type="button" data-bb="small-view" @click="sheet = 'views'">
        <span>{{ viewName(view) }}</span>
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg>
      </button>
    </div>
  </template>
</template>

<style scoped>
/* .ph .float{position:absolute;display:flex;gap:8px;align-items:center} */
.float { position: absolute; display: flex; gap: 8px; align-items: center; z-index: 2; }
.pill { display: inline-flex; align-items: center; gap: 6px; min-height: 44px; padding: 0 14px; border-radius: var(--bb-radius-pill); background: var(--bb-sheet); border: 1px solid var(--bb-stone); color: var(--bb-ink); font: inherit; font-size: 15px; white-space: nowrap; cursor: pointer; }
.pill:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
</style>
