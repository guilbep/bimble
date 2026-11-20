// Ribbon tabs (wireframes v01, nav.tabs): ids in order, their group, and the active tab. Vue panel state, no Rust.
import { ref } from "vue";

export const TABS = [
  { id: "project", group: "project" }, { id: "collaborate", group: "project" },
  { id: "settingOut", group: "project" }, { id: "site", group: "project" },
  { id: "concrete", group: "model" }, { id: "rebar", group: "model" },
  { id: "steel", group: "model" }, { id: "timber", group: "model" },
  { id: "annotate", group: "output" }, { id: "view", group: "output" },
  { id: "sheets", group: "output" }, { id: "graphics", group: "output" },
  { id: "lca", group: "output" }, { id: "quantities", group: "output" },
] as const;

export type TabId = (typeof TABS)[number]["id"];

/** The active tab. D1 (View3D) shows "view" active. */
export const activeTab = ref<TabId>("view");
