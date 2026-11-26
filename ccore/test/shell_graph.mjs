// The lab shell's state machine as a graph (ticket UX1): node ccore/test/shell_graph.mjs
// Imports web-ui/src/shell/machine.ts as it is (Node strips the types; the module has no import)
// and walks TRANSITIONS from the boot states with a session that can open the sample model. A node
// is (shell state, what the session says: file, unsaved, current tab). Checks:
//   every state kind is reachable; no node is a sink; from every node there is a path to Home and a
//   path to the model's view; no node shows a model state without a model, or the view without a
//   tab after the session said so; every control (machine.ts CONTROLS, the list the UI shows its
//   controls from) has a row in every node where it is shown; every event has a row somewhere.
// Prints the tables' sizes, "N passed, M failed"; exits non-zero on a failure.
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const M = await import(pathToFileURL(join(here, "../../web-ui/src/shell/machine.ts")).href);
const { TRANSITIONS, KINDS, EVENTS, CONTROLS, kind, step, derive, VIEW } = M;

let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};

const EMPTY = { file: null, unsaved: false, tab: false };
const LOADED = { file: "sample.bimble", unsaved: false, tab: true };
const key = (s, c) => JSON.stringify([s, c]);

// What the session does around an event: the core the event is read with, and the core after it.
// User events run with the session as it is; their calls (simulated) change it. Results and outside
// changes (an agent, another window, the user closing a tab) change the session first.
function moves(s, core) {
  const out = [];
  const user = [{ do: "new" }, { do: "open" }, { do: "open", path: "/x/sample.bimble" }, { do: "import" }, { do: "close" },
    { do: "discard" }, { do: "cancel" }, { do: "showView" }, { do: "showPage", page: "quantities" },
    { do: "showSheet", sheet: "s1" }, { do: "back" }, { do: "home" }, { do: "retry" }];
  for (const e of user) {
    const to = step(s, e, core);
    if (to === null) continue;
    let after = core;
    // send.ts: showView or back to the view with a model and no tab opens a view.
    if ((e.do === "showView" || e.do === "back") && to.at === "model" && to.show === "view" && core.file !== null) after = { ...core, tab: true };
    out.push([e, to, after]);
  }
  const outside = [
    [{ do: "opened" }, LOADED], [{ do: "closed" }, EMPTY], [{ do: "failed", message: "refused" }, core],
    [{ do: "core" }, core],
    [{ do: "core" }, EMPTY], // the model closed by an agent
    // FIXUI: the host stops answering; it comes back with the same session, or another (a fresh
    // host with the sample open, or none).
    [{ do: "lost" }, core], [{ do: "reconnected", same: true }, core],
    [{ do: "reconnected", same: false }, LOADED], [{ do: "reconnected", same: false }, EMPTY],
    ...(core.file !== null ? [[{ do: "core" }, { ...core, tab: false }], [{ do: "core" }, { ...core, unsaved: true }], [{ do: "core" }, { ...core, unsaved: false }]] : []),
    ...(core.file !== null && !core.tab ? [[{ do: "core" }, { ...core, tab: true }]] : []),
  ];
  for (const [e, c] of outside) {
    const to = step(s, e, c);
    if (to !== null) out.push([e, to, c]);
  }
  return out;
}

// Breadth-first from the boot states: the shell starts on the view, the session's first change follows.
const nodes = new Map(); // key -> { s, core, edges: [key, event] }
const queue = [];
const add = (s, core) => { const k = key(s, core); if (!nodes.has(k)) { nodes.set(k, { s, core, edges: [] }); queue.push(k); } return k; };
for (const c of [EMPTY, LOADED]) add(step(VIEW, { do: "core" }, c), c);
while (queue.length) {
  const k = queue.shift();
  const n = nodes.get(k);
  for (const [e, to, after] of moves(n.s, n.core)) n.edges.push([add(to, after), e]);
  if (nodes.size > 5000) throw new Error("the graph does not close: more than 5000 nodes");
}

