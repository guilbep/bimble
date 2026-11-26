// The history queries of the C session (ticket H1, session/versions.h): node ccore/test/versions_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on a copy of the history folder Rust wrote
// (golden/history/frame.bimble-history, ccore_golden's history mode) with its HEAD model
// (golden/history/model_at_6.json as frame.bimble) and checks, over HTTP:
//   query versions: HEAD on main, the five first-parent commits of golden/history/log.txt (hash,
//     parents, author, time, message); {rev: "variant", limit: 2}; an unknown revision is 400 with
//     Rust's text;
//   query branches: golden/history/branches.txt;
//   query version_changes: {a, b} and {a} (b is HEAD), the changes of diff_1_2.txt and diff_4_6.txt
//     in serde's form;
//   query pending: nothing on load (the model is HEAD's), one change after a verb, none after undo;
//   a model with no history folder: {"history": false, "reason": Rust's "no history next to ..."};
//   H2a, the writes (action commit, branch_create, branch_switch; BIMBLE_CONFIG_DIR a temp folder):
//     no name in the settings: refused; nothing to commit: Rust's text; an edit, then commit: the
//     version first in query versions with the settings' author, pending clean, saved; a new
//     branch, a bad branch name; switch refused with changes not committed, then a commit on the
//     branch, switch back: the model is main's (pending clean, project field back); discard;
//     a model without a history: commit starts one; capabilities history_write true;
//   a damaged object (one byte flipped): 400 with Rust's "damaged history: ... does not match its hash";
//   the wasm modules (out/wasm/ccore.wasm, ccore-view.wasm when built): the same four queries on a
//   model answer {"history": false, "reason": "no history folder in the browser"}; action commit is
//   400 with that reason and capabilities history_write false (H2a).
// Prints "N passed, M failed"; exits non-zero on a failure. Kills what it starts.
import { spawn } from "node:child_process";
import { copyFileSync, cpSync, existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { gunzipSync } from "node:zlib";
import { port } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const GOLD = join(CCORE, "golden/history");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("versions_test");
const TOKEN = "versionstest-token";
const base = `http://127.0.0.1:${PORT}`;
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail && !ok ? `: ${detail}` : ""}`);
};
const eq = (name, got, want) => check(name, JSON.stringify(got) === JSON.stringify(want), `got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const call = async (op, name = "-", body = "") => {
  const r = await fetch(`${base}/${op}/${encodeURIComponent(name)}?t=${TOKEN}`, { method: "POST", body, headers: { "Content-Type": "text/plain" } });
  return { status: r.status, text: await r.text() };
};
const q = async (name, args) => {
  const r = await call("query", name, args === undefined ? "" : JSON.stringify(args));
  return { status: r.status, text: r.text, json: r.status === 200 ? JSON.parse(r.text) : null };
};

// golden/history/log.txt: the "log HEAD" block, `<hash> <time> <author> <parents|-> <message>`.
const logTxt = readFileSync(join(GOLD, "log.txt"), "utf8").split("\n");
const headLog = [];
for (let i = logTxt.indexOf("log HEAD") + 1; i < logTxt.length && !logTxt[i].startsWith("log "); i++) {
  const m = /^(\S+) (\S+) (\S+) (\S+) (.*)$/.exec(logTxt[i]);
  if (m) headLog.push({ hash: m[1], parents: m[4] === "-" ? [] : m[4].split(","), author: m[3], time: Number(m[2]), message: m[5] });
}
const branchesTxt = readFileSync(join(GOLD, "branches.txt"), "utf8").trim().split("\n").slice(1)
  .map((l) => { const [name, commit] = l.split(" "); return { name, commit }; });
const [c6, c4, c3, c2, c1] = headLog.map((v) => v.hash);

