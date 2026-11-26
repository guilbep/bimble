// The viewport's right-click menu (ticket ISS71, issue #71): its items, one row each, in order. A row
// names a C core action (`query actions`: its enabled state greys it out, a click runs it) and its copy
// (i18n `ctx.*`, parts/contextmenu.ts); `tip`: the tooltip's key. Another ticket adds its items here
// (ISS72's Show all and Show previous are rows below), the component (ContextMenu.vue) stays as it is.
import type { PartKey } from "../i18n/keys.gen";

/** Box to selection's margin on every side (mm): ccore/session/visibility.h CC_BOX_SELECTION_MARGIN_MM. */
export const BOX_SELECTION_MARGIN_MM = 1000;

export interface ContextItem {
  /** The C core action the item runs. */
  action: string;
  key: PartKey;
  tip?: PartKey;
  /** A separator above the item. */
  sep?: boolean;
}

export const CONTEXT_ITEMS: ContextItem[] = [
  // ---- ISS71 ----
  { action: "box_to_selection", key: "ctx.boxToSelection", tip: "ctx.boxToSelectionTip" },
  { action: "hide_now", key: "ctx.hide", sep: true },
  { action: "isolate_now", key: "ctx.isolate" },
  // ---- end ISS71 ----
  // ---- ISS72's actions (GitHub issue #72), greyed out as their enable state says ----
  { action: "show_all", key: "ctx.showAll" },
  { action: "show_previous", key: "ctx.showPrevious" },
];
