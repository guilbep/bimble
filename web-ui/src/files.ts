// Files in the page (ticket FS1): one picker for every action that reads or writes a file.
// - `pickFile(purpose)`: the native host (`?native=1`, the desktop build) has no system dialog the
//   page can show, so the page draws its own (dialogs/FileBrowser.vue) over the host's queries
//   `fs_roots`, `fs_home`, `fs_list` (ccore/host/fs.h: the path rule) and answers an absolute path;
//   the C core in wasm (`?ccore=1`) answers the bytes of the browser's picker (`<input type=file>`).
//   The shell (shell/send.ts) calls it from its `opening` state for Open and Import, and turns the
//   answer into `opened`, `failed {message}` or `cancel`.
// - Opening is navigation, so it goes through the shell: a recent file (`openPath`) and a dropped
//   file send `{do: "open"}`; the shell asks its one unsaved-changes question (`confirmClose`,
//   shell/ShellNotice.vue), leaves Home on `opened` and shows `error` on `failed`.
// - Not navigation, plain functions (`runFileAction`, bimble.ts runAction on the native host):
//   link_model, link_pdf, link_dxf, save_as, a Save of a model without a file (Save as),
//   export_ifc4, export_ifc2x3, share_for_viewing; each sends the picked path in its body. The wasm
//   build keeps its own (ccore.ts fileAction: downloads), on the same picker.
// - Drag and drop (`installFiles`): a .bimble, .json, .ifc, .bimbleview opens (through the shell: the
//   picker of its `opening` state hands over the dropped file instead of asking), a .pdf or .dxf
//   links to the active view. Wasm: the bytes (ccore.ts). Native: the browser gives no path, so the
//   bytes go to the host (`POST /stage`, host/fs.h: its own folder, 256 MB at most) and open from
//   there as a dropped document (Save asks where; no recent file). A dropped PDF or DXF is refused
//   there: a reference plan keeps its file's path, and a staged copy is no place to keep.
// - The page's `beforeunload` asks the browser's own question with unsaved changes.
import { nextTick, shallowRef, watch } from "vue";
import { currentDialog } from "./dialogs";
import { actionBody, fileRoutes, state } from "./bimble";
import { here, send } from "./shell/send";

export type Purpose = "open" | "import_ifc" | "link_model" | "link_folder" | "link_pdf" | "link_dxf" | "save_as" | "export_ifc4" | "export_ifc2x3" | "share_for_viewing";

