<script setup lang="ts">
import Navigator from "./Navigator.vue";
import Properties from "../Properties.vue";
import PlaceSide from "./PlaceSide.vue";
import { page } from "../pages";
import { placing, state } from "../bimble";
import CollabSide from "./CollabSide.vue";
import SettingOutSide from "./SettingOutSide.vue";
import SiteSide from "./SiteSide.vue";
import RebarSide from "./RebarSide.vue";
import SteelSide from "./SteelSide.vue";
import { activeTab } from "./tabs";
// ---- FAKE1: a side panel the core does not serve (shell/capability.ts) shows NotAvailable ----
import { computed, h } from "vue";
import { t } from "../i18n";
import NotAvailable from "./NotAvailable.vue";
import { blocked, has, INSTEAD } from "./capability";
const na = (id: string, why: "preview" | "readOnly" = "preview") =>
  () => h(NotAvailable, { what: t(`na.screen.${id}`), reason: why, compact: true, ...(INSTEAD[id] ? { instead: t(INSTEAD[id]!) } : {}) });
const SIDES = { collab: na("collab"), settingOut: na("settingOut"), site: na("site"), rebar: na("rebar"), steel: na("steel") };
const pageSide = computed(() => { const m = page(); return m?.side && !blocked(m) ? m.side : null; });
// ---- end FAKE1 ----
</script>

<template>
  <!-- Collaborate tab: "Travail partagé" (CollabSide). Site tab with nothing selected: the excavation (SiteSide, A8). A page may export `side`: it replaces the properties while the page is current. Reference: aside.left > nav.nav + .props (fr/View3D.html). -->
  <aside class="left" data-bb="left-panel">
    <Navigator />
    <div class="props" data-bb="properties"><component :is="pageSide ?? (placing?.active ? PlaceSide
      : activeTab === 'collaborate' ? (has('collaboration') ? CollabSide : SIDES.collab) : activeTab === 'settingOut' ? (has('georef') ? SettingOutSide : SIDES.settingOut)
      : activeTab === 'site' && !state.selection.length ? (has('site') ? SiteSide : SIDES.site)
      : activeTab === 'rebar' && !state.selection.length ? (has('rebar') ? RebarSide : SIDES.rebar)
      : (activeTab === 'steel' || activeTab === 'timber') && !state.selection.length ? (has('steel') ? SteelSide : SIDES.steel) : Properties)" /></div>
  </aside>
</template>

<style scoped>
/* .left{width:296px;flex:none;display:flex;flex-direction:column;border-right:1px solid #DDD6C8;min-height:0} */
.left { width: 296px; flex: none; display: flex; flex-direction: column; border-right: var(--bb-border-hairline); min-height: 0; }
/* .props{flex:1 1 54%;overflow:hidden;padding:12px 14px;display:flex;flex-direction:column;gap:7px} */
.props { flex: 1 1 54%; overflow: hidden; padding: 12px 14px; display: flex; flex-direction: column; gap: 7px; }
</style>
