<script setup lang="ts">
import { state, startError, undo, redo } from "./bimble";
import Properties from "./Properties.vue";
</script>

<template>
  <header class="bar">
    <!-- Lab mark: logo study 7 (moss disc), ADR 0049. -->
    <svg class="mark" viewBox="0 0 207.18 207.18" role="img" aria-label="Bimble lab">
      <circle cx="103.59" cy="103.59" r="103.59" fill="var(--moss)" />
      <path fill="var(--ink)" d="M101.25 38.54H134.85V116.44A13 13 0 1 0 139.85 106.19V70.03A46.6 46.6 0 1 1 101.25 115.94Z" />
    </svg>
    <span class="file">{{ state.file ?? "No file" }}</span>
    <span v-if="state.unsaved" class="chip chip-ochre">Not saved</span>
    <span v-else class="chip chip-moss">Saved</span>
    <span class="spacer" />
    <button class="pill" type="button" aria-label="Undo" @click="undo()">Undo</button>
    <button class="pill" type="button" aria-label="Redo" @click="redo()">Redo</button>
  </header>
  <main class="body">
    <Properties />
    <div class="view">
      <!-- Never re-created (no v-if): the viewer binds to it at init(). -->
      <canvas id="bimble" />
      <p v-if="startError" class="start-error">Bimble could not start: {{ startError }}</p>
    </div>
  </main>
</template>

<style>
*, *::before, *::after { box-sizing: border-box; }
html, body { margin: 0; height: 100%; }
body { background: var(--cream); color: var(--ink); font: 16px/1.4 var(--font-text); }
#app { display: flex; flex-direction: column; min-height: 100vh; }
</style>

<style scoped>
.bar { display: flex; align-items: center; gap: 12px; padding: 8px 16px; border-bottom: 1px solid var(--line); flex-wrap: wrap; }
.mark { width: 32px; height: 32px; flex: none; }
.file { font-family: var(--font-heading); font-weight: 500; overflow-wrap: anywhere; }
.chip { border-radius: 999px; padding: 2px 10px; font-size: 14px; font-weight: 600; color: var(--ink); }
.chip-ochre { background: var(--ochre); }
.chip-moss { background: var(--moss); }
.spacer { flex: 1; }
.pill { font: 600 15px var(--font-text); color: var(--ink); background: var(--sheet); border: 1px solid var(--line); border-radius: 999px; padding: 6px 16px; cursor: pointer; }
.pill:hover { border-color: var(--stone); }
.pill:focus-visible { outline: 2px solid var(--ink); outline-offset: 2px; }
.body { flex: 1; display: flex; flex-wrap: wrap; min-height: 0; }
.view { flex: 1 1 320px; position: relative; min-height: 360px; min-width: 0; }
#bimble { position: absolute; inset: 0; width: 100%; height: 100%; display: block; }
.start-error { position: absolute; inset: auto 16px 16px; margin: 0; padding: 8px 12px; background: var(--ochre); color: var(--ink); }
</style>
