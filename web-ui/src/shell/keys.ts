// The lab's keyboard shortcuts (ticket KEY1): one table, one `keydown` listener (installKeys, from
// App.vue), the shown labels (tooltips, File menu, palette rows) read from the same table.
//
// Rust's shortcuts are `crates/bimble-viewer/src/lib.rs` handle_keys (2930-3051) and palette_key
// (1059); `ccore/golden/parity/shortcuts.json` (PAR2) lists them, tools/parity.mjs checks this file
// against it. Rust's meaning, read there, not guessed from the letters:
//   - Ctrl N, Ctrl K: any time (Ctrl K even while typing: palette_key runs before
//     egui_wants_keyboard_input); nothing while the palette (here: any dialog) is open.
//   - Ctrl Z, Ctrl Y / Ctrl Shift Z, Ctrl S, Delete: an editable (.bimble) model only; Delete
//     deletes every selected element (one `delete` verb: edit::delete, RemoveElement each).
//   - F O E H T B M P: key_pressed; O, T and B do nothing in a 2D view (lib.rs:3002 `!flat`); P only
//     in one (lib.rs:2991 `in_2d`).
//   - Escape: layered (ESCAPE below).
// Rust has no Ctrl O, no Ctrl Shift S, no Ctrl P and no Ctrl ,: not here (the File menu showed
// three of them without handling them).
// A shortcut runs only when its action is enabled (`query actions`, and the shell's `can` for the
// shell's actions) and never while typing in a field or while a dialog is open; the browser's
// default is prevented only when the key was handled (Ctrl S: also when there is nothing to save,
// so the browser's own Save dialog never opens over the app). macOS: Cmd for Ctrl.
import { actions, backend, editableNow, openViews, redo, runVerb, select, selectLinked, state, undo, viewportKeys } from "../bimble";
import { currentDialog } from "../dialogs";
import { act, can, here, send as shellSend } from "./send";
import { small, sheet } from "../small/screen";

/** Where a row applies. `anywhere`: every shell state; `view`: the model's view is shown (not a page,
 *  Home or a sheet); `3d` / `2d`: that view is a 3D view / a plan or section. */
export type KeyContext = "anywhere" | "view" | "3d" | "2d";

export interface KeyRow {
  /** As shown and as Rust spells it (`Action::keys`, ribbon.rs:318): "Ctrl N", "F", "Delete". */
  keys: string;
  /** `KeyboardEvent.key`, lower case. */
  key: string;
  ctrl?: boolean;
  shift?: boolean;
  /** The ribbon action it runs (`query actions`), or null for an effect of its own. */
  action: string | null;
  /** An effect that is not one action: "delete", "escape", "palette". */
  effect?: "delete" | "escape" | "palette";
  context: KeyContext;
  /** Rust's source of the binding. */
  rust: string;
  /** Also while typing in a field (Ctrl K, Escape's dialog and shell layers). */
  typing?: boolean;
  /** Ctrl S: the browser's default prevented even when there is nothing to do. */
  swallow?: boolean;
}

export const KEYS: readonly KeyRow[] = [
  { keys: "Ctrl N", key: "n", ctrl: true, action: "new", context: "anywhere", rust: "lib.rs:2939" },
  { keys: "Ctrl Z", key: "z", ctrl: true, action: "undo", context: "anywhere", rust: "lib.rs:2948" },
  { keys: "Ctrl Y", key: "y", ctrl: true, action: "redo", context: "anywhere", rust: "lib.rs:2949" },
  { keys: "Ctrl Shift Z", key: "z", ctrl: true, shift: true, action: "redo", context: "anywhere", rust: "lib.rs:2949" },
  { keys: "Ctrl S", key: "s", ctrl: true, action: "save", context: "anywhere", rust: "lib.rs:2950", swallow: true },
  { keys: "Delete", key: "delete", action: null, effect: "delete", context: "view", rust: "lib.rs:2951" },
  { keys: "F", key: "f", action: "fit", context: "view", rust: "lib.rs:2980" },
  { keys: "O", key: "o", action: "orthographic", context: "3d", rust: "lib.rs:2981" },
  { keys: "E", key: "e", action: "edges", context: "view", rust: "lib.rs:2982" },
  { keys: "H", key: "h", action: "hatching", context: "view", rust: "lib.rs:2983" },
  { keys: "T", key: "t", action: "top", context: "3d", rust: "lib.rs:2984" },
  { keys: "Escape", key: "escape", action: null, effect: "escape", context: "anywhere", rust: "lib.rs:2985", typing: true },
  { keys: "B", key: "b", action: "section_box", context: "3d", rust: "lib.rs:2986" },
  { keys: "M", key: "m", action: "measure", context: "view", rust: "lib.rs:2987" },
  { keys: "P", key: "p", action: "paper_view", context: "2d", rust: "lib.rs:2988" },
  { keys: "Ctrl K", key: "k", ctrl: true, action: null, effect: "palette", context: "anywhere", rust: "lib.rs:1059", typing: true },
];