const rows = Object.values(TRANSITIONS).reduce((a, r) => a + Object.keys(r).length, 0);
console.log(`machine: ${KINDS.length} state kinds, ${EVENTS.length} events, ${rows} rows, ${CONTROLS.length} controls; graph: ${nodes.size} nodes, ${[...nodes.values()].reduce((a, n) => a + n.edges.length, 0)} edges`);
for (const kd of KINDS) console.log(`  ${kd.padEnd(13)} ${Object.keys(TRANSITIONS[kd]).join(" ")}`);

const reached = new Set([...nodes.values()].map((n) => kind(n.s)));
check("every state kind is reachable from boot", KINDS.every((k) => reached.has(k)), KINDS.filter((k) => !reached.has(k)).join(", "));

const sinks = [...nodes.entries()].filter(([k, n]) => !n.edges.some(([to]) => to !== k));
check("no node is a sink (each has a way out)", sinks.length === 0, sinks.slice(0, 3).map(([k]) => k).join(" | "));

// Reverse reachability: which nodes reach a target.
function reaching(pred) {
  const back = new Map();
  for (const [k, n] of nodes) for (const [to] of n.edges) { if (!back.has(to)) back.set(to, []); back.get(to).push(k); }
  const ok = new Set([...nodes.entries()].filter(([, n]) => pred(n)).map(([k]) => k));
  const q = [...ok];
  while (q.length) for (const from of back.get(q.shift()) ?? []) if (!ok.has(from)) { ok.add(from); q.push(from); }
  return ok;
}
const toHome = reaching((n) => n.s.at === "home");
const noHome = [...nodes.keys()].filter((k) => !toHome.has(k));
check("every node has a path to Home", noHome.length === 0, noHome.slice(0, 3).join(" | "));
const toView = reaching((n) => kind(n.s) === "model.view" && n.core.file !== null);
const noView = [...nodes.keys()].filter((k) => !toView.has(k));
check("every node has a path to the model's view (a session that opens the sample)", noView.length === 0, noView.slice(0, 3).join(" | "));

const modelless = [...nodes.values()].filter((n) => n.s.at === "model" && n.core.file === null);
check("no model state without a model", modelless.length === 0, modelless.slice(0, 3).map((n) => key(n.s, n.core)).join(" | "));
// After the session spoke (`core`), the shell agrees with it.
const disagree = [];
for (const n of nodes.values()) for (const c of [EMPTY, LOADED, { ...LOADED, tab: false }]) {
  const to = step(n.s, { do: "core" }, c);
  if (to && to.at === "model" && (c.file === null || (to.show === "view" && !c.tab))) disagree.push(key(n.s, c));
  if (to && derive(to, c) !== to) disagree.push(key(n.s, c));
}
check("after `core`, a model state has a model and the view a tab", disagree.length === 0, disagree.slice(0, 3).join(" | "));

const dropped = [];
for (const n of nodes.values()) for (const ctl of CONTROLS) {
  if (ctl.shown(n.s, n.core) && step(n.s, ctl.event, n.core) === null) dropped.push(`${ctl.id} in ${key(n.s, n.core)}`);
}
check(`every shown control has a row (${CONTROLS.length} controls x ${nodes.size} nodes)`, dropped.length === 0, dropped.slice(0, 5).join(" | "));
const shownSomewhere = CONTROLS.filter((c) => [...nodes.values()].some((n) => c.shown(n.s, n.core)));
check("every control is shown in some reachable node", shownSomewhere.length === CONTROLS.length,
  CONTROLS.filter((c) => !shownSomewhere.includes(c)).map((c) => c.id).join(", "));
const unused = EVENTS.filter((e) => !KINDS.some((k) => TRANSITIONS[k][e]));
check("every event has a row in some state", unused.length === 0, unused.join(", "));

