// A link's tile pyramid served (ticket TILE1, ADR 0078 amendment 6: ccore/src/tiles.h, session/mesh.h cc_link_tiles,
// cc_mesh_tiles, cc_tiles_frame), read by the page's decoder (web-ui/src/gl/tiles.ts):
//   CCORE_PORT_BASE=auto node ccore/test/link_tiles_test.mjs
// A native host (out/tcc/bimble-ccore or $HOST_BIN), every IFC through a cache (BIMBLE_LINK_CACHE_MIN=0), indexing
// delayed 1.5 s (BIMBLE_LINK_INDEX_DELAY_MS):
//   - golden/ifc/frame.ifc linked: while it indexes, its tiles answer 202 "indexing"; once indexed, its cache holds a
//     section TILE (SRCE complete, format 7), GET /mesh?link=0&tiles=toc reads back in tiles.ts (every element in one
//     leaf), every tile asked in one request decodes (leaves: TLF1, their triangles the table's; proxies: TPX1 in their
//     box), the stale-request rule (another name: 204 X-Superseded), bad ids 400, the report says the pyramid;
//   - the walker's frame (GET /mesh?tiles_frame): the links' world box, a far view drawing the coarsest tile alone, a
//     close one drawing every leaf;
//   - a format 6 cache (the previous format, complete, no TILE): linked from it (the IFC not read) and re-indexed:
//     format 7 with its TILE;
//   - a small link without a cache (an IFC under 1 MiB read as it is): its pyramid built at once;
//   - the open model (bench/mesh_heavy_model.py 160 x 600: several leaves): GET /mesh?tiles=toc&step=N answers 202
//     "done total" (a run of leaves a call), then its table; its tiles decode;
//   - tiles.ts on damaged bytes: 2,000 damaged tables and answers throw or read, never hang.
// Fails on the code before TILE1: no section TILE, no tiles route (404 / the link's mesh).
import { execFileSync, spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { port } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const T = await import(pathToFileURL(join(CCORE, "../web-ui/src/gl/tiles.ts")).href);
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const TOKEN = "link-tiles-token";
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const tmp = mkdtempSync(join(tmpdir(), "ccore-link-tiles-"));
const hosts = [];
process.on("exit", (code) => {
  if (code) for (const h of hosts) console.log(`host log (tail):\n${h.log.slice(-2000)}`);
  for (const h of hosts) try { h.kill("SIGKILL"); } catch { /* gone */ }
  rmSync(tmp, { recursive: true, force: true });
});
copyFileSync(join(CCORE, "golden/ifc/frame.ifc"), join(tmp, "a.ifc"));
copyFileSync(join(CCORE, "golden/ifc/frame.ifc"), join(tmp, "small.ifc"));

async function startHost(name, env = {}, args = []) {
  const h = spawn(BIN, [String(port(name)), ...args], {
    env: hostEnv({ BIMBLE_TOKEN: TOKEN, BIMBLE_CACHE_DIR: join(tmp, "cache"), BIMBLE_LINK_INDEX_DELAY_MS: "1500", ...env }),
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
  const get = async (q) => {
    const r = await fetch(`http://127.0.0.1:${p}/mesh?t=${TOKEN}&${q}`, { cache: "no-store" });
    return { status: r.status, headers: r.headers, bytes: new Uint8Array(await r.arrayBuffer()) };
  };
  return { h, call, row, get };
}
async function until(f, ms = 20000) {
  const t0 = Date.now();
  for (;;) {
    const v = await f();
    if (v || Date.now() - t0 > ms) return v;
    await sleep(50);
  }
}
const cacheOf = (nm) => join(tmp, `${nm}.bimbleview`); // beside the IFC (the folder can be written)
const text = (b) => Buffer.from(b).toString("latin1");

/** Every tile of a table asked in one request and decoded; returns what they hold. */
async function allTiles(H, who, toc) {
  const ids = [...Array(toc.ntiles).keys()].join(",");
  const r = await H.get(`${who}&tile=${ids}`);
  if (r.status !== 200) return { ok: false, why: `status ${r.status} ${text(r.bytes)}` };
  const ans = T.parseTils(r.bytes);
  const seen = new Map();
  let leafTris = 0, ok = ans.length === toc.ntiles, why = "";
  for (const a of ans) {
    const f = toc.flags[a.id];
    if (a.kind === 1) {
      const leaf = T.decodeLeaf(a.bytes);
      leafTris += leaf.triangles;
      if (leaf.triangles !== toc.tris[a.id]) ok = false, why = `leaf ${a.id}: ${leaf.triangles} triangles, the table says ${toc.tris[a.id]}`;
      for (const row of leaf.rows) seen.set(row, (seen.get(row) ?? 0) + 1);
      const want = toc.list.subarray(toc.el0[a.id], toc.el0[a.id] + toc.nel[a.id]);
      if (want.join() !== [...leaf.rows].join()) ok = false, why = `leaf ${a.id}: its rows`;
      if (!(f & T.TILE_LEAF)) ok = false, why = `tile ${a.id}: kind 1 for flags ${f}`;
    } else if (a.kind === 2) {
      const p = T.decodeProxy(a.bytes);
      const pos = T.proxyPositions(p, toc.box.subarray(6 * a.id, 6 * a.id + 6));
      if (p.ni / 3 !== toc.tris[a.id]) ok = false, why = `proxy ${a.id}: triangles`;
      for (let v = 0; v < pos.length; v++) { const k = v % 3, b = toc.box; if (pos[v] < b[6 * a.id + k] - 1e-3 || pos[v] > b[6 * a.id + 3 + k] + 1e-3) ok = false, why = `proxy ${a.id} outside its box`; }
    }
  }
  return { ok, why, leafTris, seen, ans };
}

// ---- a link through a cache, indexed ----
let A = await startHost("link_tiles_test", { BIMBLE_LINK_CACHE_MIN: "0" });
await A.call("action", "new");
await A.call("action", "link_model", JSON.stringify({ path: join(tmp, "a.ifc") }));
let r = await until(async () => { const m = await A.row("a.ifc"); return m?.state === "ready" ? m : null; });
let g = await A.get("link=0&name=a.ifc&tiles=toc");
check("while it indexes: its tiles answer 202 indexing", r?.indexing === true && g.status === 202 && text(g.bytes) === "indexing", `${g.status} ${text(g.bytes)}`);
r = await until(async () => { const m = await A.row("a.ifc"); return m && !m.indexing ? m : null; });
const cache = readFileSync(cacheOf("a.ifc"));
check("indexed: its cache holds a section TILE, complete, format 7", text(cache).includes("TILE") && /"complete":true/.test(text(cache)) && text(cache).includes("bimble-link-cache 7"));
g = await A.get("link=0&name=a.ifc&tiles=toc");
let toc;
try { toc = T.parseToc(g.bytes, 72); } catch (e) { toc = null; console.log(e.message); }
check("its table of contents reads back in tiles.ts (72 elements, every one in a leaf)", g.status === 200 && toc && toc.nlist === 72 && toc.ntiles >= 1,
  toc ? `${toc.ntiles} tiles, ${toc.levels} levels, ${g.bytes.length} bytes` : `${g.status}`);
if (toc) {
  const all = await allTiles(A, "link=0&name=a.ifc", toc);
  const once = [...all.seen.values()].every((n) => n === 1) && all.seen.size === 72;
  check("every tile in one request decodes: leaves' rows and triangles as the table says, each element once, proxies in their box",
    all.ok && once && all.leafTris === toc.detail[0], `${all.why} leaf triangles ${all.leafTris} of ${toc.detail[0]}`);
}
g = await A.get("link=0&name=a.ifc&tiles=occ");
let occ = null;
try { occ = T.parseOcc(g.bytes, toc); } catch (e) { console.log(e.message); }
check("its occluders apart (tiles=occ), as the table counts them", g.status === 200 && occ && occ.length === 9 * toc.nocc, `${toc?.nocc} triangles`);
g = await A.get("link=0&name=b.ifc&tiles=toc");
check("another name: 204 superseded (never 404)", g.status === 204 && g.headers.get("X-Superseded") === "1", `${g.status}`);
g = await A.get("link=0&name=a.ifc&tile=99999");
check("a tile id past the table: 400", g.status === 400, `${g.status} ${text(g.bytes)}`);
g = await A.get("link=7&tiles=toc");
check("a bare index of no link: 404", g.status === 404, `${g.status}`);
const report = readFileSync(join(tmp, "a.ifc.log.txt"), "utf8");
check("the report says the pyramid", /tile pyramid \(TILE, for drawing by tiles\): \d+ tiles in \d+ levels/.test(report));
// the walker's frame
g = await A.get("tiles_frame=bounds");
const bounds = JSON.parse(text(g.bytes)).bounds;
const c = [0, 1, 2].map((k) => (bounds[k] + bounds[3 + k]) / 2), size = Math.max(bounds[3] - bounds[0], bounds[4] - bounds[1], 1);
const view = async (d, err = 1) => JSON.parse(text((await A.get(`tiles_frame=1&eye=${c[0] - d * size},${c[1] - d * size},${c[2] + d * size / 2}&at=${c.join(",")}&fov=60&w=1600&h=1000&err=${err}&leaf_bytes=1`)).bytes));
const far = await view(200), close = await view(0.7);
check("the walker's frame: far, the coarsest tile alone; close, every leaf", g.status === 200 && far.tiles === 1 && far.links[0].proxies + far.links[0].leaves === 1 &&
  close.triangles === close.detail && close.leaf_bytes > 0, `bounds ${bounds.map(Math.round)}; far ${far.triangles} tris ${far.bytes} B; close ${close.triangles} tris ${close.bytes} B`);

// ---- a format 6 cache: re-indexed, not read again ----
A.h.kill("SIGKILL");
const c6 = Buffer.from(readFileSync(cacheOf("a.ifc")));
const at = c6.indexOf("bimble-link-cache 7");
c6.write("bimble-link-cache 6", at, "latin1");
writeFileSync(cacheOf("a.ifc"), c6);
const B = await startHost("link_tiles_test_2", { BIMBLE_LINK_CACHE_MIN: "0" });
await B.call("action", "new");
await B.call("action", "link_model", JSON.stringify({ path: join(tmp, "a.ifc") }));
r = await until(async () => { const m = await B.row("a.ifc"); return m?.state === "ready" ? m : null; });
check("a format 6 cache: linked from it (the IFC not read), indexing", r?.from_cache === true && r.indexing === true, JSON.stringify(r)?.slice(0, 160));
r = await until(async () => { const m = await B.row("a.ifc"); return m && !m.indexing ? m : null; });
const c7 = text(readFileSync(cacheOf("a.ifc")));
g = await B.get("link=0&tiles=toc");
check("then re-indexed: format 7, complete, its TILE served", c7.includes("bimble-link-cache 7") && /"complete":true/.test(c7) && g.status === 200);

// ---- a small link read as it is (no cache): its pyramid built at once ----
await B.call("action", "link_model", JSON.stringify({ path: join(tmp, "small.ifc") }));
// the open model: bench/mesh_heavy_model.py's (golden/frame.json and 160 closed meshes of 600 triangles: several leaves)
execFileSync("python3", [join(CCORE, "bench/mesh_heavy_model.py"), "160", "600", join(tmp, "heavy.json")]);
const C = await startHost("link_tiles_test_3", {}, [join(tmp, "heavy.json")]);
await C.call("action", "link_model", JSON.stringify({ path: join(tmp, "small.ifc") }));
await until(async () => (await C.row("small.ifc"))?.state === "ready");
g = await C.get("link=0&name=small.ifc&tiles=toc");
let st = null;
try { st = T.parseToc(g.bytes, 72); } catch { /* checked below */ }
check("a small link without a cache: its pyramid built at once", g.status === 200 && st && st.nlist === 72, `${g.status}`);

// ---- the open model, in steps ----
const steps = [];
for (let i = 0; i < 200; i++) {
  g = await C.get("tiles=toc&step=1");
  steps.push(g.status);
  if (g.status !== 202) break;
}
let ot = null;
try { ot = T.parseToc(g.bytes); } catch (e) { console.log(e.message); }
check("the open model: 202 while built in steps, then its table", steps[0] === 202 && g.status === 200 && ot && ot.ntiles > 0, `${steps.length} calls`);
if (ot) {
  const ids = [...Array(ot.ntiles).keys()].join(",");
  const ra = await C.get(`tile=${ids}`);
  let ok = ra.status === 200;
  try { for (const a of T.parseTils(ra.bytes)) a.kind === 1 ? T.decodeLeaf(a.bytes) : a.kind === 2 ? T.decodeProxy(a.bytes) : 0; } catch (e) { ok = false, console.log(e.message); }
  check("the open model's tiles decode", ok);
}

// ---- tiles.ts on damaged bytes ----
{
  const goodToc = (await B.get("link=0&tiles=toc")).bytes;
  const tb = toc ? (await B.get(`link=0&tile=${[...Array(T.parseToc(goodToc).ntiles).keys()].join(",")}`)).bytes : new Uint8Array(0);
  let thrown = 0, read = 0, seed = 7;
  const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) >>> 8;
  for (let i = 0; i < 2000; i++) {
    const src = i % 2 ? goodToc : tb;
    if (!src.length) continue;
    const d = new Uint8Array(src);
    for (let k = 0; k < 1 + (i % 3); k++) d[rnd() % d.length] = rnd() & 255;
    const cut = i % 7 === 0 ? d.subarray(0, rnd() % d.length) : d;
    try {
      if (i % 2) T.parseToc(cut);
      else for (const a of T.parseTils(cut)) a.kind === 1 ? T.decodeLeaf(a.bytes) : a.kind === 2 ? T.decodeProxy(a.bytes) : 0;
      read++;
    } catch { thrown++; }
  }
  check("tiles.ts: 2,000 damaged tables and answers throw or read (no hang, no other error)", thrown > 0, `${thrown} thrown, ${read} read`);
  // pack21: a vertex shared by two leaves on one lattice point
  const p = new Float32Array([30000, 12345.678, 1749.97]);
  const a = T.pack21(p, [0, 0, 0, 30000, 20000, 3500]), b = T.pack21(p, [30000, 0, 0, 61000, 20000, 3500]);
  const ua = T.unpack21(a.words, a.k, a.o), ub = T.unpack21(b.words, b.k, b.o);
  check("tiles.ts pack21: a shared vertex lands on one point, within 1/32 mm", [0, 1, 2].every((k) => ua[k] === ub[k] && Math.abs(ua[k] - p[k]) <= 1 / 32 + 1e-4), `${[...ua]} ${[...ub]}`);
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
