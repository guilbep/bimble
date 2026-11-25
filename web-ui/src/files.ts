// Files in the page (ticket FS1): one code path for every action that reads or writes a file.
// - `pickFile(purpose)`: the native host (`?native=1`, the desktop build) has no system dialog the
//   page can show, so the page draws its own (dialogs/FileBrowser.vue) over the host's queries
//   `fs_roots`, `fs_home`, `fs_list` (ccore/host/fs.h: the path rule) and answers an absolute path;
//   the C core in wasm (`?ccore=1`) answers the bytes of a browser picker (`<input type=file>`).
// - `confirmDiscard()`: the one question before a route drops unsaved changes (new, open, import,
//   close, a recent file, a drop): an in-page dialog (dialogs/ConfirmDiscard.vue), true to go on.
//   The page's `beforeunload` asks the browser's own question (no page can draw one then).
// - `openPath(path)`: open a file the host can read (a recent file, the dialog's pick).
// - `runFileAction(name)`: the file actions of the native host (bimble.ts runAction calls it):
//   open, import_ifc, link_model, link_pdf, link_dxf, save_as, a Save of a new model (Save as),
//   export_ifc4, export_ifc2x3, share_for_viewing, new; each sends the picked path in its body.
//   The wasm build keeps its own (ccore.ts fileAction), on these same functions.
// - Drag and drop (`installFiles`): a .bimble, .json, .ifc, .bimbleview opens, a .pdf or .dxf links
//   to the active view. Wasm: the bytes (ccore.ts). Native: the browser gives no path, so the bytes
//   go to the host (`POST /stage`, host/fs.h: its own folder, 256 MB at most) and open from there as a
//   dropped document (Save asks where; no recent file). A dropped PDF or DXF is refused there: a
//   reference plan keeps its file's path, and a staged copy is no place to keep.
// UX1 (the shell's state machine) drives these: plain async functions, no navigation state here.
import { nextTick, shallowRef, watch } from "vue";
import { currentDialog } from "./dialogs";
import { actionBody, fileRoutes, query, state } from "./bimble";

export type Purpose = "open" | "import_ifc" | "link_model" | "link_pdf" | "link_dxf" | "save_as" | "export_ifc4" | "export_ifc2x3" | "share_for_viewing";

/** Per purpose: the extensions listed, and for a write the name proposed. */
export const PURPOSES: Record<Purpose, { ext: string[]; save?: (stem: string) => string }> = {
  open: { ext: [".bimble", ".json", ".ifc", ".bimbleview"] },
  import_ifc: { ext: [".ifc"] },
  link_model: { ext: [".ifc", ".bimbleview", ".bimble", ".json"] },
  link_pdf: { ext: [".pdf"] },
  link_dxf: { ext: [".dxf"] },
  save_as: { ext: [".bimble", ".json"], save: (s) => `${s}.bimble` },
  export_ifc4: { ext: [".ifc"], save: (s) => `${s}.ifc` },
  export_ifc2x3: { ext: [".ifc"], save: (s) => `${s}.ifc` },
  share_for_viewing: { ext: [".bimbleview"], save: (s) => `${s}.bimbleview` },
};

const params = new URLSearchParams(location.search);
/** The page talks to the native host (the desktop build): paths, not bytes. */
export const isNative = (): boolean => params.get("native") === "1";
/** The page runs the C core in wasm (the picker gives bytes). */
export const isWasm = (): boolean => params.get("ccore") === "1" || import.meta.env.VITE_CCORE === "1";

// ---- the dialogs' requests (one at a time; Esc or a click outside cancels) ----

export interface FileRequest { purpose: Purpose; name: string; resolve: (path: string | null) => void }
export const fileRequest = shallowRef<FileRequest | null>(null);
export interface ConfirmRequest { resolve: (go: boolean) => void }
export const confirmRequest = shallowRef<ConfirmRequest | null>(null);

/** A dialog closed some other way (Esc, the backdrop): its request answers "cancelled". */
function settleOnClose(id: string, settle: () => void): void {
  const stop = watch(currentDialog, (v) => {
    if (v !== id) {
      stop();
      settle();
    }
  });
}

/** The model's file name without extension ("model" for none). */
function stem(): string {
  const f = state.value.file ?? "model";
  return f.replace(/\.[^.]*$/, "") || "model";
}

