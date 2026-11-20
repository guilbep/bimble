<script setup lang="ts">
import Properties from "../Properties.vue";
</script>

<template>
  <!-- Reference: aside.left > nav.nav + .props. The navigator tree comes in the LeftPanel ticket. -->
  <aside class="left" data-bb="left-panel">
    <nav class="nav" data-bb="navigator" />
    <div class="props" data-bb="properties"><Properties /></div>
  </aside>
</template>

<style scoped>
/* .left{width:296px;flex:none;display:flex;flex-direction:column;border-right:1px solid #DDD6C8;min-height:0} */
.left { width: 296px; flex: none; display: flex; flex-direction: column; border-right: var(--bb-border-hairline); min-height: 0; }
/* .nav{flex:none;overflow:hidden;padding:12px 12px 8px;border-bottom:1px solid #DDD6C8;display:flex;flex-direction:column;gap:1px} */
.nav { flex: none; overflow: hidden; padding: 12px 12px 8px; border-bottom: var(--bb-border-hairline); display: flex; flex-direction: column; gap: 1px; }
/* .props{flex:1 1 54%;overflow:hidden;padding:12px 14px;display:flex;flex-direction:column;gap:7px} */
.props { flex: 1 1 54%; overflow: hidden; padding: 12px 14px; display: flex; flex-direction: column; gap: 7px; }
</style>
