// The lab shell as a state machine (ticket UX1): what the window shows (Home, a file
// being opened, the model's view, a page, a sheet, an error, the unsaved-changes question) and the
// only function that moves between them, `next(state, event, core)`. Plain TypeScript, no import:
// node runs it as it is (ccore/test/shell_graph.mjs walks TRANSITIONS); shell/send.ts holds the
// one Vue ref and runs the calls; components move only through `send(event)`.
//
// Derived, not duplicated: the C session owns the model; the shell only reads it (`CoreState`):
//
//   | shell needs        | read from (the session's change JSON / `query state`)       |
//   |--------------------|-------------------------------------------------------------|
//   | a model is loaded  | `file` is not null (calls.c cc_change_json, q_state)        |
//   | a current tab      | `tab` is not null (calls.c: `loaded && tab_open && active`)  |
//   | unsaved changes    | `unsaved` (cc_session_unsaved)                              |
//
// so `model.*` is only reachable while `file` is set, and every change of the session sends `core`,
// which leaves `model.*` for Home when the file is gone or, on the view, when the last tab closed
// (the C core's Home: views.h "closing the last tab leaves no current tab").

/** What the shell reads from the session. `tab`: a current tab (absent in the change JSON: true). */
export interface CoreState { file: string | null; unsaved: boolean; tab: boolean }

/** A file call the shell waits for: new, open, import, or close (the model). */
export type FileCall = "new" | "open" | "import" | "close";

export type Shell =
  | { at: "home" }
  | { at: "opening"; what: FileCall; path?: string; discard: boolean; back: Shell }
  | { at: "model"; show: "view" }
  | { at: "model"; show: "page"; page: string }
  | { at: "model"; show: "sheet"; sheet: string }
  | { at: "error"; message: string; back: Shell }
  | { at: "confirmClose"; then: FileCall; path?: string; back: Shell }
  // FIXUI: the native host stopped answering (send.ts: the change poll failed for a while). What
  // was known when it went: the file and whether it had unsaved changes. Blocking: the page cannot
  // reach the model. `reconnected` decides: the same session goes back, another says so.
  | { at: "disconnected"; file: string | null; unsaved: boolean; back: Shell }
  | { at: "sessionLost"; file: string | null; unsaved: boolean };

export type Event =
  | { do: "new" }
  | { do: "open"; path?: string }
  | { do: "import"; path?: string }
  | { do: "close" }
  | { do: "opened" }
  | { do: "closed" }
  | { do: "failed"; message: string }
  | { do: "cancel" }
  | { do: "discard" }
  | { do: "showView" }
  | { do: "showPage"; page: string }
  | { do: "showSheet"; sheet: string }
  | { do: "back" }
  | { do: "home" }
  | { do: "core" }
  // FIXUI: the connection to the native host (send.ts watches bimble.ts `connection`).
  | { do: "lost" }
  | { do: "retry" }
  | { do: "reconnected"; same: boolean };

export type EventName = Event["do"];
/** The state's name in the transition table: `model` is split by what it shows. */
export type Kind = "home" | "opening" | "model.view" | "model.page" | "model.sheet" | "error" | "confirmClose" | "disconnected" | "sessionLost";
export const KINDS: readonly Kind[] = ["home", "opening", "model.view", "model.page", "model.sheet", "error", "confirmClose", "disconnected", "sessionLost"];
export const EVENTS: readonly EventName[] = ["new", "open", "import", "close", "opened", "closed", "failed", "cancel", "discard",
  "showView", "showPage", "showSheet", "back", "home", "core", "lost", "retry", "reconnected"];

export const kind = (s: Shell): Kind => (s.at === "model" ? `model.${s.show}` : s.at);

export const HOME: Shell = { at: "home" };
export const VIEW: Shell = { at: "model", show: "view" };

/** What the session allows `s` to be now: model states need a model; the view needs a tab. */
export function derive(s: Shell, core: CoreState): Shell {
  if (s.at === "model" && core.file === null) return HOME;
  if (s.at === "model" && s.show === "view" && !core.tab) return HOME;
  return s;
}

type Row = (s: Shell, e: Event, core: CoreState) => Shell | null;
const as = <D extends EventName>(e: Event, d: D) => e as Extract<Event, { do: D }>;