/** Escape's layers, first that applies wins (lib.rs:3031-3050, then the lab's own screens):
 *  the open dialog; the small screen's sheet; the shell's question or error (cancel, back); the
 *  viewport's tool (Rust: placing, then the measure's first point, then the measure tool; the
 *  lab's annotate tool first); the selection (an annotation, the elements, a linked element); then
 *  the shell's `back` (a page or sheet back to the view). */
export const ESCAPE: readonly { id: string; typing: boolean; run: () => boolean }[] = [
  { id: "dialog", typing: true, run: () => (currentDialog.value ? ((currentDialog.value = null), true) : false) },
  { id: "small-sheet", typing: true, run: () => (small.value && sheet.value ? ((sheet.value = null), true) : false) },
  { id: "shell-cancel", typing: true, run: () => (can({ do: "cancel" }) ? send("cancel") : false) },
  { id: "shell-error", typing: true, run: () => (here.value.at === "error" ? send("back") : false) },
  { id: "tool", typing: false, run: () => inView() && (viewportKeys.cancelTool?.() ?? false) },
  { id: "selection", typing: false, run: () => inView() && clearSelection() },
  { id: "shell-back", typing: true, run: () => (can({ do: "back" }) ? send("back") : false) },
];

const send = (d: "cancel" | "back"): boolean => shellSend({ do: d });

const isMac = typeof navigator !== "undefined" && /Mac|iPhone|iPad/.test(navigator.platform || navigator.userAgent);
/** A row's keys as shown on this machine (Cmd on macOS). */
export const label = (r: KeyRow): string => (isMac ? r.keys.replace(/^Ctrl /, "⌘ ") : r.keys);
/** The shortcut shown for an action (the first row that runs it: Ctrl Y for redo, as Action::keys), or "". */
export function shortcutFor(action: string | null | undefined): string {
  const r = action ? KEYS.find((k) => k.action === action || (k.effect === action)) : undefined;
  return r ? label(r) : "";
}

const enabled = (name: string): boolean => actions.value.some((a) => a.name === name && a.enabled);
/** The model's view is shown: no page, no sheet, not Home, no question. */
const inView = (): boolean => here.value.at === "model" && here.value.show === "view";
/** The view shown is a plan or a section (viewing.rs:258 in_2d). */
function flat(): boolean {
  const cur = openViews.value.current;
  if (cur?.type !== "view") return false;
  const v = openViews.value.views.find((x) => x.id === cur.id);
  return v !== undefined && v.type !== "three_d";
}
function inContext(c: KeyContext): boolean {
  if (c === "anywhere") return true;
  if (!inView()) return false;
  return c === "view" || (c === "2d") === flat();
}

function clearSelection(): boolean {
  if (viewportKeys.selectedAnnotation?.()) { viewportKeys.dropAnnotation?.(); return true; }
  const linked = state.value.linked;
  if (linked) { selectLinked(linked.link, null); return true; }
  if (state.value.selection.length) { select([]); return true; }
  return false;
}

/** Delete (lib.rs:2951-2969): the selected annotation (the lab's plan), else every selected
 *  element of an editable model, one `delete` verb (one history step). */