/** Per purpose: the extensions listed, and for a write the name proposed. */
export const PURPOSES: Record<Purpose, { ext: string[]; save?: (stem: string) => string; folder?: boolean }> = {
  open: { ext: [".bimble", ".json", ".ifc", ".bimbleview"] },
  import_ifc: { ext: [".ifc"] },
  link_model: { ext: [".ifc", ".bimbleview", ".bimble", ".json"] },
  link_folder: { ext: [".ifc", ".bimbleview"], folder: true }, // LINKF: the folder shown is the answer
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

// ---- the dialog's request (one at a time; Esc or a click outside cancels) ----

/** What the dialog answers: a path, several (LINKF: link_model's multi-select), a folder ("Link this folder"), or null. */
export type DialogAnswer = string | string[] | { folder: string } | null;
export interface FileRequest { purpose: Purpose; name: string; resolve: (path: DialogAnswer) => void }
export const fileRequest = shallowRef<FileRequest | null>(null);

/** The model's file name without extension ("model" for none). */
function stem(): string {
  const f = state.value.file ?? "model";
  return f.replace(/\.[^.]*$/, "") || "model";
}

/** The browser's picker (wasm): the picked file, or null when it says it was cancelled (it may never say). */
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

/** The page's own dialog (native): an absolute path, or null (closed). */
async function dialogPick(purpose: Purpose): Promise<DialogAnswer> {
  if (fileRequest.value) {
    fileRequest.value.resolve(null);
    await nextTick(); // the dialog closes before the next one mounts
  }
  return new Promise<DialogAnswer>((resolve) => {
    const req: FileRequest = { purpose, name: PURPOSES[purpose].save?.(stem()) ?? "", resolve: () => undefined };
    let done = false;
    let stop: (() => void) | null = null;
    req.resolve = (p) => {
      if (done) return;
      done = true;
      stop?.();
      if (fileRequest.value === req) fileRequest.value = null;
      if (currentDialog.value === "file-browser") currentDialog.value = null;
      resolve(p);
    };
    fileRequest.value = req;
    currentDialog.value = "file-browser";
    // Closed some other way (Esc, the backdrop): cancelled.
    stop = watch(currentDialog, (v) => { if (v !== "file-browser") req.resolve(null); });
  });
}

/** A file dropped on the page, waiting for the shell's `opening` state to take it (pickFile). */
let dropped: File | null = null;

/** What the picker answers: a path the host reads (`dropped`: a staged copy of a dropped file), or a
 *  file's bytes (wasm). */
export interface Picked { path?: string; file?: File; dropped?: boolean; paths?: string[]; folder?: string }

/** Ask for a file: on the native host the page's own dialog (an absolute path), in wasm the
 *  browser's picker (the file's bytes; a write purpose has no picker there: null). A file dropped on
 *  the page just before is the answer of Open, without asking. Throws when a drop cannot be staged. */
/** LINKF: the browser's picker for several files (wasm: Link a folder… links each one). */
export function browserPickMany(ext: string[]): Promise<File[]> {
  return new Promise((resolve) => {
    const input = document.createElement("input");
    input.type = "file";
    input.multiple = true;
    input.accept = ext.join(",");
    input.dataset.bb = "ccore-link-folder";
    input.style.display = "none";
    input.addEventListener("change", () => { const f = [...(input.files ?? [])]; input.remove(); resolve(f); });
    input.addEventListener("cancel", () => { input.remove(); resolve([]); });
    document.body.appendChild(input);
    input.click();
  });
}

export async function pickFile(purpose: Purpose): Promise<Picked | null> {
  const drop = purpose === "open" ? dropped : null;
  dropped = null;
  if (drop) return isNative() ? { path: await stage(drop), dropped: true } : { file: drop };
  if (!isNative()) {
    if (PURPOSES[purpose].save) return null;
    const f = await browserPick(purpose);
    return f ? { file: f } : null;
  }
  const a = await dialogPick(purpose);
  if (!a) return null;
  if (Array.isArray(a)) return a.length === 1 && a[0] ? { path: a[0] } : { paths: a }; // LINKF
  if (typeof a === "object") return { folder: a.folder };
  return { path: a };
}

/** Open a file the host can read (a recent file): through the shell. False when it refused now. */
export const openPath = (path: string): boolean => send({ do: "open", path });

/** The last error of a file action that is not navigation, for the console and the tests (null: none). */
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

/** The native host's file actions that are not navigation (bimble.ts runAction). True when `name`
 *  is one of them. Open, import, new and close are the shell's (shell/send.ts). */
export function runFileAction(name: string): boolean {
  if (!isNative()) return false;
  switch (name) {
    case "link_model":
    case "link_pdf":
    case "link_dxf":
    case "save_as":
    case "export_ifc4":
    case "export_ifc2x3":
    case "share_for_viewing":
      void pickFile(name).then((r) => r?.path && report(name, act(name, { path: r.path })));
      return true;
    case "save": {
      // A model without a file of its own (a new one, a dropped one): Save is Save as (lib.rs:1832).
      const err = act("save", null);
      if (err?.startsWith("a new model has no file yet")) return runFileAction("save_as");
      report("save", err);
      return true;
    }
  }
  return false;
}

// ---- drag and drop, the page's unload ----

const DROP_OPEN = /\.(bimble|json|ifc|bimbleview)$/i, DROP_LINK = /\.(pdf|dxf)$/i;
const STAGE_MAX = 256 * 1024 * 1024; // host/fs.h FS_STAGE_MAX

/** The bytes of a dropped file to the native host (POST /stage): the path of its copy. Throws the host's text. */
async function stage(f: File): Promise<string> {
  if (f.size > STAGE_MAX) throw new Error(`${f.name}: too large to drop (256 MB at most): use File → Open`);
  const port = Number(params.get("port")), t = encodeURIComponent(params.get("token") ?? "");
  // text/plain: a CORS simple request, as every call of the native client (bimble.ts).
  const r = await fetch(`http://127.0.0.1:${port}/stage?name=${encodeURIComponent(f.name)}&t=${t}`, { method: "POST", headers: { "Content-Type": "text/plain" }, body: f });
  if (!r.ok) throw new Error((await r.text()) || `stage: HTTP ${r.status}`);
  return ((await r.json()) as { path: string }).path;
}

/** A file dropped on the page. A model opens through the shell (send open: the question, then the
 *  shell's picker takes this file); a PDF or DXF links to the active view (wasm only). */
export async function dropFile(f: File): Promise<string | null> {
  if (DROP_OPEN.test(f.name)) {
    dropped = f;
    if (!send({ do: "open" })) dropped = null; // the shell is busy (a question, an error): not now
    return null;
  }
  if (!DROP_LINK.test(f.name)) return report("drop", `${f.name}: not a .bimble, .json, .ifc, .bimbleview, .pdf or .dxf`);
  if (isNative()) return report("drop", `${f.name}: a reference plan keeps its file's path, and a dropped file has none: use Link PDF… or Link DXF…`);
  if (!isWasm()) return null;
  const cc = await import("./ccore");
  try {
    (/\.pdf$/i.test(f.name) ? cc.ccoreLinkPdf : cc.ccoreLinkDxf)(f.name, new Uint8Array(await f.arrayBuffer()));
    return report("drop", null);
  } catch (e) {
    return report("drop", msg(e));
  }
}

/** LINKF: several files dropped with a model open: each model file is linked, in ONE call (one undo
 *  step, the host's queue). The browser gives no path, so natively each is staged first (256 MB at
 *  most: a larger one is said, "use Link IFC… and pick it in the dialog", the others go on); in
 *  wasm the bytes go to the module, one file after the other. */
export async function dropMany(fs: File[]): Promise<string | null> {
  if (state.value.file === null) return report("drop", "open or create a model first: several dropped files are linked to it");
  const ok = fs.filter((f) => DROP_OPEN.test(f.name)), errs: string[] = [];
  for (const f of fs) if (!DROP_OPEN.test(f.name)) errs.push(`${f.name}: not a .ifc, .bimbleview, .bimble or .json`);
  if (isNative()) {
    const paths: string[] = [];
    for (const f of ok) {
      if (f.size > STAGE_MAX) { errs.push(`${f.name}: too large to drop (256 MB at most): use Link IFC… and pick it in the dialog`); continue; }
      try { paths.push(await stage(f)); } catch (e) { errs.push(msg(e)); }
    }
    if (paths.length) { const err = act("link_model", { paths }); if (err) errs.push(err); }
  } else if (isWasm()) {
    const cc = await import("./ccore");
    for (const f of [...ok].sort((a, b) => a.size - b.size))
      try { cc.ccoreLinkFile(f.name, new Uint8Array(await f.arrayBuffer())); } catch (e) { errs.push(msg(e)); }
  }
  return report("drop", errs.length ? errs.join("; ") : null);
}

let installed = false;
/** The page-wide listeners: drop a file, and the browser's question before leaving with unsaved changes. */
export function installFiles(): void {
  if (installed || (!isNative() && !isWasm())) return; // the Rust lab: egui takes its own drops
  installed = true;
  // A dropped file waits only while the shell asks or opens: answered Cancel, it is forgotten.
  watch(here, (s) => { if (s.at !== "confirmClose" && s.at !== "opening") dropped = null; });
  window.addEventListener("dragover", (e) => {
    if (e.dataTransfer?.types.includes("Files")) e.preventDefault();
  });
  window.addEventListener("drop", (e) => {
    const all = [...(e.dataTransfer?.files ?? [])];
    const f = all[0];
    if (!f) return;
    e.preventDefault();
    if (all.length > 1) void dropMany(all); // LINKF: several files: linked
    else void dropFile(f);
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
