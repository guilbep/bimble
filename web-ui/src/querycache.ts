// SMALL1: one edit, one round trip. After an edit the page read 28 answers of the core one call at a
// time (a footing click: 28 synchronous calls, 74.5 ms of the click's 213 ms on the native host). This
// cache sits in front of the client's `query`: within one generation (between two changes of the core
// or two calls of the page that may change it) a query asked again is answered from the cache, and on
// a change the page asks in ONE call (`query batch`, ccore/session/calls.c) for what it read in the
// last generation: what is on screen. A panel closed since is asked once more, then no longer; a
// panel opened asks for its own data when it opens (a miss), and is in the next batch.
// No DOM and no Vue here: ccore/test/refresh_test.mjs runs it in node against the host.

/** The queries `query batch` answers (calls.c BATCH_NAMES, kept in step): reads of the model and the session. */
export const BATCHABLE = new Set([
  "actions", "annotation_types", "collab", "coordinates", "element", "elements", "foundation_rows",
  "grid_axes", "grids", "history", "level_refusals", "links", "materials", "measure", "measurements",
  "north", "open_views", "opening_rows", "openings", "placing", "project", "reference_planes",
  "schedule_list", "schedules", "setting_out", "sheets", "state", "status", "storeys", "view_annotations",
  "view_clip", "view_references", "view_visibility", "views", "visibility", "visibility_sets",
]);
/** At most this many queries in one batch (calls.c CC_BATCH_MAX). */
export const BATCH_MAX = 32;
/** Not kept: `coordinates` is asked per pointer position. */
const UNCACHED = new Set(["coordinates"]);
/** Answered from a batch once, then asked again (the 1 s status poll reads the scene's progress). */
const ONCE = new Set(["status"]);
/** Answers that depend on the model only (rev, file, links): kept across a change that leaves those. */
const MODEL_ONLY = new Set(["storeys", "materials", "project", "north", "sheets", "schedule_list", "grids", "grid_axes", "views", "element", "elements", "openings"]);
/** The page's refresh reads these after every change (bimble.ts `refresh`). */
export const ALWAYS = ["state", "actions", "history", "visibility", "open_views", "status", "placing", "setting_out", "reference_planes"];

type Answer = { ok: string } | { error: string };
/** The part of the change JSON that names the model (rev, file, the linked models' counter). */
export interface ChangeKey { rev: number; file: string | null; links?: number }

/** The top-level items of a JSON array, as text (strings and nesting respected). */
function items(text: string): string[] {
  const out: string[] = [];
  let depth = 0, start = 0, str = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (str) { if (c === "\\") i++; else if (c === '"') str = false; continue; }
    if (c === '"') str = true;
    else if (c === "[" || c === "{") { if (depth++ === 1) start = i; }
    else if (c === "]" || c === "}") { if (--depth === 1) out.push(text.slice(start, i + 1)); }
  }
  return out;
}

export class QueryCache {
  /** Calls of the core made through `ask` (a test counts them; a batch is one). */
  calls = 0;
  private answers = new Map<string, Answer>();
  private asked = new Map<string, [string, string]>();
  /** Asked since the last change of the model: the panels read again only when rev moves (computed on rev). */
  private sinceModel = new Map<string, [string, string]>();
  private batchOk = true;
  private model = "";
  private ask: (name: string, args: string) => string;
  constructor(ask: (name: string, args: string) => string) { this.ask = ask; }

  /** The client's query: from the cache when this generation already has the answer. Throws the error text. */
  query(name: string, args: string): string {
    if (!BATCHABLE.has(name) || UNCACHED.has(name)) return this.raw(name, args);
    const k = `${name}\n${args}`;
    this.asked.set(k, [name, args]);
    this.sinceModel.set(k, [name, args]);
    let a = this.answers.get(k);
    if (a && ONCE.has(name)) this.answers.delete(k);
    if (!a) {
      try { a = { ok: this.raw(name, args) }; } catch (e) { a = { error: e instanceof Error ? e.message : String(e) }; }
      if (!ONCE.has(name)) this.answers.set(k, a);
    }
    if ("error" in a) throw a.error;
    return a.ok;
  }

  /** The page made a call that may change the core: nothing kept (`modelToo` false: select, tab,
   *  set_placing, a pointer move of a tool: the model's answers stay). */
  mutated(modelToo = true): void {
    if (modelToo) { this.answers.clear(); return; }
    for (const k of this.answers.keys()) if (!MODEL_ONLY.has(k.slice(0, k.indexOf("\n")))) this.answers.delete(k);
  }

  /** A change of the core: the session's answers go (the model's too when rev, file or links
   *  moved), then one batch asks for `need`, what the page read since the last change and, when the
   *  model moved, what it read since the model's last change (a panel reads again on a new rev only). */
  changed(c: ChangeKey, need: readonly string[] = ALWAYS): void {
    const model = JSON.stringify([c.rev, c.file, c.links ?? 0]);
    const moved = model !== this.model;
    this.mutated(moved);
    this.model = model;
    const want = new Map<string, [string, string]>();
    for (const n of need) want.set(`${n}\n{}`, [n, "{}"]);
    for (const [k, v] of this.asked) want.set(k, v);
    if (moved) for (const [k, v] of this.sinceModel) want.set(k, v);
    this.asked = new Map();
    if (moved) this.sinceModel = new Map();
    const list = [...want].filter(([k]) => !this.answers.has(k)).slice(0, BATCH_MAX);
    if (!this.batchOk || list.length < 2) return;
    let text: string;
    try {
      text = this.raw("batch", JSON.stringify({ queries: list.map(([, [n, a]]) => (a === "{}" ? n : { name: n, args: a })) }));
    } catch {
      this.batchOk = false; // a core without `query batch` (the Rust viewer): one call per query, as before
      return;
    }
    const parts = items(text);
    if (parts.length !== list.length) return;
    parts.forEach((p, i) => {
      const a: Answer = p.startsWith('{"ok":') ? { ok: p.slice(6, -1) } : { error: String((JSON.parse(p) as { error: unknown }).error) };
      this.answers.set(list[i]![0], a);
    });
  }

  private raw(name: string, args: string): string {
    this.calls++;
    return this.ask(name, args);
  }
}

/** Calls of the page that cannot change the model (the model's answers stay). */
const SESSION_ONLY = new Set(["select", "tab", "set_placing", "dismiss_belongs", "standards_action"]);
/** The client with its `query` through the cache, and every other call telling the cache it may have
 *  changed the core (`default`, `on_change` and `bytes` — a PDF — change nothing and pass as they are). */
export function cachedClient<T extends object>(w: T, c: QueryCache): T {
  const out: Record<string, unknown> = {};
  for (const [k, f] of Object.entries(w)) {
    if (typeof f !== "function") out[k] = f;
    else if (k === "query") out[k] = (n: string, a: string) => c.query(n, a);
    else if (k === "default" || k === "on_change" || k === "bytes") out[k] = f.bind(w);
    else {
      out[k] = (...args: unknown[]) => {
        try {
          return f.apply(w, args);
        } finally {
          c.mutated(!(SESSION_ONLY.has(k) || (k === "tool" && /pointer$|^box_move$/.test(String(args[0])))));
        }
      };
    }
  }
  return out as T;
}
