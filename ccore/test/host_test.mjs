// Checks the native host's HTTP side (ticket 9a): node ccore/test/host_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on port 47311 with a token and checks the transport
// of crates/bimble-viewer/src/serve.rs: 403 on a bad token, Origin or Host; 413 over 1 MiB;
// the stub queries; the change feed (at once, then a long-poll that does not block a call);
// garbage closes the connection only; SIGTERM exits 0. Then (ticket 9b) a host on a copy of
// golden/frame.json on 47331: the model's queries, verb place, undo/redo, unsaved, action save
// (bytes), a second host on the saved file (47332), and the latency of verb place and query element.
// Prints timings and "N passed, M failed"; exits non-zero on a failure. No dependencies.
import { spawn } from "node:child_process";
import { copyFileSync, mkdirSync, readFileSync, rmSync } from "node:fs";
import http from "node:http";
import net from "node:net";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = 47311;
const TOKEN = "hosttest-token";
const HOST = `127.0.0.1:${PORT}`;
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** One request over a fresh raw socket: the whole answer text, until the server closes or `ms`. */
function raw(bytes, ms = 3000) {
  return new Promise((res) => {
    const s = net.connect(PORT, "127.0.0.1");
    let out = "", closed = false;
    s.on("data", (d) => (out += d));
    s.on("error", () => {});
    s.on("close", () => { closed = true; res({ out, closed }); });
    s.write(bytes);
    setTimeout(() => { if (!closed) { s.destroy(); res({ out, closed: false }); } }, ms);
  });
}
const status = (text) => Number(/^HTTP\/1\.1 (\d{3})/.exec(text)?.[1] ?? 0);

const agent = new http.Agent({ keepAlive: true, maxSockets: 4 });
/** One request through node:http: {status, headers, body, ms}. */
function req(method, path, { body = "", headers = {}, ag = agent, port = PORT } = {}) {
  return new Promise((res, rej) => {
    const t0 = performance.now();
    const r = http.request({ host: "127.0.0.1", port, method, path, headers, agent: ag }, (rs) => {
      let b = "";
      rs.setEncoding("utf8");
      rs.on("data", (d) => (b += d));
      rs.on("end", () => res({ status: rs.statusCode, headers: rs.headers, body: b, ms: performance.now() - t0 }));
    });
    r.on("error", rej);
    r.end(body);
  });
}
const call = (op, name, body = "") => req("POST", `/${op}/${name}?t=${TOKEN}`, { body, headers: { "Content-Type": "text/plain" } });

