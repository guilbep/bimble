<script setup lang="ts">
// Collaborate tab's left panel, "Travail partagé" (fr/SharedFolder.html, fr/Borrowed.html: aside.left > .props).
// ponytail: no Rust call reads the shared folder, the user, the right, the last sync or the borrowed counts:
// every value is "—" with data-missing naming the call it needs; the per-person borrowed rows are omitted (no list source).
import { t } from "../i18n";
const MISSING = "query('collab')";
const rows = ["shell.collab.folder", "shell.collab.you", "shell.collab.right", "shell.collab.lastSync"] as const;
</script>

<template>
  <div class="pn">{{ t("shell.collab.title") }}</div>
  <div v-for="k in rows" :key="k" class="row"><span class="k">{{ t(k) }}</span><span :data-missing="MISSING">—</span></div>
  <div class="sec">{{ t("shell.collab.borrowed") }}</div>
  <div class="row"><span class="k">{{ t("shell.collab.byYou") }}</span><span class="mono" :data-missing="MISSING">—</span></div>
  <div style="margin-top:auto;display:flex;flex-direction:column;gap:6px;font-size:13px">
    <div style="display:flex;gap:8px;align-items:center"><span class="dot" style="background:var(--bb-ochre)"></span>{{ t("shell.collab.legendBorrowed") }}</div>
    <div style="display:flex;gap:8px;align-items:center" :data-missing="MISSING"><span class="dot" style="background:var(--bb-slate)"></span>{{ t("shell.collab.legendLinked") }}</div>
  </div>
</template>

<style scoped>
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: var(--bb-font-display); font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
/* .k{color:#5C574D;white-space:nowrap} */
.k { color: var(--bb-stone); white-space: nowrap; }
/* .mono{font-family:'DM Mono',monospace;font-size:13px} */
.mono { font-family: var(--bb-font-data); font-size: 13px; }
/* .sec{font-size:12px;font-family:'DM Mono',monospace;letter-spacing:.08em;text-transform:uppercase;color:#5C574D;margin-top:6px} */
.sec { font-size: 12px; font-family: var(--bb-font-data); letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
/* .dot{width:12px;height:12px;border-radius:50%;border:1px solid #141414;display:inline-block;flex:none} */
.dot { width: 12px; height: 12px; border-radius: 50%; border: 1px solid var(--bb-ink); display: inline-block; flex: none; }
</style>