const tmp = mkdtempSync(join(tmpdir(), "ccore-versions-"));
const model = join(tmp, "frame.bimble");
copyFileSync(join(GOLD, "model_at_6.json"), model);
cpSync(join(GOLD, "frame.bimble-history"), join(tmp, "frame.bimble-history"), { recursive: true });
const plain = join(tmp, "plain.bimble");
copyFileSync(join(CCORE, "golden/frame.json"), plain);
const config = join(tmp, "config"); // H2a: never the user's settings
const host = spawn(BIN, [String(PORT), model], { env: { ...process.env, BIMBLE_TOKEN: TOKEN, BIMBLE_CONFIG_DIR: config }, stdio: ["ignore", "ignore", "pipe"], detached: true });
let log = "";
host.stderr.on("data", (d) => (log += d));
try {
  for (let i = 0; i < 400 && !log.includes("listening"); i++) await sleep(10);
  if (!log.includes("listening")) throw new Error(`host did not start: ${log}`);

  let r = await q("versions");
  check("versions 200", r.status === 200, r.text);
  eq("versions head", r.json?.head, { branch: "main", commit: c6 });
  eq("versions = log.txt (log HEAD)", r.json?.versions, headLog);
  check("versions history true", r.json?.history === true);
  r = await q("versions", { rev: "variant", limit: 2 });
  eq("versions variant limit 2", r.json?.versions.map((v) => v.message), ["place on the map", "move a column"]);
  r = await q("versions", { rev: "nope" });
  eq("versions unknown revision", [r.status, r.text], [400, 'unknown revision "nope"']);
  r = await q("versions", { limit: "x" });
  check("versions bad limit 400", r.status === 400, r.text);

  r = await q("branches");
  eq("branches = branches.txt", r.json?.branches, branchesTxt);
  eq("branches head", r.json?.head, { branch: "main", commit: c6 });

  r = await q("version_changes", { a: c1, b: c2 });
  eq("version_changes 1 2 (diff_1_2.txt)", r.json?.changes, [{
    change: "modified", kind: "IfcBeam", id: "01KnL9d95C2OMpOLdTJDE_", name: 'Poutre "P1" — renommée',
    fields: [{ path: "name", before: "B2.x01", after: 'Poutre "P1" — renommée' }],
  }]);
  eq("version_changes a b", [r.json?.a, r.json?.b], [c1, c2]);
  r = await q("version_changes", { a: "HEAD~1" });
  const d46 = readFileSync(join(GOLD, "diff_4_6.txt"), "utf8").split("\n").filter((l) => /^[~+-] /.test(l)).map((l) => l[0]);
  const sym = { modified: "~", added: "+", removed: "-" };
  eq("version_changes HEAD~1 HEAD (diff_4_6.txt)", r.json?.changes.map((c) => sym[c.change]), d46);
  eq("version_changes b defaults to HEAD", r.json?.b, c6);
  r = await q("version_changes", {});
  check("version_changes without a: 400", r.status === 400, r.text);

  r = await q("pending");
  eq("pending on load", [r.json?.base, r.json?.changes], [c6, []]);
  const v = await call("verb", "set_project_field", JSON.stringify({ label: "Phase", value: "51" }));
  check("verb 200", v.status === 200, v.text);
  r = await q("pending");
  eq("pending after a verb", r.json?.changes, [{
    change: "modified", kind: "IfcProject", id: "1ECFGZP4n20ADaVDzRQc6J", name: "project information",
    fields: [{ path: "project.phase", before: "", after: "51" }],
  }]);
  await call("undo");
  r = await q("pending");
  eq("pending after undo", r.json?.changes, []);

  // ---- H2a: commit, branch_create, branch_switch ----
  const act = async (name, args) => call("action", name, args === undefined ? "" : JSON.stringify(args));
  const project = async () => JSON.stringify((await q("project")).json);
  r = await q("capabilities");
  check("capabilities history_write true natively", r.json?.history_write === true, r.text);
  r = await act("commit", { message: "no author" });
  eq("commit without a name: refused", [r.status, r.text], [400, "commit: set your name in Settings first (the author of each version)"]);
  r = await act("set_settings", { user_name: "Test User", user_email: "test@example.org" });
  check("set_settings 200", r.status === 200, r.text);
  r = await act("commit", { message: "nothing" });
  eq("nothing to commit", [r.status, r.text], [400, `nothing to commit: the model is the same as ${c6.slice(0, 12)}`]);
  r = await act("commit", { message: " \n " });
  eq("empty message", [r.status, r.text], [400, "a commit needs a message"]);
  r = await call("verb", "set_project_field", JSON.stringify({ label: "Phase", value: "51" }));
  check("edit 200", r.status === 200, r.text);
  r = await act("commit", { message: "  phase 51 " });
  check("commit 200", r.status === 200, r.text);
  const k1 = r.status === 200 ? JSON.parse(r.text).commit : "";
  r = await q("versions");
  eq("the version first, the settings' author", [r.json?.versions[0]?.hash, r.json?.versions[0]?.message, r.json?.versions[0]?.author, r.json?.versions[0]?.parents],
    [k1, "phase 51", "Test User <test@example.org>", [c6]]);
  eq("head moved", r.json?.head, { branch: "main", commit: k1 });
  r = await q("pending");
  eq("pending clean after the commit", [r.json?.base, r.json?.changes], [k1, []]);
  r = await q("state");
  eq("saved before the commit", r.json?.unsaved, false);
  r = await act("branch_create", { name: "bad name" });
  eq("bad branch name", [r.status, r.text], [400, 'invalid branch name "bad name": use letters, digits, \'-\', \'_\', \'.\' and \'/\'']);
  r = await act("branch_create", { name: "main" });
  eq("branch exists", [r.status, r.text], [400, "branch main already exists"]);
  r = await act("branch_create", { name: "idea" });
  eq("branch_create 200", [r.status, r.text], [200, '{"branch":"idea"}']);
  r = await q("branches");
  eq("on the new branch", [r.json?.head, r.json?.branches.map((b) => b.name)], [{ branch: "idea", commit: k1 }, ["idea", "main", "variant"]]);
  r = await call("verb", "set_project_field", JSON.stringify({ label: "Phase", value: "idea" }));
  check("edit on the branch", r.status === 200, r.text);
  r = await act("branch_switch", { name: "main" });
  eq("switch refused with changes not committed", [r.status, r.text], [400, 'uncommitted changes: commit them first, or send {"discard": true}']);
  r = await act("commit", { message: "on idea" });
  check("commit on the branch", r.status === 200, r.text);
  const k2 = r.status === 200 ? JSON.parse(r.text).commit : "";
  r = await act("branch_switch", { name: "main" });
  eq("switch back to main", [r.status, r.text], [200, '{"branch":"main"}']);
  r = await q("state");
  eq("switch: saved", r.json?.unsaved, false);
  r = await q("pending");
  eq("switch: the model is main's (pending clean against main)", [r.json?.base, r.json?.changes], [k1, []]);
  check("switch: the project field is main's", (await project()).includes('"51"') && !(await project()).includes('"idea"'), await project());
  r = await q("version_changes", { a: "main", b: "idea" });
  eq("main and idea differ", r.json?.changes.length, 1);
  r = await call("verb", "set_project_field", JSON.stringify({ label: "Phase", value: "dropped" }));
  r = await act("branch_switch", { name: "idea", discard: true });
  eq("switch with discard", [r.status, r.text], [200, '{"branch":"idea"}']);
  r = await q("pending");
  eq("discard: the model is idea's", [r.json?.base, r.json?.changes], [k2, []]);
  check("discard: the project field is idea's", (await project()).includes('"idea"'), await project());
  r = await q("history");
  check("switch: a fresh undo history", r.status === 200 && !r.text.includes("Phase"), r.text);
  r = await act("branch_switch", { name: "nope" });
  eq("switch to an unknown revision", [r.status, r.text], [400, 'unknown revision "nope"']);
  r = await act("branch_switch", { name: "main" });
  check("back to main", r.status === 200, r.text);

  // A damaged object: c2 as plain JSON with one byte flipped.
  const obj = join(tmp, "frame.bimble-history/objects", c2.slice(0, 2), c2.slice(2));
  const bytes = gunzipSync(readFileSync(obj));
  bytes[10] ^= 1;
  writeFileSync(obj, bytes);
  r = await q("versions");
  eq("damaged object", [r.status, r.text], [400, `damaged history: object ${c2} does not match its hash`]);

  // A model without a history folder.
  r = await call("action", "open", JSON.stringify({ path: plain, discard: true }));
  check("open plain 200", r.status === 200, r.text);
  for (const name of ["versions", "branches", "pending"]) {
    r = await q(name);
    eq(`${name} without history`, r.json, { history: false, reason: `no history next to ${plain} (run \`bimble history init\`)` });
  }
  r = await q("version_changes", { a: "HEAD~1" });
  eq("version_changes without history", r.json?.history, false);
  // H2a: commit starts the history (lib.rs:1932 "Start history and commit").
  r = await act("commit", { message: "first" });
  check("commit without a history: 200", r.status === 200, r.text);
  r = await q("versions");
  eq("the new history", [r.json?.history, r.json?.head?.branch, r.json?.versions.map((v) => v.message)], [true, "main", ["first"]]);
  check("the folder next to the file", existsSync(join(tmp, "plain.bimble-history/format")));
  if (process.env.H2A_KEEP) cpSync(join(tmp, "plain.bimble-history"), process.env.H2A_KEEP, { recursive: true }); // for a Rust reader
} catch (e) {
  check("run", false, `${e.stack}\n${log}`);
} finally {
  try { process.kill(-host.pid, "SIGKILL"); } catch {}
  rmSync(tmp, { recursive: true, force: true });
}
// The wasm modules: no folders in the browser.
for (const name of ["ccore.wasm", "ccore-view.wasm"]) {
  const file = join(CCORE, "out/wasm", name);
  if (!existsSync(file)) {
    console.log(`skip ${name}: not built`);
    continue;
  }
  const { instance } = await WebAssembly.instantiate(readFileSync(file), {});
  const X = instance.exports;
  const enc = new TextEncoder(), dec = new TextDecoder();
  const res = X.cc_alloc(12);
  const put = (t) => { const b = enc.encode(t); const p = X.cc_alloc(b.length || 1); new Uint8Array(X.memory.buffer).set(b, p); return [p, b.length]; };
  const wcall = (f, args) => {
    const ins = args.map(put);
    const status = f(...ins.flat(), res);
    for (const [p] of ins) X.cc_free(p);
    const dv = new DataView(X.memory.buffer);
    const ptr = dv.getUint32(res, true), len = dv.getUint32(res + 4, true);
    const text = dec.decode(new Uint8Array(X.memory.buffer).slice(ptr, ptr + len));
    X.cc_free(ptr);
    return { status, text };
  };
  const op = wcall(X.host_open_text, ["frame.bimble", readFileSync(join(GOLD, "model_at_6.json"), "utf8")]);
  check(`${name}: open`, op.status === 200, op.text);
  for (const [q, body] of [["versions", ""], ["branches", ""], ["version_changes", '{"a":"HEAD~1"}'], ["pending", ""]]) {
    const r = wcall(X.call, ["query", q, body]);
    eq(`${name}: ${q}`, [r.status, r.text], [200, '{"history":false,"reason":"no history folder in the browser"}']);
  }
  if (name === "ccore.wasm") { // H2a
    const c = wcall(X.call, ["action", "commit", '{"message":"x"}']);
    eq(`${name}: action commit`, [c.status, c.text], [400, "no history folder in the browser"]);
    const caps = JSON.parse(wcall(X.call, ["query", "capabilities", ""]).text);
    eq(`${name}: history_write false`, caps.history_write, false);
  }
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
