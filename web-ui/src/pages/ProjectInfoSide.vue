<script setup lang="ts">
// A3's left panel properties (fr/ProjectInfo.html `.props`): shown by LeftPanel while the page is current.
import { computed } from "vue";
import { project, state } from "../bimble";
import { t } from "../i18n";

const entries = computed(() => Object.keys(project.value?.info ?? {}).length);
</script>

<template>
  <div class="pn">{{ t('projectinfo.side.title') }}</div>
  <div class="row"><span class="k">{{ t('projectinfo.side.page') }}</span><span>{{ t('filemenu.projectInfo') }}</span></div>
  <div class="row" data-bb="project-side-entries"><span class="k">{{ t('projectinfo.side.entries') }}</span><span class="mono">{{ entries }}</span></div>
  <div class="row"><span class="k">{{ t('projectinfo.side.stored') }}</span><span>{{ state.file ?? '' }}</span></div>
  <div class="sec">{{ t('projectinfo.side.titleBlocks') }}</div>
  <p class="note" style="margin:0">{{ t('projectinfo.side.keyNote') }}</p>
</template>

<style scoped>
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
.k { color: var(--bb-stone); white-space: nowrap; }
.mono { font-family: 'DM Mono', monospace; }
/* .sec{font-size:12px;font-family:'DM Mono',monospace;letter-spacing:.08em;text-transform:uppercase;color:#5C574D;margin-top:6px} */
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
.note { font-size: 13px; color: var(--bb-stone); }
</style>
