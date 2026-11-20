<script setup lang="ts">
import Navigator from "./Navigator.vue";
import Properties from "../Properties.vue";
import PlaceSide from "./PlaceSide.vue";
import { page } from "../pages";
import { placing } from "../bimble";
import CollabSide from "./CollabSide.vue";
import SettingOutSide from "./SettingOutSide.vue";
import { activeTab } from "./tabs";
</script>

<template>
  <!-- Collaborate tab: "Travail partagé" (CollabSide). A page may export `side`: it replaces the properties while the page is current. Reference: aside.left > nav.nav + .props (fr/View3D.html). -->
  <aside class="left" data-bb="left-panel">
    <Navigator />
    <div class="props" data-bb="properties"><component :is="page()?.side ?? (placing?.active ? PlaceSide
      : activeTab === 'collaborate' ? CollabSide : activeTab === 'settingOut' ? SettingOutSide : Properties)" /></div>
  </aside>
</template>

<style scoped>
/* .left{width:296px;flex:none;display:flex;flex-direction:column;border-right:1px solid #DDD6C8;min-height:0} */
.left { width: 296px; flex: none; display: flex; flex-direction: column; border-right: var(--bb-border-hairline); min-height: 0; }
/* .props{flex:1 1 54%;overflow:hidden;padding:12px 14px;display:flex;flex-direction:column;gap:7px} */
.props { flex: 1 1 54%; overflow: hidden; padding: 12px 14px; display: flex; flex-direction: column; gap: 7px; }
</style>
