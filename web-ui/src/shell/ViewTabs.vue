<template>
  <!-- Reference: .main > .vtabs. The view tabs come in the ViewTabs ticket. -->
  <div class="vtabs" data-bb="viewtabs"><span class="vtab" aria-hidden="true"><span class="xx" /></span></div>
</template>

<style scoped>
/* .vtabs{display:flex;gap:4px;padding:6px 10px;border-bottom:1px solid #DDD6C8;align-items:center;flex-wrap:wrap} */
.vtabs { display: flex; gap: 4px; padding: 6px 10px; border-bottom: var(--bb-border-hairline); align-items: center; flex-wrap: wrap; }
/* .vtab{padding:2px 4px 2px 12px;font-size:13px;border:1px solid transparent} .vtab .xx{20x20}: the placeholder holds the strip height. */
.vtab { padding: 2px 4px 2px 12px; font-size: var(--bb-size-small); display: inline-flex; border: 1px solid transparent; }
.xx { width: 20px; height: 20px; }
</style>
