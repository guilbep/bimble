// GET /mesh of the native host (ticket 11a): node ccore/test/mesh_http_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) with CCORE_MODEL=golden/frame.json on 47321, fetches
// /mesh, parses BMSH (layout: host/mesh.c) and checks it against golden/mesh/scene.txt (the
// scene of the same model, cc_scene_build): element count, each row's GlobalId, category and
// vertex / triangle / edge counts, the origin bits, index ranges; the X-Rev header; a second
// fetch is served from the cache (same bytes, one build); 403 on a bad token. Then a host with
// no model on 47322 answers 404. Prints sizes and times, "N passed, M failed"; exits non-zero on
// a failure. No dependencies. Kills the hosts it starts.
import { spawn } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const TOKEN = "meshtest-token";
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const hosts = [];

async function startHost(port, env) {
  const h = spawn(BIN, [String(port)], { env: { ...process.env, BIMBLE_TOKEN: TOKEN, ...env }, stdio: ["ignore", "ignore", "pipe"] });
  h.log = "";
  h.stderr.on("data", (d) => (h.log += d));
  hosts.push(h);
  for (let i = 0; i < 400 && !h.log.includes("listening"); i++) await sleep(5);
  if (!h.log.includes("listening")) throw new Error(`host on ${port} did not start: ${h.log}`);
  return h;
}

/** Parses BMSH (host/mesh.c). Throws on a malformed buffer. */
function parseBmsh(buf) {
  const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  const dv = new DataView(ab);
  const magic = String.fromCharCode(...new Uint8Array(ab, 0, 4));
  const u32 = (o) => dv.getUint32(o, true);
  const h = { magic, version: u32(4), rev: u32(8), nE: u32(12), nV: u32(16), nI: u32(20), nS: u32(24) };
  h.originBits = [0, 1, 2].map((k) => dv.getBigUint64(28 + 8 * k, true).toString(16).padStart(16, "0"));
  h.rows = [];
  let o = 52;
  for (let e = 0; e < h.nE; e++, o += 48) {
    h.rows.push({
      id: String.fromCharCode(...new Uint8Array(ab, o, 22)), cat: dv.getUint8(o + 22), pad: dv.getUint8(o + 23),
      v0: u32(o + 24), nv: u32(o + 28), i0: u32(o + 32), ni: u32(o + 36), s0: u32(o + 40), ns: u32(o + 44),
    });
  }
  h.posOff = o;
  h.pos = new Float32Array(ab, o, 3 * h.nV); o += 12 * h.nV;
  h.idx = new Uint32Array(ab, o, h.nI); o += 4 * h.nI;
  h.edges = new Float32Array(ab, o, 6 * h.nS); o += 24 * h.nS;
  h.normals = new Int8Array(ab, o, 8 * h.nS); o += 8 * h.nS;
  h.end = o;
  return h;
}