const t0 = performance.now();
const host = spawn(BIN, [String(PORT)], { env: { ...process.env, BIMBLE_TOKEN: TOKEN }, stdio: ["ignore", "ignore", "pipe"] });
let log = "";
host.stderr.on("data", (d) => (log += d));
const exited = new Promise((r) => host.on("exit", (code, sig) => r({ code, sig })));
try {
  for (let i = 0; i < 200 && !log.includes("listening"); i++) await sleep(5);
  const startMs = performance.now() - t0;
  check("host prints 'listening'", log.includes(`listening ${PORT} token ${TOKEN}`), log.trim());
  console.log(`host start (spawn to 'listening'): ${startMs.toFixed(1)} ms`);

  // 403: token, Origin, Host (serve.rs:206-223).
  check("bad token -> 403", (await req("POST", "/query/state?t=nope")).status === 403);
  check("no token -> 403", (await req("POST", "/query/state")).status === 403);
  check("bad Origin -> 403", (await req("POST", `/query/state?t=${TOKEN}`, { headers: { Origin: "http://evil.example" } })).status === 403);
  const good = await req("POST", `/query/state?t=${TOKEN}`, { headers: { Origin: "http://localhost:5173" } });
  check("allowed Origin -> 200 with Access-Control-Allow-Origin", good.status === 200 && good.headers["access-control-allow-origin"] === "http://localhost:5173");
  check("bad Host -> 403", (await req("POST", `/query/state?t=${TOKEN}`, { headers: { Host: "evil.example:47311" } })).status === 403);
  check("Host localhost:<port> -> 200", (await req("POST", `/query/state?t=${TOKEN}`, { headers: { Host: `localhost:${PORT}` } })).status === 200);
  const pre = await req("OPTIONS", "/query/state", { headers: { Origin: "http://127.0.0.1:5173" } });
  check("OPTIONS without token -> 204 with CORS headers", pre.status === 204 && pre.headers["access-control-allow-methods"] === "GET, POST, OPTIONS");

  // 413 (serve.rs:14, :225-227): a body just over 1 MiB, sent in full.
  const big = "x".repeat((1 << 20) + 1);
  const r413 = await raw(`POST /query/state?t=${TOKEN} HTTP/1.1\r\nHost: ${HOST}\r\nContent-Length: ${big.length}\r\n\r\n${big}`);
  check("body > 1 MiB -> 413", status(r413.out) === 413, r413.out.split("\r\n")[0]);
  const ok1m = await call("query", "state", " ".repeat(1 << 20));
  check("body of exactly 1 MiB -> 200", ok1m.status === 200, String(ok1m.status));

  // Queries.
  const st = await call("query", "state");
  let state = null;
  try { state = JSON.parse(st.body); } catch {}
  check("query state -> 200 and parses", st.status === 200 && state !== null, st.body);
  check("query state key order (calls.rs:239, json! sorted)",
    JSON.stringify(Object.keys(state ?? {})) === JSON.stringify(["editable", "file", "schema", "unsaved", "user", "version"]));
  check("query state: file null", state?.file === null);
  const ac = await call("query", "actions");
  let actions = [];
  try { actions = JSON.parse(ac.body); } catch {}
  const ORDER = JSON.stringify(["enabled", "label", "name", "pressed", "shortcut", "tab"]);
  check("query actions -> 54 rows", ac.status === 200 && actions.length === 54, `${actions.length}`);
  check("query actions key order (calls.rs:325, json! sorted)", actions.length > 0 && actions.every((a) => JSON.stringify(Object.keys(a)) === ORDER));
  check("query actions: unique names", new Set(actions.map((a) => a.name)).size === 54);
  const enabled = actions.filter((a) => a.enabled).map((a) => a.name).sort().join(",");
  // No model: save, undo, redo, place_* and export_ifc* need one (ribbon.rs:577-590).
  check("query actions: enabled set without a model (ribbon.rs:555)", enabled === "home", enabled);
  const hi = await call("query", "history");
  check("query history -> {redo:[],undo:[]}", hi.status === 200 && hi.body === '{"redo":[],"undo":[]}', hi.body);
  for (const [name, want] of [["status", "null"], ["placing", "null"], ["pointer", "null"], ["views", "[]"], ["sheets", "[]"],
    ["recent_files", "[]"], ["standards", "null"],
    ["collab", '{"belongs":null,"borrowed_by_me":0,"borrowed_by_others":[],"folder":null,"last_sync":null,"right":null,"user":null}'],
    ["open_views", '{"active":null,"current":null,"paper":{"offered":false,"on":false},"tabs":[],"views":[]}']]) {
    const r = await call("query", name);
    check(`query ${name} -> ${want.length > 20 ? want.slice(0, 20) + "…" : want}`, r.status === 200 && r.body === want, `${r.status} ${r.body}`);
  }
  const vis = await call("query", "visibility");
  check("query visibility parses, section_box last", vis.status === 200 && Object.keys(JSON.parse(vis.body)).at(-1) === "section_box", vis.body);
  const unk = await call("query", "nope");
  check("unknown query -> 400 text", unk.status === 400 && unk.body.startsWith('unknown query "nope"'), unk.body);
  const op = await call("nope", "-");
  check("unknown op -> 400 'unknown op nope'", op.status === 400 && op.body === "unknown op nope", op.body);
  const nf = await req("POST", `/a/b/c?t=${TOKEN}`);
  check("three path segments -> 404", nf.status === 404);
  const act = await call("action", "fit");
  check("action not enabled -> 400", act.status === 400 && act.body === 'action "fit" is not enabled now', act.body);

  // Change feed: at once, then a long-poll that does not block a call (spec §6, last risk).
  const c0 = await req("GET", `/change?since=0&t=${TOKEN}`);
  let ch = null;
  try { ch = JSON.parse(c0.body); } catch {}
  check("/change?since=0 returns at once", c0.status === 200 && c0.ms < 100, `${c0.ms.toFixed(1)} ms`);
  check("/change has epoch, seq >= 1, change (serve.rs:249 order)", ch && /^[0-9a-f]{32}$/.test(ch.epoch) && ch.seq >= 1 && ch.change?.file === null &&
    JSON.stringify(Object.keys(ch)) === '["epoch","seq","change"]', c0.body);
  const lpAgent = new http.Agent({ keepAlive: false });
  let lpDone = false;
  const lp = req("GET", `/change?since=${ch?.seq}&epoch=${ch?.epoch}&t=${TOKEN}`, { ag: lpAgent }).then((r) => { lpDone = true; return r; }, () => { lpDone = true; });
  await sleep(200);
  check("second /change long-polls (no answer after 200 ms)", !lpDone);
  const during = await call("query", "state");
  check("POST /query/state during the long-poll < 50 ms", during.status === 200 && during.ms < 50, `${during.ms.toFixed(2)} ms`);
  const stale = await req("GET", `/change?since=${ch?.seq}&epoch=other&t=${TOKEN}`);
  check("/change with another epoch answers at once", stale.status === 200 && stale.ms < 100 && JSON.parse(stale.body).seq === ch?.seq, `${stale.ms.toFixed(1)} ms`);
  check("long-poll still waiting", !lpDone);
  lpAgent.destroy();

  // Garbage: the connection closes, the server lives.
  const g = await raw("\x00\xff garbage \x01\r\n\r\n");
  check("garbage request -> 400 and connection closed", g.closed && status(g.out) === 400, g.out.split("\r\n")[0]);
  const g2 = await raw("GET\r\n\r\n");
  check("one-word request line -> 400 and connection closed", g2.closed && status(g2.out) === 400, g2.out.split("\r\n")[0]);
  const big2 = await raw(`GET /x HTTP/1.1\r\n${"X-A: b\r\n".repeat(3000)}\r\n`);
  check("header block > 16 KiB -> 431", status(big2.out) === 431, big2.out.split("\r\n")[0]);
  const alive = await call("query", "state");
  check("server alive after garbage", alive.status === 200);

  // Latency: 1000 x query state on one keep-alive connection.
  const one = new http.Agent({ keepAlive: true, maxSockets: 1 });
  const lat = [];
  for (let i = 0; i < 1000; i++) {
    const r = await req("POST", `/query/state?t=${TOKEN}`, { ag: one });
    if (r.status !== 200) { lat.length = 0; break; }
    lat.push(r.ms);
  }
  one.destroy();
  lat.sort((a, b) => a - b);
  check("1000 query state, all 200", lat.length === 1000);
  if (lat.length) console.log(`query state latency over 1000 (node:http, keep-alive): p50 ${lat[499].toFixed(3)} ms, p95 ${lat[949].toFixed(3)} ms, max ${lat[999].toFixed(3)} ms`);

  // SIGTERM -> exit 0.
  agent.destroy();
  host.kill("SIGTERM");
  const ex = await Promise.race([exited, sleep(3000).then(() => null)]);
  check("SIGTERM -> exits 0", ex && ex.code === 0, JSON.stringify(ex));
} catch (e) {
  check("no exception in the test", false, e.stack ?? String(e));
} finally {
  if (host.exitCode === null && host.signalCode === null) host.kill("SIGKILL");
}

