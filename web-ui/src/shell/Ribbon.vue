<script setup lang="ts">
import { t } from "../i18n";
import { activeTab } from "./tabs";
import { RIBBON } from "./ribbon-tabs";
import RibbonTool from "./RibbonTool.vue";
</script>

<template>
  <!-- Reference: .ribbon > .grp (.tools > .tool, .gname). Content: ribbon-tabs.ts for the active tab. -->
  <div class="ribbon" role="toolbar" data-bb="ribbon">
    <div v-for="(grp, g) in RIBBON[activeTab]" :key="grp.key" class="grp" :data-bb="`ribbon-grp-${g + 1}`">
      <div class="tools">
        <RibbonTool v-for="(tool, m) in grp.tools" :key="tool.key" :tool="tool" :bb="`ribbon-tool-${g + 1}-${m + 1}`" />
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
/* .gname{font-size:12px;color:#5C574D;text-align:center;padding-top:3px;white-space:nowrap} */
.gname { font-size: var(--bb-size-eyebrow); color: var(--bb-stone); text-align: center; padding-top: 3px; white-space: nowrap; }
</style>
