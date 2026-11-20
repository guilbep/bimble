<script setup lang="ts">
// Collaborate tab's left panel, "Travail partagé" (fr/SharedFolder.html, fr/Borrowed.html: aside.left > .props).
// query('collab'): desktop only today, so on the web every value is "—" (null).
import { computed } from "vue";
import { collab, state } from "../bimble";
import { t } from "../i18n";
const c = computed(() => (state.value.rev, collab()));
const when = (s: number | null) => (s == null ? null : new Date(s * 1000).toLocaleString());
const rows = computed(() => [
  ["shell.collab.folder", c.value.folder],
  ["shell.collab.you", c.value.user],
  ["shell.collab.right", c.value.right],
  ["shell.collab.lastSync", when(c.value.last_sync)],
] as const);
</script>

<template>
  <div class="pn">{{ t("shell.collab.title") }}</div>
  <div v-for="[k, v] in rows" :key="k" class="row"><span class="k">{{ t(k) }}</span><span>{{ v ?? "—" }}</span></div>
  <div class="sec">{{ t("shell.collab.borrowed") }}</div>
  <div class="row"><span class="k">{{ t("shell.collab.byYou") }}</span><span class="mono">{{ c.folder ? c.borrowed_by_me : "—" }}</span></div>
  <div v-for="o in c.borrowed_by_others" :key="o.who" class="row"><span class="k">{{ o.who }}</span><span class="mono">{{ o.count }}</span></div>
  <div style="margin-top:auto;display:flex;flex-direction:column;gap:6px;font-size:13px">
    <div style="display:flex;gap:8px;align-items:center"><span class="dot" style="background:var(--bb-ochre)"></span>{{ t("shell.collab.legendBorrowed") }}</div>
    <div style="display:flex;gap:8px;align-items:center" data-missing="linked elements (no Rust notion yet)"><span class="dot" style="background:var(--bb-slate)"></span>{{ t("shell.collab.legendLinked") }}</div>
  </div>
</template>

<style scoped>
/* .pn{font-family:'Sora',sans-serif;font-weight:500;font-size:14px;margin:0 0 4px} */
.pn { font-family: 'Sora', sans-serif; font-weight: 500; font-size: 14px; margin: 0 0 4px; }
/* .row{display:flex;justify-content:space-between;align-items:center;gap:10px;font-size:13.5px;min-height:22px} */
.row { display: flex; justify-content: space-between; align-items: center; gap: 10px; font-size: 13.5px; min-height: 22px; }
/* .k{color:#5C574D;white-space:nowrap} */
.k { color: var(--bb-stone); white-space: nowrap; }
/* .mono{font-family:'DM Mono',monospace;font-size:13px} */
.mono { font-family: 'DM Mono', monospace; font-size: 13px; }
/* .sec{font-size:12px;font-family:'DM Mono',monospace;letter-spacing:.08em;text-transform:uppercase;color:#5C574D;margin-top:6px} */
.sec { font-size: 12px; font-family: 'DM Mono', monospace; letter-spacing: .08em; text-transform: uppercase; color: var(--bb-stone); margin-top: 6px; }
/* .dot{width:12px;height:12px;border-radius:50%;border:1px solid #141414;display:inline-block;flex:none} */
.dot { width: 12px; height: 12px; border-radius: 50%; border: 1px solid var(--bb-ink); display: inline-block; flex: none; }
</style>
