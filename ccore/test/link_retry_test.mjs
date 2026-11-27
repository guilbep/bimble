// A link refused for memory, converted again by itself once the memory is free, and "Convert now"
// (ticket SIZE3 F; ADR 0079 decision 16):
//   CCORE_PORT_BASE=auto node ccore/test/link_retry_test.mjs
// A native host (out/tcc/bimble-ccore or $HOST_BIN) whose free memory is read from a file
// (BIMBLE_MEM_AVAILABLE=@<file>, 1 MB at first) and with every IFC converted through a cache
// (BIMBLE_LINK_CACHE_MIN=0): golden/ifc/frame.ifc linked is refused ("too large for this machine");
// the file then says 8 GB: within 6 s the link is ready (72 elements) without a call; another link
// refused while the memory is short stays refused (its retry waits for the memory), and
// manage_links reload (the Links panel's "Convert now") converts it once the memory is there.
// On the code before SIZE3 the automatic retry never ran when nothing else was converting (the host
// looked at its links only while one was queued or converting): the first link stayed refused.
// Prints "N passed, M failed"; exits non-zero on a failure.
import { spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const TOKEN = "link-retry-token";
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const tmp = mkdtempSync(join(tmpdir(), "ccore-link-retry-"));
const MEM = join(tmp, "mem.txt");
writeFileSync(MEM, String(1024 * 1024));
const A = join(tmp, "a.ifc"), B = join(tmp, "b.ifc");
copyFileSync(join(CCORE, "golden/ifc/frame.ifc"), A);
copyFileSync(join(CCORE, "golden/ifc/frame.ifc"), B);
const h = spawn(BIN, [String(port("link_retry_test"))], {
  env: hostEnv({ BIMBLE_TOKEN: TOKEN, BIMBLE_CACHE_DIR: join(tmp, "cache"), BIMBLE_LINK_CACHE_MIN: "0", BIMBLE_MEM_AVAILABLE: `@${MEM}` }),
  stdio: ["ignore", "ignore", "pipe"],
});
h.log = "";
h.stderr.on("data", (d) => (h.log += d));
process.on("exit", () => {
  try { h.kill("SIGKILL"); } catch { /* gone */ }
  rmSync(tmp, { recursive: true, force: true });
});
for (let i = 0; i < 400 && !h.log.includes("listening"); i++) await sleep(25);
const p = Number(h.log.match(/listening (\d+)/)?.[1]);
const call = async (op, nm, body = "") => {
  const r = await fetch(`http://127.0.0.1:${p}/${op}/${encodeURIComponent(nm)}?t=${TOKEN}`, { method: "POST", body, headers: { "Content-Type": "text/plain" } });
  return { status: r.status, text: await r.text() };
};
const row = async (name) => JSON.parse((await call("query", "links_panel", "{}")).text).models.find((m) => m.name === name);
/** Until `name` is neither queued nor converting (at most `ms`): its row. */
async function settle(name, ms = 20000) {
  const t0 = Date.now();
  for (;;) {
    const m = await row(name);
    if ((m && m.state !== "queued" && m.state !== "converting") || Date.now() - t0 > ms) return m;
    await sleep(50);
  }
}
/** Until `name` is in state `want` (at most `ms`): its row and the time taken. */
async function until(name, want, ms) {
  const t0 = Date.now();
  for (;;) {
    const m = await row(name);
    if (m?.state === want || Date.now() - t0 > ms) return { m, ms: Date.now() - t0 };
    await sleep(100);
  }
}

let r = await call("action", "new");
check("a new model", r.status === 200, r.text);
r = await call("action", "link_model", JSON.stringify({ path: A }));
let m = await settle("a.ifc");
check("a.ifc refused with 1 MB free", r.status === 200 && m?.state === "refused" && /too large for this machine/.test(m.message ?? ""), JSON.stringify(m));
await sleep(2500);
m = await row("a.ifc");
check("still refused while the memory stays short", m?.state === "refused", JSON.stringify(m));
writeFileSync(MEM, String(8 * 1024 ** 3));
const got = await until("a.ifc", "ready", 6000);
check("the memory frees: a.ifc converted by itself, no call", got.m?.state === "ready" && got.m.elements === 72, `${got.ms} ms ${JSON.stringify(got.m)}`);

writeFileSync(MEM, String(1024 * 1024));
r = await call("action", "link_model", JSON.stringify({ path: B }));
m = await settle("b.ifc");
check("b.ifc refused with 1 MB free", m?.state === "refused", JSON.stringify(m));
r = await call("action", "manage_links", JSON.stringify({ op: "reload", name: "b.ifc" }));
m = await settle("b.ifc");
check("Convert now while the memory is short: refused again, said", m?.state === "refused" && /too large for this machine/.test(m.message ?? ""), `${r.text} ${JSON.stringify(m)}`);
writeFileSync(MEM, String(8 * 1024 ** 3));
r = await call("action", "manage_links", JSON.stringify({ op: "reload", name: "b.ifc" }));
m = await settle("b.ifc");
check("Convert now with the memory free: ready", r.status === 200 && m?.state === "ready" && m.elements === 72, `${r.text} ${JSON.stringify(m)}`);

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