// ---- With a model (ticket 9b) ----
const MPORT = 47331, MPORT2 = 47332;
const TMP = join(CCORE, "out/t9b-host_test");
const GOLDEN = join(CCORE, "golden");
const hosts = [];
const magent = new http.Agent({ keepAlive: true, maxSockets: 4 });
function startHost(port, model) {
  const t = performance.now();
  const h = spawn(BIN, [String(port), model], { env: { ...process.env, BIMBLE_TOKEN: TOKEN }, stdio: ["ignore", "ignore", "pipe"] });
  h.log = "";
  h.stderr.on("data", (d) => (h.log += d));
  h.exited = new Promise((r) => h.on("exit", (code, sig) => r({ code, sig })));
  hosts.push(h);
  return { h, t };
}
async function listening(h, t) {
  for (let i = 0; i < 400 && !h.log.includes("listening"); i++) await sleep(5);
  return performance.now() - t;
}
const mcall = (op, name, body = "", port = MPORT) =>
  req("POST", `/${op}/${name}?t=${TOKEN}`, { body, headers: { "Content-Type": "text/plain" }, port, ag: magent });
const mq = async (name, args, port = MPORT) => {
  const r = await mcall("query", name, args === undefined ? "" : JSON.stringify(args), port);
  if (r.status !== 200) throw new Error(`query ${name}: ${r.status} ${r.body}`);
  return JSON.parse(r.body);
};
const rev = async () => JSON.parse((await req("GET", `/change?since=0&t=${TOKEN}`, { port: MPORT, ag: magent })).body).change.rev;
const pct = (ms, p) => { const s = [...ms].sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.ceil(p * s.length) - 1)]; };
/** golden/query.txt: the answer text of the first record `>>> query <name> ...`. */
function goldenAnswer(name) {
  const lines = readFileSync(join(GOLDEN, "query.txt"), "utf8").split("\n");
  const i = lines.findIndex((l) => l.startsWith(`>>> query ${name}`));
  return i >= 0 ? lines[i + 1].replace(/^200 /, "") : null;
}

