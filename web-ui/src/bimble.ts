// The one typed client of the wasm viewer (ADR 0048). Components never touch the wasm directly.
import { shallowRef } from "vue";

export interface State {
  selection: string[];
  rev: number;
  unsaved: boolean;
  file: string | null;
}

/** `query("element")`: crates/bimble-api/src/query.rs ElementDetail (summary flattened). */
export interface ElementDetail {
  id: string;
  class: string;
  name: string;
  storey: string;
  material: string;
  generator: string;
  storey_name: string | null;
  material_name: string | null;
  placement: { origin: number[]; z_axis: number[]; x_axis: number[] };
  generator_params: Record<string, unknown>;
  openings: number;
  properties: Record<string, string>;
}

interface Wasm {
  default: () => Promise<unknown>;
  query(name: string, argsJson: string): string;
  apply(cmdsJson: string): void;
  undo(): void;
  redo(): void;
  on_change(cb: (json: string) => void): void;
}

export const state = shallowRef<State>({ selection: [], rev: 0, unsaved: false, file: null });
/** Set when the viewer could not start. */
export const startError = shallowRef<string | null>(null);

// Served as is from public/bimble/ (npm run wasm), not bundled.
const WASM = "/bimble/";
let wasm: Wasm | null = null;

function ready(): Wasm {
  if (!wasm) throw new Error("Bimble is not started");
  return wasm;
}

/** Load and start the viewer. The canvas #bimble must be in the DOM. */
export async function start(): Promise<void> {
  try {
    // The viewer reads its flags from the address before init(); other flags (?url=) stay.
    const url = new URL(location.href);
    if (!url.searchParams.has("viewport")) {
      url.searchParams.set("viewport", "1");
      history.replaceState(null, "", url);
    }
    try {
      const bridge = (await import(/* @vite-ignore */ `${WASM}manifold-bridge.js`)) as { loadManifold: () => Promise<void> };
      await bridge.loadManifold();
    } catch (e) {
      console.warn("manifold not loaded:", e);
    }
    const w = (await import(/* @vite-ignore */ `${WASM}bimble_web.js`)) as Wasm;
    await w.default();
    wasm = w;
    // The callback runs inside a frame: calls back into the viewer would throw "busy", so defer.
    w.on_change((json) => {
      const s = JSON.parse(json) as State;
      queueMicrotask(() => (state.value = s));
    });
  } catch (e) {
    startError.value = message(e);
    throw e;
  }
}

/** Rust errors arrive as strings (JsValue); keep their message. */
function message(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

export function element(id: string): ElementDetail {
  try {
    return JSON.parse(ready().query("element", JSON.stringify({ id }))) as ElementDetail;
  } catch (e) {
    throw new Error(message(e));
  }
}

/** Rename one element as one undoable step. Returns the Rust error, or null. */
export function rename(id: string, name: string): string | null {
  try {
    ready().apply(JSON.stringify([{ op: "rename", id, name }]));
    return null;
  } catch (e) {
    return message(e);
  }
}

export function undo(): string | null {
  try { ready().undo(); return null; } catch (e) { return message(e); }
}

export function redo(): string | null {
  try { ready().redo(); return null; } catch (e) { return message(e); }
}