// A file event: with unsaved changes (and a model to lose), ask first; else wait for the call.
const file = (what: FileCall): Row => (s, e, core) => {
  const path = "path" in e ? e.path : undefined;
  if (core.unsaved && core.file !== null) return { at: "confirmClose", then: what, ...(path === undefined ? {} : { path }), back: s };
  return { at: "opening", what, ...(path === undefined ? {} : { path }), discard: false, back: s };
};
const FILE = { new: file("new"), open: file("open"), import: file("import") };
// Where a file event fails or is cancelled, or an error is dismissed: back, as the session allows it now.
const back = (s: Shell, core: CoreState): Shell => derive(s, core);
const failed: Row = (s, e) => ({ at: "error", message: as(e, "failed").message, back: s });
const opened: Row = (_s, _e, core) => (core.file === null ? null : VIEW);
const page: Row = (_s, e, core) => (core.file === null ? null : { at: "model", show: "page", page: as(e, "showPage").page });
const sheet: Row = (_s, e, core) => (core.file === null ? null : { at: "model", show: "sheet", sheet: as(e, "showSheet").sheet });
const view: Row = (_s, _e, core) => (core.file === null ? null : VIEW);
const same: Row = (s) => s;
const toHome: Row = () => HOME;
const sync: Row = (s, _e, core) => derive(s, core);
// FIXUI: the host stopped answering: block, keeping what was known and where the user was.
const lost: Row = (s, _e, core) => ({ at: "disconnected", file: core.file, unsaved: core.unsaved, back: s });
// A second error while one shows: the newer message, the same way back (it was dropped with a warning).
const failedAgain: Row = (s, e) => ({ ...(s as Extract<Shell, { at: "error" }>), message: as(e, "failed").message });

// Every model state: the same rows, but `back` (a page and a sheet go back to the view).
const MODEL: Partial<Record<EventName, Row>> = {
  ...FILE, close: file("close"), opened, closed: toHome, failed, showView: view, showPage: page, showSheet: sheet,
  home: toHome, core: sync, lost,
};

/** The transition table: state kind -> event -> row. An event with no row (or a row answering
 *  null) is refused: `next` keeps the state and `send` reports it. */
export const TRANSITIONS: Record<Kind, Partial<Record<EventName, Row>>> = {
  home: { ...FILE, close: (s, e, core) => (core.file === null ? null : file("close")(s, e, core)), opened, closed: same, failed,
    showView: view, showPage: page, showSheet: sheet, home: same, core: sync, lost },
  opening: {
    // The call answered ok: the session's change (with the file) may come later; `core` re-checks.
    opened: () => VIEW,
    closed: toHome,
    failed: (s, e) => ({ at: "error", message: as(e, "failed").message, back: (s as Extract<Shell, { at: "opening" }>).back }),
    cancel: (s, _e, core) => back((s as Extract<Shell, { at: "opening" }>).back, core),
    core: same,
    lost,
  },
  "model.view": { ...MODEL },
  "model.page": { ...MODEL, back: view },
  "model.sheet": { ...MODEL, back: view },
  error: {
    back: (s, _e, core) => back((s as Extract<Shell, { at: "error" }>).back, core),
    home: toHome,
    opened, closed: toHome,
    failed: failedAgain,
    core: same,
    lost,
  },
  confirmClose: {
    discard: (s) => {
      const c = s as Extract<Shell, { at: "confirmClose" }>;
      return { at: "opening", what: c.then, ...(c.path === undefined ? {} : { path: c.path }), discard: true, back: c.back };
    },
    cancel: (s, _e, core) => back((s as Extract<Shell, { at: "confirmClose" }>).back, core),
    opened, closed: toHome,
    // A refusal while the question shows (an action run from the palette): the error, then back here.
    failed,
    core: same,
    lost,
  },
  // FIXUI: nothing reaches the host. The session's changes cannot come (a restarted host's first
  // change is read after `reconnected`); Retry asks again now; a call's failure is the outage's.
  disconnected: {
    retry: same,
    lost: same,
    failed: same,
    opened: same, closed: same, core: same,
    reconnected: (s, e, core) => {
      const d = s as Extract<Shell, { at: "disconnected" }>;
      return as(e, "reconnected").same ? derive(d.back, core) : { at: "sessionLost", file: d.file, unsaved: d.unsaved };
    },
  },
  // The host answers again, but with another session (restarted without it): said plainly, never a
  // silent Home. Home, reopen a file (Home's recent list, or the file by its path), or show what the
  // host has open now.
  sessionLost: {
    home: toHome,
    ...FILE,
    showView: view,
    opened: same, closed: same, core: same,
    failed,
    lost: (s, _e, core) => ({ at: "disconnected", file: (s as Extract<Shell, { at: "sessionLost" }>).file ?? core.file, unsaved: false, back: s }),
  },
};

/** The state after `e`, or null when `s` has no row for it (refused). Pure. */
export function step(s: Shell, e: Event, core: CoreState): Shell | null {
  const row = TRANSITIONS[kind(s)][e.do];
  return row ? row(s, e, core) : null;
}

