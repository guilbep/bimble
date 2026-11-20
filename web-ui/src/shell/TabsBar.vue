<template>
  <!-- Reference: nav.tabs. The 14 tabs come in the TabsBar ticket. -->
  <nav class="tabs" data-bb="tabsbar"><span class="tab" aria-hidden="true">&nbsp;</span></nav>
</template>

<style scoped>
/* .tabs{display:flex;gap:2px;padding:2px 12px 6px;background:#FFFFFF;flex-wrap:wrap;align-items:center} */
.tabs { display: flex; gap: 2px; padding: 2px 12px 6px; background: var(--bb-sheet); flex-wrap: wrap; align-items: center; }
/* .tab{padding:4px 12px;font-size:14px;line-height:20px}: the placeholder holds the row height. */
.tab { padding: 4px 12px; font-size: var(--bb-size-ui); line-height: 20px; }
</style>