// A few rows by name (the machine's promises, read in the UI's words).
const at = (s, e, c) => (s === null ? null : step(s, e, c));
const kindOf = (s) => (s === null ? null : kind(s));
const page = { at: "model", show: "page", page: "quantities" };
check("Home -> New with unsaved changes asks first", at({ at: "home" }, { do: "new" }, { ...LOADED, unsaved: true })?.at === "confirmClose");
check("a page -> back is the view", kindOf(at(page, { do: "back" }, LOADED)) === "model.view");
check("the model closed under a page -> Home", at(page, { do: "core" }, EMPTY)?.at === "home");
check("a failed open -> the error; back -> where it came from",
  kindOf(at(at({ at: "opening", what: "open", discard: false, back: page }, { do: "failed", message: "x" }, LOADED), { do: "back" }, LOADED)) === "model.page");
check("a page without a model is refused", at({ at: "home" }, { do: "showPage", page: "quantities" }, EMPTY) === null);

// ---- FIXUI: expectations written by hand, not derived from the machine's own step/CONTROLS: a
// row removed, added or sent elsewhere fails here (the audit's mutations: opening.closed,
// MODEL.failed, error.home, home.failed, confirmClose.cancel -> HOME all survived the checks above).
const WANT_KINDS = ["home", "opening", "model.view", "model.page", "model.sheet", "error", "confirmClose", "disconnected", "sessionLost"];
check("the reachable state kinds are exactly the expected nine", JSON.stringify([...reached].sort()) === JSON.stringify([...WANT_KINDS].sort()) &&
  JSON.stringify([...KINDS].sort()) === JSON.stringify([...WANT_KINDS].sort()), [...reached].join(", "));