try {
  rmSync(TMP, { recursive: true, force: true });
  mkdirSync(TMP, { recursive: true });
  const model = join(TMP, "frame.bimble");
  copyFileSync(join(GOLDEN, "frame.json"), model);
  const frameBytes = readFileSync(model);
  const { h, t } = startHost(MPORT, model);
  const startMs = await listening(h, t);
  check("model host prints 'listening'", h.log.includes(`listening ${MPORT}`), h.log.trim());
  console.log(`host start with frame.json (spawn to 'listening'): ${startMs.toFixed(1)} ms`);

  const st = await mq("state");
  check("model: query state", st.file === "frame.bimble" && st.editable === true && st.unsaved === false && st.schema === ".bimble", JSON.stringify(st));
  const sto = await mcall("query", "storeys");
  check("model: query storeys = golden/query.txt first record", sto.status === 200 && sto.body === goldenAnswer("storeys"), sto.body);
  const en = async () => (await mq("actions")).filter((a) => a.enabled).map((a) => a.name).sort().join(",");
  const en0 = await en();
  check("model: enabled actions, nothing to undo or save", en0 === "export_ifc2x3,export_ifc4,home,place_beam,place_column,place_slab,place_wall", en0);
  const r0 = await rev();
  check("model: rev 1 after load (lib.rs:1679)", r0 === 1, String(r0));

  // Save writes cc_model_save's bytes: a model equal to the loaded one (an element moved there and
  // back) saves as frame.json, which cc_model_save reproduces byte for byte (file_test golden round trip).
  const col = (await mq("elements", { class: "IfcColumn", limit: 1 }))[0].id;
  const mv1 = await mcall("verb", "move", JSON.stringify({ ids: [col], d: [1, 0, 0] }));
  const mv2 = await mcall("verb", "move", JSON.stringify({ ids: [col], d: [-1, 0, 0] }));
  check("model: verb move there and back", mv1.status === 200 && mv2.status === 200, `${mv1.body} ${mv2.body}`);
  check("model: unsaved after edits", (await mq("state")).unsaved === true);
  const sv0 = await mcall("action", "save");
  check("model: action save -> 200", sv0.status === 200, sv0.body);
  check("model: saved bytes = cc_model_save (= frame.json)", Buffer.compare(readFileSync(model), frameBytes) === 0);
  check("model: saved -> unsaved false, save disabled", (await mq("state")).unsaved === false && !(await en()).includes("save"));

  // action export_ifc4 writes frame.ifc next to the model (share.rs:50): ifc_out's bytes, which the
  // ifc_out suite checks against golden/ifc/frame.ifc; here the version and the timestamp (now) masked.
  const ex = await mcall("action", "export_ifc4");
  const ifcPath = join(TMP, "frame.ifc");
  let ifc = "";
  try { ifc = readFileSync(ifcPath, "utf8"); } catch {}
  const stamp = /^FILE_NAME\('frame\.ifc','(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d)'/m.exec(ifc);
  const age = stamp ? Math.abs(Date.now() - Date.parse(`${stamp[1]}Z`)) : NaN;
  const masked = ifc.replace(/^(FILE_NAME\('frame\.ifc',)'[^']*'/m, "$1'2026-01-01T00:00:00'").replace("'bimble 0.1.0-ccore'", "'bimble <version>'");
  check("model: action export_ifc4 writes frame.ifc = golden/ifc/frame.ifc (timestamp now, UTC)",
    ex.status === 200 && age < 120000 && masked === readFileSync(join(GOLDEN, "ifc/frame.ifc"), "utf8"), `${ex.status} ${ex.body} age ${age} ms, ${ifc.length} bytes`);

  // verb place: rev + 1, the history label of golden/verbs.txt.
  const r1 = await rev();
  const place = { a: [1500.0, 1500.0], kind: "column", storey: "1uAZA0Yfn05f$xRd1k4uPR" };
  const pl = await mcall("verb", "place", JSON.stringify(place));
  check("model: verb place column -> 200", pl.status === 200, pl.body);
  const r2 = await rev();
  check("model: verb place -> rev + 1", r2 === r1 + 1, `${r1} -> ${r2}`);
  const hi = await mcall("query", "history");
  check("model: history label 'add P1' (golden/verbs.txt)", JSON.parse(hi.body).undo[0] === "add P1" && JSON.parse(hi.body).redo.length === 0, hi.body);
  const els = await mq("elements", { limit: 10000 });
  check("model: 73 elements after place", els.length === 73, String(els.length));
  const sv1 = await mcall("action", "save");
  const saved1 = readFileSync(model);
  check("model: save after place writes the new column", sv1.status === 200 && Buffer.compare(saved1, frameBytes) !== 0 &&
    Object.values(JSON.parse(saved1.toString()).model.elements).some((e) => e.name === "P1"));

  // undo: the model changes, the file does not; unsaved again; redo back to the saved step.
  const un = await mcall("undo", "-");
  check("model: undo -> 200, rev + 1", un.status === 200 && (await rev()) === r2 + 1, un.body);
  check("model: undo leaves the saved file's bytes unchanged", Buffer.compare(readFileSync(model), saved1) === 0);
  check("model: after undo, unsaved and redo enabled", (await mq("state")).unsaved === true && (await en()).includes("redo"));
  const hu = await mq("history");
  check("model: history after undo", hu.redo[0] === "add P1", JSON.stringify(hu));
  const re = await mcall("action", "redo");
  check("model: action redo -> back at the saved step (unsaved false)", re.status === 200 && (await mq("state")).unsaved === false, re.body);

  // select and element.
  const bad = await mcall("select", "-", JSON.stringify(["0000000000000000000000"]));
  check("model: select unknown id -> 400 'unknown GlobalIds: …' (calls.rs:566)", bad.status === 400 && bad.body === "unknown GlobalIds: 0000000000000000000000", bad.body);
  const sel = await mcall("select", "-", JSON.stringify([col]));
  const ch = JSON.parse((await req("GET", `/change?since=0&t=${TOKEN}`, { port: MPORT, ag: magent })).body).change;
  check("model: select -> change.selection", sel.status === 200 && JSON.stringify(ch.selection) === JSON.stringify([col]), JSON.stringify(ch.selection));
  const el = await mq("element", { id: col });
  check("model: query element", el.id === col && el.class === "IfcColumn", JSON.stringify(el).slice(0, 120));
  const ne = await mcall("query", "element", JSON.stringify({ id: "0000000000000000000000" }));
  check("model: query element unknown -> 'no element with this id' (calls.rs:308)", ne.status === 400 && ne.body === "no element with this id", ne.body);

  // set_placing and placing.
  const sp = await mcall("set_placing", "-", JSON.stringify({ kind: "beam", sizes: { beam_width: 300 } }));
  const pj = await mq("placing");
  check("model: set_placing starts placing a beam", sp.status === 200 && pj.active && pj.kind === "beam" && pj.sizes.beam_width === 300 && pj.hint === "Click the start point.", JSON.stringify(pj));
  const spb = await mcall("set_placing", "-", JSON.stringify({ sizes: { wall: 5 } }));
  check("model: set_placing size out of range -> 400 (verbs.rs:15)", spb.status === 400 && spb.body === "wall: 5 is not a size within [10, 100000] mm", spb.body);

  // A second host on the saved file: the same elements (the text form round trips).
  const second = startHost(MPORT2, model);
  await listening(second.h, second.t);
  const els2 = await mq("elements", { limit: 10000 }, MPORT2);
  const els1 = await mq("elements", { limit: 10000 });
  check("model: a host on the saved file lists the same elements", JSON.stringify(els1) === JSON.stringify(els2), `${els1.length} vs ${els2.length}`);

  // Latency: verb place (each one a step), query element.
  const lp = [];
  for (let i = 0; i < 200; i++) {
    const r = await mcall("verb", "place", JSON.stringify({ ...place, a: [1500 + 10 * i, 1500] }));
    if (r.status !== 200) { check("verb place x200", false, r.body); break; }
    lp.push(r.ms);
  }
  const le = [];
  for (let i = 0; i < 200; i++) le.push((await mcall("query", "element", JSON.stringify({ id: col }))).ms);
  check("model: 200 verb place, all 200", lp.length === 200);
  console.log(`verb place latency over 200: p50 ${pct(lp, 0.5).toFixed(3)} ms, p95 ${pct(lp, 0.95).toFixed(3)} ms`);
  console.log(`query element latency over 200: p50 ${pct(le, 0.5).toFixed(3)} ms, p95 ${pct(le, 0.95).toFixed(3)} ms`);
} catch (e) {
  check("no exception in the model test", false, e.stack ?? String(e));
} finally {
  magent.destroy();
  for (const h of hosts) {
    if (h.exitCode === null && h.signalCode === null) h.kill("SIGTERM");
    const ex = await Promise.race([h.exited, sleep(3000).then(() => null)]);
    if (!ex) h.kill("SIGKILL");
  }
  rmSync(TMP, { recursive: true, force: true });
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
