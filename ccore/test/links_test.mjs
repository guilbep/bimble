// Linked models in the native host (ticket B5, session/links.h): node ccore/test/links_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) on golden/frame.json and checks, through the calls:
// /mesh before any link is the single-model BMSH (no trailer; byte-equal with a second host's);
// link_model of golden/ifc/fixtures/mapped_item.ifc and golden/view/frame2.bimbleview; query links
// (state.rs links_json shape); /mesh = the open model's BMSH unchanged + the LNKS trailer, each
// link's BMSH byte-equal (rev field aside) with the /mesh of a host that opened that file
// read-only; pick/select of a linked element: the change JSON's selection and "linked", query
// element {id, link} answers read-only info, the open model's selection is empty and the edit
// verbs cannot touch it; selecting in the open model drops it; hide (visible false, flag 0 in the
// trailer, select refused), show; reload (same content, in place), the same file linked again
// replaces it; remove; errors (no path, unknown link, unknown id); a new open drops the links;
// the enabled lists (link_model, manage_links on with a model, off without).
// Prints "N passed, M failed"; exits non-zero on a failure. Kills what it starts.
import { spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const G = join(CCORE, "golden");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const TOKEN = "links-test-token";
const PORT = port("links_test", 47396);
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const tmp = mkdtempSync(join(tmpdir(), "ccore-links-"));
const hosts = [];

async function startHost(port, model) {
  const h = spawn(BIN, [String(port), ...(model ? [model] : [])], {
    env: { ...process.env, BIMBLE_TOKEN: TOKEN, CCORE_SEED: "1" }, stdio: ["ignore", "ignore", "pipe"],
  });
  h.log = "";
  h.stderr.on("data", (d) => (h.log += d));
  hosts.push(h);
  for (let i = 0; i < 400 && !h.log.includes("listening"); i++) {
    if (h.exitCode !== null) break;
    await sleep(10);
  }
  if (!h.log.includes("listening")) throw new Error(`host on ${port} did not start: ${h.log}`);
  const call = async (op, name = "-", body = "") => {
    const r = await fetch(`http://127.0.0.1:${port}/${op}/${encodeURIComponent(name)}?t=${TOKEN}`,
      { method: "POST", body, headers: { "Content-Type": "text/plain" } });
    return { status: r.status, text: await r.text() };
  };
  const q = async (name, args = {}) => {
    const r = await call("query", name, JSON.stringify(args));
    if (r.status !== 200) throw new Error(`query ${name}: ${r.status} ${r.text}`);
    return JSON.parse(r.text);
  };
  const mesh = async () => {
    const r = await fetch(`http://127.0.0.1:${port}/mesh?t=${TOKEN}`);
    return { status: r.status, bytes: Buffer.from(await r.arrayBuffer()) };
  };
  const change = async () => (await (await fetch(`http://127.0.0.1:${port}/change?since=0&t=${TOKEN}`)).json()).change;
  return { h, call, q, mesh, change };
}

/** The size of the BMSH at `off` (session/mesh.c). */
function bmshSize(b, off) {
  if (b.toString("latin1", off, off + 4) !== "BMSH") throw new Error(`no BMSH at ${off}`);
  const u = (o) => b.readUInt32LE(off + o);
  return 52 + 48 * u(12) + 12 * u(16) + 4 * u(20) + 32 * u(24);
}
/** The open model's BMSH and the links of the LNKS trailer (session/links.h). */
function parseMesh(b) {
  const main = bmshSize(b, 0);
  const out = { main: b.subarray(0, main), links: [], trailer: b.length > main };
  if (!out.trailer) return out;
  let o = main;
  if (b.toString("latin1", o, o + 4) !== "LNKS") throw new Error("no LNKS");
  if (b.readUInt32LE(o + 4) !== 1) throw new Error("LNKS version");
  const n = b.readUInt32LE(o + 8);
  o += 12;
  for (let i = 0; i < n; i++) {
    const L = b.readUInt32LE(o);
    const name = b.toString("utf8", o + 4, o + 4 + L);
    o += 4 + L + ((4 - (L % 4)) % 4);
    const flags = b.readUInt32LE(o), B = b.readUInt32LE(o + 4);
    o += 8;
    if (o % 4) throw new Error("BMSH not 4-aligned");
    const bm = b.subarray(o, o + B);
    if (bmshSize(b, o) !== B) throw new Error(`link ${name}: BMSH size`);
    o += B;
    out.links.push({ name, visible: (flags & 1) === 1, bmsh: bm });
  }
  if (o !== b.length) throw new Error(`trailing bytes: ${b.length - o}`);
  return out;
}
/** BMSH bytes with the rev field zeroed. */
const norev = (b) => { const c = Buffer.from(b); c.writeUInt32LE(0, 8); return c; };
const ids = (bm) => {
  const n = bm.readUInt32LE(12), r = [];
  for (let e = 0; e < n; e++) r.push(bm.toString("latin1", 52 + 48 * e, 52 + 48 * e + 22));
  return r;
};

try {
  // Each linked file opened by itself in its own host: its /mesh is what the trailer must hold.
  const IFC = join(tmp, "mapped_item.ifc"), VIEW = join(tmp, "frame2.bimbleview");
  copyFileSync(join(G, "ifc/fixtures/mapped_item.ifc"), IFC);
  copyFileSync(join(G, "view/frame2.bimbleview"), VIEW);
  const alone = {};
  for (const [k, f] of [["ifc", IFC], ["view", VIEW], ["frame", join(G, "frame.json")]]) {
    const o = await startHost(PORT, k === "frame" ? f : null);
    if (k !== "frame") {
      // read-only open, as File -> Open (CCORE_MODEL / argv opens editable: use the action)
      const r = await o.call("action", "open", JSON.stringify({ path: f, discard: true }));
      if (r.status !== 200) throw new Error(`open ${f}: ${r.text}`);
    }
    alone[k] = (await o.mesh()).bytes;
    o.h.kill("SIGKILL");
    await new Promise((r) => o.h.once("exit", r));
  }

  const { call, q, mesh, change } = await startHost(PORT, join(G, "frame.json"));
  const enabled = async () => Object.fromEntries((await q("actions")).filter((a) => /link/.test(a.name)).map((a) => [a.name, a.enabled]));
  const en0 = await enabled();
  check("link_model and manage_links enabled with a model (ribbon.rs `loaded`)", en0.link_model === true && en0.manage_links === true, JSON.stringify(en0));
  // link_pdf: ticket L1 (session/links_ref.h), enabled with an editable model (ribbon.rs `_ => loaded`); link_dxf: L2.
  check("link_dxf stays off; link_pdf on with an editable model (ticket L1)", en0.link_dxf === false && en0.link_pdf === true, JSON.stringify(en0));
  const m0 = await mesh();
  const c0 = await change();
  check("no link: /mesh is the single-model BMSH, no trailer", m0.status === 200 && m0.bytes.length === bmshSize(m0.bytes, 0), `${m0.bytes.length} bytes`);
  check("no link: /mesh byte-equal with a fresh host's", m0.bytes.equals(alone.frame));
  check("no link: query links is []", JSON.stringify(await q("links")) === "[]");
  check("no link: the change JSON has no links/linked keys", !("links" in c0) && !("linked" in c0), JSON.stringify(c0));
  const rev0 = c0.rev;

  let r = await call("action", "link_model");
  check("link_model without a path -> 400 (no file dialog)", r.status === 400 && /path/.test(r.text), r.text);
  r = await call("action", "link_model", JSON.stringify({ path: join(tmp, "missing.ifc") }));
  check("link_model of a missing file -> 400, nothing linked", r.status === 400 && /cannot open/.test(r.text) && (await q("links")).length === 0, r.text);

  r = await call("action", "link_model", JSON.stringify({ path: IFC }));
  check("link_model mapped_item.ifc -> 200", r.status === 200, r.text);
  r = await call("action", "link_model", JSON.stringify({ path: VIEW }));
  check("link_model frame2.bimbleview -> 200", r.status === 200, r.text);
  const links = await q("links");
  check("query links: both, in link order, state.rs shape", JSON.stringify(links) ===
    JSON.stringify([{ id: "mapped_item.ifc", kind: "ifc", name: "mapped_item.ifc", visible: true },
      { id: "frame2.bimbleview", kind: "bimbleview", name: "frame2.bimbleview", visible: true }]), JSON.stringify(links));
  const c1 = await change();
  check("change JSON: rev unchanged (no model change), links counter", c1.rev === rev0 && c1.links === 2, JSON.stringify(c1));
  const m1 = await mesh();
  const p1 = parseMesh(m1.bytes);
  check("/mesh: the open model's BMSH unchanged, then the trailer", p1.trailer && p1.main.equals(m0.bytes));
  check("/mesh trailer: two links, names and visible", p1.links.length === 2 && p1.links[0].name === "mapped_item.ifc" &&
    p1.links[1].name === "frame2.bimbleview" && p1.links.every((k) => k.visible), JSON.stringify(p1.links.map((k) => [k.name, k.visible])));
  check("link 0's BMSH = /mesh of mapped_item.ifc opened read-only (rev aside)", norev(p1.links[0].bmsh).equals(norev(alone.ifc)),
    `${p1.links[0].bmsh.length} vs ${alone.ifc.length} bytes`);
  check("link 1's BMSH = /mesh of frame2.bimbleview opened read-only (rev aside)", norev(p1.links[1].bmsh).equals(norev(alone.view)),
    `${p1.links[1].bmsh.length} vs ${alone.view.length} bytes`);
  const lids = ids(p1.links[0].bmsh), vids = ids(p1.links[1].bmsh), mids = ids(p1.main);
  console.log(`elements: model ${mids.length}, mapped_item.ifc ${lids.length}, frame2.bimbleview ${vids.length}; mesh ${m0.bytes.length} -> ${m1.bytes.length} bytes`);
  check("/mesh contains every element of the three files", lids.length > 0 && vids.length > 0 && mids.length > 0);
  const m1b = await mesh();
  check("/mesh with links served again: same bytes", m1b.bytes.equals(m1.bytes));

  // Select a linked element (the page's pick sends this).
  const pickIfc = lids[0], pickView = vids[0];
  await call("select", "-", JSON.stringify([mids[0]]));
  r = await call("select", "-", JSON.stringify({ link: "mapped_item.ifc", ids: [pickIfc] }));
  check("select {link, ids} -> 200", r.status === 200, r.text);
  let c = await change();
  check("change JSON: selection [linked id] and linked {id, link}", JSON.stringify(c.selection) === JSON.stringify([pickIfc]) &&
    c.linked?.id === pickIfc && c.linked?.link === "mapped_item.ifc", JSON.stringify(c));
  const vis = await q("visibility");
  check("the open model's selection is empty (hide_now off)", !(await q("actions")).find((a) => a.name === "hide_now").enabled, JSON.stringify(vis).slice(0, 80));
  let el = await call("query", "element", JSON.stringify({ id: pickIfc, link: "mapped_item.ifc" }));
  let d = el.status === 200 ? JSON.parse(el.text) : null;
  console.log(`linked element (ifc): ${el.text}`);
  check("query element {id, link}: read-only info of the linked element", d && d.read_only === true && d.link === "mapped_item.ifc" &&
    d.id === pickIfc && typeof d.class === "string" && d.class.startsWith("Ifc") && typeof d.name === "string" && d.volume_m3 > 0 &&
    typeof d.family === "string" && JSON.stringify(d.generator_params) === "{}", el.text);
  el = await call("query", "element", JSON.stringify({ id: pickIfc }));
  check("query element without link: not the open model's (no such id there)", el.status === 400, el.text);
  r = await call("verb", "rename", JSON.stringify({ id: pickIfc, name: "edited" }));
  check("rename of the linked element -> 400, rev unchanged", r.status === 400 && (await change()).rev === rev0, r.text);
  r = await call("select", "-", JSON.stringify({ link: "frame2.bimbleview", ids: [pickView] }));
  el = await call("query", "element", JSON.stringify({ id: pickView, link: "frame2.bimbleview" }));
  d = el.status === 200 ? JSON.parse(el.text) : null;
  console.log(`linked element (bimbleview): ${el.text}`);
  check("select and query element in the .bimbleview link", r.status === 200 && d?.link === "frame2.bimbleview" && d.id === pickView &&
    d.storey_name !== undefined && d.volume_m3 > 0, el.text);
  r = await call("select", "-", JSON.stringify({ link: "frame2.bimbleview", ids: ["0000000000000000000000"] }));
  check("select an unknown id in a link -> 400", r.status === 400 && /unknown GlobalIds/.test(r.text), r.text);
  r = await call("select", "-", JSON.stringify({ link: "nope.ifc", ids: [pickView] }));
  check("select in an unknown link -> 400", r.status === 400 && /no linked model/.test(r.text), r.text);
  await call("select", "-", JSON.stringify([mids[0]]));
  c = await change();
  check("a selection in the open model drops the linked one", JSON.stringify(c.selection) === JSON.stringify([mids[0]]) && !("linked" in c), JSON.stringify(c));

  // Hide, show.
  await call("select", "-", JSON.stringify({ link: "mapped_item.ifc", ids: [pickIfc] }));
  r = await call("action", "manage_links", JSON.stringify({ op: "hide", name: "mapped_item.ifc" }));
  c = await change();
  let p = parseMesh((await mesh()).bytes);
  check("hide: 200, visible false in links and the trailer, its selection dropped", r.status === 200 &&
    (await q("links"))[0].visible === false && p.links[0].visible === false && p.links[1].visible && !("linked" in c), r.text);
  r = await call("select", "-", JSON.stringify({ link: "mapped_item.ifc", ids: [pickIfc] }));
  check("select in a hidden link -> 400", r.status === 400 && /hidden/.test(r.text), r.text);
  await call("action", "manage_links", JSON.stringify({ op: "show", name: "mapped_item.ifc" }));
  check("show: visible again", (await q("links"))[0].visible === true);
  r = await call("action", "manage_links");
  check("manage_links with no body answers the list", r.status === 200 && JSON.stringify(JSON.parse(r.text)) === JSON.stringify(await q("links")), r.text);
  r = await call("action", "manage_links", JSON.stringify({ op: "frobnicate", name: "x" }));
  check("manage_links with an unknown op -> 400", r.status === 400, r.text);

  // Reload: in place, same content; the same file again replaces.
  const before = (await mesh()).bytes;
  const g0 = (await change()).links;
  r = await call("action", "manage_links", JSON.stringify({ op: "reload", name: "mapped_item.ifc" }));
  const after = (await mesh()).bytes;
  check("reload: 200, counter + 1, same /mesh bytes, same order", r.status === 200 && (await change()).links === g0 + 1 &&
    after.equals(before) && (await q("links"))[0].name === "mapped_item.ifc", r.text);
  r = await call("action", "link_model", JSON.stringify({ path: IFC }));
  check("the same file linked again replaces it (still two links)", r.status === 200 && (await q("links")).length === 2);
  rmSync(VIEW);
  r = await call("action", "manage_links", JSON.stringify({ op: "reload", name: "frame2.bimbleview" }));
  check("reload of a file gone -> 400, the link kept", r.status === 400 && (await q("links")).length === 2, r.text);

  // Remove.
  r = await call("action", "manage_links", JSON.stringify({ op: "remove", name: "frame2.bimbleview" }));
  p = parseMesh((await mesh()).bytes);
  check("remove: 200, one link left in links and /mesh", r.status === 200 && (await q("links")).length === 1 && p.links.length === 1 &&
    p.links[0].name === "mapped_item.ifc", r.text);
  r = await call("action", "manage_links", JSON.stringify({ op: "remove", name: "frame2.bimbleview" }));
  check("remove again -> 400 no linked model", r.status === 400 && /no linked model/.test(r.text), r.text);
  r = await call("action", "manage_links", JSON.stringify({ op: "remove", name: "mapped_item.ifc" }));
  const m2 = await mesh();
  check("last link removed: /mesh is the single-model BMSH again", r.status === 200 && m2.bytes.equals(m0.bytes));

  // A new open drops the links (lib.rs:1668).
  await call("action", "link_model", JSON.stringify({ path: IFC }));
  r = await call("action", "open", JSON.stringify({ path: join(G, "frame.json"), discard: true }));
  check("open another model drops the links", r.status === 200 && (await q("links")).length === 0 &&
    (await mesh()).bytes.length === bmshSize((await mesh()).bytes, 0), r.text);
  // A read-only scene can take links too (mission 1: superpose files).
  await call("action", "open", JSON.stringify({ path: IFC, discard: true }));
  r = await call("action", "link_model", JSON.stringify({ path: join(G, "frame.json") }));
  p = parseMesh((await mesh()).bytes);
  check("an IFC open read-only takes a .bimble link (its scene)", r.status === 200 && p.links.length === 1 &&
    norev(p.main).equals(norev(alone.ifc)) && norev(p.links[0].bmsh).equals(norev(alone.frame)), r.text);
  await call("action", "close", JSON.stringify({ discard: true }));
  const en1 = await enabled();
  check("nothing open: link_model and manage_links off", en1.link_model === false && en1.manage_links === false, JSON.stringify(en1));
  r = await call("action", "link_model", JSON.stringify({ path: IFC }));
  check("link_model with nothing open -> 400 not enabled", r.status === 400 && /not enabled/.test(r.text), r.text);
} catch (e) {
  check("no exception", false, e.stack ?? String(e));
} finally {
  for (const h of hosts) if (h.exitCode === null) h.kill("SIGKILL");
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
