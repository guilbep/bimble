// The Links panel's calls (ticket LNK1; Rust: links.rs Manage links, lib.rs:4402 the side panel's
// Links group). One place for every link action, so the ribbon (Link IFC…, Link PDF…, Link DXF…,
// Manage links…) and the panel's buttons do the same thing and show their error in the panel.
// - Link…: the file picker (files.ts pickFile: the page's dialog on the native host, the browser's
//   picker in wasm), then `link_model` / `link_pdf` / `link_dxf` with the path (native) or the bytes
//   (wasm, ccore.ts). Enabled per the core (`query actions`): PDF and DXF only in a 2D view.
// - Show, hide, remove: `manage_links {op, name}` for a linked model (session state, no undo step,
//   as Rust), `manage_links {op, id}` for a reference plan (one undo step each, L2).
// - Reload: native, the core reads the file again from where it was read (a model's path, a
//   reference's `source`). Wasm keeps no path: Reload asks for the file again (the picker) and
//   hands its bytes over (a model: linked again under its name, which replaces it in place; a
//   reference: `manage_links {op: "reload", id, path}` with the bytes staged, host_action_file).
// - Reload from… (Rust's): another file in its place. Locate… (DEVIATIONS 29): a linked model the
//   saved .bimble names whose file was not found; picking it links it again.
import { ref, shallowRef } from "vue";
import { actions, actionBody, manageLink, manageReference, query, screenRoutes, state } from "../bimble";
import { browserPickMany, isNative, isWasm, pickFile, type Purpose } from "../files";

export interface PanelModel {
  kind: string; missing: boolean; name: string; path: string; visible: boolean;
  // LINKF (ccore/session/links_panel.h): the conversion's state and the link's numbers
  state?: "ready" | "missing" | "queued" | "converting" | "failed" | "refused" | "unloaded"; progress?: number;
  message?: string | null; bytes?: number; elements?: number; triangles?: number; cache?: string | null;
  cache_mtime_s?: number; log?: string | null; from_cache?: boolean; pinned?: boolean; colour?: number | null;
  /** SIZE3 (E): ready, its proxies being baked after (ccore/session/links.h `indexing`). */
  indexing?: boolean;
  origin?: "file" | "origin" | "base_point" | "survey_point"; move_mm?: [number, number, number]; rotation_deg?: number;
  /** TESS1 (ADR 0085): its own reader detail, named at a Reload; absent: the settings' link_detail. */
  detail?: { name: "exact" | "standard" | "light" | "custom"; chord_mm?: number; max_deg?: number };
}
export interface PanelReference {
  id: string; kind: string; name: string; offset_mm: [number, number]; rotation_deg: number;
  source: string | null; view: string; view_name: string; visible: boolean;
}
export interface LinksPanel { models: PanelModel[]; references: PanelReference[] }

/** The Links group of the Navigator is open (Manage links… opens it). */
export const linksOpen = ref(false);
/** The last link action's error (shown in the panel), null: none. */
export const linksError = shallowRef<string | null>(null);
/** A link action is waiting for the picker or the core. */
export const linksBusy = ref(false);

const msg = (e: unknown): string => (e instanceof Error ? e.message : String(e));

/** The panel's rows (`query links_panel`), empty when the core has none. */
/** LINKF: bumped after each link call, so the rows answer at once (not only at the next change). */
export const linksTick = ref(0);

export function linksPanel(): LinksPanel {
  void state.value;
  void linksTick.value;
  try { return query<LinksPanel>("links_panel"); } catch { return { models: [], references: [] }; }
}

/** The core's enable rule of an action (`query actions`). */
export const enabled = (name: string): boolean => actions.value.find((a) => a.name === name)?.enabled ?? false;

type LinkPurpose = Extract<Purpose, "link_model" | "link_pdf" | "link_dxf">;

/** The bytes of a picked file to the module (wasm). */
async function linkBytes(purpose: LinkPurpose, f: File): Promise<void> {
  const cc = await import("../ccore");
  const b = new Uint8Array(await f.arrayBuffer());
  (purpose === "link_model" ? cc.ccoreLinkFile : purpose === "link_pdf" ? cc.ccoreLinkPdf : cc.ccoreLinkDxf)(f.name, b);
}

async function run(what: () => Promise<string | null>): Promise<string | null> {
  linksBusy.value = true;
  let err: string | null;
  try { err = await what(); } catch (e) { err = msg(e); }
  linksBusy.value = false;
  linksTick.value++;
  linksError.value = err;
  if (err) console.warn(`links: ${err}`);
  return err;
}

/** Link… : pick a file and link it (a model beside the open one; a PDF or DXF into the active 2D view). */
export function linkNew(purpose: LinkPurpose): Promise<string | null> {
  return run(async () => {
    if (!enabled(purpose)) return purpose === "link_model" ? "open a model first" : "open a plan or a section first";
    const r = await pickFile(purpose);
    if (!r) return null; // cancelled
    if (r.paths) { actionBody("link_model", { paths: r.paths }); return null; } // LINKF: one call, one undo step
    if (r.folder) { actionBody("link_folder", { path: r.folder }); return null; }
    if (r.path) { actionBody(purpose, { path: r.path }); return null; }
    if (r.file && isWasm()) { await linkBytes(purpose, r.file); return null; }
    return "no file";
  });
}

