<script setup lang="ts">
import { state, undo, redo } from "../bimble";
import { t } from "../i18n";
import labIcon from "../brand/bimble-icon-lab.svg";
</script>

<template>
  <!-- Reference: header.top. Content (Home, Fichier, icon buttons, command field) comes in the QuickBar ticket. -->
  <header class="top" data-bb="quickbar">
    <span class="ib" data-bb="quickbar-logo"><img :src="labIcon" :alt="t('app.logoAlt')" width="26" height="26" /></span>
    <div class="fname">
      <span data-bb="filename">{{ state.file ?? t('app.noFile') }}</span>
      <span v-if="state.unsaved" class="chip c-o" data-bb="state-chip">{{ t('app.notSaved') }}</span>
      <span v-else class="chip c-m" data-bb="state-chip">{{ t('app.saved') }}</span>
    </div>
    <button class="qb" type="button" @click="undo()">{{ t('app.undo') }}</button>
    <button class="qb" type="button" @click="redo()">{{ t('app.redo') }}</button>
  </header>
</template>

<style scoped>
/* .top{display:flex;align-items:center;gap:6px;padding:6px 12px;background:#FFFFFF;min-height:46px;flex-wrap:wrap} */
.top { display: flex; align-items: center; gap: 6px; padding: 6px 12px; background: var(--bb-sheet); min-height: 46px; flex-wrap: wrap; }
/* .ib{width:32px;height:32px;border-radius:999px;...} */
.ib { width: var(--bb-target); height: var(--bb-target); border-radius: var(--bb-radius-pill); display: inline-flex; align-items: center; justify-content: center; flex: none; }
.ib img { display: block; }
/* .qb{height:32px;padding:0 12px;border-radius:999px;border:0;background:none} */
.qb { display: inline-flex; align-items: center; gap: 4px; height: var(--bb-target); padding: 0 12px; border-radius: var(--bb-radius-pill); border: 0; background: none; color: var(--bb-ink); font: inherit; cursor: pointer; white-space: nowrap; }
.qb:hover { background: var(--bb-sunk); }
.qb:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* .fname{display:flex;align-items:center;gap:8px;margin:0 auto;font-size:14px;min-width:0} */
.fname { display: flex; align-items: center; gap: var(--bb-space-2); margin: 0 auto; font-size: var(--bb-size-ui); min-width: 0; }
/* .chip{padding:2px 10px;border-radius:999px;font-size:13px} (.chip.nx) */
.chip { padding: 2px 10px; border-radius: var(--bb-radius-pill); font-size: var(--bb-size-small); display: inline-flex; gap: 4px; align-items: center; white-space: nowrap; color: var(--bb-ink); }
.c-o { background: var(--bb-attention); }
.c-m { background: var(--bb-success); }
</style>
