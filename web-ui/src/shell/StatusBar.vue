<template>
  <!-- Reference: footer.status. Hint, chips and FR/EN come in the StatusBar ticket. -->
  <footer class="status" data-bb="statusbar" />
</template>

<style scoped>
/* .status{display:flex;align-items:center;gap:8px;padding:4px 12px;background:#FFFFFF;border-top:1px solid #DDD6C8;font-size:13px;min-height:34px;flex-wrap:nowrap;white-space:nowrap} */
.status { display: flex; align-items: center; gap: var(--bb-space-2); padding: 4px 12px; background: var(--bb-sheet); border-top: var(--bb-border-hairline); font-size: var(--bb-size-small); min-height: 34px; flex-wrap: nowrap; white-space: nowrap; }
</style>
