// Ribbon content per tab: data copied from the wireframes v01 (the screen where the tab is active).
// icon: inner SVG of the reference's 22 px icon. action: `query("actions")` name, null = no Rust call yet.
// The pressed / unavailable state comes from `actions` (the reference draws one sample state).
// timber: no reference screen. settingOut "Altitude": the reference shows a readout (.rbox), not tools.
import type { TabId } from "./tabs";
import type { fr } from "../i18n/parts/ribbon";

export type RibbonKey = keyof typeof fr;
// page / panel: the Vue screen the tool opens instead of its egui action (pressed while that screen is open).
export interface RibbonTool { key: RibbonKey; tag: "button" | "a"; icon: string; action: string | null; page?: string; panel?: string }
export interface RibbonGroup { key: RibbonKey; tools: RibbonTool[] }

export const RIBBON: Record<TabId, RibbonGroup[]> = {
  project: [
    { key: "ribbon.project.g1", tools: [
      { key: "ribbon.project.g1.t1", tag: "a", icon: "<circle cx=\"12\" cy=\"12\" r=\"8.5\"/><path d=\"M12 11v5\"/><path d=\"M12 8h.01\"/>", action: "project" },
      { key: "ribbon.project.g1.t2", tag: "a", icon: "<path d=\"M4 18h4v-4h4v-4h4V6h4\"/><path d=\"M4 21h16\"/>", action: null },
    ] },
    { key: "ribbon.project.g2", tools: [
      { key: "ribbon.project.g2.t1", tag: "button", icon: "<path d=\"M4 7h10M18 7h2M4 17h4M12 17h8\"/><circle cx=\"16\" cy=\"7\" r=\"2\"/><circle cx=\"10\" cy=\"17\" r=\"2\"/>", action: null },
      { key: "ribbon.project.g2.t2", tag: "button", icon: "<circle cx=\"6\" cy=\"12\" r=\"2.2\"/><circle cx=\"17\" cy=\"6\" r=\"2.2\"/><circle cx=\"17\" cy=\"18\" r=\"2.2\"/><path d=\"m8 11 7-4M8 13l7 4\"/>", action: null },
    ] },
    { key: "ribbon.project.g3", tools: [
      { key: "ribbon.project.g3.t1", tag: "button", icon: "<path d=\"M10 14a4 4 0 0 0 6 0l3-3a4 4 0 0 0-6-6l-1 1\"/><path d=\"M14 10a4 4 0 0 0-6 0l-3 3a4 4 0 0 0 6 6l1-1\"/>", action: "manage_links" },
      { key: "ribbon.project.g3.t2", tag: "button", icon: "<path d=\"M9 15a4 4 0 0 0 6 0l3-3a4 4 0 0 0-6-6\"/><path d=\"M13 9a4 4 0 0 0-6 0l-3 3a4 4 0 0 0 6 6\"/><path d=\"M19 16v6M16 19h6\"/>", action: "link_model" },
    ] },
  ],
  collaborate: [
    { key: "ribbon.collaborate.g1", tools: [
      { key: "ribbon.collaborate.g1.t1", tag: "a", icon: "<path d=\"M3 6h6l2 2h10v11H3z\"/>", action: "shared_folder" },
      { key: "ribbon.collaborate.g1.t2", tag: "a", icon: "<path d=\"M20 11a8 8 0 0 0-14.5-4.5L4 8\"/><path d=\"M4 4v4h4\"/><path d=\"M4 13a8 8 0 0 0 14.5 4.5L20 16\"/><path d=\"M20 20v-4h-4\"/>", action: "synchronize" },
    ] },
    { key: "ribbon.collaborate.g2", tools: [
      { key: "ribbon.collaborate.g2.t1", tag: "a", icon: "<path d=\"m12 4 9 5-9 5-9-5z\"/><path d=\"m3 13 9 5 9-5\"/>", action: null },
    ] },
    { key: "ribbon.collaborate.g3", tools: [
      { key: "ribbon.collaborate.g3.t1", tag: "a", icon: "<circle cx=\"9\" cy=\"9\" r=\"3\"/><path d=\"M3 20a6 6 0 0 1 12 0\"/><circle cx=\"17\" cy=\"8\" r=\"2.5\"/><path d=\"M16 14a5 5 0 0 1 5 5\"/>", action: null },
    ] },
    { key: "ribbon.collaborate.g4", tools: [
      { key: "ribbon.collaborate.g4.t1", tag: "a", icon: "<path d=\"M4 12a8 8 0 1 0 2.4-5.7L4 8\"/><path d=\"M4 4v4h4\"/><path d=\"M12 8v4l3 2\"/>", action: "history", page: "history" },
    ] },
  ],
  settingOut: [
    { key: "ribbon.settingOut.g1", tools: [
      { key: "ribbon.settingOut.g1.t1", tag: "button", icon: "<path d=\"M12 21s-6-6-6-11a6 6 0 0 1 12 0c0 5-6 11-6 11z\"/><circle cx=\"12\" cy=\"10\" r=\"2\"/>", action: null },
      { key: "ribbon.settingOut.g1.t2", tag: "button", icon: "<circle cx=\"12\" cy=\"12\" r=\"7\"/><path d=\"M12 3v18M3 12h18\"/>", action: null },
      { key: "ribbon.settingOut.g1.t3", tag: "button", icon: "<path d=\"M12 3 7 20l5-4 5 4z\"/>", action: null },
    ] },
    { key: "ribbon.settingOut.g2", tools: [
    ] },
    { key: "ribbon.settingOut.g3", tools: [
      { key: "ribbon.settingOut.g3.t1", tag: "a", icon: "<path d=\"M3 7h3M9 7h3M15 7h6\"/><path d=\"M3 17h3M9 17h3M15 17h6\"/><path d=\"m18 4 2 3-2 0\"/>", action: null },
      { key: "ribbon.settingOut.g3.t2", tag: "a", icon: "<path d=\"M8 3v18M16 3v18M3 8h18M3 16h18\"/>", action: null },
      { key: "ribbon.settingOut.g3.t3", tag: "button", icon: "<path d=\"M4 8 12 4l8 4-8 4z\"/><path d=\"M4 8v8l8 4 8-4V8\"/>", action: null },
    ] },
    { key: "ribbon.settingOut.g4", tools: [
      { key: "ribbon.settingOut.g4.t1", tag: "button", icon: "<path d=\"m4 16 12-12 4 4L8 20z\"/><path d=\"m8 12 2 2M11 9l2 2M14 6l2 2\"/>", action: "measure" },
    ] },
  ],
  site: [
    { key: "ribbon.site.g1", tools: [
      { key: "ribbon.site.g1.t1", tag: "button", icon: "<circle cx=\"6\" cy=\"14\" r=\"1.2\"/><circle cx=\"11\" cy=\"9\" r=\"1.2\"/><circle cx=\"17\" cy=\"12\" r=\"1.2\"/><circle cx=\"14\" cy=\"18\" r=\"1.2\"/><circle cx=\"19\" cy=\"6\" r=\"1.2\"/>", action: null },
      { key: "ribbon.site.g1.t2", tag: "button", icon: "<path d=\"M3 17c4-3 7-3 9-1s6 1 9-2\"/><path d=\"M5 12c3-2 6-2 8 0s5 0 7-2\"/><path d=\"M8 7c2-1 4-1 6 0\"/>", action: null },
    ] },
    { key: "ribbon.site.g2", tools: [
      { key: "ribbon.site.g2.t1", tag: "button", icon: "<path d=\"M2 18c4-6 6-8 9-5s5 1 11-4\"/><path d=\"M2 21h20\"/>", action: null },
      { key: "ribbon.site.g2.t2", tag: "button", icon: "<path d=\"M3 12 12 7l9 5-9 5z\"/><path d=\"M3 12v2l9 5 9-5v-2\"/>", action: null },
    ] },
    { key: "ribbon.site.g3", tools: [
      { key: "ribbon.site.g3.t1", tag: "button", icon: "<path d=\"M2 8h5l3 8h4l3-8h5\"/>", action: null },
      { key: "ribbon.site.g3.t2", tag: "button", icon: "<path d=\"M3 18h18\"/><path d=\"M6 18 14 8h7\"/>", action: null },
      { key: "ribbon.site.g3.t3", tag: "button", icon: "<path d=\"M4 8l8-4 8 4-8 4z\"/><path d=\"M4 8v8l8 4 8-4V8\"/><path d=\"M12 12v8\"/>", action: null },
    ] },
    { key: "ribbon.site.g4", tools: [
      { key: "ribbon.site.g4.t1", tag: "button", icon: "<path d=\"m4 16 12-12 4 4L8 20z\"/><path d=\"m8 12 2 2M11 9l2 2M14 6l2 2\"/>", action: "measure" },
    ] },
  ],
  concrete: [
    { key: "ribbon.concrete.g1", tools: [
      { key: "ribbon.concrete.g1.t1", tag: "button", icon: "<rect x=\"9\" y=\"3\" width=\"6\" height=\"18\"/><path d=\"M6 21h12\"/>", action: "place_column" },
      { key: "ribbon.concrete.g1.t2", tag: "button", icon: "<path d=\"M3 9h18v6H3z\"/><path d=\"M3 9l3-3h18l-3 3M21 15l3-3V6\"/>", action: "place_beam" },
      { key: "ribbon.concrete.g1.t3", tag: "button", icon: "<path d=\"M4 20V6l6-2v14z\"/><path d=\"M10 18l10 2V8L10 4\"/>", action: "place_wall" },
      { key: "ribbon.concrete.g1.t4", tag: "button", icon: "<path d=\"M3 12 12 7l9 5-9 5z\"/><path d=\"M3 12v2l9 5 9-5v-2\"/>", action: "place_slab" },
    ] },
    { key: "ribbon.concrete.g2", tools: [
      { key: "ribbon.concrete.g2.t1", tag: "button", icon: "<rect x=\"9\" y=\"4\" width=\"6\" height=\"9\"/><path d=\"M4 13h16v6H4z\"/>", action: null },
      { key: "ribbon.concrete.g2.t2", tag: "button", icon: "<path d=\"M2 15h20v4H2z\"/><path d=\"M6 15V9M12 15V7M18 15V9\"/>", action: null },
    ] },
    { key: "ribbon.concrete.g3", tools: [
      { key: "ribbon.concrete.g3.t1", tag: "button", icon: "<path d=\"M4 20h4v-4h4v-4h4V8h4V4\"/>", action: null },
      { key: "ribbon.concrete.g3.t2", tag: "button", icon: "<path d=\"M3 19h18L3 9z\"/>", action: null },
    ] },
    { key: "ribbon.concrete.g4", tools: [
      { key: "ribbon.concrete.g4.t1", tag: "button", icon: "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\"/><rect x=\"9\" y=\"8\" width=\"6\" height=\"8\"/>", action: null },
      { key: "ribbon.concrete.g4.t2", tag: "button", icon: "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\"/><rect x=\"9\" y=\"8\" width=\"6\" height=\"8\"/>", action: null },
    ] },
    { key: "ribbon.concrete.g5", tools: [
      { key: "ribbon.concrete.g5.t1", tag: "button", icon: "<path d=\"m4 16 12-12 4 4L8 20z\"/><path d=\"m8 12 2 2M11 9l2 2M14 6l2 2\"/>", action: "measure" },
    ] },
  ],
  rebar: [
    { key: "ribbon.rebar.g1", tools: [
      { key: "ribbon.rebar.g1.t1", tag: "button", icon: "<path d=\"M4 18 18 4\"/><path d=\"M7 21 21 7\"/><path d=\"M5 13l2 2M9 9l2 2M13 5l2 2\"/>", action: null },
      { key: "ribbon.rebar.g1.t2", tag: "button", icon: "<rect x=\"6\" y=\"4\" width=\"12\" height=\"16\" rx=\"2\"/><path d=\"M6 8l4-4\"/>", action: null },
      { key: "ribbon.rebar.g1.t3", tag: "button", icon: "<path d=\"M4 4h16v16H4z\"/><path d=\"M4 9h16M4 14h16M9 4v16M14 4v16\"/>", action: null },
      { key: "ribbon.rebar.g1.t4", tag: "button", icon: "<path d=\"m12 4 9 5-9 5-9-5z\"/><path d=\"m3 13 9 5 9-5\"/>", action: null },
    ] },
    { key: "ribbon.rebar.g2", tools: [
      { key: "ribbon.rebar.g2.t1", tag: "button", icon: "<path d=\"M5 20V8a4 4 0 0 1 4-4h10\"/>", action: null },
      { key: "ribbon.rebar.g2.t2", tag: "button", icon: "<path d=\"m4 20 1-5L16 4l4 4L9 19z\"/>", action: null },
    ] },
    { key: "ribbon.rebar.g3", tools: [
      { key: "ribbon.rebar.g3.t1", tag: "a", icon: "<path d=\"M9 6h11M9 12h11M9 18h11\"/><circle cx=\"5\" cy=\"6\" r=\"1\"/><circle cx=\"5\" cy=\"12\" r=\"1\"/><circle cx=\"5\" cy=\"18\" r=\"1\"/>", action: null },
      { key: "ribbon.rebar.g3.t2", tag: "button", icon: "<path d=\"M5 3h10l4 4v14H5z\"/><path d=\"M14 3v5h5\"/><rect x=\"8\" y=\"11\" width=\"8\" height=\"6\"/>", action: null },
    ] },
    { key: "ribbon.rebar.g4", tools: [
      { key: "ribbon.rebar.g4.t1", tag: "button", icon: "<path d=\"m4 16 12-12 4 4L8 20z\"/><path d=\"m8 12 2 2M11 9l2 2M14 6l2 2\"/>", action: "measure" },
    ] },
  ],
  steel: [
    { key: "ribbon.steel.g1", tools: [
      { key: "ribbon.steel.g1.t1", tag: "button", icon: "<rect x=\"9\" y=\"3\" width=\"6\" height=\"18\"/><path d=\"M6 21h12\"/>", action: null },
      { key: "ribbon.steel.g1.t2", tag: "button", icon: "<path d=\"M6 4h12M6 20h12M12 4v16\"/>", action: null },
      { key: "ribbon.steel.g1.t3", tag: "button", icon: "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\"/><path d=\"M4 20 20 4\"/>", action: null },
    ] },
    { key: "ribbon.steel.g2", tools: [
      { key: "ribbon.steel.g2.t1", tag: "button", icon: "<rect x=\"4\" y=\"9\" width=\"16\" height=\"6\"/><circle cx=\"8\" cy=\"12\" r=\"1\"/><circle cx=\"16\" cy=\"12\" r=\"1\"/>", action: null },
      { key: "ribbon.steel.g2.t2", tag: "button", icon: "<rect x=\"4\" y=\"9\" width=\"16\" height=\"6\"/><circle cx=\"8\" cy=\"12\" r=\"1\"/><circle cx=\"16\" cy=\"12\" r=\"1\"/>", action: null },
    ] },
    { key: "ribbon.steel.g3", tools: [
      { key: "ribbon.steel.g3.t1", tag: "button", icon: "<path d=\"M5 4h14v3h-5v10h5v3H5v-3h5V7H5z\"/>", action: null },
    ] },
    { key: "ribbon.steel.g4", tools: [
      { key: "ribbon.steel.g4.t1", tag: "button", icon: "<path d=\"m4 16 12-12 4 4L8 20z\"/><path d=\"m8 12 2 2M11 9l2 2M14 6l2 2\"/>", action: "measure" },
    ] },
  ],
  timber: [
  ],
  annotate: [
    { key: "ribbon.annotate.g1", tools: [
      { key: "ribbon.annotate.g1.t1", tag: "button", icon: "<path d=\"M5 3v15l4-4 3 7 2-1-3-7h6z\"/>", action: "sheet_tool_select" },
      { key: "ribbon.annotate.g1.t2", tag: "button", icon: "<path d=\"M5 6V4h14v2\"/><path d=\"M12 4v16M9 20h6\"/>", action: "sheet_tool_text" },
      { key: "ribbon.annotate.g1.t3", tag: "button", icon: "<path d=\"M4 6v12M20 6v12M4 12h16\"/><path d=\"m7 9-3 3 3 3M17 9l3 3-3 3\"/>", action: "sheet_tool_dimension" },
    ] },
    { key: "ribbon.annotate.g2", tools: [
      { key: "ribbon.annotate.g2.t1", tag: "button", icon: "<path d=\"M5 6V4h14v2\"/><path d=\"M12 4v16M9 20h6\"/>", action: null },
      { key: "ribbon.annotate.g2.t2", tag: "button", icon: "<path d=\"M4 6v12M20 6v12M4 12h16\"/><path d=\"m7 9-3 3 3 3M17 9l3 3-3 3\"/>", action: null },
    ] },
    { key: "ribbon.annotate.g3", tools: [
      { key: "ribbon.annotate.g3.t1", tag: "button", icon: "<circle cx=\"7\" cy=\"12\" r=\"3\"/><path d=\"M10 12h11\"/>", action: "section_labels" },
      { key: "ribbon.annotate.g3.t2", tag: "button", icon: "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\"/><rect x=\"9\" y=\"8\" width=\"6\" height=\"8\"/>", action: "openings" },
      { key: "ribbon.annotate.g3.t3", tag: "button", icon: "<path d=\"M3 12h18\"/><path d=\"m7 8-4 4 4 4\"/><rect x=\"9\" y=\"5\" width=\"6\" height=\"14\" stroke-dasharray=\"2 2\"/>", action: "section_marks" },
      { key: "ribbon.annotate.g3.t4", tag: "button", icon: "<path d=\"M3 7h3M9 7h3M15 7h6\"/><path d=\"M3 17h3M9 17h3M15 17h6\"/><path d=\"m18 4 2 3-2 0\"/>", action: "slab_levels" },
    ] },
  ],
  view: [
    { key: "ribbon.view.g1", tools: [
      { key: "ribbon.view.g1.t1", tag: "button", icon: "<path d=\"M4 7l8-4 8 4v10l-8 4-8-4z\"/>", action: "edges" },
      { key: "ribbon.view.g1.t2", tag: "button", icon: "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\"/><path d=\"M4 12 12 4M4 20 20 4M12 20l8-8\"/>", action: "hatching" },
      { key: "ribbon.view.g1.t3", tag: "button", icon: "<rect x=\"5\" y=\"5\" width=\"14\" height=\"14\"/><path d=\"M5 5l5-2h12l-3 2M19 19l3-2V3\"/>", action: "orthographic" },
    ] },
    { key: "ribbon.view.g2", tools: [
      { key: "ribbon.view.g2.t1", tag: "button", icon: "<path d=\"M12 3 20 7v10l-8 4-8-4V7z\"/><path d=\"M4 7l8 4 8-4M12 11v10\"/>", action: "section_box" },
      { key: "ribbon.view.g2.t2", tag: "button", icon: "<path d=\"M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5\"/>", action: "box_to_selection" },
    ] },
    { key: "ribbon.view.g3", tools: [
      { key: "ribbon.view.g3.t1", tag: "button", icon: "<path d=\"M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5\"/>", action: "fit" },
      { key: "ribbon.view.g3.t2", tag: "button", icon: "<rect x=\"5\" y=\"5\" width=\"14\" height=\"14\"/><path d=\"M12 9v6M9 12h6\"/>", action: "top" },
    ] },
    { key: "ribbon.view.g4", tools: [
      { key: "ribbon.view.g4.t1", tag: "button", icon: "<path d=\"M3 3l18 18\"/><path d=\"M10 6a10 10 0 0 1 2 0c6 0 10 6 10 6a17 17 0 0 1-3 4M6 7a17 17 0 0 0-4 5s4 7 10 7a9 9 0 0 0 4-1\"/>", action: "hide_now" },
      { key: "ribbon.view.g4.t2", tag: "button", icon: "<rect x=\"8\" y=\"8\" width=\"8\" height=\"8\"/><path d=\"M3 3h4M17 3h4M3 21h4M17 21h4M3 3v4M21 3v4M3 17v4M21 17v4\"/>", action: "isolate_now" },
      { key: "ribbon.view.g4.t3", tag: "button", icon: "<path d=\"M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/>", action: "reveal_hidden" },
      { key: "ribbon.view.g4.t4", tag: "button", icon: "<path d=\"M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/>", action: "unhide" },
    ] },
    { key: "ribbon.view.g5", tools: [
      { key: "ribbon.view.g5.t1", tag: "button", icon: "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\"/><path d=\"M4 12h9v8M13 4v4\"/>", action: null },
      { key: "ribbon.view.g5.t2", tag: "button", icon: "<path d=\"M3 12h18\"/><path d=\"m7 8-4 4 4 4\"/><rect x=\"9\" y=\"5\" width=\"6\" height=\"14\" stroke-dasharray=\"2 2\"/>", action: null },
    ] },
  ],
  sheets: [
    { key: "ribbon.sheets.g1", tools: [
      { key: "ribbon.sheets.g1.t1", tag: "a", icon: "<path d=\"M5 3h10l4 4v6\"/><path d=\"M5 3v18h8\"/><path d=\"M18 16v6M15 19h6\"/>", action: "new_sheet" },
      { key: "ribbon.sheets.g1.t2", tag: "button", icon: "<rect x=\"3\" y=\"4\" width=\"18\" height=\"16\"/><rect x=\"6\" y=\"7\" width=\"7\" height=\"6\"/>", action: null },
      { key: "ribbon.sheets.g1.t3", tag: "button", icon: "<path d=\"M6 6l12 12M18 6 6 18\"/>", action: "close_sheet" },
    ] },
    { key: "ribbon.sheets.g2", tools: [
      { key: "ribbon.sheets.g2.t1", tag: "button", icon: "<path d=\"M5 3v15l4-4 3 7 2-1-3-7h6z\"/>", action: "sheet_tool_select" },
      { key: "ribbon.sheets.g2.t2", tag: "button", icon: "<path d=\"M5 6V4h14v2\"/><path d=\"M12 4v16M9 20h6\"/>", action: "sheet_tool_text" },
      { key: "ribbon.sheets.g2.t3", tag: "button", icon: "<path d=\"M4 6v12M20 6v12M4 12h16\"/><path d=\"m7 9-3 3 3 3M17 9l3 3-3 3\"/>", action: "sheet_tool_dimension" },
    ] },
    { key: "ribbon.sheets.g3", tools: [
      { key: "ribbon.sheets.g3.t1", tag: "button", icon: "<path d=\"M5 21V4\"/><path d=\"M5 4h12l-2 4 2 4H5\"/>", action: null },
    ] },
    { key: "ribbon.sheets.g4", tools: [
      { key: "ribbon.sheets.g4.t1", tag: "a", icon: "<path d=\"M7 9V4h10v5\"/><rect x=\"4\" y=\"9\" width=\"16\" height=\"7\" rx=\"1\"/><path d=\"M7 14h10v6H7z\"/>", action: null },
    ] },
  ],
  graphics: [
    { key: "ribbon.graphics.g1", tools: [
      { key: "ribbon.graphics.g1.t1", tag: "button", icon: "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"3\"/><path d=\"M8 10h8M8 14h5\"/>", action: "standards", panel: "standards" },
      { key: "ribbon.graphics.g1.t2", tag: "button", icon: "<circle cx=\"12\" cy=\"12\" r=\"8\"/><path d=\"M6 8c4 3 8 3 12 0M5 13c4 3 10 3 14 0\"/>", action: "materials" },
    ] },
    { key: "ribbon.graphics.g2", tools: [
      { key: "ribbon.graphics.g2.t1", tag: "button", icon: "<path d=\"M3 7h18\"/><path d=\"M3 12h4M10 12h4M17 12h4\"/><path d=\"M3 17h2M8 17h1M12 17h2M17 17h1\"/>", action: null },
      { key: "ribbon.graphics.g2.t2", tag: "button", icon: "<path d=\"M5 6V4h14v2\"/><path d=\"M12 4v16M9 20h6\"/>", action: null },
      { key: "ribbon.graphics.g2.t3", tag: "button", icon: "<path d=\"M4 6v12M20 6v12M4 12h16\"/><path d=\"m7 9-3 3 3 3M17 9l3 3-3 3\"/>", action: null },
      { key: "ribbon.graphics.g2.t4", tag: "button", icon: "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\"/><path d=\"M4 12 12 4M4 20 20 4M12 20l8-8\"/>", action: null },
    ] },
    { key: "ribbon.graphics.g3", tools: [
      { key: "ribbon.graphics.g3.t1", tag: "button", icon: "<path d=\"m4 20 1-5L16 4l4 4L9 19z\"/>", action: null },
    ] },
  ],
  lca: [
    { key: "ribbon.lca.g1", tools: [
      { key: "ribbon.lca.g1.t1", tag: "button", icon: "<ellipse cx=\"12\" cy=\"6\" rx=\"7\" ry=\"3\"/><path d=\"M5 6v12c0 1.7 3 3 7 3s7-1.3 7-3V6\"/><path d=\"M5 12c0 1.7 3 3 7 3s7-1.3 7-3\"/>", action: null },
      { key: "ribbon.lca.g1.t2", tag: "button", icon: "<path d=\"M4 12h11\"/><path d=\"m11 8 4 4-4 4\"/><rect x=\"16\" y=\"6\" width=\"5\" height=\"12\"/>", action: null },
    ] },
    { key: "ribbon.lca.g2", tools: [
      { key: "ribbon.lca.g2.t1", tag: "a", icon: "<path d=\"M4 20V4\"/><path d=\"M4 20h16\"/><rect x=\"7\" y=\"11\" width=\"3\" height=\"6\"/><rect x=\"12\" y=\"7\" width=\"3\" height=\"10\"/><rect x=\"17\" y=\"13\" width=\"3\" height=\"4\"/>", action: null },
      { key: "ribbon.lca.g2.t2", tag: "button", icon: "<rect x=\"3\" y=\"5\" width=\"7\" height=\"14\"/><rect x=\"14\" y=\"5\" width=\"7\" height=\"14\"/><path d=\"M10 12h4\"/>", action: null },
    ] },
    { key: "ribbon.lca.g3", tools: [
      { key: "ribbon.lca.g3.t1", tag: "button", icon: "<path d=\"M12 15V3\"/><path d=\"m7 8 5-5 5 5\"/><path d=\"M5 13v8h14v-8\"/>", action: null },
    ] },
  ],
  quantities: [
    { key: "ribbon.quantities.g1", tools: [
      { key: "ribbon.quantities.g1.t1", tag: "button", icon: "<rect x=\"4\" y=\"5\" width=\"16\" height=\"14\"/><path d=\"M4 10h16M10 10v9\"/>", action: null },
      { key: "ribbon.quantities.g1.t2", tag: "button", icon: "<path d=\"M9 6h11M9 12h11M9 18h11\"/><circle cx=\"5\" cy=\"6\" r=\"1\"/><circle cx=\"5\" cy=\"12\" r=\"1\"/><circle cx=\"5\" cy=\"18\" r=\"1\"/>", action: null },
      { key: "ribbon.quantities.g1.t3", tag: "button", icon: "<path d=\"M4 7h10M18 7h2M4 17h4M12 17h8\"/><circle cx=\"16\" cy=\"7\" r=\"2\"/><circle cx=\"10\" cy=\"17\" r=\"2\"/>", action: null },
    ] },
    { key: "ribbon.quantities.g2", tools: [
      { key: "ribbon.quantities.g2.t1", tag: "button", icon: "<path d=\"M4 12h11\"/><path d=\"m11 8 4 4-4 4\"/><rect x=\"16\" y=\"6\" width=\"5\" height=\"12\"/>", action: null },
      { key: "ribbon.quantities.g2.t2", tag: "button", icon: "<ellipse cx=\"12\" cy=\"6\" rx=\"7\" ry=\"3\"/><path d=\"M5 6v12c0 1.7 3 3 7 3s7-1.3 7-3V6\"/><path d=\"M5 12c0 1.7 3 3 7 3s7-1.3 7-3\"/>", action: null },
    ] },
    { key: "ribbon.quantities.g3", tools: [
      { key: "ribbon.quantities.g3.t1", tag: "button", icon: "<path d=\"M4 18 18 4\"/><path d=\"M7 21 21 7\"/><path d=\"M5 13l2 2M9 9l2 2M13 5l2 2\"/>", action: null },
      { key: "ribbon.quantities.g3.t2", tag: "button", icon: "<circle cx=\"12\" cy=\"12\" r=\"8\"/><path d=\"M6 8c4 3 8 3 12 0M5 13c4 3 10 3 14 0\"/>", action: null },
    ] },
    { key: "ribbon.quantities.g4", tools: [
      { key: "ribbon.quantities.g4.t1", tag: "button", icon: "<path d=\"M12 15V3\"/><path d=\"m7 8 5-5 5 5\"/><path d=\"M5 13v8h14v-8\"/>", action: null },
    ] },
  ],
};