/** The state after `e`: `s` itself when refused. The only place that decides a transition. */
export function next(s: Shell, e: Event, core: CoreState): Shell {
  return step(s, e, core) ?? s;
}

/** The controls that move the shell, the event each sends and where it is shown. The UI shows a
 *  control only where `shown` says so; shell_graph.mjs proves each one has a row wherever shown. */
export interface Control { id: string; event: Event; shown: (s: Shell, core: CoreState) => boolean }
const k = (...ks: Kind[]) => (s: Shell) => ks.includes(kind(s));
const inModel = k("model.view", "model.page", "model.sheet");
export const CONTROLS: readonly Control[] = [
  { id: "home-new", event: { do: "new" }, shown: k("home") },
  { id: "home-open", event: { do: "open" }, shown: k("home") },
  { id: "home-import", event: { do: "import" }, shown: k("home") },
  { id: "home-back", event: { do: "showView" }, shown: (s, c) => kind(s) === "home" && c.file !== null },
  // FS1: a recent file (Home's list: files.ts openPath) and a file dropped on the page (files.ts dropFile).
  { id: "home-recent-file", event: { do: "open", path: "/recent/file.bimble" }, shown: k("home") },
  { id: "page-drop", event: { do: "open" }, shown: k("home", "model.view", "model.page", "model.sheet") },
  { id: "quickbar-logo", event: { do: "home" }, shown: k("home", "model.view", "model.page", "model.sheet") },
  { id: "quickbar-home", event: { do: "home" }, shown: inModel },
  { id: "quickbar-open", event: { do: "open" }, shown: inModel },
  { id: "file-menu-new", event: { do: "new" }, shown: k("home", "model.view", "model.page", "model.sheet") },
  { id: "file-menu-open", event: { do: "open" }, shown: k("home", "model.view", "model.page", "model.sheet") },
  { id: "file-menu-import_ifc", event: { do: "import" }, shown: k("home", "model.view", "model.page", "model.sheet") },
  { id: "file-menu-project", event: { do: "showPage", page: "project-info" }, shown: (s, c) => inModel(s) || (kind(s) === "home" && c.file !== null) },
  { id: "file-menu-history", event: { do: "showPage", page: "history" }, shown: (s, c) => inModel(s) || (kind(s) === "home" && c.file !== null) },
  { id: "viewtab", event: { do: "showView" }, shown: inModel },
  { id: "viewtab-page-close", event: { do: "back" }, shown: k("model.page", "model.sheet") },
  { id: "navigator-page", event: { do: "showPage", page: "quantities" }, shown: inModel },
  { id: "ribbon-page", event: { do: "showPage", page: "quantities" }, shown: inModel },
  { id: "palette-page", event: { do: "showPage", page: "quantities" }, shown: inModel }, // DEAD1: Go to: <schedule>
  { id: "shell-opening-cancel", event: { do: "cancel" }, shown: k("opening") },
  { id: "shell-error-back", event: { do: "back" }, shown: k("error") },
  { id: "shell-confirm-discard", event: { do: "discard" }, shown: k("confirmClose") },
  { id: "shell-confirm-cancel", event: { do: "cancel" }, shown: k("confirmClose") },
  // FIXUI: File -> Close (the model), and the connection notices.
  { id: "file-menu-close", event: { do: "close" }, shown: (s, c) => inModel(s) || (kind(s) === "home" && c.file !== null) },
  { id: "shell-disconnected-retry", event: { do: "retry" }, shown: k("disconnected") },
  { id: "shell-lost-home", event: { do: "home" }, shown: k("sessionLost") },
  { id: "shell-lost-reopen", event: { do: "open", path: "/recent/file.bimble" }, shown: (s) => s.at === "sessionLost" && s.file !== null },
  { id: "shell-lost-show", event: { do: "showView" }, shown: (s, c) => kind(s) === "sessionLost" && c.file !== null },
  // VIEWSHEET: View → New view… creates the view and shows it (dialogs/NewView.vue); Sheets → Place view… places
  // a view on the sheet shown and stays on that sheet (dialogs/PlaceView.vue). Dialogs are drawn over any model state.
  { id: "newview-create", event: { do: "showView" }, shown: inModel },
  { id: "placeview-pick", event: { do: "showSheet", sheet: "S-101" }, shown: k("model.sheet") },
  { id: "ribbon-new-sheet", event: { do: "showSheet", sheet: "S-102" }, shown: inModel }, // DEAD1: New sheet shows the sheet it made
];

/** Whether the control `id` is shown in `s` (an unknown id: never). */
export function shows(id: string, s: Shell, core: CoreState): boolean {
  return CONTROLS.find((c) => c.id === id)?.shown(s, core) ?? false;
}
