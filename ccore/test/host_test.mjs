// Checks the native host's HTTP side (ticket 9a): node ccore/test/host_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on port 47311 with a token and checks the transport
// of crates/bimble-viewer/src/serve.rs: 403 on a bad token, Origin or Host; 413 over 1 MiB;
// the stub queries; the change feed (at once, then a long-poll that does not block a call);
// garbage closes the connection only; SIGTERM exits 0. Prints timings and "N passed, M failed";
// exits non-zero on a failure. No dependencies.
import { spawn } from "node:child_process";
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
function req(method, path, { body = "", headers = {}, ag = agent } = {}) {
  return new Promise((res, rej) => {
    const t0 = performance.now();
    const r = http.request({ host: "127.0.0.1", port: PORT, method, path, headers, agent: ag }, (rs) => {
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
  check("query actions: enabled set per spec", enabled === "export_ifc4,home,place_beam,place_column,place_slab,place_wall,redo,save,undo", enabled);
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
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
