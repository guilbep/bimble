// What the core serves, for the page (ticket FAKE1). One source of truth: the core's own answers,
// never a guess from the build the page runs on:
//   - `query actions`: each action's `enabled` (bimble.ts `actions`);
//   - `query capabilities` (ccore/session/capabilities.h): pages, panels, data sets and exports by
//     name, true or false (bimble.ts `capabilities`; null = the Rust viewer: everything).
// A screen (page, dialog, panel) declares the capability it needs in its plain <script> block:
//   export const capability = "lca";        // shown only when the core serves it
//   export const editable = true;           // and only for a model open for editing
// App.vue and LeftPanel.vue then show NotAvailable.vue instead of the screen. A ribbon tool or a
// menu entry whose action belongs to a capability the core does not serve is drawn "not yet"
// (ACTION_CAPABILITY below), never as a normal button that does nothing.
import { computed, type ComputedRef } from "vue";
import { capabilities, state } from "../bimble";
import { pageModules } from "../pages";
import { dialogModules } from "../dialogs";

/** The owner's open question (round-4.md, "label, do not hide"): false labels what is not
 *  available yet; true hides it instead (ribbon tools, menu entries, pages' tabs). */
export const HIDE_NOT_AVAILABLE = false;

/** Whether the core serves `name` now (an unknown name: no). */
export function has(name: string): boolean {
  const c = capabilities.value;
  return c === null || c[name] === true;
}

/** `has(name)` as a computed ref, for a component. */
export const useCapability = (name: string): ComputedRef<boolean> => computed(() => has(name));

/** A model open for editing (not an IFC read as it is, not a .bimbleview). */
export const editableModel = (): boolean => state.value.file !== null && state.value.editable !== false;

/** What a screen module may declare (pages/index.ts ScreenModule). */
export interface Needs { capability?: string; editable?: boolean }

/** Why a screen cannot show now: "preview" (not in this core), "readOnly" (needs an editable
 *  model), or null when it can. */
export function blocked(m: Needs | null | undefined): "preview" | "readOnly" | null {
  if (!m) return null;
  if (m.capability && !has(m.capability)) return "preview";
  if (m.editable && state.value.file !== null && state.value.editable === false) return "readOnly";
  return null;
}

/** Actions that belong to a capability: off for good in a core that does not serve it (the
 *  ribbon draws them "not yet"), off for now otherwise (drawn greyed, the usual way). */
export const ACTION_CAPABILITY: Readonly<Record<string, string>> = {
  sheet_tool_select: "sheet_tools", sheet_tool_text: "sheet_tools", sheet_tool_dimension: "sheet_tools",
  section_labels: "plan_annotations", openings: "plan_annotations", section_marks: "plan_annotations", slab_levels: "plan_annotations",
  shared_folder: "collaboration", synchronize: "collaboration",
  standards: "standards", materials: "materials", settings: "settings", about: "about",
  on_paper: "export_view", paper_view: "paper_view",
  link_pdf: "references", link_dxf: "references", link_model: "references", manage_links: "references",
  project: "project_info", history: "history", new_sheet: "sheets", close_sheet: "sheets",
  measure: "measure",
  show_base_point: "setting_out", show_survey_point: "setting_out", view_true_north: "setting_out", // SETOUT
  reference_planes: "reference_planes", // SETOUT: no such action yet: "not yet" while the core does not serve it
};

/** Whether the action `name` is not available in this core at all (no action: not yet either). */
export const notYetAction = (name: string | null | undefined): boolean => {
  if (!name) return true;
  const c = ACTION_CAPABILITY[name];
  return c !== undefined && !has(c);
};

/** What exists instead of a screen not served (an i18n key), by screen id. */
export const INSTEAD: Readonly<Record<string, string>> = {
  lca: "na.instead.lca", synchronize: "na.instead.collab", users: "na.instead.collab", subprojects: "na.instead.collab",
  "shared-folder": "na.instead.collab", borrowed: "na.instead.collab", "team-member": "na.instead.collab", collab: "na.instead.collab",
  you: "na.instead.you", recent: "na.instead.recent",
};

/** A ribbon tool (shell/ribbon-tabs.ts) that cannot work in this core: no action and no screen,
 *  an action of a capability not served, or a screen whose capability is not served. */
export function toolNotYet(tool: { action: string | null; page?: string; panel?: string; dialog?: string }): boolean {
  if (tool.page) return blocked(pageModules[tool.page]) === "preview";
  if (tool.dialog) return blocked(dialogModules[tool.dialog]) === "preview";
  return notYetAction(tool.action);
}