function deleteSelected(): boolean {
  if (viewportKeys.deleteAnnotation?.()) return true;
  const ids = state.value.selection;
  if (!ids.length || state.value.linked || !editableNow()) return false;
  const err = runVerb("delete", { ids });
  if (err) console.warn(`keys: delete: ${err}`);
  return true;
}

/** Runs a row whose key and context match. True when it did something (the key is then consumed). */
function run(r: KeyRow, typing: boolean): boolean {
  if (r.effect === "escape") {
    for (const l of ESCAPE) if ((!typing || l.typing) && l.run()) return true;
    return false;
  }
  if (r.effect === "palette") {
    if (currentDialog.value) return false;
    currentDialog.value = "palette";
    return true;
  }
  if (r.effect === "delete") return deleteSelected();
  const a = r.action!;
  if (!enabled(a)) return false;
  if (a === "new" && !can({ do: "new" })) return false;
  if (a === "undo" || a === "redo") {
    const err = a === "undo" ? undo() : redo();
    if (err) console.warn(`keys: ${a}: ${err}`);
    return true;
  }
  act(a);
  return true;
}

/** The row for this event, or null. */
export function rowOf(e: Pick<KeyboardEvent, "key" | "ctrlKey" | "metaKey" | "shiftKey" | "altKey">): KeyRow | null {
  if (e.altKey) return null;
  const ctrl = e.ctrlKey || e.metaKey, key = e.key.toLowerCase();
  return KEYS.find((r) => r.key === key && !!r.ctrl === ctrl && !!r.shift === e.shiftKey) ??
    // Escape and Delete with Shift: still themselves (egui's key_pressed ignores modifiers).
    (!ctrl && (key === "escape" || key === "delete") ? KEYS.find((r) => r.key === key) ?? null : null);
}

const typingIn = (t: EventTarget | null): boolean => {
  const el = t as HTMLElement | null;
  return !!el && (el.tagName === "INPUT" || el.tagName === "TEXTAREA" || el.tagName === "SELECT" || el.isContentEditable);
};

/** Counts for the tests (window.__bbKeys): keys handled by row, keys seen and left alone. */
export const keyStats = { handled: {} as Record<string, number>, ignored: 0 };

function onKey(e: KeyboardEvent): void {
  if (e.defaultPrevented || e.isComposing) return;
  // ---- SLAB1 ---- Enter while a polygon slab is clicked closes it (lib.rs:2772, the viewport's
  // placing branch, not handle_keys: so not a row of Rust's shortcut table).
  if (e.key === "Enter" && !e.ctrlKey && !e.metaKey && !e.altKey && backend !== "rust" && !typingIn(e.target) && !currentDialog.value &&
    inView() && viewportKeys.enterTool?.()) {
    keyStats.handled.Enter = (keyStats.handled.Enter ?? 0) + 1;
    e.preventDefault();
    return;
  }
  // ---- end SLAB1 ----
  const r = rowOf(e);
  if (!r) return;
  const typing = typingIn(e.target);
  // The Rust wasm draws its own UI and binds its own keys on its canvas: only the palette and Esc here.
  const ours = backend !== "rust" || r.effect === "palette" || r.effect === "escape";
  let done = false;
  if (ours && (!typing || r.typing) && (!currentDialog.value || r.effect === "escape" || r.effect === "palette") && inContext(r.context))
    done = run(r, typing);
  if (done) keyStats.handled[r.keys] = (keyStats.handled[r.keys] ?? 0) + 1;
  else keyStats.ignored++;
  if (done || (r.swallow && !typing)) {
    e.preventDefault();
    // egui binds Ctrl K to its own palette when its canvas has focus (Rust wasm).
    if (r.effect === "palette") e.stopImmediatePropagation();
  }
}

let installed = false;
/** Installs the one listener (capture phase: before egui's canvas). Once; later calls do nothing. */
export function installKeys(): void {
  if (installed) return;
  installed = true;
  window.addEventListener("keydown", onKey, true);
  (window as unknown as { __bbKeys: unknown }).__bbKeys = { KEYS, ESCAPE: ESCAPE.map((l) => l.id), stats: keyStats, shortcutFor };
}
