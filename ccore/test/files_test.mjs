// The file actions of the native host (ticket A5): node ccore/test/files_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) with no model on 47391 (CCORE_SEED=1) and checks,
// through POST /action/<name> with the optional body {"path", "discard"} (session/calls.c
// file_action): new (lib.rs:1688: "Untitled", one storey "Level 0" at 0 mm, unsaved), save of a
// new model refused (no file yet), open refused with unsaved changes, place, save as x.bimble (the
// v2 magic; reopened: the same elements), open of golden/ifc/frame.ifc (an editable copy: the
// element count of the import golden, frame_reimport.json), export IFC4 to a path (= golden/ifc/
// frame.ifc for frame.json, timestamp and version masked), save as y.json (the v1 text, it loads
// back), close, the errors. Then a host with --session: the session file follows an open (its
// default name moves to <opened>.ccore-session) and a restart on that file restores the edit.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills what it starts.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const G = join(CCORE, "golden");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const TOKEN = "files-test-token";
const PORT = 47391, PORT2 = 47392;
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const tmp = mkdtempSync(join(tmpdir(), "ccore-files-"));
const hosts = [];

async function startHost(port, args) {
  const h = spawn(BIN, [...args.flags ?? [], String(port), ...args.model ? [args.model] : []], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, CCORE_SEED: "1" }, stdio: ["ignore", "ignore", "pipe"],
  });
  h.log = "";
  h.stderr.on("data", (d) => (h.log += d));
  hosts.push(h);
  for (let i = 0; i < 400 && !h.log.includes("listening"); i++) {
    if (h.exitCode !== null) break;
    await sleep(10);
  }
  if (!h.log.includes("listening")) throw new Error(`host on ${port} did not start: ${h.log}`);
  const call = async (op, name = "-", body = "") => {
    const r = await fetch(`http://127.0.0.1:${port}/${op}/${encodeURIComponent(name)}?t=${TOKEN}`,
      { method: "POST", body, headers: { "Content-Type": "text/plain" } });
    return { status: r.status, text: await r.text() };
  };
  const q = async (name, args = {}) => {
    const r = await call("query", name, JSON.stringify(args));
    if (r.status !== 200) throw new Error(`query ${name}: ${r.status} ${r.text}`);
    return JSON.parse(r.text);
  };
  const act = (name, body) => call("action", name, body === undefined ? "" : JSON.stringify(body));
  const change = async () => JSON.parse(await (await fetch(`http://127.0.0.1:${port}/change?since=0&t=${TOKEN}`)).text()).change;
  const stop = async () => {
    h.kill("SIGTERM");
    for (let i = 0; i < 200 && h.exitCode === null && h.signalCode === null; i++) await sleep(10);
  };
  return { h, call, q, act, change, stop };
}

const MAGIC = Buffer.from("bimble\0", "latin1");
const isV2 = (p) => existsSync(p) && Buffer.compare(readFileSync(p).subarray(0, 7), MAGIC) === 0;
const ids = (els) => JSON.stringify(els.map((e) => e.id).sort());
const UNSAVED = 'unsaved changes: save them first, or send {"discard": true}';

