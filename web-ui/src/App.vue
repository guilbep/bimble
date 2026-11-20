<script setup lang="ts">
import { state, startError, undo, redo } from "./bimble";
import Properties from "./Properties.vue";
import labIcon from "./brand/bimble-icon-lab.svg";
</script>

<template>
  <header class="bar">
    <img class="mark" :src="labIcon" alt="Bimble lab" width="26" height="26" />
    <span class="word">bimble</span>
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
      <p v-if="startError" class="start-error" role="alert">Error: Bimble could not start. {{ startError }}</p>
    </div>
  </main>
</template>

<style>
*, *::before, *::after { box-sizing: border-box; }
html, body { margin: 0; height: 100%; }
/* Ground, ink, fonts and focus come from the skill's .bb base on <html>. */
body { background: var(--bb-cream); }
#app { display: flex; flex-direction: column; min-height: 100vh; }
</style>

<style scoped>
/* Top bar: sheet, sunk bottom border, 10px 14px, 14px text (components.md "Top bar"). */
.bar { display: flex; align-items: center; gap: var(--bb-space-3); padding: 10px 14px; background: var(--bb-sheet); border-bottom: 1px solid var(--bb-sunk); font-size: var(--bb-size-ui); flex-wrap: wrap; }
.mark { width: 26px; height: 26px; flex: none; display: block; }
/* Only the word "bimble" is set in the wordmark font. */
.word { font-family: var(--bb-font-wordmark); font-size: var(--bb-size-heading); line-height: 1; }
.file { font-family: var(--bb-font-display); font-weight: 500; overflow-wrap: anywhere; }
/* Chips: pill, 3px 12px, ink text 14px on a disc fill, state in words. */
.chip { border-radius: var(--bb-radius-pill); padding: 3px 12px; font-size: var(--bb-size-ui); font-weight: 600; color: var(--bb-ink); }
.chip-ochre { background: var(--bb-attention); }
.chip-moss { background: var(--bb-success); }
.spacer { flex: 1; }
/* Secondary button: sheet fill, 1px ink border, ink text, pill, 32px min height. */
.pill { font: 600 var(--bb-size-ui) var(--bb-font-text); min-height: var(--bb-target); color: var(--bb-ink); background: var(--bb-sheet); border: 1px solid var(--bb-ink); border-radius: var(--bb-radius-pill); padding: 6px 16px; cursor: pointer; }
.pill:hover { background: var(--bb-sunk); }
.pill:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
.body { flex: 1; display: flex; flex-wrap: wrap; min-height: 0; }
/* Viewport: square, sheet ground. */
.view { flex: 1 1 320px; position: relative; min-height: 360px; min-width: 0; background: var(--bb-sheet); border-radius: var(--bb-radius-none); }
#bimble { position: absolute; inset: 0; width: 100%; height: 100%; display: block; }
/* Error: ink text on an ochre band, with the word "Error". */
.start-error { position: absolute; inset: auto var(--bb-space-4) var(--bb-space-4); margin: 0; padding: var(--bb-space-2) var(--bb-space-3); background: var(--bb-error); color: var(--bb-ink); }
</style>
