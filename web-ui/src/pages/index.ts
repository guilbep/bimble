// Page registry: every src/pages/*.vue is a page, id = file name in kebab-case (ProjectInfo.vue → project-info).
// A page replaces the viewport in the main column; #bimble stays in the DOM (hidden).
// Optional exports from the page's plain <script> block: `layout = "full"` (hides the tabs row, ribbon, left panel and
// view tabs), `hint` (an i18n key for the status bar), `side` (a component for the left panel's properties area, in a `*Side.vue` file: not a page).
import type { Component } from "vue";
import { currentScreen } from "../shell/send";

/** What a page or dialog module may export. */
/** A screen's status bar hint: an i18n key, or a function returning `{ key, params }` or a translated string. */
export type Hint = string | (() => string | { key: string; params?: Record<string, string | number> });
export type ScreenModule = { default: Component; layout?: "full"; hint?: Hint; side?: Component };

/** `import.meta.glob` result → { "kebab-id": module }. Shared by pages, dialogs and panels. */
export function byId<M>(mods: Record<string, M>): Record<string, M> {
  return Object.fromEntries(Object.entries(mods).map(([path, m]) => [
    path.replace(/^.*\/|\.vue$/g, "").replace(/([a-z0-9])([A-Z])/g, "$1-$2").toLowerCase(), m,
  ]));
}
/** `import.meta.glob` result → { "kebab-id": component }. */
export function registry(mods: Record<string, { default: Component }>): Record<string, Component> {
  return Object.fromEntries(Object.entries(byId(mods)).map(([id, m]) => [id, m.default]));
}

export const pageModules = byId(import.meta.glob<ScreenModule>(["./*.vue", "!./*Side.vue"], { eager: true }));
export const pages = registry(pageModules);
/** The page shown instead of the viewport, or null: read from the shell (shell/send.ts), never set here. */
export const currentPage = currentScreen;
/** The current page's module, or null. */
export const page = (): ScreenModule | null => (currentPage.value && pageModules[currentPage.value]) || null;
