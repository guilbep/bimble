<script setup lang="ts">
import { t } from "../i18n";
import { activeTab } from "./tabs";
import { RIBBON } from "./ribbon-tabs";
import RibbonTool from "./RibbonTool.vue";
import { HIDE_NOT_AVAILABLE } from "./capability"; // FAKE1
import BoxNorth from "./BoxNorth.vue"; // ISS71: the section box's orientation
import ProjectZeroBox from "./ProjectZeroBox.vue"; // SETOUT: project zero (the georeference's height)
</script>

<template>
  <!-- Reference: .ribbon > .grp (.tools > .tool, .gname). Content: ribbon-tabs.ts for the active tab. -->
  <div class="ribbon" role="toolbar" data-bb="ribbon">
    <div v-for="(grp, g) in RIBBON[activeTab]" :key="grp.key" class="grp" :data-bb="`ribbon-grp-${g + 1}`">
      <div class="tools" :class="{ center: grp.box }">
        <RibbonTool v-for="(tool, m) in grp.tools" :key="tool.key" :tool="tool" :bb="`ribbon-tool-${g + 1}-${m + 1}`" />
        <BoxNorth v-if="grp.north" /><!-- ISS71 -->
        <!-- Reference .rbox: no Rust call yet, drawn disabled (.off) and empty (no sample value). -->
        <!-- FAKE1: a box with no call behind it is "not yet" (ny), with the tooltip, as a tool. -->
        <ProjectZeroBox v-if="grp.box?.kind === 'zero'" :label="grp.box.key" :bb="`ribbon-box-${g + 1}`" /><!-- SETOUT -->
        <div v-else-if="grp.box && !HIDE_NOT_AVAILABLE" class="rbox off ny" :class="{ readout: grp.box.kind === 'readout' }" aria-disabled="true"
             :data-missing="grp.box.missing" :data-bb="`ribbon-box-${g + 1}`" data-not-yet="" :title="t('na.tooltip', { name: t(grp.box.key) })">
          <span class="lbl">{{ t(grp.box.key) }}</span>
          <span v-if="grp.box.kind === 'sel'" class="sel-inp" style="width:150px"><span /><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg></span>
          <span v-else class="mono" style="font-size:15px">&nbsp;</span>
        </div>
      </div>
      <div class="gname">{{ t(grp.key) }}</div>
    </div>
  </div>
</template>

<style scoped>
/* .ribbon{display:flex;background:#FFFFFF;border-top:1px solid #EDE7DB;border-bottom:1px solid #DDD6C8;padding:6px 6px 4px;min-height:88px;flex-wrap:wrap;align-items:stretch} */
.ribbon { display: flex; background: var(--bb-sheet); border-top: 1px solid var(--bb-sunk); border-bottom: var(--bb-border-hairline); padding: 6px 6px 4px; min-height: 88px; flex-wrap: wrap; align-items: stretch; }
/* .grp{display:flex;flex-direction:column;padding:0 8px;border-right:1px solid #EDE7DB} .grp:last-child{border-right:0} */
.grp { display: flex; flex-direction: column; padding: 0 8px; border-right: 1px solid var(--bb-sunk); }
.grp:last-child { border-right: 0; }
/* .tools{display:flex;gap:2px;flex:1;align-items:flex-start} */
.tools { display: flex; gap: 2px; flex: 1; align-items: flex-start; }
/* style="align-items:center" on .tools of a group holding a .rbox */
.tools.center { align-items: center; }
/* .rbox{display:flex;flex-direction:column;justify-content:center;gap:4px;padding:4px 10px;font-size:12.5px} */
.rbox { display: flex; flex-direction: column; justify-content: center; gap: 4px; padding: 4px 10px; font-size: 12.5px; }
/* Altitude: style="border:1px solid #5C574D;border-radius:12px;margin:4px 2px" */
.rbox.readout { border: 1px solid var(--bb-stone); border-radius: 12px; margin: 4px 2px; }
/* .off{color:#5C574D;opacity:.55}: no Rust call yet (data-missing). */
.off { color: var(--bb-stone); opacity: .55; }
/* FAKE1: not in this preview (RibbonTool.vue .ny) */
.rbox.ny { opacity: .8; border: 1px dashed var(--bb-line); border-radius: 12px; cursor: help; }
/* .lbl{font-size:13px;color:#5C574D} */
.lbl { font-size: 13px; color: var(--bb-stone); }
/* .sel-inp{display:inline-flex;align-items:center;justify-content:space-between;gap:6px;height:30px;border:1px solid #5C574D;border-radius:8px;background:#FFFFFF;padding:0 6px 0 8px;font-size:13.5px;white-space:nowrap} */
/* .mono{font-family:'DM Mono',ui-monospace,Menlo,monospace} */
.mono { font-family: var(--bb-font-data); }
/* .gname{font-size:12px;color:#5C574D;text-align:center;padding-top:3px;white-space:nowrap} */
.gname { font-size: var(--bb-size-eyebrow); color: var(--bb-stone); text-align: center; padding-top: 3px; white-space: nowrap; }
</style>
