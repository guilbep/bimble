// The command palette and the view switches in the C session (ticket Q2, session/palette.h,
// calls.c): node ccore/test/palette_test.mjs. Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy
// of golden/frame.json and checks, over HTTP:
//   query palette_match {q, items}: Rust's unit tests with their expected orders
//     (crates/bimble-viewer/src/ribbon.rs:1802 palette_match_ranks_and_folds_accents, :1887
//     palette_matches_word_starts_and_parts), plus upper-case and padded queries and the ribbon's
//     own labels; bad arguments answer 400;
//   query recent_actions: newest first, each once, at most five (ribbon.rs:1822
//     recent_actions_order_and_dedup, on actions the C core runs); a refused action is not kept;
//   the view switches (ribbon.rs:591, :612, :626-628, :695-723): edges and hatching pressed by
//     default, edges, hatching, orthographic toggle their pressed state (no history step, rev
//     unchanged), fit and top answer ok; in a plan orthographic and top are off, edges and fit on;
//     without a model all five are off.
// Before Q2: palette_match answered "query palette_match is not in the C core", recent_actions []
// and the five switches were never enabled.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills what it starts.
import { spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs"; // FIXSES: never the caller's BIMBLE_CONFIG_DIR

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("palette_test");
const TOKEN = "palettetest-token";
const base = `http://127.0.0.1:${PORT}`;
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail && !ok ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const call = async (op, name = "-", body = "") => {
  const r = await fetch(`${base}/${op}/${encodeURIComponent(name)}?t=${TOKEN}`, { method: "POST", body, headers: { "Content-Type": "text/plain" } });
  return { status: r.status, text: await r.text() };
};
const q = (name, args) => call("query", name, args === undefined ? "" : JSON.stringify(args));
const actions = async () => Object.fromEntries(JSON.parse((await q("actions")).text).map((a) => [a.name, a]));
const SWITCHES = ["edges", "hatching", "orthographic", "fit", "top"];
const match = async (query, items) => {
  const r = await q("palette_match", { q: query, items });
  return r.status === 200 ? r.text : `${r.status} ${r.text}`;
};

const tmp = mkdtempSync(join(tmpdir(), "ccore-palette-"));
const model = join(tmp, "frame.json");
copyFileSync(join(CCORE, "golden/frame.json"), model);
const host = spawn(BIN, [String(PORT), model], { env: hostEnv({ BIMBLE_TOKEN: TOKEN }), stdio: ["ignore", "ignore", "pipe"], detached: true });
let log = "";
host.stderr.on("data", (d) => (log += d));
try {
  for (let i = 0; i < 400 && !log.includes("listening"); i++) await sleep(10);
  if (!log.includes("listening")) throw new Error(`host did not start: ${log}`);

  // ---- palette_match: ribbon.rs:1802, the expected orders as Rust's test writes them ----
  const items = ["Zone de coupe Section box", "Élévation Elevation", "Coupe Section", "Enregistrer Save"];
  for (const [query, want] of [["sec", "[0,2]"], ["sec box", "[0]"], ["elev", "[1]"], ["élé", "[1]"], ["", "[0,1,2,3]"]])
    check(`palette_match ${JSON.stringify(query)} -> ${want}`, (await match(query, items)) === want, await match(query, items));
  check("matches(\"Élévation\", \"elevation\")", (await match("elevation", ["Élévation"])) === "[0]");
  // ribbon.rs:1887 palette_matches_word_starts_and_parts
  for (const [label, query, want] of [["Section box", "sec box", true], ["Sheet: dimension", "dim", true], ["Place: Column", "col", true],
    ["Anything", "", true], ["Section box", "beam", false], ["Place: Column", "col wall", false]])
    check(`matches(${JSON.stringify(label)}, ${JSON.stringify(query)}) = ${want}`, (await match(query, [label])) === (want ? "[0]" : "[]"));
  // Case, accents and whitespace (fold, trim, split_whitespace); a part of a word (contains).
  check("upper case and accents: ÉLÉV finds Élévation", (await match("ÉLÉV", items)) === "[1]", await match("ÉLÉV", items));
  check("padded query: \"  sec\\t\" = \"sec\"", (await match("  sec\t", items)) === "[0,2]", await match("  sec\t", items));
  check("a part inside a word: \"ction\" -> [0,2]", (await match("ction", items)) === "[0,2]", await match("ction", items));
  check("words in any order: \"box zone\" -> [0]", (await match("box zone", items)) === "[0]", await match("box zone", items));
  check("ç and ñ fold: \"facade ano\" finds \"Façade Año\"", (await match("facade ano", ["Façade Año", "Other"])) === "[0]");
  check("the ellipsis separates words: \"ifc open\" finds \"Link IFC…Open\"", (await match("ifc open", ["Link IFC…Open"])) === "[0]", await match("ifc open", ["Link IFC…Open"]));
  const labels = JSON.parse((await q("actions")).text).map((a) => a.label);
  const sb = await match("sec box", labels);
  // ISS71: + the orientation's two actions (Section box: project north, true north), after Rust's 54.
  check("the ribbon's labels: \"sec box\" -> Section box, Section box to selection, Section box: project / true north", sb === JSON.stringify([labels.indexOf("Section box"), labels.indexOf("Section box to selection"),
    labels.indexOf("Section box: project north"), labels.indexOf("Section box: true north")]), sb);
  for (const [body, why] of [[{ items: [] }, "missing field `q`"], [{ q: "x" }, "missing field `items`"], [{ q: "x", items: [1] }, "items: invalid type"]]) {
    const r = await q("palette_match", body);
    check(`palette_match ${JSON.stringify(body)}: 400 ${why}`, r.status === 400 && r.text.startsWith(why), `${r.status} ${r.text}`);
  }

  // ---- recent_actions: ribbon.rs:1822, on actions the C core runs ----
  const ch0 = await (await fetch(`${base}/change?since=0&t=${TOKEN}`)).json();
  check("recent_actions: [] before any action", (await q("recent_actions")).text === "[]");
  for (const n of ["measure", "reveal_hidden", "measure", "section_box", "home", "box_to_selection"]) {
    const r = await call("action", n);
    check(`action ${n}: 200`, r.status === 200, r.text);
  }
  const rec = (await q("recent_actions")).text;
  check("recent_actions: newest first, each once", rec === '["box_to_selection","home","section_box","measure","reveal_hidden"]', rec);
  const ru = await call("action", "undo");
  check("undo with no history: refused", ru.status === 400 && ru.text === 'action "undo" is not enabled now', `${ru.status} ${ru.text}`);
  check("a refused action is not kept", (await q("recent_actions")).text === rec, (await q("recent_actions")).text);
  // ---- the view switches (ribbon.rs:591, :612, :626-628, :695-723) ----
  const a0 = await actions();
  check("with a model: edges, hatching, orthographic, fit, top enabled", SWITCHES.every((n) => a0[n].enabled), JSON.stringify(SWITCHES.map((n) => a0[n])));
  check("defaults: edges and hatching pressed, orthographic not", a0.edges.pressed && a0.hatching.pressed && !a0.orthographic.pressed && !a0.fit.pressed && !a0.top.pressed);
  for (const n of ["edges", "hatching", "edges", "orthographic", "fit", "top"]) {
    const r = await call("action", n);
    check(`action ${n}: 200`, r.status === 200, r.text);
  }
  const a1 = await actions();
  check("after edges twice, hatching and orthographic once: edges on, hatching off, orthographic on",
    a1.edges.pressed && !a1.hatching.pressed && a1.orthographic.pressed, JSON.stringify([a1.edges, a1.hatching, a1.orthographic]));
  // ribbon.rs:1822: the last five; the oldest ones are dropped.
  const rec2 = (await q("recent_actions")).text;
  check("recent_actions: the last five, newest first, each once", rec2 === '["top","fit","orthographic","edges","hatching"]', rec2);
  const ch1 = await (await fetch(`${base}/change?since=0&t=${TOKEN}`)).json();
  check("switches: no history step, rev unchanged", ch1.change?.rev === ch0.change?.rev && JSON.parse((await q("history")).text).undo.length === 0, JSON.stringify([ch0.change?.rev, ch1.change?.rev]));
  const ov = JSON.parse((await q("open_views")).text);
  const plan = ov.views.find((v) => v.type === "plan");
  const t = await call("tab", "-", JSON.stringify({ op: "open", tab: { type: "view", id: plan.id } }));
  check("open a plan tab", t.status === 200, t.text);
  const a2 = await actions();
  check("in a plan: orthographic and top disabled, edges, hatching and fit enabled",
    !a2.orthographic.enabled && !a2.top.enabled && a2.edges.enabled && a2.hatching.enabled && a2.fit.enabled, JSON.stringify(SWITCHES.map((n) => a2[n].enabled)));
  const rt = await call("action", "top");
  check("in a plan: action top refused", rt.status === 400 && rt.text === 'action "top" is not enabled now', `${rt.status} ${rt.text}`);
  check("in a plan: orthographic keeps its pressed state", a2.orthographic.pressed === true);
  const rec3 = (await q("recent_actions")).text;

  // ---- nothing open ----
  await call("action", "close", JSON.stringify({ discard: true }));
  check("close (not a ribbon action) is not kept in recent_actions", (await q("recent_actions")).text === rec3, (await q("recent_actions")).text);
  const a3 = await actions();
  check("no model: the five switches disabled", SWITCHES.every((n) => !a3[n].enabled), JSON.stringify(SWITCHES.map((n) => a3[n].enabled)));
} catch (e) {
  check("no exception", false, e.stack ?? String(e));
} finally {
  try { process.kill(-host.pid, "SIGKILL"); } catch {}
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`palette_test: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
