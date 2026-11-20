// Page registry: every src/pages/*.vue is a page, id = file name in kebab-case (ProjectInfo.vue → project-info).
// A page replaces the viewport in the main column; #bimble stays in the DOM (hidden).
import { ref, type Component } from "vue";

/** `import.meta.glob` result → { "kebab-id": component }. Shared by pages, dialogs and panels. */
export function registry(mods: Record<string, { default: Component }>): Record<string, Component> {
  return Object.fromEntries(Object.entries(mods).map(([path, m]) => [
    path.replace(/^.*\/|\.vue$/g, "").replace(/([a-z0-9])([A-Z])/g, "$1-$2").toLowerCase(), m.default,
  ]));
}

export const pages = registry(import.meta.glob<{ default: Component }>("./*.vue", { eager: true }));
/** The page shown instead of the viewport, or null. */
export const currentPage = ref<string | null>(null);
