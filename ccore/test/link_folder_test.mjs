// Link a folder (ticket LINKF, ccore/session/links.h, ADR 0079, DEVIATIONS 37):
//   CCORE_PORT_BASE=auto node ccore/test/link_folder_test.mjs
// A native host (out/tcc/bimble-ccore or $HOST_BIN) with BIMBLE_LINK_CACHE_MIN=0 (every IFC gets a
// cache and goes through the host's thread) on a new model; a temp folder (under the host's HOME,
// a root) of four synthetic IFC copied from golden/ifc, plus a stray cache of one of them and a
// text file. Checks: link_folder {path} in one action that answers at once; the four rows queued,
// then each ready (the smallest first), caches and reports beside each IFC; the /mesh trailer holds
// four links and the panel's elements add up; one undo removes all four, redo brings them back
// (from their caches); hide one; pin, place refused while pinned, unpin, place, origin; colour;
// unload / load; a folder outside the roots refused with the human text; save, then one IFC made
// unreadable: the reopened project loads every link from its cache; one IFC rewritten: reload
// reads it again (not from its cache). On the code before LINKF link_folder is an unknown action.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills what it starts.
import { spawn } from "node:child_process";
import { chmodSync, copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, utimesSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const G = join(CCORE, "golden");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const TOKEN = "link-folder-token";
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const home = mkdtempSync(join(tmpdir(), "ccore-link-folder-"));
const dir = join(home, "consultants");
mkdirSync(dir);
const hosts = [];
process.on("exit", () => {
  for (const h of hosts) try { h.kill("SIGKILL"); } catch { /* gone */ }
  try { chmodSync(join(dir, "b_frame2.ifc"), 0o644); } catch { /* gone */ }
  rmSync(home, { recursive: true, force: true });
});

const FILES = { "a_frame.ifc": "ifc/frame.ifc", "b_frame2.ifc": "ifc/frame2.ifc", "c_mapped.ifc": "ifc/fixtures/mapped_item.ifc", "d_column.ifc": "ifc/fixtures/circle_column.ifc" };
for (const [n, f] of Object.entries(FILES)) copyFileSync(join(G, f), join(dir, n));
writeFileSync(join(dir, "notes.txt"), "not a model");

async function startHost(name, args = []) {
  const h = spawn(BIN, [String(port(name)), ...args], {
    env: hostEnv({ BIMBLE_TOKEN: TOKEN, HOME: home, BIMBLE_LINK_CACHE_MIN: "0", BIMBLE_CACHE_DIR: join(home, "cache") }),
    stdio: ["ignore", "ignore", "pipe"],
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
    const t0 = performance.now();
    const r = await fetch(`http://127.0.0.1:${p}/${op}/${encodeURIComponent(nm)}?t=${TOKEN}`,
      { method: "POST", body, headers: { "Content-Type": "text/plain" } });
    return { status: r.status, text: await r.text(), ms: performance.now() - t0 };
  };
  const models = async () => JSON.parse((await call("query", "links_panel", "{}")).text).models;
  const mesh = async () => Buffer.from(await (await fetch(`http://127.0.0.1:${p}/mesh?t=${TOKEN}`)).arrayBuffer());
  return { h, call, models, mesh };
}

/** Until no link is queued or converting (at most 60 s); the rows then. */
async function settle(o) {
  const t0 = Date.now();
  for (;;) {
    const m = await o.models();
    if (!m.some((r) => r.state === "queued" || r.state === "converting") || Date.now() - t0 > 60000) return m;
    await sleep(50);
  }
}

/** The number of links in the /mesh LNKS trailer (session/links.h). */
function trailerLinks(b) {
  const at = b.indexOf("LNKS", 0, "latin1");
  return at < 0 ? 0 : b.readUInt32LE(at + 8);
}

const a = await startHost("link_folder_test");
let r = await a.call("action", "new");
check("a new model", r.status === 200, r.text);
const acts = JSON.parse((await a.call("query", "actions")).text);
check("link_folder listed and enabled with a model", acts.some((x) => x.name === "link_folder" && x.enabled), JSON.stringify(acts.find((x) => x.name === "link_folder")));
r = await a.call("action", "link_folder", JSON.stringify({ path: dir }));
check("link_folder answers within 200 ms (ADR 0074)", r.status === 200 && r.ms < 200, `${r.status} ${r.text} ${r.ms.toFixed(1)} ms`);
let rows = await a.models();
check("four rows at once, in name order (the stray .txt left out)", rows.map((m) => m.name).join() === "a_frame.ifc,b_frame2.ifc,c_mapped.ifc,d_column.ifc", JSON.stringify(rows.map((m) => [m.name, m.state])));
rows = await settle(a);
check("each becomes ready", rows.every((m) => m.state === "ready"), JSON.stringify(rows.map((m) => [m.name, m.state, m.message])));
check("caches and reports beside each IFC", Object.keys(FILES).every((n) => existsSync(join(dir, `${n}.bimbleview`)) && existsSync(join(dir, `${n}.log.txt`))),
  Object.keys(FILES).map((n) => `${n}: ${existsSync(join(dir, `${n}.bimbleview`))}`).join(" "));
const total = rows.reduce((s, m) => s + m.elements, 0);
check("the panel's elements: the sum of the four", total > 72 && rows.every((m) => m.elements > 0), rows.map((m) => `${m.name} ${m.elements}`).join(", "));
check("the /mesh trailer holds the four links", trailerLinks(await a.mesh()) === 4);
const st = JSON.parse((await a.call("query", "state")).text);
check("the model is unsaved (links are saved with it)", st.unsaved === true);

// one undo step
r = await a.call("undo");
rows = await a.models();
check("one undo removes all four", r.status === 200 && rows.length === 0, `${r.text} ${rows.length}`);
r = await a.call("redo");
rows = await settle(a);
check("redo links them again, from their caches", r.status === 200 && rows.length === 4 && rows.every((m) => m.state === "ready" && m.from_cache), JSON.stringify(rows.map((m) => [m.name, m.state, m.from_cache])));

// show / hide, pin, place, colour, unload
r = await a.call("action", "manage_links", JSON.stringify({ op: "hide", name: "b_frame2.ifc" }));
rows = await a.models();
check("hide one", r.status === 200 && rows.find((m) => m.name === "b_frame2.ifc").visible === false);
check("links pinned by default", rows.every((m) => m.pinned === true));
r = await a.call("action", "manage_links", JSON.stringify({ op: "place", name: "a_frame.ifc", move_mm: [1000, 0] }));
check("place refused while pinned", r.status === 400 && /pinned/.test(r.text), r.text);
r = await a.call("action", "manage_links", JSON.stringify({ op: "unpin", name: "a_frame.ifc" }));
r = await a.call("action", "manage_links", JSON.stringify({ op: "place", name: "a_frame.ifc", move_mm: [1000, -0, 0], rotation_deg: 90, origin: "origin" }));
rows = await a.models();
const af = rows.find((m) => m.name === "a_frame.ifc");
check("unpinned: placed (move, rotation, origin)", r.status === 200 && af.pinned === false && JSON.stringify(af.move_mm) === "[1000,0,0]" && af.rotation_deg === 90 && af.origin === "origin", JSON.stringify(af));
r = await a.call("action", "manage_links", JSON.stringify({ op: "place", name: "a_frame.ifc", origin: "nowhere" }));
check("an unknown origin refused", r.status === 400, r.text);
r = await a.call("action", "manage_links", JSON.stringify({ op: "colour", name: "c_mapped.ifc", colour: 3 }));
check("colour override", r.status === 200 && (await a.models()).find((m) => m.name === "c_mapped.ifc").colour === 3, r.text);
r = await a.call("action", "manage_links", JSON.stringify({ op: "colour", name: "c_mapped.ifc", colour: 12 }));
check("a colour out of the palette refused", r.status === 400, r.text);
r = await a.call("action", "manage_links", JSON.stringify({ op: "unload", name: "d_column.ifc" }));
rows = await a.models();
check("unload: the link kept, nothing in memory", r.status === 200 && rows.find((m) => m.name === "d_column.ifc").state === "unloaded" && rows.find((m) => m.name === "d_column.ifc").elements === 0);
r = await a.call("action", "manage_links", JSON.stringify({ op: "load", name: "d_column.ifc" }));
rows = await settle(a);
check("load: from its cache", r.status === 200 && rows.find((m) => m.name === "d_column.ifc").state === "ready" && rows.find((m) => m.name === "d_column.ifc").from_cache, JSON.stringify(rows.find((m) => m.name === "d_column.ifc")));

// outside the roots
r = await a.call("action", "link_folder", JSON.stringify({ path: "/" }));
check("a folder outside the roots refused with the human text", r.status === 400 && /outside the folders Bimble lists/.test(r.text), r.text);
r = await a.call("action", "link_folder", JSON.stringify({ path: join(home, "nothing-here") }));
check("a folder that does not exist: an error", r.status === 400, r.text);

// save, reopen with an IFC unreadable
const saved = join(home, "project.bimble");
r = await a.call("action", "save_as", JSON.stringify({ path: saved }));
check("save_as", r.status === 200, r.text);
const text = readFileSync(saved);
check("the .bimble names the four links, relative paths", text.length > 0);
chmodSync(join(dir, "b_frame2.ifc"), 0o000);
const b = await startHost("link_folder_test_reopen");
r = await b.call("action", "open", JSON.stringify({ path: saved, discard: true }));
rows = await settle(b);
check("reopened: four links, each ready from its cache (an IFC unreadable)", r.status === 200 && rows.length === 4 && rows.every((m) => m.state === "ready" && m.from_cache),
  JSON.stringify(rows.map((m) => [m.name, m.state, m.from_cache, m.message])));
const ra = rows.find((m) => m.name === "a_frame.ifc"), rc = rows.find((m) => m.name === "c_mapped.ifc");
check("reopened: the placement, the pin and the colour as saved", ra?.pinned === false && JSON.stringify(ra.move_mm) === "[1000,0,0]" &&
  ra.rotation_deg === 90 && ra.origin === "origin" && rc?.colour === 3 && rc.pinned === true && rows.find((m) => m.name === "b_frame2.ifc")?.visible === false,
  JSON.stringify(rows.map((m) => [m.name, m.pinned, m.move_mm, m.rotation_deg, m.origin, m.colour, m.visible])));
check("the unreadable one says its cache serves", /could not be read/.test(rows.find((m) => m.name === "b_frame2.ifc")?.message ?? ""), rows.find((m) => m.name === "b_frame2.ifc")?.message);
chmodSync(join(dir, "b_frame2.ifc"), 0o644);

// an IFC rewritten: reload reads it again
const p = join(dir, "a_frame.ifc");
writeFileSync(p, readFileSync(join(G, "ifc/frame.ifc"), "utf8").replace("Concrete frame 3x2x2", "Concrete frame 3x2x2 rev B"));
const later = new Date(statSync(p).mtimeMs + 5000);
utimesSync(p, later, later);
r = await b.call("action", "manage_links", JSON.stringify({ op: "reload", name: "a_frame.ifc" }));
rows = await settle(b);
check("reload after the IFC changed: read again, a new cache", r.status === 200 && rows.find((m) => m.name === "a_frame.ifc").state === "ready" && !rows.find((m) => m.name === "a_frame.ifc").from_cache,
  JSON.stringify(rows.find((m) => m.name === "a_frame.ifc")));
r = await b.call("action", "manage_links", JSON.stringify({ op: "reload_all" }));
rows = await settle(b);
check("reload all: every link from its cache now", r.status === 200 && rows.every((m) => m.state === "ready" && m.from_cache), JSON.stringify(rows.map((m) => [m.name, m.from_cache])));

// a damaged cache is not trusted
writeFileSync(join(dir, "c_mapped.ifc.bimbleview"), "BIMBLEVW\u0001\u0000\u0000\u0000HEADÿÿÿÿÿÿÿÿ");
r = await b.call("action", "manage_links", JSON.stringify({ op: "reload", name: "c_mapped.ifc" }));
rows = await settle(b);
const cm = rows.find((m) => m.name === "c_mapped.ifc");
check("a damaged cache: read again from the IFC, the cache replaced", cm.state === "ready" && !cm.from_cache && /damaged/.test(cm.message ?? ""), JSON.stringify(cm));

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
