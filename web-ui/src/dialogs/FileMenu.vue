<script lang="ts">
export const hint = "filemenu.hint";
</script>

<script setup lang="ts">
// Screen A2, File menu (wireframes v01 fr/FileMenu.html). Each entry runs one egui action by name (`query("actions")`).
import { actions, runAction } from "../bimble";
import { t } from "../i18n";
import type * as copy from "../i18n/parts/filemenu";
import { currentDialog } from ".";

type Key = keyof typeof copy.fr;
const tx = (k: Key) => t(k);

// Icon paths copied from the reference.
const NEW = '<path d="M5 3h10l4 4v14H5z"/><path d="M12 10v6M9 13h6"/>';
const UP = '<path d="M12 15V3"/><path d="m7 8 5-5 5 5"/><path d="M5 13v8h14v-8"/>';
const INFO = '<circle cx="12" cy="12" r="8.5"/><path d="M12 11v5"/><path d="M12 8h.01"/>';

interface Entry { key: Key; icon: string; kbd?: string; action?: string; missing?: string }
const sections: { title: Key; entries: Entry[] }[] = [
  { title: "filemenu.files", entries: [
    { key: "filemenu.new", icon: NEW, kbd: "Ctrl+N", action: "new" },
    { key: "filemenu.open", icon: '<path d="M3 7h6l2 2h10v10H3z"/><path d="M3 7V5h6"/>', kbd: "Ctrl+O", action: "open" },
    { key: "filemenu.importIfc", icon: '<path d="M12 3v12"/><path d="m7 10 5 5 5-5"/><path d="M5 13v8h14v-8"/>', action: "import_ifc" },
    { key: "filemenu.convert", icon: NEW, action: "convert_open" },
    { key: "filemenu.save", icon: '<path d="M5 4h11l3 3v13H5z"/><path d="M8 4v5h7V4"/><rect x="8" y="13" width="8" height="5"/>', kbd: "Ctrl+S", action: "save" },
  ] },
  { title: "filemenu.share", entries: [
    { key: "filemenu.shareView", icon: '<circle cx="6" cy="12" r="2.2"/><circle cx="17" cy="6" r="2.2"/><circle cx="17" cy="18" r="2.2"/><path d="m8 11 7-4M8 13l7 4"/>', action: "share_for_viewing" },
    { key: "filemenu.ifc4", icon: UP, action: "export_ifc4" },
    { key: "filemenu.ifc2x3", icon: UP, action: "export_ifc2x3" },
  ] },
  { title: "filemenu.paper", entries: [
    // Print and export is screen D4 (not built): no Rust call opens it.
    { key: "filemenu.print", icon: '<path d="M7 9V4h10v5"/><rect x="4" y="9" width="16" height="7" rx="1"/><path d="M7 14h10v6H7z"/>', kbd: "Ctrl+P", missing: "export-screen-D4" },
    // egui "Export view… (PDF, SVG)".
    { key: "filemenu.exportView", icon: UP, action: "on_paper" },
  ] },
  { title: "filemenu.project", entries: [
    { key: "filemenu.projectInfo", icon: INFO, action: "project" },
    { key: "filemenu.history", icon: '<path d="M4 12a8 8 0 1 0 2.4-5.7L4 8"/><path d="M4 4v4h4"/><path d="M12 8v4l3 2"/>', action: "history" },
    { key: "filemenu.settings", icon: '<circle cx="12" cy="12" r="3"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3M4.9 4.9l2.1 2.1M17 17l2.1 2.1M4.9 19.1 7 17M17 7l2.1-2.1"/>', kbd: "Ctrl+,", action: "settings" },
    { key: "filemenu.about", icon: INFO, action: "about" },
  ] },
];

const on = (name?: string) => !!name && actions.value.some((a) => a.name === name && a.enabled);
function run(name?: string) {
  currentDialog.value = null;
  if (name) {
    const err = runAction(name);
    if (err) console.warn(`file menu ${name}: ${err}`);
  }
}
</script>

<template>
  <div class="bb-menu fm" role="menu" :aria-label="tx('filemenu.label')" data-bb="file-menu">
    <template v-for="s in sections" :key="s.title">
      <div class="eyebrow">{{ tx(s.title) }}</div>
      <button v-for="e in s.entries" :key="e.key" class="ti" type="button" role="menuitem"
        :disabled="!on(e.action)" :data-missing="e.missing" :data-bb="`file-menu-${e.action ?? e.missing}`"
        @click="run(e.action)">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" v-html="e.icon"></svg>
        <span style="flex:1">{{ tx(e.key) }}</span><span class="mono">{{ e.kbd ?? '' }}</span>
      </button>
    </template>
    <div class="sep"></div>
    <!-- Exit closes the desktop app; the web has no window to close. -->
    <button class="ti" type="button" role="menuitem" style="gap:6px" disabled data-missing="quit-on-web" data-bb="file-menu-quit">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M6 6l12 12M18 6 6 18"/></svg>
      <span>{{ tx('filemenu.quit') }}</span>
    </button>
  </div>
</template>

<style scoped>
/* style="position:absolute;left:62px;top:44px;width:380px;background:#FFFFFF;border:1px solid #5C574D;border-radius:16px;padding:6px 6px 8px;display:flex;flex-direction:column" */
.fm { position: absolute; left: 62px; top: 44px; width: 380px; padding: 6px 6px 8px; display: flex; flex-direction: column; }
/* .eyebrow{font-family:'DM Mono',monospace;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#5C574D} style="padding:8px 12px 2px" */
.eyebrow { font-family: 'DM Mono', monospace; font-size: var(--bb-size-eyebrow); letter-spacing: var(--bb-tracking-eyebrow); text-transform: uppercase; color: var(--bb-stone); padding: 8px 12px 2px; }
/* .ti{display:flex;gap:6px;align-items:center;padding:2px 6px;border-radius:8px;color:#141414;text-decoration:none;font-size:13.5px;line-height:20px;white-space:nowrap} style="padding:5px 12px;gap:10px" (an <a> there) */
.ti { display: flex; gap: 10px; align-items: center; padding: 5px 12px; border-radius: var(--bb-radius-sm); color: var(--bb-ink); font: inherit; font-size: 13.5px; line-height: 20px; white-space: nowrap; border: 0; background: none; text-align: left; cursor: pointer; }
.ti:hover { background: var(--bb-sunk); }
.ti:focus-visible { outline: var(--bb-focus); outline-offset: 2px; }
/* Unavailable: .tool.off{color:#5C574D;opacity:.55} */
.ti:disabled { color: var(--bb-stone); opacity: .55; cursor: default; background: none; }
/* style="font-size:11.5px;color:#5C574D" */
.mono { font-family: 'DM Mono', monospace; font-size: 11.5px; color: var(--bb-stone); }
/* style="border-top:1px solid #EDE7DB;margin:6px 0" */
.sep { border-top: 1px solid var(--bb-sunk); margin: 6px 0; }
</style>
