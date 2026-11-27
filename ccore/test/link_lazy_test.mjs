// A link read in place from its cache (ticket SIZE3 D; ADR 0079 amendment): its meshes are decoded only
// when a call needs its geometry, served from the cache's own bytes, and dropped again past the budget.
//   CCORE_PORT_BASE=auto node ccore/test/link_lazy_test.mjs
// A native host (out/tcc/bimble-ccore or $HOST_BIN) with every IFC converted through a cache
// (BIMBLE_LINK_CACHE_MIN=0), a budget of 0 MB of decoded link meshes and a keep time of 1.5 s
// (BIMBLE_LINK_MESH_BUDGET_MB=0, BIMBLE_LINK_KEEP_MS=1500): golden/ifc/frame.ifc linked twice (the second
// time from its cache): ready with nothing decoded; the panel, visibility_sets and GET /mesh?link=0 decode
// nothing (the mesh is the cache's blocks); Box to selection on a linked element and the plan through the
// link decode it (logged with its time), the same /mesh?link=0 bytes; 1.5 s later its meshes are dropped
// (logged); then Box to selection and the plan give the same answers again (decoded again).
// On the code before SIZE3 a cache's meshes were decoded at load and kept: nothing is ever "dropped".
// Prints "N passed, M failed"; exits non-zero on a failure.
import { spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const TOKEN = "link-lazy-token";
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const tmp = mkdtempSync(join(tmpdir(), "ccore-link-lazy-"));
const A = join(tmp, "a.ifc");
copyFileSync(join(CCORE, "golden/ifc/frame.ifc"), A);
const h = spawn(BIN, [String(port("link_lazy_test"))], {
  env: hostEnv({ BIMBLE_TOKEN: TOKEN, BIMBLE_CACHE_DIR: join(tmp, "cache"), BIMBLE_LINK_CACHE_MIN: "0", BIMBLE_LINK_MESH_BUDGET_MB: "0", BIMBLE_LINK_KEEP_MS: "1500" }),
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
const q = async (nm, body = "{}") => JSON.parse((await call("query", nm, body)).text);
const linkMesh = async () => Buffer.from(await (await fetch(`http://127.0.0.1:${p}/mesh?link=0&t=${TOKEN}`)).arrayBuffer());
const count = (re) => (h.log.match(re) ?? []).length;
async function ready() {
  for (let i = 0; i < 400; i++) {
    const m = (await q("links_panel")).models[0];
    if (m && m.state !== "queued" && m.state !== "converting") return m;
    await sleep(50);
  }
  return null;
}

await call("action", "new");
await call("action", "link_model", JSON.stringify({ path: A }));
let m = await ready();
check("linked, its cache written", m?.state === "ready" && m.elements === 72 && !!m.cache, JSON.stringify(m));
await call("action", "manage_links", JSON.stringify({ op: "reload", name: "a.ifc" }));
m = await ready();
check("linked again from its cache", m?.state === "ready" && m.from_cache === true && m.elements === 72, JSON.stringify(m));
const decoded0 = count(/its meshes decoded/g);
await q("visibility_sets");
const vis0 = await q("visibility");
const mesh0 = await linkMesh();
m = (await q("links_panel")).models[0];
check("the panel, visibility_sets and the link's mesh decode nothing", count(/its meshes decoded/g) === decoded0 && m.triangles > 0 && mesh0.length > 52 &&
  mesh0.toString("latin1", 0, 4) === "BMSH" && mesh0.readUInt32LE(4) === 4, `${count(/its meshes decoded/g)} ${m.triangles} ${mesh0.length}`);
// geometry through the link: Box to selection around a linked element, the plan's cut
const id = mesh0.toString("latin1", 52, 74);
let r = await call("select", "-", JSON.stringify({ link: "a.ifc", ids: [id] }));
const bx = await call("action", "box_to_selection");
const vis1 = await q("visibility");
const box1 = JSON.stringify(vis1.section_box);
check("Box to selection around a linked element decodes it", r.status === 200 && bx.status === 200 && vis1.section_box?.on === true &&
  count(/its meshes decoded/g) === decoded0 + 1, `${r.text} ${box1}`);
await call("action", "section_box", JSON.stringify({ on: false })).catch(() => null);
const ov = await q("open_views"), plan = ov.views.find((v) => v.type === "plan");
const svg1 = plan ? JSON.parse((await call("query", "view_svg", JSON.stringify({ view: plan.id, bare: true }))).text).svg : "";
check("the plan draws the link's cut", (svg1.match(/<path/g) ?? []).length > 10, String((svg1.match(/<path/g) ?? []).length));
const mesh1 = await linkMesh();
check("the link's mesh once decoded: the same bytes", mesh1.equals(mesh0), `${mesh1.length} ${mesh0.length}`);
const line = h.log.split("\n").find((l) => /its meshes decoded/.test(l)) ?? "";
console.log(`  ${line.replace(/^.*link [^:]*: /, "")}`);
await sleep(2200);
check("past the budget, after the keep time: its meshes dropped", count(/its meshes dropped/g) >= 1, h.log.split("\n").filter((l) => /dropped/.test(l)).join(" | "));
r = await call("select", "-", JSON.stringify({ link: "a.ifc", ids: [id] }));
await call("action", "box_to_selection");
const vis2 = await q("visibility");
check("Box to selection again: the same box (decoded again)", JSON.stringify(vis2.section_box) === box1 && count(/its meshes decoded/g) >= decoded0 + 2, JSON.stringify(vis2.section_box));
const svg2 = plan ? JSON.parse((await call("query", "view_svg", JSON.stringify({ view: plan.id, bare: true }))).text).svg : "";
check("the plan again: the same drawing", svg2 === svg1 && svg2.length > 0);
void vis0;
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
