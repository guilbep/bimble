// The shell's one Vue ref and `send` (ticket UX1): components move the shell only through
// `send(event)`; `next` (shell/machine.ts) decides, this file runs the calls a new state needs
// (a file call while `opening`, a view tab for `showView`) and turns the session's changes into
// events (`core`, and `opened` when another file is loaded).
import { computed, shallowRef, watch } from "vue";
import { backend, fileCall, openViews, openView, runAction, screenRoutes, shellHooks, state, type State } from "../bimble";
import { HOME, VIEW, kind, shows, step, type CoreState, type Event, type Shell } from "./machine";

/** What the session says now (the change JSON in `state`). */
export const coreOf = (s: State): CoreState => ({ file: s.file, unsaved: s.unsaved, tab: s.tab !== null });
const core = (): CoreState => coreOf(state.value);

/** The shell's state. Read it through `here`; change it only with `send`. Before the session's
 *  first change: the view, so a page that never hears from the session still shows its viewport. */
const shell = shallowRef<Shell>(VIEW);
export const here = computed(() => shell.value);

/** Events refused (no row in the state): logged once each, kept for the tests. */
export const refusals: string[] = [];

/** Move the shell. False when the state has no row for `e` (it stays, and the refusal is logged). */
export function send(e: Event): boolean {
  const s = shell.value, c = core();
  const to = step(s, e, c);
  if (to === null) {
    const key = `${kind(s)} ${e.do}`;
    if (!refusals.includes(key)) { refusals.push(key); console.warn(`shell: ${key}: no transition`); }
    return false;
  }
  shell.value = to;
  if (to.at === "opening" && to !== s) runFile(to);
  else if ((e.do === "showView" || e.do === "back") && to.at === "model" && to.show === "view" && c.file !== null && !c.tab) showAView();
  return true;
}

/** Whether `e` has a row now (a control for it may show). */
export const can = (e: Event): boolean => step(shell.value, e, core()) !== null;
/** Whether the control `id` (machine.ts CONTROLS) is shown now. */
export const shown = (id: string): boolean => shows(id, shell.value, core());

/** Run an action by name; a refusal of the session is shown (the error state), not only logged. */
export function act(name: string): string | null {
  const err = runAction(name);
  if (err) send({ do: "failed", message: err });
  return err;
}

/** The screen under the shell: "home", a page id, "sheets", or null for the viewport. */
function screen(s: Shell): string | null {
  switch (s.at) {
    case "home": return "home";
    case "model": return s.show === "page" ? s.page : s.show === "sheet" ? "sheets" : null;
    default: return screen(s.back);
  }
}
export const currentScreen = computed(() => screen(shell.value));

// The Home page's last tab closed: `showView` opens a view (the 3D view first) as the core has none.
let showing = false;
function showAView(): void {
  const v = openViews.value.views.find((x) => x.type === "three_d") ?? openViews.value.views[0];
  if (!v || showing) return;
  showing = true; // openView tells `viewShown`, which sends showView again: once is enough
  const err = openView(v.id);
  showing = false;
  if (err) send({ do: "failed", message: err });
}

// The file call of an `opening` state; its answer is the next event (unless the user left it).
function runFile(o: Extract<Shell, { at: "opening" }>): void {
  const finish = (err: string | null) => {
    if (shell.value !== o) return; // cancelled meanwhile
    send(err ? { do: "failed", message: err } : { do: o.what === "close" ? "closed" : "opened" });
  };
  const name = o.what === "import" ? "import_ifc" : o.what;
  if ((o.what === "open" || o.what === "import") && o.path === undefined && backend !== "rust") return pick(o, finish); // FS1: native too
  const body = o.path === undefined ? { discard: o.discard } : { path: o.path, discard: o.discard };
  const err = fileCall(name, body);
  // The Rust wasm opens its own dialog: the change brings the file (`opened` below), if any.
  if (!err && backend === "rust" && (o.what === "open" || o.what === "import")) { send({ do: "cancel" }); return; }
  finish(err);
}

// ---- FS1: the one picker (files.ts pickFile): the page's own dialog on the native host (a path), the
// browser's picker in the wasm (the file's bytes), or a file dropped on the page. Its answer is the
// next event: `opened`, `failed`, or `cancel` (the picker closed). The browser's picker may never say it
// was cancelled: the `opening` state keeps a Cancel button (ShellNotice; hidden while the page's own
// dialog shows, which has its own).
function pick(o: Extract<Shell, { at: "opening" }>, finish: (err: string | null) => void): void {
  const name = o.what === "import" ? "import_ifc" : "open";
  // A dynamic import: files.ts imports the dialogs and the pages, which import this file.
  import("../files")
    .then((m) => m.pickFile(name))
    .then(async (r) => {
      if (shell.value !== o) return; // cancelled meanwhile
      if (!r) return void send({ do: "cancel" });
      if (r.path !== undefined) return finish(fileCall(name, { path: r.path, discard: o.discard, ...(r.dropped ? { dropped: true } : {}) }));
      const f = r.file!;
      const cc = await import("../ccore");
      try {
        await cc.ccoreOpenFile(f, o.discard, o.what === "import");
        finish(null);
      } catch (e) {
        finish(`${f.name}: ${e instanceof Error ? e.message : String(e)}`);
      }
    })
    .catch((e: unknown) => finish(e instanceof Error ? e.message : String(e)));
}
// ---- end FS1 ----

// The session's changes: `opened` when another file is loaded, then `core` (the guard: model
// states need a model, the view a tab).
watch(state, (s, prev) => {
  if (s.file !== null && s.file !== prev.file) send({ do: "opened" });
  send({ do: "core" });
});
shellHooks.viewShown = () => { send({ do: "showView" }); };

// Actions the lab shows as its own screens, and the file actions: every caller of runAction
// (quick bar, File menu, palette, ribbon, Home) goes through the shell.
Object.assign(screenRoutes, {
  home: () => send({ do: "home" }),
  project: () => send({ do: "showPage", page: "project-info" }),
  history: () => send({ do: "showPage", page: "history" }),
  new: () => send({ do: "new" }),
  open: () => send({ do: "open" }),
  import_ifc: () => send({ do: "import" }),
  close: () => send({ do: "close" }),
});

export { HOME, VIEW };
