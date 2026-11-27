// A link is ready before its proxies exist (ticket SIZE3 E; ADR 0079 amendment): the conversion writes the cache's
// geometry, names and property sets (SRCE "complete": false) and the link is drawn; its proxies are baked after,
// by a job of their own, appended to the cache (SRCE "complete": true).
//   CCORE_PORT_BASE=auto node ccore/test/link_index_test.mjs
// A native host (out/tcc/bimble-ccore or $HOST_BIN), every IFC through a cache (BIMBLE_LINK_CACHE_MIN=0), each
// indexing job waiting 1.5 s first (BIMBLE_LINK_INDEX_DELAY_MS) so that its phases can be seen:
//   - golden/ifc/frame.ifc linked: "ready" and "indexing" at once, no proxies served yet, its cache incomplete;
//     then indexed: link_draw's "proxies" moves, its proxies served, its cache complete with a section HLOD, the
//     report says the proxies;
//   - a host killed while it bakes a link's proxies: the cache left is incomplete but valid; a new host links it from
//     that cache (no IFC read) and completes it;
//   - cancel while indexing: the link stays ready, its cache incomplete, nothing more baked.
// On the code before SIZE3 the link became ready only after its proxies were baked (no "indexing" phase).
// Prints "N passed, M failed"; exits non-zero on a failure.
import { spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const TOKEN = "link-index-token";
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const tmp = mkdtempSync(join(tmpdir(), "ccore-link-index-"));
const hosts = [];
process.on("exit", (code) => {
  if (code) for (const h of hosts) console.log(`host log (tail):\n${h.log.slice(-2000)}`);
  for (const h of hosts) try { h.kill("SIGKILL"); } catch { /* gone */ }
  rmSync(tmp, { recursive: true, force: true });
});
for (const n of ["a", "b", "c"]) copyFileSync(join(CCORE, "golden/ifc/frame.ifc"), join(tmp, `${n}.ifc`));

async function startHost(name) {
  const h = spawn(BIN, [String(port(name))], {
    env: hostEnv({ BIMBLE_TOKEN: TOKEN, BIMBLE_CACHE_DIR: join(tmp, "cache"), BIMBLE_LINK_CACHE_MIN: "0", BIMBLE_LINK_INDEX_DELAY_MS: "1500" }),
    stdio: ["ignore", "ignore", "pipe"],
  });
  h.log = "";
  h.stderr.on("data", (d) => (h.log += d));
  hosts.push(h);
  for (let i = 0; i < 400 && !h.log.includes("listening"); i++) await sleep(25);
  const p = Number(h.log.match(/listening (\d+)/)?.[1]);
  const call = async (op, nm, body = "") => {
    const r = await fetch(`http://127.0.0.1:${p}/${op}/${encodeURIComponent(nm)}?t=${TOKEN}`, { method: "POST", body, headers: { "Content-Type": "text/plain" } });
    return { status: r.status, text: await r.text() };
  };
  const row = async (nm) => JSON.parse((await call("query", "links_panel", "{}")).text).models.find((m) => m.name === nm);
  const draw = async (nm) => JSON.parse((await call("query", "link_draw", "{}")).text).find((m) => m.name === nm);
  const prx = async (k) => (await fetch(`http://127.0.0.1:${p}/mesh?link_proxies=${k}&t=${TOKEN}`)).status;
  return { h, call, row, draw, prx };
}
async function until(f, ms = 20000) {
  const t0 = Date.now();
  for (;;) {
    const v = await f();
    if (v || Date.now() - t0 > ms) return v;
    await sleep(50);
  }
}
const srce = (nm) => { const b = readFileSync(join(tmp, `${nm}.ifc.bimbleview`)).toString("latin1"); return { complete: /"complete":true/.test(b), incomplete: /"complete":false/.test(b), hlod: b.includes("HLOD") }; };

let A = await startHost("link_index_test");
await A.call("action", "new");
const t0 = Date.now();
await A.call("action", "link_model", JSON.stringify({ path: join(tmp, "a.ifc") }));
let r = await until(async () => { const m = await A.row("a.ifc"); return m?.state === "ready" ? m : null; });
const readyMs = Date.now() - t0;
check("ready and indexing at once, its proxies not served yet, its cache incomplete", r?.state === "ready" && r.indexing === true && r.elements === 72 &&
  (await A.prx(0)) === 204 && srce("a").incomplete && !srce("a").hlod, `${readyMs} ms ${JSON.stringify(r)?.slice(0, 120)} ${JSON.stringify(srce("a"))}`);
const g0 = (await A.draw("a.ifc"))?.proxies;
r = await until(async () => { const m = await A.row("a.ifc"); return m && !m.indexing ? m : null; });
const g1 = (await A.draw("a.ifc"))?.proxies;
const report = readFileSync(join(tmp, "a.ifc.log.txt"), "utf8");
check("then indexed: link_draw's proxies moved, its proxies served, its cache complete with HLOD, the report says them",
  r?.state === "ready" && g1 === g0 + 1 && (await A.prx(0)) === 200 && srce("a").complete && srce("a").hlod &&
  /proxies \(HLOD, baked for the 3D view\): \d+ cells, \d+ triangles/.test(report), `${g0} -> ${g1} ${JSON.stringify(srce("a"))}`);

// cancel while indexing
await A.call("action", "link_model", JSON.stringify({ path: join(tmp, "c.ifc") }));
await until(async () => (await A.row("c.ifc"))?.indexing === true);
await sleep(300);
const cr = await A.call("action", "manage_links", JSON.stringify({ op: "cancel", name: "c.ifc" }));
r = await until(async () => { const m = await A.row("c.ifc"); return m && !m.indexing ? m : null; }, 10000);
await sleep(2000);
check("cancel while indexing: still ready, its cache incomplete, nothing more baked", cr.status === 200 && r?.state === "ready" && srce("c").incomplete &&
  !srce("c").hlod && (await A.row("c.ifc")).indexing === false, `${cr.text} ${JSON.stringify(srce("c"))}`);

// a host killed while it bakes b's proxies
await A.call("action", "link_model", JSON.stringify({ path: join(tmp, "b.ifc") }));
await until(async () => (await A.row("b.ifc"))?.indexing === true);
A.h.kill("SIGKILL");
await sleep(200);
check("killed while indexing: b's cache left incomplete (geometry, names)", srce("b").incomplete && !srce("b").hlod, JSON.stringify(srce("b")));
const B = await startHost("link_index_test_2");
await B.call("action", "new");
await B.call("action", "link_model", JSON.stringify({ path: join(tmp, "b.ifc") }));
r = await until(async () => { const m = await B.row("b.ifc"); return m?.state === "ready" ? m : null; });
check("a new host: linked from that cache (the IFC not read), indexing", r?.from_cache === true && r.indexing === true && r.elements === 72, JSON.stringify(r)?.slice(0, 160));
r = await until(async () => { const m = await B.row("b.ifc"); return m && !m.indexing ? m : null; });
check("then completed: its cache complete with HLOD, its proxies served", r?.state === "ready" && srce("b").complete && srce("b").hlod && (await B.prx(0)) === 200, JSON.stringify(srce("b")));
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