/** The browser's picker (wasm): the picked file, or null. */
function browserPick(purpose: Purpose): Promise<File | null> {
  return new Promise((resolve) => {
    const input = document.createElement("input");
    input.type = "file";
    input.accept = PURPOSES[purpose].ext.join(",");
    input.dataset.bb = purpose === "open" || purpose === "import_ifc" ? "ccore-open" : purpose === "link_model" ? "ccore-link" : `ccore-${purpose.replace("_", "-")}`;
    input.style.display = "none";
    input.addEventListener("change", () => {
      const f = input.files?.[0] ?? null;
      input.remove();
      resolve(f);
    });
    input.addEventListener("cancel", () => { input.remove(); resolve(null); });
    document.body.appendChild(input);
    input.click();
  });
}

/** Ask for a file: on the native host the page's own dialog (an absolute path), in wasm the
 *  browser's picker (the file's bytes; a write purpose has no picker there: null). */
export async function pickFile(purpose: Purpose): Promise<{ path?: string; file?: File } | null> {
  if (!isNative()) {
    if (PURPOSES[purpose].save) return null;
    const f = await browserPick(purpose);
    return f ? { file: f } : null;
  }
  if (fileRequest.value) {
    fileRequest.value.resolve(null);
    await nextTick(); // the dialog closes before the next one mounts
  }
  const path = await new Promise<string | null>((resolve) => {
    const req: FileRequest = { purpose, name: PURPOSES[purpose].save?.(stem()) ?? "", resolve: () => undefined };
    let done = false;
    req.resolve = (p) => {
      if (done) return;
      done = true;
      if (fileRequest.value === req) fileRequest.value = null;
      if (currentDialog.value === "file-browser") currentDialog.value = null;
      resolve(p);
    };
    fileRequest.value = req;
    currentDialog.value = "file-browser";
    settleOnClose("file-browser", () => req.resolve(null));
  });
  return path ? { path } : null;
}

/** True when nothing unsaved would be lost, or the user chose to discard it (in-page dialog). */
export async function confirmDiscard(): Promise<boolean> {
  let unsaved = state.value.unsaved;
  try { unsaved = query<{ unsaved?: boolean }>("state").unsaved === true; } catch { /* not started: the last change's */ }
  if (!unsaved) return true;
  if (confirmRequest.value) {
    confirmRequest.value.resolve(false);
    await nextTick();
  }
  return new Promise<boolean>((resolve) => {
    let done = false;
    const req: ConfirmRequest = {
      resolve: (go) => {
        if (done) return;
        done = true;
        if (confirmRequest.value === req) confirmRequest.value = null;
        if (currentDialog.value === "confirm-discard") currentDialog.value = null;
        resolve(go);
      },
    };
    confirmRequest.value = req;
    currentDialog.value = "confirm-discard";
    settleOnClose("confirm-discard", () => req.resolve(false));
  });
}

/** The last error of a file action, for the status bar (null: none). */
export const fileError = shallowRef<string | null>(null);

const msg = (e: unknown): string => (e instanceof Error ? e.message : String(e));

/** An action with a JSON body on the host; the error text, or null. */
function act(name: string, body: unknown): string | null {
  try {
    actionBody(name, body);
    return null;
  } catch (e) {
    return msg(e);
  }
}

function report(name: string, err: string | null): string | null {
  fileError.value = err;
  if (err) console.warn(`${name}: ${err}`);
  return err;
}

/** Open (or with `importIfc` import) a file the host can read; asks first when changes would be
 *  lost. The error text, or null (also when the user kept the changes: "cancelled"). */
export async function openPath(path: string, importIfc = false): Promise<string | null> {
  if (!(await confirmDiscard())) return "cancelled";
  return report(importIfc ? "import_ifc" : "open", act(importIfc ? "import_ifc" : "open", { path, discard: true }));
}

