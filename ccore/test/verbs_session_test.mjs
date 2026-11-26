// Verbs first through the session (ticket E1, ADR 0068 rules 1 and 3): node ccore/test/verbs_session_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a temporary copy of golden/frame.json and checks,
// over HTTP:
// - query verbs = golden/verbs_list.json and query describe {name} = golden/verbs_schema/<verb>.json
//   for each of the 14 (compacted, number tokens kept), describe of an unknown verb is a 400, and
//   describe rename (the C addition) answers its schema;
// - Rust's transcript golden/verbs_all.txt replayed step by step: each verb first with
//   "dry_run": true (its answer = the transcript's commands, new GlobalIds masked; rev, history and
//   element count unchanged), then for real (200 with an empty answer, or the transcript's 400
//   text); after each step the undo and redo labels (the viewer pushes no "nothing" step, so those
//   are left out of Rust's lists) and the element count; undo, redo and queries as written;
// - if_rev: the current rev runs, another rev is a 400 "conflict: ..." and changes nothing;
//   dry_run and if_rev of the wrong type are 400s; an old body (no extra keys) still works;
// - rename: a verb (one undo step, the element's name), a blank name refused, undo restores it.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, readFileSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const GOLDEN = join(CCORE, "golden");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("verbs_session", 47393);
const TOKEN = "verbs-session-test";
const BASE = `http://127.0.0.1:${PORT}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-verbs-session-"));
const MODEL = join(tmp, "frame.bimble");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};

// JSON text without the whitespace outside strings: number tokens stay as written (500.0).
function compact(t) {
  let o = "", inStr = false;
  for (let i = 0; i < t.length; i++) {
    const c = t[i];
    if (inStr) {
      o += c;
      if (c === "\\") o += t[++i];
      else if (c === '"') inStr = false;
    } else if (c === '"') { o += c; inStr = true; }
    else if (!" \t\n\r".includes(c)) o += c;
  }
  return o;
}

const call = async (op, name, body = "") => {
  const r = await fetch(`${BASE}/${op}/${encodeURIComponent(name)}?t=${TOKEN}`, { method: "POST", body, headers: { "Content-Type": "text/plain" } });
  return { status: r.status, body: await r.text() };
};
const q = async (name, args) => {
  const r = await call("query", name, args === undefined ? "" : JSON.stringify(args));
  if (r.status !== 200) throw new Error(`query ${name}: ${r.status} ${r.body}`);
  return JSON.parse(r.body);
};
const rev = async () => (await (await fetch(`${BASE}/change?since=0&t=${TOKEN}`)).json()).change.rev;
const history = async () => q("history");
const elements = async () => q("elements");

// ---- masking (golden/README.md "Masking of new GlobalIds") ----
// Known ids: frame.json's and every literal id of the transcript (the default views' ids are
// derived, not random, so Rust's transcript leaves them unmasked).
const known = new Set((readFileSync(join(GOLDEN, "frame.json"), "utf8") + readFileSync(join(GOLDEN, "verbs_all.txt"), "utf8")).match(/[0-9A-Za-z_$]+/g).filter((t) => t.length === 22));
const bound = new Map(); // real id -> "<new-N>"
const placeholderOf = new Map(); // "<new-N>" -> real id
const isNew = (t) => t.length === 22 && "0123".includes(t[0]) && !known.has(t) && t !== "0".repeat(22);
// A real answer: bound ids become their <new-N>, other new ids "<fresh>".
const maskReal = (s) => s.replace(/[0-9A-Za-z_$]+/g, (t) => (bound.has(t) ? bound.get(t) : isNew(t) ? "<fresh>" : t));
// A transcript line: <new-N> not bound yet becomes "<fresh>".
const maskGolden = (s) => s.replace(/<new-\d+>/g, (p) => (placeholderOf.has(p) ? p : "<fresh>"));
// Transcript args: bound <new-N> become the real ids.
const unmask = (s) => s.replace(/<new-\d+>/g, (p) => placeholderOf.get(p) ?? p);

function parseTranscript(text) {
  const steps = [];
  for (const block of text.split("\n\n")) {
    const lines = block.split("\n").filter((l) => l !== "");
    if (!lines.length || !lines[0].startsWith(">>> ")) continue;
    const head = lines[0].slice(4);
    const sp1 = head.indexOf(" "), sp2 = head.indexOf(" ", sp1 + 1);
    const step = { op: head.slice(0, sp1), name: head.slice(sp1 + 1, sp2), args: head.slice(sp2 + 1) };
    const ans = lines[1];
    step.status = Number(ans.slice(0, 3));
    step.answer = ans.slice(4);
    for (const l of lines.slice(2)) {
      if (l.startsWith("undo ")) step.undo = JSON.parse(l.slice(5));
      else if (l.startsWith("redo ")) step.redo = JSON.parse(l.slice(5));
      else if (l.startsWith("elements ")) step.elements = Number(l.slice(9));
    }
    steps.push(step);
  }
  return steps;
}

let host;
try {
  copyFileSync(join(GOLDEN, "frame.json"), MODEL);
  host = spawn(BIN, [String(PORT), MODEL], { stdio: ["ignore", "pipe", "pipe"], env: { ...process.env, BIMBLE_TOKEN: TOKEN } });
  host.out = "";
  host.stdout.on("data", (d) => (host.out += d));
  host.stderr.on("data", (d) => (host.out += d));
  for (let i = 0; i < 1000 && !host.out.includes("listening"); i++) {
    if (host.exitCode !== null) throw new Error(`host exited: ${host.out}`);
    await sleep(5);
  }
  if (!host.out.includes("listening")) throw new Error(`host not listening: ${host.out}`);

  // ---- verbs and describe ----
  const list = await call("query", "verbs", "");
  check("query verbs = golden/verbs_list.json", list.status === 200 && list.body === compact(readFileSync(join(GOLDEN, "verbs_list.json"), "utf8")), list.body.slice(0, 120));
  const names = JSON.parse(list.body).map((v) => v.name);
  check("14 verbs listed", names.length === 14, names.join(","));
  const schemaFiles = readdirSync(join(GOLDEN, "verbs_schema")).filter((f) => f.endsWith(".json"));
  let same = 0;
  for (const f of schemaFiles) {
    const r = await call("query", "describe", JSON.stringify({ name: f.slice(0, -5) }));
    if (r.status === 200 && r.body === compact(readFileSync(join(GOLDEN, "verbs_schema", f), "utf8"))) same++;
    else console.log(`     describe ${f}: ${r.status} ${r.body.slice(0, 100)}`);
  }
  check("query describe {name} = golden/verbs_schema/<verb>.json for every verb", same === 14 && schemaFiles.length === 14, `${same}/${schemaFiles.length}`);
  const unk = await call("query", "describe", JSON.stringify({ name: "fly" }));
  check("describe fly -> 400 no verb with this name", unk.status === 400 && unk.body === "no verb with this name", `${unk.status} ${unk.body}`);
  const dr = await call("query", "describe", JSON.stringify({ name: "rename" }));
  check("describe rename (C addition) -> its schema", dr.status === 200 && JSON.parse(dr.body).title === "RenameArgs" && JSON.parse(dr.body).required.join() === "id,name", dr.body.slice(0, 100));

  // ---- the transcript ----
  const steps = parseTranscript(readFileSync(join(GOLDEN, "verbs_all.txt"), "utf8"));
  let nverbs = 0, ndry = 0, nreal = 0, nhist = 0, nother = 0, bad = 0;
  const fail = (what) => { bad++; if (bad <= 10) console.log(`     ${what}`); };
  const seen = new Set();
  const noNothing = (l) => (l ?? []).filter((x) => x !== "nothing");
  for (const st of steps) {
    const args = unmask(st.args === "null" ? "" : st.args);
    const label = `${st.op} ${st.name} ${st.args.slice(0, 60)}`;
    if (st.op === "verb") {
      nverbs++;
      seen.add(st.name);
      // dry run: the transcript's answer, nothing changes
      const r0 = await rev(), h0 = JSON.stringify(await history()), n0 = (await elements()).length;
      const obj = JSON.parse(args || "{}");
      const dryBody = obj !== null && typeof obj === "object" && !Array.isArray(obj) ? JSON.stringify({ ...obj, dry_run: true }) : args;
      const d = await call("verb", st.name, dryBody);
      const want = maskGolden(st.answer), got = maskReal(d.body);
      if (d.status === st.status && got === want) ndry++;
      else fail(`dry ${label}: ${d.status} ${got.slice(0, 200)} != ${st.status} ${want.slice(0, 200)}`);
      if (r0 !== (await rev()) || h0 !== JSON.stringify(await history()) || n0 !== (await elements()).length) fail(`dry ${label}: changed the model`);
      // real run
      const before = new Set((await elements()).map((e) => e.id)), sheets0 = new Set((await q("sheets")).map((s) => s.id));
      const r = await call("verb", st.name, args);
      if (r.status === st.status && (st.status === 200 ? r.body === "" : r.body === st.answer)) nreal++;
      else fail(`real ${label}: ${r.status} ${r.body.slice(0, 200)}`);
      if (st.status === 200) { // bind the transcript's new ids to the real ones (by name, by number)
        const els = (await elements()).filter((e) => !before.has(e.id));
        const shs = (await q("sheets")).filter((s) => !sheets0.has(s.id));
        for (const c of JSON.parse(st.answer)) {
          if (typeof c.id !== "string" || !c.id.startsWith("<new-") || placeholderOf.has(c.id)) continue;
          const hit = c.op === "add_element" ? els.find((e) => e.name === c.name) : c.op === "add_sheet" ? shs.find((s) => s.number === c.number) : null;
          if (!hit) { fail(`real ${label}: no new object for ${c.id}`); continue; }
          placeholderOf.set(c.id, hit.id);
          bound.set(hit.id, c.id);
        }
      }
    } else if (st.op === "undo" || st.op === "redo") {
      const r = await call(st.op, "-", "");
      if (r.status === 200) nother++;
      else fail(`${label}: ${r.status} ${r.body}`);
    } else if (st.op === "query") {
      const r = await call("query", st.name, args);
      if (r.status === st.status && maskReal(r.body) === maskGolden(st.answer)) nother++;
      else fail(`${label}: ${r.status} ${maskReal(r.body).slice(0, 200)} != ${st.answer.slice(0, 200)}`);
    }
    const h = await history(), n = (await elements()).length;
    const hu = JSON.stringify(h.undo.map(maskReal)), wu = JSON.stringify(noNothing(st.undo).map(maskGolden));
    const hr = JSON.stringify(h.redo.map(maskReal)), wr = JSON.stringify(noNothing(st.redo).map(maskGolden));
    if (hu === wu && hr === wr && n === st.elements) nhist++;
    else fail(`after ${label}: undo ${hu.slice(0, 160)} != ${wu.slice(0, 160)}; redo ${hr.slice(0, 80)} != ${wr.slice(0, 80)}; elements ${n} != ${st.elements}`);
  }
  console.log(`     verbs_all.txt: ${steps.length} steps, ${nverbs} verb calls of ${seen.size} verbs`);
  check("transcript: every dry run answers Rust's commands, changes nothing", ndry === nverbs, `${ndry}/${nverbs}`);
  check("transcript: every real run answers as Rust's (200 empty or the 400 text)", nreal === nverbs, `${nreal}/${nverbs}`);
  check("transcript: undo, redo and queries as Rust's", nother === steps.length - nverbs, `${nother}/${steps.length - nverbs}`);
  check("transcript: history labels and element count after each step", nhist === steps.length, `${nhist}/${steps.length}`);
  check("transcript: no failure", bad === 0, `${bad}`);

  // the verbs of frame2's transcript not in this one (place, move, delete, set_generator, set_material, set_note)
  const col = "004z1Cm4z4hgNOg0ez4Id1";
  const before = new Set((await elements()).map((e) => e.id));
  const more = [
    ["place", { a: [1500.0, 1500.0], kind: "column", storey: "1uAZA0Yfn05f$xRd1k4uPR" }],
    ["move", { d: [10.0, 0.0, 0.0], ids: [col] }],
    ["set_generator", { id: col, params: { width: 350.0 } }],
    ["set_material", { ids: [col], material: "C35/45" }],
    ["set_note", { id: col, text: "checked" }],
  ];
  for (const [v, a] of more) {
    const d = await call("verb", v, JSON.stringify({ ...a, dry_run: true }));
    const r = await call("verb", v, JSON.stringify(a));
    check(`verb ${v}: dry run lists commands, real run 200`, d.status === 200 && JSON.parse(d.body).length > 0 && r.status === 200, `${d.status} ${d.body.slice(0, 80)} / ${r.status} ${r.body}`);
  }
  const placed = (await elements()).find((e) => !before.has(e.id));
  const del = await call("verb", "delete", JSON.stringify({ ids: [placed.id] }));
  check("verb delete: the placed column goes", del.status === 200 && !(await elements()).some((e) => e.id === placed.id), del.body);
  check("all 14 verbs reached over HTTP", new Set([...seen, ...more.map((m) => m[0]), "delete"]).size === 14, [...seen].join(","));

  // ---- if_rev ----
  const r1 = await rev(), n1 = (await elements()).length, u1 = (await history()).undo.length;
  const conflict = await call("verb", "move", JSON.stringify({ d: [5.0, 0.0, 0.0], ids: [col], if_rev: r1 + 7 }));
  check("if_rev mismatch -> 400 conflict, nothing changes", conflict.status === 400 && conflict.body === `conflict: the model changed (rev ${r1}, expected ${r1 + 7})` && (await rev()) === r1 && (await history()).undo.length === u1, `${conflict.status} ${conflict.body}`);
  const okRev = await call("verb", "move", JSON.stringify({ if_rev: r1, d: [5.0, 0.0, 0.0], ids: [col] }));
  check("if_rev = rev -> runs (one step, rev + 1)", okRev.status === 200 && (await history()).undo.length === u1 + 1 && (await rev()) === r1 + 1, `${okRev.status} ${okRev.body}`);
  const both = await call("verb", "move", JSON.stringify({ if_rev: r1 + 1, dry_run: true, d: [5.0, 0.0, 0.0], ids: [col] }));
  check("if_rev + dry_run -> the commands, no step", both.status === 200 && JSON.parse(both.body)[0].op === "set_placement" && (await history()).undo.length === u1 + 1, both.body.slice(0, 80));
  const badDry = await call("verb", "move", JSON.stringify({ dry_run: 1, d: [5.0, 0.0, 0.0], ids: [col] }));
  check("dry_run not a boolean -> 400", badDry.status === 400 && badDry.body === "bad arguments: dry_run: expected a boolean", badDry.body);
  const badRev = await call("verb", "move", JSON.stringify({ if_rev: -1, d: [5.0, 0.0, 0.0], ids: [col] }));
  check("if_rev negative -> 400", badRev.status === 400 && badRev.body === "bad arguments: if_rev: expected a non-negative integer", badRev.body);
  const extra = await call("verb", "move", JSON.stringify({ dry: true, d: [5.0, 0.0, 0.0], ids: [col] }));
  check("another unknown key is still refused by the verb", extra.status === 400 && extra.body.includes("unknown field `dry`"), extra.body);
  check("rev and elements kept by the refused calls", (await elements()).length === n1, "");

  // ---- rename (C addition) ----
  const nm0 = (await q("element", { id: col })).name;
  const u2 = (await history()).undo.length;
  const dryRen = await call("verb", "rename", JSON.stringify({ id: col, name: "P-renamed", dry_run: true }));
  check("rename dry run -> [rename]", dryRen.status === 200 && dryRen.body === `[{"op":"rename","id":"${col}","name":"P-renamed"}]`, dryRen.body);
  const ren = await call("verb", "rename", JSON.stringify({ id: col, name: "P-renamed" }));
  check("rename -> 200, the name, one step", ren.status === 200 && (await q("element", { id: col })).name === "P-renamed" && (await history()).undo.length === u2 + 1, `${ren.status} ${ren.body} ${(await history()).undo[0]}`);
  const blank = await call("verb", "rename", JSON.stringify({ id: col, name: "  " }));
  check("rename to a blank name -> 400, kept", blank.status === 400 && (await q("element", { id: col })).name === "P-renamed", `${blank.status} ${blank.body}`);
  const noEl = await call("verb", "rename", JSON.stringify({ id: "0000000000000000000000", name: "x" }));
  check("rename of no element -> 400", noEl.status === 400, noEl.body);
  const missing = await call("verb", "rename", JSON.stringify({ id: col }));
  check("rename without name -> 400 missing field", missing.status === 400 && missing.body.includes("missing field `name`"), missing.body);
  await call("undo", "-", "");
  check("undo restores the name", (await q("element", { id: col })).name === nm0, (await q("element", { id: col })).name);
} catch (e) {
  check("run", false, e.stack ?? String(e));
} finally {
  if (host && host.exitCode === null) {
    const gone = new Promise((r) => host.once("exit", r));
    host.kill("SIGTERM");
    await Promise.race([gone, sleep(3000)]);
  }
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
