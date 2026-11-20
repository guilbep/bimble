<script setup lang="ts">
import Navigator from "./Navigator.vue";
import Properties from "../Properties.vue";
import { page } from "../pages";
</script>

<template>
  <!-- A page may export `side`: it replaces the properties while the page is current. Reference: aside.left > nav.nav + .props (fr/View3D.html). -->
  <aside class="left" data-bb="left-panel">
    <Navigator />
    <div class="props" data-bb="properties"><component :is="page()?.side ?? Properties" /></div>
  </aside>
</template>

<style scoped>
/* .left{width:296px;flex:none;display:flex;flex-direction:column;border-right:1px solid #DDD6C8;min-height:0} */
.left { width: 296px; flex: none; display: flex; flex-direction: column; border-right: var(--bb-border-hairline); min-height: 0; }
/* .props{flex:1 1 54%;overflow:hidden;padding:12px 14px;display:flex;flex-direction:column;gap:7px} */
.props { flex: 1 1 54%; overflow: hidden; padding: 12px 14px; display: flex; flex-direction: column; gap: 7px; }
</style>