/** The native host's file actions (bimble.ts runAction). True when `name` is one of them. */
export function runFileAction(name: string): boolean {
  if (!isNative()) return false;
  switch (name) {
    case "open":
    case "import_ifc":
      void pickFile(name).then((r) => (r?.path ? openPath(r.path, name === "import_ifc") : null));
      return true;
    case "link_model":
    case "link_pdf":
    case "link_dxf":
      void pickFile(name).then((r) => r?.path && report(name, act(name, { path: r.path })));
      return true;
    case "save_as":
    case "export_ifc4":
    case "export_ifc2x3":
    case "share_for_viewing":
      void pickFile(name).then((r) => r?.path && report(name, act(name, { path: r.path })));
      return true;
    case "save": {
      // A new model has no file yet: Save is Save as (lib.rs:1832).
      const err = act("save", null);
      if (err?.startsWith("a new model has no file yet")) return runFileAction("save_as");
      report("save", err);
      return true;
    }
    case "new":
    case "close":
      void confirmDiscard().then((go) => go && report(name, act(name, { discard: true })));
      return true;
  }
  return false;
}

// ---- drag and drop, the page's unload ----

const DROP_OPEN = /\.(bimble|json|ifc|bimbleview)$/i, DROP_LINK = /\.(pdf|dxf)$/i;

const STAGE_MAX = 256 * 1024 * 1024; // host/fs.h FS_STAGE_MAX

/** A file dropped on the native page: its bytes staged by the host, then opened from there. */
async function dropNative(f: File): Promise<string | null> {
  if (!DROP_OPEN.test(f.name)) return report("drop", `${f.name}: a reference plan keeps its file's path, and a dropped file has none: use Link PDF… or Link DXF…`);
  if (f.size > STAGE_MAX) return report("drop", `${f.name}: too large to drop (256 MB at most): use File → Open`);
  if (!(await confirmDiscard())) return "cancelled";
  const port = Number(params.get("port")), t = encodeURIComponent(params.get("token") ?? "");
  try {
    // text/plain: a CORS simple request, as every call of the native client (bimble.ts).
    const r = await fetch(`http://127.0.0.1:${port}/stage?name=${encodeURIComponent(f.name)}&t=${t}`, { method: "POST", headers: { "Content-Type": "text/plain" }, body: f });
    if (!r.ok) return report("drop", (await r.text()) || `stage: HTTP ${r.status}`);
    const { path } = (await r.json()) as { path: string };
    return report("drop", act("open", { path, discard: true, dropped: true }));
  } catch (e) {
    return report("drop", msg(e));
  }
}

/** A file dropped on the page (wasm: its bytes; native: staged by the host). */
export async function dropFile(f: File): Promise<string | null> {
  if (!DROP_OPEN.test(f.name) && !DROP_LINK.test(f.name)) return report("drop", `${f.name}: not a .bimble, .json, .ifc, .bimbleview, .pdf or .dxf`);
  if (isNative()) return dropNative(f);
  if (!isWasm()) return null;
  const cc = await import("./ccore");
  try {
    if (DROP_OPEN.test(f.name)) {
      if (!(await confirmDiscard())) return "cancelled";
      await cc.ccoreOpenFile(f, true);
    } else {
      const bytes = new Uint8Array(await f.arrayBuffer());
      (/\.pdf$/i.test(f.name) ? cc.ccoreLinkPdf : cc.ccoreLinkDxf)(f.name, bytes);
    }
    return report("drop", null);
  } catch (e) {
    return report("drop", msg(e));
  }
}

let installed = false;
/** The page-wide listeners: drop a file, and the browser's question before leaving with unsaved changes. */
export function installFiles(): void {
  if (installed || (!isNative() && !isWasm())) return; // the Rust lab: egui takes its own drops
  installed = true;
  window.addEventListener("dragover", (e) => {
    if (e.dataTransfer?.types.includes("Files")) e.preventDefault();
  });
  window.addEventListener("drop", (e) => {
    const f = e.dataTransfer?.files?.[0];
    if (!f) return;
    e.preventDefault();
    void dropFile(f);
  });
  // The built page only (the desktop build, the published lab), or `&unload=1`: the Vite dev server
  // the lab tests run on reloads pages with edits in them, and a question would stop them.
  const armed = !import.meta.env.DEV || params.get("unload") === "1";
  window.addEventListener("beforeunload", (e) => {
    if (armed && state.value.unsaved) {
      e.preventDefault();
      e.returnValue = ""; // older browsers need it set
    }
  });
}

fileRoutes.run = runFileAction; // bimble.ts runAction: the native host's file actions come here
