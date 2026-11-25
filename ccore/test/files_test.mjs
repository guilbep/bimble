// The file actions of the native host (ticket A5): node ccore/test/files_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) with no model on 47391 (CCORE_SEED=1) and checks,
// through POST /action/<name> with the optional body {"path", "discard"} (session/calls.c
// file_action): new (lib.rs:1688: "Untitled", one storey "Level 0" at 0 mm, unsaved), save of a
// new model refused (no file yet), open refused with unsaved changes, place, save as x.bimble (the
// v2 magic; reopened: the same elements), import_ifc of golden/ifc/frame.ifc (an editable copy:
// the element count of the import golden, frame_reimport.json), export IFC4 to a path (= golden/
// ifc/frame.ifc for frame.json, timestamp and version masked), save as y.json (the v1 text, it
// loads back), close, the errors. Read-only files (ticket A5c, as the Rust viewer: lib.rs:494-498,
// calls.rs:65-71): open of golden/ifc/fixtures/mapped_item.ifc is a read-only scene (/mesh element
// count = golden/ifc/read/mapped_item/scene.txt's, state editable false and schema IFC4, verb place
// and storeys answer Rust's read-only text, the edit actions off), import_ifc of it is editable;
// share_for_viewing of frame.json writes frame.bimbleview (inflated = golden/view/
// frame.bimbleview.raw), and open of that file draws the same element count, read-only. Then a host with --session: the session file follows an open (its
// default name moves to <opened>.ccore-session) and a restart on that file restores the edit.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills what it starts.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { gunzipSync } from "node:zlib";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const G = join(CCORE, "golden");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const TOKEN = "files-test-token";
const PORT = port("files_test", 47391), PORT2 = port("files_test2", 47392);
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
  const mesh = async () => {
    const r = await fetch(`http://127.0.0.1:${port}/mesh?t=${TOKEN}`);
    const b = Buffer.from(await r.arrayBuffer());
    return { status: r.status, magic: b.subarray(0, 4).toString("latin1"), elements: b.length >= 16 ? b.readUInt32LE(12) : -1, triangles: b.length >= 24 ? b.readUInt32LE(20) / 3 : -1 };
  };
  const change = async () => JSON.parse(await (await fetch(`http://127.0.0.1:${port}/change?since=0&t=${TOKEN}`)).text()).change;
  const stop = async () => {
    h.kill("SIGTERM");
    for (let i = 0; i < 200 && h.exitCode === null && h.signalCode === null; i++) await sleep(10);
  };
  return { h, call, q, act, mesh, change, stop };
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

  // import an IFC: an editable copy (lib.rs:1403 import_ifc, 526-538). Ticket A5c: `open` of an
  // IFC is read-only now (below), as the Rust viewer's; the editable copy is `import_ifc`.
  const ifc = join(tmp, "frame.ifc");
  copyFileSync(join(G, "ifc/frame.ifc"), ifc);
  const op2 = await H.act("import_ifc", { path: ifc });
  const s3 = await H.q("state");
  const golden = Object.keys(JSON.parse(readFileSync(join(G, "ifc/frame_reimport.json"), "utf8")).model.elements).length;
  const e3 = await H.q("elements", { limit: 10000 });
  check("import_ifc frame.ifc -> 200: the copy frame.bimble, unsaved", op2.status === 200 && s3.file === "frame.bimble" && s3.unsaved, `${op2.text} ${JSON.stringify(s3)}`);
  check("import_ifc frame.ifc: elements = the import golden's count", e3.length === golden && golden === 72, `${e3.length} vs ${golden}`);
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

  // Read-only files (ticket A5c). open of an IFC: the display reader's scene, no model.
  const READ_ONLY = "this file is read only: import it to edit";
  const mi = join(tmp, "mapped_item.ifc");
  copyFileSync(join(G, "ifc/fixtures/mapped_item.ifc"), mi);
  const sceneTxt = readFileSync(join(G, "ifc/read/mapped_item/scene.txt"), "utf8");
  const [, goldenEls, goldenTris] = /^elements (\d+) triangles (\d+)/m.exec(sceneTxt).map(Number);
  const r0 = (await H.change()).rev;
  const om = await H.act("open", { path: mi });
  const sm = await H.q("state");
  check("open mapped_item.ifc -> 200, read-only: editable false, file, schema IFC4, saved",
    om.status === 200 && sm.editable === false && sm.file === "mapped_item.ifc" && sm.schema === "IFC4" && sm.unsaved === false, `${om.text} ${JSON.stringify(sm)}`);
  const mm = await H.mesh();
  const chm = await H.change();
  check("open mapped_item.ifc: /mesh elements and triangles = golden/ifc/read/mapped_item/scene.txt's",
    mm.status === 200 && mm.magic === "BMSH" && mm.elements === goldenEls && goldenEls === 5 && mm.triangles === goldenTris, `${JSON.stringify(mm)} vs ${goldenEls} ${goldenTris}`);
  check("open mapped_item.ifc: rev + 1, file in the change feed, no placing", chm.rev === r0 + 1 && chm.file === "mapped_item.ifc" && chm.placing === null, JSON.stringify(chm));
  const vp = await H.call("verb", "place", JSON.stringify({ a: [0, 0], kind: "column" }));
  check("read-only: verb place -> 400 Rust's read-only text", vp.status === 400 && vp.text === READ_ONLY, vp.text);
  const qs = await H.call("query", "storeys", "{}"), qe = await H.call("query", "elements", "{}");
  check("read-only: query storeys and elements -> 400 Rust's read-only text (calls.rs `model(v)?`)",
    qs.status === 400 && qs.text === READ_ONLY && qe.status === 400 && qe.text === READ_ONLY, `${qs.text} | ${qe.text}`);
  const ap = await H.call("apply", "-", "[]"), un = await H.call("undo");
  check("read-only: apply and undo -> 400 Rust's read-only text", ap.text === READ_ONLY && un.text === READ_ONLY, `${ap.text} | ${un.text}`);
  const acts = Object.fromEntries((await H.q("actions")).map((a) => [a.name, a.enabled]));
  check("read-only: place, save, undo, export IFC off; open, import, convert_open, home on",
    !acts.place_column && !acts.place_wall && !acts.save && !acts.undo && !acts.export_ifc4 && acts.open && acts.import_ifc && acts.convert_open && acts.home, JSON.stringify(acts));
  const sel = await H.call("select", "-", JSON.stringify(["2T2cMapped000000000011"]));
  check("read-only: select an element of the scene -> 200, in the change feed",
    sel.status === 200 && JSON.stringify((await H.change()).selection) === '["2T2cMapped000000000011"]', sel.text);
  const st = await H.q("status");
  check("read-only: status counts the scene", st.total === goldenEls && st.triangles === goldenTris, JSON.stringify(st));
  // import_ifc of the same file: an editable copy (the C import of mapped_item: its own count).
  const im = await H.act("import_ifc", { path: mi });
  const si = await H.q("state");
  check("import_ifc mapped_item.ifc -> 200: editable copy mapped_item.bimble, unsaved",
    im.status === 200 && si.editable === true && si.file === "mapped_item.bimble" && si.unsaved === true, `${im.text} ${JSON.stringify(si)}`);
  check("import_ifc mapped_item.ifc: elements = /mesh elements", (await H.q("elements", { limit: 10000 })).length === (await H.mesh()).elements);
  // convert_open: the read-only IFC imported in place (ribbon.rs:659).
  await H.act("open", { path: mi, discard: true });
  const cv = await H.act("convert_open");
  const sc = await H.q("state");
  check("convert_open of the read-only IFC -> the editable copy", cv.status === 200 && sc.editable === true && sc.file === "mapped_item.bimble", `${cv.text} ${JSON.stringify(sc)}`);

  // share_for_viewing (share.rs:15): frame.json's view file, then open it read-only.
  const fv = join(tmp, "fv.json");
  copyFileSync(join(G, "frame.json"), fv);
  await H.act("open", { path: fv, discard: true });
  const fm = await H.mesh();
  const sh = await H.act("share_for_viewing");
  const fvv = join(tmp, "fv.bimbleview");
  let raw = Buffer.alloc(0);
  try { raw = gunzipSync(readFileSync(fvv)); } catch {}
  check("share_for_viewing -> 200, writes fv.bimbleview next to the model: inflated = golden/view/frame.bimbleview.raw",
    sh.status === 200 && raw.length > 0 && Buffer.compare(raw, readFileSync(join(G, "view/frame.bimbleview.raw"))) === 0, `${sh.text} ${raw.length}`);
  check("share_for_viewing: the log says Saved … read-only", H.h.log.includes(`Saved ${fvv} (`) && H.h.log.includes("opens in Bimble, also in a browser, read-only."));
  const sh2 = await H.act("share_for_viewing", { path: join(tmp, "named.bimbleview") });
  check("share_for_viewing {path} writes there", sh2.status === 200 && existsSync(join(tmp, "named.bimbleview")), sh2.text);
  const ov = await H.act("open", { path: fvv });
  const sv = await H.q("state");
  const vm = await H.mesh();
  check("open fv.bimbleview -> 200, read-only, schema null", ov.status === 200 && sv.editable === false && sv.file === "fv.bimbleview" && sv.schema === null, `${ov.text} ${JSON.stringify(sv)}`);
  check("open fv.bimbleview: /mesh elements and triangles = frame.json's", vm.status === 200 && vm.elements === fm.elements && fm.elements > 0 && vm.triangles === fm.triangles, `${JSON.stringify(vm)} vs ${JSON.stringify(fm)}`);
  const vp2 = await H.call("verb", "place", JSON.stringify({ a: [0, 0], kind: "column" }));
  check("view file: verb place -> 400 Rust's read-only text; share_for_viewing off (a model only, deviation)",
    vp2.text === READ_ONLY && (await H.act("share_for_viewing")).status === 400, vp2.text);
  const bv = join(tmp, "broken.bimbleview");
  copyFileSync(join(G, "guid.txt"), bv);
  const ob = await H.act("open", { path: bv });
  check("open a broken .bimbleview -> 400 (viewfile's text), the open scene kept", ob.status === 400 && ob.text.includes("bimbleview") && (await H.q("state")).file === "fv.bimbleview", ob.text);
  const cv2 = await H.act("close");
  check("close of a read-only scene -> 200, nothing open, /mesh 404", cv2.status === 200 && (await H.q("state")).file === null && (await H.mesh()).status === 404, cv2.text);
  await H.act("open", { path: fj });

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