const MODEL_ROWS = "new open import close opened closed failed showView showPage showSheet home core lost";
const WANT_ROWS = {
  home: MODEL_ROWS,
  opening: "opened closed failed cancel core lost",
  "model.view": MODEL_ROWS,
  "model.page": `${MODEL_ROWS} back`,
  "model.sheet": `${MODEL_ROWS} back`,
  error: "back home opened closed failed core lost",
  confirmClose: "discard cancel opened closed failed core lost",
  disconnected: "retry lost failed opened closed core reconnected",
  sessionLost: "home new open import showView opened closed core failed lost",
};
for (const kd of WANT_KINDS) {
  const got = Object.keys(TRANSITIONS[kd] ?? {}).sort().join(" "), want = WANT_ROWS[kd].split(" ").sort().join(" ");
  check(`${kd}: its rows are exactly the expected events`, got === want, `got ${got}; want ${want}`);
}
const DIRTY = { ...LOADED, unsaved: true };
const sheetS = { at: "model", show: "sheet", sheet: "s1" };
const opening = (back, what = "open") => ({ at: "opening", what, discard: false, back });
const err = { at: "error", message: "first", back: page };
const ask = { at: "confirmClose", then: "open", back: page };
const gone = { at: "disconnected", file: "sample.bimble", unsaved: true, back: page };
const lostS = { at: "sessionLost", file: "sample.bimble", unsaved: true };
// [name, state, event, core, expected kind (null: refused), extra predicate on the result]
const CASES = [
  ["Home -> New, no model: the call", { at: "home" }, { do: "new" }, EMPTY, "opening"],
  ["Home -> a failed action: the error", { at: "home" }, { do: "failed", message: "x" }, EMPTY, "error", (r) => r.back.at === "home"],
  ["Home -> Close with no model: refused", { at: "home" }, { do: "close" }, EMPTY, null],
  ["Home -> Close with a model: the call", { at: "home" }, { do: "close" }, LOADED, "opening", (r) => r.what === "close"],
  ["opening a close -> closed: Home", opening(VIEW, "close"), { do: "closed" }, EMPTY, "home"],
  ["opening -> opened: the view", opening({ at: "home" }), { do: "opened" }, LOADED, "model.view"],
  ["opening -> cancel: back where it came from", opening(page), { do: "cancel" }, LOADED, "model.page"],
  ["the view -> failed: the error, back to the view", VIEW, { do: "failed", message: "x" }, LOADED, "error", (r) => kind(r.back) === "model.view"],
  ["a page -> failed: the error, back to the page", page, { do: "failed", message: "x" }, LOADED, "error", (r) => kind(r.back) === "model.page"],
  ["a sheet -> failed: the error, back to the sheet", sheetS, { do: "failed", message: "x" }, LOADED, "error", (r) => kind(r.back) === "model.sheet"],
  ["a sheet -> back: the view", sheetS, { do: "back" }, LOADED, "model.view"],
  ["the view -> Home", VIEW, { do: "home" }, LOADED, "home"],
  ["the error -> Home", err, { do: "home" }, LOADED, "home"],
  ["the error -> back: the page it came from", err, { do: "back" }, LOADED, "model.page"],
  ["the error -> a second error: the newer message, the same way back", err, { do: "failed", message: "second" }, LOADED, "error",
    (r) => r.message === "second" && kind(r.back) === "model.page"],
  ["the question -> cancel: the page it came from (not Home)", ask, { do: "cancel" }, DIRTY, "model.page"],
  ["the question -> discard: the call, discarding", ask, { do: "discard" }, DIRTY, "opening", (r) => r.discard === true],
  ["the question -> an error: shown", ask, { do: "failed", message: "x" }, DIRTY, "error"],
  ["the view -> the host lost: blocking, what was known kept", VIEW, { do: "lost" }, DIRTY, "disconnected",
    (r) => r.file === "sample.bimble" && r.unsaved === true && kind(r.back) === "model.view"],
  ["a page -> the host lost", page, { do: "lost" }, LOADED, "disconnected"],
  ["the host lost -> Retry: still disconnected", gone, { do: "retry" }, LOADED, "disconnected"],
  ["the host lost -> Home: refused (no silent jump)", gone, { do: "home" }, LOADED, null],
  ["the host lost -> the session's change: no move", gone, { do: "core" }, EMPTY, "disconnected"],
  ["the host back, same session: the page it showed", gone, { do: "reconnected", same: true }, LOADED, "model.page"],
  ["the host back, another session: said so", gone, { do: "reconnected", same: false }, EMPTY, "sessionLost",
    (r) => r.file === "sample.bimble" && r.unsaved === true],
  ["session lost -> its change: no silent Home", lostS, { do: "core" }, EMPTY, "sessionLost"],
  ["session lost -> Home", lostS, { do: "home" }, EMPTY, "home"],
  ["session lost -> reopen the file", lostS, { do: "open", path: "/x/sample.bimble" }, EMPTY, "opening", (r) => r.path === "/x/sample.bimble"],
  ["session lost -> show what the host has open", lostS, { do: "showView" }, LOADED, "model.view"],
];
for (const [name, st, e, c, want, pred] of CASES) {
  const r = step(st, e, c);
  check(name, (r === null ? null : kind(r)) === want && (!pred || (r !== null && pred(r))), JSON.stringify(r));
}
// The controls: the File menu's Close and the notices' buttons, where a hand list says.
const WANT_CONTROLS = ["file-menu-close", "shell-disconnected-retry", "shell-lost-home", "shell-lost-reopen", "shell-lost-show"];
check("the connection notices' and File -> Close's controls exist", WANT_CONTROLS.every((id) => CONTROLS.some((x) => x.id === id)));
const ctl = (id) => CONTROLS.find((x) => x.id === id);
check("File -> Close shows with a model, not without", ctl("file-menu-close")?.shown(VIEW, LOADED) === true && ctl("file-menu-close")?.shown({ at: "home" }, EMPTY) === false);
check("Retry shows only while disconnected", ctl("shell-disconnected-retry")?.shown(gone, LOADED) === true && ctl("shell-disconnected-retry")?.shown(VIEW, LOADED) === false);

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
