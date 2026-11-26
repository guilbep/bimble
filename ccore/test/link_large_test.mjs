// A linked or opened IFC over 256 MiB (ticket LINKF, ADR 0079, DEVIATIONS 37):
//   CCORE_PORT_BASE=auto node ccore/test/link_large_test.mjs
// Generates golden/ifc/frame.ifc padded with many small IFCPROPERTYSINGLEVALUE entities (stepped
// over by the reader) to 270 MiB in a temp folder (not committed), then through a native host
// (out/tcc/bimble-ccore or $HOST_BIN): action open of it read-only (72 elements), link_model of it
// into a new model (queued, converted on the host's thread, then ready with 72 elements, its cache
// and report beside it), linked again (from the cache); and with BIMBLE_MEM_AVAILABLE at 100 MB
// both refused before a byte is read, "too large for this machine". On the code before LINKF the
// open and the link answered "<file>: larger than 268435456 bytes".
// Skipped (said, exit 0) when the temp folder has less than 1 GB free or the machine less than
// 2.5 GB of available memory. Prints "N passed, M failed"; exits non-zero on a failure.
import { spawn } from "node:child_process";
import { closeSync, existsSync, mkdtempSync, openSync, readFileSync, rmSync, statfsSync, writeSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const TOKEN = "link-large-token";
const SIZE = 270 * 1024 * 1024;
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const tmp = mkdtempSync(join(tmpdir(), "ccore-link-large-"));
const hosts = [];
process.on("exit", () => {
  for (const h of hosts) try { h.kill("SIGKILL"); } catch { /* gone */ }
  rmSync(tmp, { recursive: true, force: true });
});

const fs = statfsSync(tmp);
const freeDisk = fs.bavail * fs.bsize;
const avail = Number((readFileSync("/proc/meminfo", "utf8").match(/MemAvailable:\s+(\d+)/) ?? [0, 0])[1]) * 1024;
if (freeDisk < 1e9 || (avail && avail < 2.5e9)) {
  console.log(`skipped: ${(freeDisk / 1e9).toFixed(1)} GB free in ${tmpdir()} (1 GB needed), ` +
    `${(avail / 1e9).toFixed(1)} GB of memory available (2.5 GB needed)`);
  console.log("0 passed, 0 failed (skipped)");
  process.exit(0);
}

// frame.ifc with its DATA padded: entities the display reader steps over, ids above the file's.
const BIG = join(tmp, "big.ifc");
{
  const src = readFileSync(join(CCORE, "golden/ifc/frame.ifc"), "utf8");
  const end = src.lastIndexOf("ENDSEC;");
  const fd = openSync(BIG, "w");
  writeSync(fd, src.slice(0, end));
  let id = 1000000, written = end;
  while (written < SIZE) {
    let s = "";
    for (let i = 0; i < 4096; i++) s += `#${id++}=IFCPROPERTYSINGLEVALUE('Pad',$,IFCLABEL('x'),$);\n`;
    writeSync(fd, s);
    written += s.length;
  }
  writeSync(fd, src.slice(end));
  closeSync(fd);
}

async function startHost(name, env = {}) {
  const h = spawn(BIN, [String(port(name))], {
    env: hostEnv({ BIMBLE_TOKEN: TOKEN, BIMBLE_CACHE_DIR: join(tmp, "cache"), ...env }), stdio: ["ignore", "ignore", "pipe"],
  });
  h.log = "";
  h.stderr.on("data", (d) => (h.log += d));
  hosts.push(h);
  for (let i = 0; i < 400 && !h.log.includes("listening"); i++) {
    if (h.exitCode !== null) break;
    await sleep(10);
  }
  if (!h.log.includes("listening")) throw new Error(`host did not start: ${h.log}`);
  const p = Number(h.log.match(/listening (\d+)/)[1]);
  const call = async (op, nm = "-", body = "") => {
    const r = await fetch(`http://127.0.0.1:${p}/${op}/${encodeURIComponent(nm)}?t=${TOKEN}`,
      { method: "POST", body, headers: { "Content-Type": "text/plain" } });
    return { status: r.status, text: await r.text() };
  };
  const panel = async () => JSON.parse((await call("query", "links_panel", "{}")).text).models;
  return { h, call, panel };
}

/** Waits until the link `name` leaves queued/converting (at most `ms`). */
async function settle(o, name, ms = 120000) {
  const t0 = Date.now();
  for (;;) {
    const row = (await o.panel()).find((m) => m.name === name);
    if (!row || !["queued", "converting"].includes(row.state) || Date.now() - t0 > ms) return row;
    await sleep(100);
  }
}

const a = await startHost("link_large_test");
let r = await a.call("action", "open", JSON.stringify({ path: BIG, discard: true }));
check("open read-only of a 270 MiB IFC", r.status === 200, r.text);
const tree = JSON.parse((await a.call("query", "scene_tree", "{}")).text);
check("its 72 elements", tree.total === 72, JSON.stringify(tree).slice(0, 200));
r = await a.call("action", "new");
check("a new model", r.status === 200, r.text);
const t0 = Date.now();
r = await a.call("action", "link_model", JSON.stringify({ path: BIG }));
const ms = Date.now() - t0;
check("link_model answers at once (the conversion runs on the host's thread)", r.status === 200 && ms < 1000, `${r.status} ${r.text} ${ms} ms`);
let row = await settle(a, "big.ifc");
check("the link becomes ready with 72 elements", row?.state === "ready" && row.elements === 72, JSON.stringify(row));
check("its cache and report beside it", existsSync(`${BIG}.bimbleview`) && existsSync(`${BIG}.log.txt`) && row?.cache === `${BIG}.bimbleview`,
  JSON.stringify(row));
const report = existsSync(`${BIG}.log.txt`) ? readFileSync(`${BIG}.log.txt`, "utf8") : "";
check("the report names the schema and the classes", /schema: IFC4/.test(report) && /IfcColumn 24/.test(report));
r = await a.call("action", "manage_links", JSON.stringify({ op: "reload", name: "big.ifc" }));
row = await settle(a, "big.ifc");
check("reload: from the cache, the IFC not read", r.status === 200 && row?.state === "ready" && row.from_cache === true && row.elements === 72,
  `${r.text} ${JSON.stringify(row)}`);

const b = await startHost("link_large_test_small", { BIMBLE_MEM_AVAILABLE: String(100 * 1024 * 1024) });
r = await b.call("action", "open", JSON.stringify({ path: BIG, discard: true }));
check("open refused before reading on a machine with 100 MB free", r.status === 400 && /too large for this machine/.test(r.text), r.text);
r = await b.call("action", "new");
r = await b.call("action", "link_model", JSON.stringify({ path: join(CCORE, "golden/ifc/frame.ifc") }));
check("a small IFC still links there", r.status === 200, r.text);
rmSync(`${BIG}.bimbleview`, { force: true }), rmSync(`${BIG}.log.txt`, { force: true });
r = await b.call("action", "link_model", JSON.stringify({ path: BIG }));
row = await settle(b, "big.ifc");
check("link refused: state refused, the reason", r.status === 200 && row?.state === "refused" && /too large for this machine/.test(row.message ?? ""),
  `${r.text} ${JSON.stringify(row)}`);

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
