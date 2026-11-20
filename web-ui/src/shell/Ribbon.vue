<template>
  <!-- Reference: .ribbon. Groups and tools come in the Ribbon ticket. -->
  <div class="ribbon" role="toolbar" data-bb="ribbon">
    <!-- Placeholder: one group, one tool with a two-line label, gives the reference height (97.94). -->
    <div class="grp" aria-hidden="true"><div class="tools"><span class="tool"><span class="ic" />&nbsp;<br />&nbsp;</span></div><div class="gname">&nbsp;</div></div>
  </div>
</template>

<style scoped>
/* .ribbon{display:flex;background:#FFFFFF;border-top:1px solid #EDE7DB;border-bottom:1px solid #DDD6C8;padding:6px 6px 4px;min-height:88px;flex-wrap:wrap;align-items:stretch} */
.ribbon { display: flex; background: var(--bb-sheet); border-top: 1px solid var(--bb-sunk); border-bottom: var(--bb-border-hairline); padding: 6px 6px 4px; min-height: 88px; flex-wrap: wrap; align-items: stretch; }
/* .grp{display:flex;flex-direction:column;padding:0 8px} .tools{display:flex;flex:1} .gname{font-size:12px;padding-top:3px} */
.grp { display: flex; flex-direction: column; padding: 0 8px; }
.tools { display: flex; gap: 2px; flex: 1; align-items: flex-start; }
.gname { font-size: var(--bb-size-eyebrow); padding-top: 3px; }
/* .tool{flex-direction:column;gap:3px;min-width:58px;padding:6px 6px 5px;font-size:12.5px;line-height:1.15;border:1px solid transparent}, icon 22 */
.tool { display: flex; flex-direction: column; align-items: center; gap: 3px; min-width: 58px; padding: 6px 6px 5px; font-size: 12.5px; line-height: 1.15; border: 1px solid transparent; }
.ic { width: 22px; height: 22px; }
</style>