const PORT = 47321, NOMODEL = 47322;
try {
  const golden = readFileSync(join(CCORE, "golden/mesh/scene.txt"), "utf8").split("\n");
  const gOrigin = golden[0].split(" ").slice(1);
  const gRows = golden.filter((l) => /^\d{3} /.test(l)).map((l) => {
    const [, id, , cat, , v, i, e] = l.match(/^\d{3} (\S{22}) (\S+) (\d+) (\S+) v=(\d+) i=(\d+) e=(\d+)/);
    return { id, cat: +cat, v: +v, tris: +i, e: +e };
  });
  const host = await startHost(PORT, { CCORE_MODEL: join(CCORE, "golden/frame.json") });
  const url = `http://127.0.0.1:${PORT}/mesh?t=${TOKEN}`;
  let t0 = performance.now();
  const r = await fetch(url);
  const body = Buffer.from(await r.arrayBuffer());
  const firstMs = performance.now() - t0;
  check("GET /mesh -> 200", r.status === 200, String(r.status));
  check("Content-Type application/octet-stream", r.headers.get("content-type") === "application/octet-stream", r.headers.get("content-type"));
  check("X-Rev header is 0", r.headers.get("x-rev") === "0", r.headers.get("x-rev"));
  const m = parseBmsh(body);
  check("magic BMSH, version 1", m.magic === "BMSH" && m.version === 1, `${m.magic} ${m.version}`);
  check("rev in body = X-Rev", String(m.rev) === r.headers.get("x-rev"));
  check("72 elements (scene.txt)", m.nE === 72 && gRows.length === 72, `${m.nE} / ${gRows.length}`);
  const sum = (f) => gRows.reduce((a, g) => a + f(g), 0);
  check("nV = sum of v", m.nV === sum((g) => g.v), `${m.nV} vs ${sum((g) => g.v)}`);
  check("nI = 3 x sum of triangles", m.nI === 3 * sum((g) => g.tris), `${m.nI} vs ${3 * sum((g) => g.tris)}`);
  check("nS = sum of e", m.nS === sum((g) => g.e), `${m.nS} vs ${sum((g) => g.e)}`);
  check("every element v=8 i=12 (triangles)", m.rows.every((x) => x.nv === 8 && x.ni === 36));
  check("origin bits = scene.txt", m.originBits.join(" ") === gOrigin.join(" "), `${m.originBits.join(" ")} vs ${gOrigin.join(" ")}`);
  check("buffer length = layout", m.end === body.length, `${m.end} vs ${body.length}`);
  check("positions block 4-aligned", m.posOff % 4 === 0, String(m.posOff));
  let rowsOk = true, rowErr = "";
  let v0 = 0, i0 = 0, s0 = 0;
  m.rows.forEach((x, k) => {
    const g = gRows[k];
    const ok = x.id === g.id && x.cat === g.cat && x.pad === 0 && x.nv === g.v && x.ni === 3 * g.tris && x.ns === g.e &&
      x.v0 === v0 && x.i0 === i0 && x.s0 === s0;
    if (!ok && rowsOk) rowErr = `row ${k}: ${JSON.stringify(x)} vs ${JSON.stringify(g)}`;
    rowsOk &&= ok;
    v0 += x.nv, i0 += x.ni, s0 += x.ns;
  });
  check("rows = scene.txt (id, category, counts, contiguous ranges)", rowsOk, rowErr);
  let idxOk = true;
  for (const x of m.rows) for (let i = x.i0; i < x.i0 + x.ni; i++) idxOk &&= m.idx[i] >= x.v0 && m.idx[i] < x.v0 + x.nv;
  check("indices stay in their element's vertex range", idxOk);
  check("positions finite", m.pos.every(Number.isFinite) && m.edges.every(Number.isFinite));
  t0 = performance.now();
  const r2 = await fetch(url);
  const body2 = Buffer.from(await r2.arrayBuffer());
  const secondMs = performance.now() - t0;
  check("second fetch: same bytes", body2.equals(body));
  const builds = (host.log.match(/mesh rev \d+ built in/g) ?? []).length;
  check("scene built once (cache)", builds === 1, `${builds} builds`);
  const buildLine = host.log.match(/mesh rev \d+ built in ([\d.]+) ms, (\d+) bytes/);
  check("bad token -> 403", (await fetch(`http://127.0.0.1:${PORT}/mesh?t=nope`)).status === 403);
  check("no token -> 403", (await fetch(`http://127.0.0.1:${PORT}/mesh`)).status === 403);
  const bad = await fetch(url, { headers: { Origin: "http://evil.example" } });
  check("bad Origin -> 403", bad.status === 403, String(bad.status));
  const good = await fetch(url, { headers: { Origin: "http://localhost:5173" } });
  check("allowed Origin: ACAO and exposes X-Rev", good.status === 200 && good.headers.get("access-control-allow-origin") === "http://localhost:5173" &&
    /X-Rev/.test(good.headers.get("access-control-expose-headers") ?? ""));
  console.log(`mesh: ${body.length} bytes; host build ${buildLine?.[1]} ms; first fetch ${firstMs.toFixed(2)} ms, cached fetch ${secondMs.toFixed(2)} ms`);

  await startHost(NOMODEL, { CCORE_MODEL: "" });
  const nm = await fetch(`http://127.0.0.1:${NOMODEL}/mesh?t=${TOKEN}`);
  const nmText = await nm.text();
  check("no model -> 404 text", nm.status === 404 && /no model/.test(nmText), `${nm.status} ${nmText}`);
} catch (e) {
  check("no exception in the test", false, e.stack ?? String(e));
  for (const h of hosts) console.log(`--- host ---\n${h.log.slice(-2000)}`);
} finally {
  for (const h of hosts) if (h.exitCode === null) h.kill("SIGTERM");
  await sleep(100);
  for (const h of hosts) if (h.exitCode === null) h.kill("SIGKILL");
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