try {
  const H = await startHost(PORT, {});
  const s0 = await H.q("state");
  check("no model: state file null", s0.file === null, JSON.stringify(s0));
  const cl0 = await H.act("close");
  check("close without a model -> 400 not enabled", cl0.status === 400 && cl0.text === 'action "close" is not enabled now', cl0.text);

  // new (lib.rs:1688-1712, verbs.rs:101).
  const nw = await H.act("new");
  const s1 = await H.q("state");
  check("new -> 200", nw.status === 200, nw.text);
  check("new: Untitled.bimble, editable, unsaved, schema .bimble", s1.file === "Untitled.bimble" && s1.editable && s1.unsaved && s1.schema === ".bimble", JSON.stringify(s1));
  const sto = await H.q("storeys");
  check("new: one storey 'Level 0' at 0 mm", sto.length === 1 && sto[0].name === "Level 0" && sto[0].elevation === 0, JSON.stringify(sto));
  const pr = await H.q("project");
  check("new: project 'Untitled'", pr.name === "Untitled", JSON.stringify(pr));
  check("new: no element", (await H.q("elements", { limit: 10 })).length === 0);
  const ch1 = await H.change();
  check("new: change feed file and rev 1", ch1.file === "Untitled.bimble" && ch1.rev === 1 && ch1.unsaved === true, JSON.stringify(ch1));
  const sv0 = await H.act("save");
  check("save of a new model -> 400 (no file yet: save_as)", sv0.status === 400 && sv0.text.includes("save_as"), sv0.text);
  const op0 = await H.act("open", { path: join(G, "frame.json") });
  check("open with unsaved changes -> 400", op0.status === 400 && op0.text === UNSAVED, op0.text);
  const nw2 = await H.act("new");
  check("new with unsaved changes -> 400", nw2.status === 400 && nw2.text === UNSAVED, nw2.text);
  const opd = await H.act("open");
  check("open without a path -> 400", opd.status === 400 && opd.text.includes('"path"'), opd.text);
  const bad = await H.call("action", "open", "[1]");
  check("open with a bad body -> 400", bad.status === 400, bad.text);

  // place, save as x.bimble (v2), reopen.
  const pl = await H.call("verb", "place", JSON.stringify({ a: [0, 0], kind: "column", storey: sto[0].id }));
  check("place a column on the new model -> 200", pl.status === 200, pl.text);
  const x = join(tmp, "x.bimble");
  const sa = await H.act("save_as", { path: x });
  const s2 = await H.q("state");
  check("save_as x.bimble -> 200, file x.bimble, saved", sa.status === 200 && s2.file === "x.bimble" && !s2.unsaved, `${sa.text} ${JSON.stringify(s2)}`);
  check("x.bimble is v2 (the magic \"bimble\\0\")", isV2(x));
  const e1 = await H.q("elements", { limit: 10000 });
  const r1 = (await H.change()).rev;
  const op1 = await H.act("open", { path: x });
  const e2 = await H.q("elements", { limit: 10000 });
  check("open x.bimble -> 200, the same elements", op1.status === 200 && e1.length === 1 && ids(e1) === ids(e2), `${op1.text} ${e1.length} vs ${e2.length}`);
  const ch2 = await H.change();
  check("open: rev + 1 (lib.rs:1679 scene_rev += 1), saved", ch2.rev === r1 + 1 && ch2.unsaved === false, JSON.stringify(ch2));
  const sv1 = await H.act("save");
  check("save with nothing to save -> 400 not enabled", sv1.status === 400, sv1.text);

  // open an IFC: imported as an editable copy (lib.rs:526-538).
  const ifc = join(tmp, "frame.ifc");
  copyFileSync(join(G, "ifc/frame.ifc"), ifc);
  const op2 = await H.act("open", { path: ifc });
  const s3 = await H.q("state");
  const golden = Object.keys(JSON.parse(readFileSync(join(G, "ifc/frame_reimport.json"), "utf8")).model.elements).length;
  const e3 = await H.q("elements", { limit: 10000 });
  check("open frame.ifc -> 200: the copy frame.bimble, unsaved", op2.status === 200 && s3.file === "frame.bimble" && s3.unsaved, `${op2.text} ${JSON.stringify(s3)}`);
  check("open frame.ifc: elements = the import golden's count", e3.length === golden && golden === 72, `${e3.length} vs ${golden}`);
  const sv2 = await H.act("save");
  check("save of the imported copy writes frame.bimble (v2) next to the IFC", sv2.status === 200 && isV2(join(tmp, "frame.bimble")), sv2.text);

  // export IFC4 of frame.json to a path = golden/ifc/frame.ifc (the ifc_out bytes; masked).
  const fj = join(tmp, "frame.json");
  copyFileSync(join(G, "frame.json"), fj);
  const op3 = await H.act("open", { path: fj });
  check("open frame.json -> 200", op3.status === 200, op3.text);
  const ex = await H.act("export_ifc4", { path: join(tmp, "exported.ifc") });
  let txt = "";
  try { txt = readFileSync(join(tmp, "exported.ifc"), "utf8"); } catch {}
  const stamp = /^FILE_NAME\('exported\.ifc','(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)'/m.exec(txt);
  const age = stamp ? Math.abs(Date.now() - Date.parse(`${stamp[1]}Z`)) : NaN;
  const masked = txt.replace(/^FILE_NAME\('exported\.ifc','[^']*'/m, "FILE_NAME('frame.ifc','2026-01-01T00:00:00'").replace("'bimble 0.1.0-ccore'", "'bimble <version>'");
  check("export_ifc4 {path} = golden/ifc/frame.ifc (name, timestamp now, version masked)",
    ex.status === 200 && age < 120000 && masked === readFileSync(join(G, "ifc/frame.ifc"), "utf8"), `${ex.status} ${ex.text} ${txt.length} bytes`);
  const ex3 = await H.act("export_ifc2x3", { path: join(tmp, "exported3.ifc") });
  let t3 = "";
  try { t3 = readFileSync(join(tmp, "exported3.ifc"), "utf8"); } catch {}
  check("export_ifc2x3 {path} writes IFC2X3", ex3.status === 200 && t3.includes("FILE_SCHEMA(('IFC2X3'));"), `${ex3.status} ${t3.length}`);

  // save as a .json path: the v1 text (file.rs:145), and it loads back.
  const y = join(tmp, "y.json");
  const sj = await H.act("save_as", { path: y });
  let yt = "";
  try { yt = readFileSync(y, "utf8"); } catch {}
  check("save_as y.json writes the v1 text (= frame.json, cc_model_save)", sj.status === 200 && yt === readFileSync(join(G, "frame.json"), "utf8"), `${sj.text} ${yt.length}`);
  check("save_as y.json: state file y.json, schema null", (await H.q("state")).file === "y.json" && (await H.q("state")).schema === null);

  // close; errors.
  const cl = await H.act("close");
  const s4 = await H.q("state");
  check("close -> 200, no model", cl.status === 200 && s4.file === null && !s4.editable, `${cl.text} ${JSON.stringify(s4)}`);
  const nf = await H.act("open", { path: join(tmp, "nope.bimble") });
  check("open a missing file -> 400 '<path>: cannot open'", nf.status === 400 && nf.text.endsWith("nope.bimble: cannot open"), nf.text);
  const garbage = join(tmp, "garbage.bimble");
  copyFileSync(join(G, "guid.txt"), garbage);
  const og = await H.act("open", { path: garbage });
  check("open a broken file -> 400, still no model", og.status === 400 && (await H.q("state")).file === null, og.text);
  await H.act("open", { path: x });
  const og2 = await H.act("open", { path: garbage });
  check("a failed open keeps the open model", og2.status === 400 && (await H.q("state")).file === "x.bimble", og2.text);
  await H.stop();
  check("host exits 0 on SIGTERM", H.h.exitCode === 0, `${H.h.exitCode} ${H.h.signalCode}`);

  // The session file follows the opened path (default name <model>.ccore-session).
  const a = join(tmp, "a.json");
  copyFileSync(join(G, "frame.json"), a);
  const P = await startHost(PORT2, { flags: ["--session"], model: a });
  check("--session: a.json.ccore-session written", existsSync(`${a}.ccore-session`));
  const po = await P.act("open", { path: x });
  const xs = `${x}.ccore-session`;
  let first = {};
  try { first = JSON.parse(readFileSync(xs, "utf8").split("\n")[0]); } catch {}
  check("open x.bimble: the session file moves to x.bimble.ccore-session", po.status === 200 && first.model_path === x && first.model === null, `${po.text} ${JSON.stringify(first).slice(0, 200)}`);
  const xsto = (await P.q("storeys"))[0].id;
  const pl2 = await P.call("verb", "place", JSON.stringify({ a: [3000, 0], kind: "column", storey: xsto }));
  const e4 = await P.q("elements", { limit: 10000 });
  await P.stop();
  check("place in x.bimble, then the host stops", pl2.status === 200 && e4.length === 2, `${pl2.text} ${e4.length}`);
  const R = await startHost(PORT2, { flags: ["--session"], model: x });
  const e5 = await R.q("elements", { limit: 10000 });
  const s5 = await R.q("state");
  check("a restart on x.bimble restores the edit from x.bimble.ccore-session (unsaved)", ids(e4) === ids(e5) && s5.unsaved === true, `${e5.length} ${JSON.stringify(s5)} ${R.h.log.split("\n").filter((l) => l.includes("session")).join(" | ")}`);
  await R.stop();
} catch (e) {
  check("no exception", false, e.stack ?? String(e));
} finally {
  for (const h of hosts) if (h.exitCode === null && h.signalCode === null) h.kill("SIGKILL");
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
