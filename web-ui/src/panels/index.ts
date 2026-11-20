// Right panel registry: every src/panels/*.vue, id = file name in kebab-case. 400 wide, under the view tabs, right of the viewport.
// A panel module may export `hint` (status bar) and `label` (the aside's aria-label), both i18n keys.
import { ref, type Component } from "vue";
import { byId, registry, type ScreenModule } from "../pages";

const panelModules = byId(import.meta.glob<ScreenModule & { label?: string }>("./*.vue", { eager: true }));
export const panels = registry(panelModules);
/** The right panel shown, or null. */
export const currentPanel = ref<string | null>(null);
/** The current panel's module, or null. */
export const panel = () => (currentPanel.value && panelModules[currentPanel.value]) || null;
