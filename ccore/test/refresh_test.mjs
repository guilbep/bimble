// The page's refresh after an edit, without a browser (ticket SMALL1): node ccore/test/refresh_test.mjs
// Starts out/tcc/bimble-ccore on a copy of golden/frame.json and drives web-ui/src/querycache.ts (the
// cache bimble.ts puts in front of the client) with synchronous calls, as the page does. After each of
// three edits (place a footing with the place tool, move it, delete it) the page reads what it read in
// the lab after a footing click (lab_concrete, WAVE1B_PROFILE=1: 25 queries of the refresh and the
// panels shown). Checks: the calls from the edit to the last read are at most 3 (the edit, one batch,
// at most one more), every answer read through the cache is byte for byte the query's own answer, and
// a pointer move of a tool keeps the model's answers. Prints the count of the same reads without the
// cache (the page before SMALL1). On the code before SMALL1 the core has no `query batch` and
// querycache.ts does not exist: the test fails.
import { execFileSync, spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const { QueryCache, cachedClient } = await import(join(CCORE, "../web-ui/src/querycache.ts"));
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("refresh_test");
const TOKEN = "refresh-test";
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};

const tmp = mkdtempSync(join(tmpdir(), "small1-refresh-"));
const model = join(tmp, "frame.json");
copyFileSync(join(CCORE, "golden/frame.json"), model);
const host = spawn(BIN, [String(PORT), model], { env: hostEnv({ HOME: tmp, BIMBLE_TOKEN: TOKEN }), stdio: ["ignore", "pipe", "pipe"] });
let hostOut = "";
host.stderr.on("data", (d) => (hostOut += d));
host.stdout.on("data", (d) => (hostOut += d));

/** One synchronous call, as the page's sync XHR: curl. Throws the error text on a status other than 200. */
let transport = 0;
function call(op, name, body = "") {
  transport++;
  const out = execFileSync("curl", ["-s", "-X", "POST", "-H", "Content-Type: text/plain", "--data-binary", "@-", "-w", "\n%{http_code}",
    `http://127.0.0.1:${PORT}/${op}/${name === null ? "-" : encodeURIComponent(name)}?t=${TOKEN}`], { input: body, encoding: "utf8" });
  const k = out.lastIndexOf("\n");
  const status = out.slice(k + 1), text = out.slice(0, k);
  if (status !== "200") throw text || `HTTP ${status}`;
  return text;
}
const raw = {
  query: (n, a) => call("query", n, a), verb: (n, a) => void call("verb", n, a), action: (n) => void call("action", n),
  select: (ids) => void call("select", null, ids), tool: (n, a) => call("tool", n, a), set_placing: (j) => void call("set_placing", null, j),
};
let seq = 0;
async function change() { // the page's long poll (not a call of the click path)
  const r = await fetch(`http://127.0.0.1:${PORT}/change?since=${seq}&t=${TOKEN}`);
  const j = await r.json();
  seq = j.seq;
  return j.change;
}

// What the page read after a footing click (lab_concrete, WAVE1B_PROFILE=1, 26 Nov 2026), in order:
// bimble.ts refresh(), then the panels shown (Navigator, StatusBar, the plan's layers).
const READS = ["state", "actions", "history", "visibility", "open_views", "status", "placing", "measure", "storeys",
  "materials", "project", "north", "setting_out", "reference_planes", "collab", "schedule_list", "sheets", "links",
  "measurements", "links", "view_clip", "visibility_sets", "view_annotations", "view_references", "grid_axes"];
const read = (client) => READS.map((n) => { try { return client.query(n, "{}"); } catch (e) { return `error: ${e}`; } });

try {
  for (let i = 0; i < 250 && !hostOut.includes("listening"); i++) await new Promise((r) => setTimeout(r, 20));
  check("host listening", hostOut.includes("listening"), hostOut.slice(-200));
  const cache = new QueryCache((n, a) => raw.query(n, a));
  const page = cachedClient(raw, cache);
  // the page at load: one change, its reads (they become what the next batch asks for)
  cache.changed(await change());
  read(page);
  page.action("place_footing");
  cache.changed(await change());
  read(page);

  let placed = null;
  const edits = [
    ["place a footing (tool click)", () => { placed = JSON.parse(page.tool("click", JSON.stringify({ at: [20, 20], free: true }))).placed; }],
    ["move it", () => page.verb("move", JSON.stringify({ ids: [placed], d: [100, 0, 0] }))],
    ["delete it", () => page.verb("delete", JSON.stringify({ ids: [placed] }))],
  ];
  const counts = [];
  let rev0 = null;
  for (const [what, edit] of edits) {
    const t0 = transport;
    edit();
    const c = await change();
    cache.changed(c);
    const got = read(page);
    const n = transport - t0;
    counts.push(n);
    const want = read(raw); // the same reads without the cache: the page before SMALL1
    const direct = transport - t0 - n;
    const same = got.every((g, i) => g === want[i]);
    check(`${what}: rev ${rev0} -> ${c.rev}, ${n} calls from the edit to the last read (at most 3; ${direct + 1} without the cache)`,
      (rev0 === null || c.rev === rev0 + 1) && n <= 3, `${n}`);
    rev0 = c.rev;
    check(`${what}: each answer read through the cache is the query's own, byte for byte`, same,
      READS.filter((_, i) => got[i] !== want[i]).join(", "));
  }
  check("the footing was placed (its id)", typeof placed === "string" && placed.length === 22, String(placed));

  // A pointer move of the tool keeps the model's answers (storeys) and drops the session's (placing).
  const t0 = transport;
  page.query("storeys", "{}");
  page.tool("pointer", JSON.stringify({ at: [500, 500] }));
  page.query("storeys", "{}");
  page.query("placing", "{}");
  check("a pointer move: storeys from the cache, placing asked again (2 calls)", transport - t0 === 2, String(transport - t0));
  // A selection keeps the model's answers; a verb (even refused) drops them.
  const t1 = transport;
  page.select("[]");
  page.query("storeys", "{}");
  try { page.verb("rename", JSON.stringify({ id: "0000000000000000000000", name: "x" })); } catch { /* refused: no such element */ }
  page.query("storeys", "{}");
  check("select keeps storeys; a verb drops it (select, verb, storeys: 3 calls)", transport - t1 === 3, String(transport - t1));
} catch (e) {
  check("no exception", false, e?.stack ?? String(e));
} finally {
  host.kill("SIGTERM");
  rmSync(tmp, { recursive: true, force: true });
  console.log(`${passed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
}