/** Show or hide, remove: a linked model by name. */
export const modelOp = (op: "show" | "hide" | "remove", name: string) => run(async () => manageLink(op, name));

/** TESS1 (ADR 0085): Reload a linked IFC at a detail of its own ("" : the settings' again). Custom takes the
 *  settings' chord distance and angle. Natively (a path to read again). */
export function modelReloadDetail(m: PanelModel, detail: "" | "exact" | "standard" | "light" | "custom",
                                  custom?: { chord_mm: number; max_deg: number }): Promise<string | null> {
  return run(async () => {
    const d = detail === "" ? { detail: null } : detail === "custom" ? { detail, ...(custom ?? { chord_mm: 2, max_deg: 18 }) } : { detail };
    actionBody("manage_links", { op: "reload", name: m.name, ...d });
    return null;
  });
}

/** Reload a linked model: natively from its path; in wasm (no path) the picker asks for it again.
 *  `from`: Reload from… / Locate…, another file in its place. */
export function modelReload(m: PanelModel, from = false): Promise<string | null> {
  return run(async () => {
    if (isNative() && !from && !m.missing) return manageLink("reload", m.name);
    const r = await pickFile("link_model");
    if (!r) return null;
    if (m.missing) { // Locate…: the saved entry takes the new path (DEVIATIONS 29)
      if (r.path) { actionBody("manage_links", { op: "locate", name: m.name, path: r.path }); return null; }
      if (r.file) {
        const cc = await import("../ccore");
        cc.ccoreActionFile("manage_links", { op: "locate", name: m.name, path: r.file.name }, new Uint8Array(await r.file.arrayBuffer()));
        return null;
      }
      return "no file";
    }
    let name: string;
    if (r.path) { actionBody("link_model", { path: r.path }); name = r.path.split(/[\\/]/).pop() ?? ""; }
    else if (r.file) { await linkBytes("link_model", r.file); name = r.file.name; }
    else return "no file";
    // Another file name: Rust relinks in the old one's place (lib.rs:1520 relink); the C core links
    // the same name in place, so a new name is added and the old link goes.
    return name !== m.name ? manageLink("remove", m.name) : null;
  });
}

/** Show, hide, remove, move (mm), rotate (degrees): a reference plan, one undo step each. */
export const refOp = (id: string, o: Parameters<typeof manageReference>[1]) => run(async () => manageReference(id, o));

/** Reload a reference plan: natively from its source; in wasm (or `from`: Reload from…) the picker. */
export function refReload(r: PanelReference, from = false): Promise<string | null> {
  return run(async () => {
    if (isNative() && !from) return manageReference(r.id, { op: "reload" });
    const p = await pickFile(r.kind === "pdf" ? "link_pdf" : "link_dxf");
    if (!p) return null;
    if (p.path) return manageReference(r.id, { op: "reload", path: p.path });
    if (p.file) {
      const cc = await import("../ccore");
      cc.ccoreActionFile("manage_links", { op: "reload", id: r.id, path: p.file.name }, new Uint8Array(await p.file.arrayBuffer()));
      return null;
    }
    return "no file";
  });
}

// The ribbon's link tools run the panel's calls (their error shows in the panel); Manage links…
// opens the Links group (Rust opens its Manage links window).
Object.assign(screenRoutes, {
  link_model: () => { linksOpen.value = true; void linkNew("link_model"); },
  link_pdf: () => { linksOpen.value = true; void linkNew("link_pdf"); },
  link_dxf: () => { linksOpen.value = true; void linkNew("link_dxf"); },
  manage_links: () => { linksOpen.value = true; },
  link_folder: () => { linksOpen.value = true; void linkFolder(); }, // LINKF
});

/** LINKF: Link a folder…: natively the page's dialog picks a folder and the host links every IFC of it
 *  (converted in turn, the smallest first, ccore/session/links.h); in the browser (no folders) several
 *  files are picked and linked one at a time. */
export function linkFolder(): Promise<string | null> {
  return run(async () => {
    if (!enabled("link_folder")) return "open a model first";
    if (isNative()) {
      const r = await pickFile("link_folder");
      if (r?.path) actionBody("link_folder", { path: r.path });
      return null;
    }
    if (!isWasm()) return "no folders here";
    const fs = await browserPickMany([".ifc", ".bimbleview"]);
    for (const f of [...fs].sort((a, b) => a.size - b.size)) await linkBytes("link_model", f);
    return null;
  });
}

/** LINKF: a linked model's other operations (manage_links {op, name, ...}). */
export const modelSet = (op: "cancel" | "unload" | "load" | "pin" | "unpin" | "place" | "colour", name: string, extra: Record<string, unknown> = {}) =>
  run(async () => { actionBody("manage_links", { op, name, ...extra }); return null; });

/** LINKF: Reload all (manage_links {op: "reload_all"}): each link from its cache unless its IFC changed. */
export const reloadAll = () => run(async () => { actionBody("manage_links", { op: "reload_all" }); return null; });

/** LINKF: a link's conversion report (`query link_log {name}`: the file the core wrote, by the link's name). */
export function linkLog(name: string): string {
  try { return query<{ text: string }>("link_log", { name }).text; } catch (e) { return msg(e); }
}
