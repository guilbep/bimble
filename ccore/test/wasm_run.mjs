// The C core as wasm32 (ticket 13): node ccore/test/wasm_run.mjs
// Instantiates out/wasm/ccore.wasm (build.sh wasm; no imports) and replays the goldens through its
// exports (src/wasm_api.h), byte for byte, like the native suites:
//   floats   golden/floats/floats.txt: fmt_f64 in both formats, parse_f64 of both texts (bits)
//   query    golden/query.txt and golden/frame2/query.txt
//   verbs    golden/verbs.txt, verbs_after_undo.json, verbs_after.json (masking as verbs_test.c)
//   file     frame.json and frame2.json: open_text then save gives the same bytes
//   ifc      golden/ifc/frame.ifc -> golden/ifc/frame_reimport.json (masking as ifc_in_test.c)
//   mesh     mesh(local) of frame and frame2 against <k>.txt and scene.txt; mesh(world) against
//            the local meshes placed in JS with f32 arithmetic (scene.c cc_scene_world_mesh)
//   memory   1000 x (open, mesh, close) and 100 x open_ifc: no growth after the first round
//   host     the host call layer in the module (call, change_json, host_open_text, host_mesh,
//            host_saved; ticket T13b) against the native host (out/tcc/bimble-ccore or $HOST_BIN,
//            port <port>, CCORE_SEED=1 and guid_seed(1) on both): boot without a model (state,
//            actions: 61 rows: Rust's 54 + ISS72's 2 + ISS71's 2 + SETOUT's 3), open frame.json, the same calls on both (select, verb place,
//            history, undo, redo, action save…): every answer, status and change JSON byte for
//            byte, the saved text = the native host's saved file, host_mesh = GET /mesh; ticket E1:
//            query verbs (= golden/verbs_list.json) and describe, verb with dry_run and if_rev, rename;
//            ticket B4: the place tool (tool pointer/click/cancel, query ghost) on both, byte for byte;
//            ticket B3: the visibility actions, query visibility(_sets), the box tool (box_set, box_press,
//            box_move, box_release, box_cancel) on both, byte for byte, rev unchanged; ticket B5:
//            linked models (link_model by path natively, host_open flag 4 in the module; links,
//            select/element of a linked element, manage_links, /mesh with the LNKS trailer) on both;
//            ticket C1: views and tabs (open_views, tab open/activate/close, view_clip, the place
//            tool with an orthographic ray in a plan) on both, byte for byte
//   files    the file actions in a fresh module (ticket A5): new, place, save (v2), save as,
//            host_open of the saved bytes (the same elements), host_open import of golden/ifc/
//            frame.ifc (the import golden's count) and of frame.json; sheet_svg of drawing/model.json
//            (ticket D5) = golden/drawing/sheet/S-101.svg; export IFC4 with host_clock =
//            golden/ifc/frame.ifc; save as y.json = the v1 text; save as f.bimble = gzip of
//            golden/bin/frame.cbor; read-only files (ticket A5c): host_open of golden/ifc/fixtures/
//            mapped_item.ifc (host_mesh count = golden/ifc/read/mapped_item/scene.txt's, editable
//            false, verb place refused with Rust's text), share_for_viewing of frame.json
//            (inflated = golden/view/frame.bimbleview.raw) and host_open of it; shared again (the
//            same raw stream) and share_for_viewing of the read-only mapped_item.ifc (opens with 5
//            elements; ticket V1); close
//   bounds   a binary .bimble whose CBOR array count wraps size_t on wasm32 (ticket FIXLIB): refused
// Prints per-suite "N passed, M failed", the module size and timings; exits non-zero on a failure.
// No dependencies.
import { spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { gunzipSync, gzipSync } from "node:zlib";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs"; // FIXSES: never the caller's BIMBLE_CONFIG_DIR

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const G = join(CCORE, "golden");
const WASM = process.env.CCORE_WASM ?? join(CCORE, "out/wasm/ccore.wasm");
const read = (p) => readFileSync(join(G, p), "utf8");
const enc = new TextEncoder(), dec = new TextDecoder("utf-8", { fatal: true });

// ---- the module ----
const bytes = readFileSync(WASM);
let t0 = performance.now();
const { instance } = await WebAssembly.instantiate(bytes, {});
const instantiateMs = performance.now() - t0;
const X = instance.exports;
const mem = () => new Uint8Array(X.memory.buffer);
const res = X.cc_alloc(12);

/** Copies bytes (or a string as UTF-8) into the module; returns [ptr, len]. */
function put(data) {
  const b = typeof data === "string" ? enc.encode(data) : data;
  const p = X.cc_alloc(b.length);
  if (!p) throw new Error("cc_alloc failed");
  mem().set(b, p);
  return [p, b.length];
}
/** Runs f(...rest, ...ptr,len pairs of args, res); returns {status, bytes, text, value}; frees everything. */
function call(f, args, ...rest) {
  const ins = args.map(put);
  const status = f(...rest, ...ins.flat(), res);
  for (const [p] of ins) X.cc_free(p);
  const dv = new DataView(X.memory.buffer);
  const ptr = dv.getUint32(res, true), len = dv.getUint32(res + 4, true), value = dv.getUint32(res + 8, true);
  const out = mem().slice(ptr, ptr + len);
  X.cc_free(ptr);
  return { status, bytes: out, get text() { return dec.decode(out); }, value };
}
const openText = (t) => call(X.open_text, [t]);
const openIfc = (t) => call(X.open_ifc, [t]);
const save = (h) => call(X.save, [], h).text;

// ---- checks ----
const suites = [];
let cur;
function suite(name) {
  cur = { name, passed: 0, failed: 0, shown: 0 };
  suites.push(cur);
}
function check(what, ok, detail = "") {
  if (ok) return void cur.passed++;
  cur.failed++;
  if (cur.shown++ < 20) console.log(`FAIL ${cur.name}: ${what}${detail ? `\n  ${detail}` : ""}`);
}
function firstDiff(a, b) {
  let i = 0;
  while (i < a.length && i < b.length && a[i] === b[i]) i++;
  return `first difference at ${i}:\n  got:  ${JSON.stringify(a.slice(i, i + 160))}\n  want: ${JSON.stringify(b.slice(i, i + 160))}`;
}
const eq = (what, got, want) => check(what, got === want, got === want ? "" : firstDiff(got, want));
const end = () => console.log(`${cur.name}: ${cur.passed} passed, ${cur.failed} failed`);

// ---- floats ----
suite("floats");
{
  const f64 = new Float64Array(1), u64 = new BigUint64Array(f64.buffer);
  const hex = (x) => (f64[0] = x, u64[0].toString(16).padStart(16, "0"));
  let lines = 0;
  for (const line of read("floats/floats.txt").split("\n")) {
    if (!line) continue;
    const [bits, json, debug] = line.split(" ");
    u64[0] = BigInt("0x" + bits);
    const x = f64[0];
    eq(`json ${bits}`, call(X.fmt_f64, [], x, 0).text, json);
    eq(`debug ${bits}`, call(X.fmt_f64, [], x, 1).text, debug);
    for (const t of [json, debug]) {
      const [p, n] = put(t);
      eq(`parse_f64 ${t}`, hex(X.parse_f64(p, n)), bits);
      X.cc_free(p);
    }
    lines++;
  }
  check("6247 lines", lines === 6247, String(lines));
  for (const x of [NaN, Infinity, -Infinity]) check(`fmt_f64 ${x} is 400`, call(X.fmt_f64, [], x, 0).status === 400);
  // overflow and underflow as glibc strtod (the callers expect inf and 0)
  const parse = (t) => { const [p, n] = put(t); const x = X.parse_f64(p, n); X.cc_free(p); return x; };
  check("1e309 -> inf", parse("1e309") === Infinity && parse("-1e400") === -Infinity);
  check("1e-400 -> 0", Object.is(parse("1e-400"), 0) && Object.is(parse("-1e-400"), -0));
  check("long input", parse("0." + "0".repeat(400) + "1" + "e401") === 1);
  end();
}

// ---- query ----
function replayQueries(modelFile, goldenFile) {
  const want = read(goldenFile);
  const o = openText(read(modelFile));
  check(`${modelFile} opens`, o.status === 200, o.text);
  let got = "";
  for (const line of want.split("\n")) {
    const m = line.match(/^>>> query (\S+) (.*)$/);
    if (!m) continue;
    const r = call(X.query, [m[1], m[2]], o.value);
    got += `${line}\n${r.status} ${r.text}\n\n`;
  }
  eq(goldenFile, got, want);
  X.close(o.value);
}
suite("query");
replayQueries("frame.json", "query.txt");
replayQueries("frame2.json", "frame2/query.txt");
end();

// ---- masking (golden/README.md; ports of verbs_test.c and ifc_in_test.c) ----
const ALPHA = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$";
const fromU128 = (k) => "0".repeat(21) + ALPHA[k]; // k < 64
const validGid = (s) => s.length === 22 && s[0] >= "0" && s[0] <= "3";
const TOKEN = /[0-9A-Za-z_$]+/g;
const frame = JSON.parse(read("frame.json")).model;
const known = new Set([frame.project_id, ...Object.keys(frame.storeys), ...Object.keys(frame.materials), ...Object.keys(frame.elements)]);

class Masker {
  constructor() { this.news = []; }
  number(t) { // N of a new id (numbered on first sight), 0 otherwise
    if (!validGid(t) || known.has(t) || t === "0".repeat(22)) return 0;
    const i = this.news.indexOf(t);
    if (i >= 0) return i + 1;
    this.news.push(t);
    return this.news.length;
  }
  mask(s, mode) { return s.replace(TOKEN, (t) => { const k = this.number(t); return !k ? t : mode === 0 ? `<new-${k}>` : fromU128(k); }); }
  remask(s) {
    return s.replace(TOKEN, (t) => {
      for (let k = 1; t.length === 22 && k <= this.news.length; k++) if (t === fromU128(k)) return `<new-${k}>`;
      return t;
    });
  }
  unmask(s) { return s.replace(/<new-(\d+)>/g, (m, v) => (v >= 1 && v <= this.news.length ? this.news[v - 1] : m)); }
  // the dumper's model_text: renumber new ids from_u128(N), load and save (sorted), then mask
  modelText(h) {
    const r = this.mask(save(h), 1);
    const o = openText(r);
    if (o.status !== 200) return `renumbered model does not load: ${o.text}`;
    const s = save(o.value);
    X.close(o.value);
    return this.remask(s);
  }
}

// ---- verbs ----
suite("verbs");
{
  const want = read("verbs.txt"), wantUndo = read("verbs_after_undo.json"), wantAfter = read("verbs_after.json");
  X.guid_seed(1n);
  const o = openText(read("frame.json"));
  const h = o.value;
  const M = new Masker();
  let got = "", afterUndo = "", undos = 0, steps = 0;
  const labels = () => {
    const t = call(X.history, [], h).text; // {"redo":[…],"undo":[…]}
    const k = t.indexOf('],"undo":[');
    return `undo ${t.slice(k + 9, -1)}\nredo ${t.slice(8, k + 1)}\n`;
  };
  const elements = () => JSON.parse(call(X.query, ["elements", '{"limit":10000}'], h).text).length;
  t0 = performance.now();
  for (const line of want.split("\n")) {
    const m = line.match(/^>>> (\S+) (\S+) (.*)$/);
    if (!m) continue;
    const [, op, name, a] = m;
    const args = M.unmask(a);
    let ok = true, ans;
    if (op === "verb") {
      const r = call(X.verb, [name, args], h);
      ok = r.status === 200;
      ans = ok ? M.mask(r.text, 0) : r.text;
    } else {
      ans = call(op === "redo" ? X.redo : X.undo, [], h).text;
    }
    got += `>>> ${op} ${name} ${M.mask(args, 0)}\n${ok ? 200 : 400} ${ans}\n${labels()}elements ${elements()}\n\n`;
    steps++;
    if (op === "undo" && ++undos === 3) afterUndo = M.modelText(h);
  }
  const replayMs = performance.now() - t0;
  const after = M.modelText(h);
  check("transcript has 18 steps", steps === 18, String(steps));
  eq("verbs.txt", got, want);
  eq("verbs_after_undo.json", afterUndo, wantUndo);
  eq("verbs_after.json", after, wantAfter);
  // undo everything: the file's bytes again; redo everything: the end model again
  while (call(X.undo, [], h).value) {}
  eq("undo all = frame.json", save(h), read("frame.json"));
  while (call(X.redo, [], h).value) {}
  eq("redo all = verbs_after.json", M.modelText(h), wantAfter);
  // apply: a command list as one step, its errors, and the history
  const r1 = call(X.apply, ['[{"op":"rename","id":"004z1Cm4z4hgNOg0ez4Id1","name":"P-01"}]'], h);
  check("apply rename -> 200, empty", r1.status === 200 && r1.text === "", `${r1.status} ${r1.text}`);
  check("history has the rename", /"undo":\["rename [^"]* to P-01"/.test(call(X.history, [], h).text), call(X.history, [], h).text);
  const r2 = call(X.apply, ['[{"op":"rename","id":"0000000000000000000000","name":"X"}]'], h);
  eq("apply unknown id -> 400", `${r2.status} ${r2.text}`, "400 no such object: 0000000000000000000000");
  const r3 = call(X.apply, ["[{"], h);
  check("apply bad JSON -> 400", r3.status === 400, `${r3.status} ${r3.text}`);
  eq("verb fly -> 400", ((r) => `${r.status} ${r.text}`)(call(X.verb, ["fly", "{}"], h)), want.match(/>>> verb fly \{\}\n(.*)\n/)[1]);
  check("bad handle -> 404", call(X.save, [], 999).status === 404 && X.close(999) === 404);
  X.close(h);
  X.guid_seed(0n);
  console.log(`verbs: ${steps} steps replayed in ${replayMs.toFixed(2)} ms`);
  end();
}

// ---- file round trip ----
suite("file");
for (const f of ["frame.json", "frame2.json"]) {
  const text = read(f);
  const o = openText(text);
  check(`${f} opens`, o.status === 200, o.text);
  eq(`${f}: open then save`, save(o.value), text);
  X.close(o.value);
}
{
  const bad = openText(read("frame.json").replace(/"version": 1,/, "\"version\": 99,"));
  check("unsupported version -> 400", bad.status === 400 && /version/.test(bad.text), bad.text);
  const bad2 = openText("{");
  check("parse error -> 400", bad2.status === 400 && bad2.text.startsWith("parse"), bad2.text);
}
end();

// ---- ifc ----
suite("ifc");
{
  X.guid_seed(1n);
  const o = openIfc(read("ifc/frame.ifc"));
  check("frame.ifc imports", o.status === 200, o.text);
  // to_u128 (own numbering, known = frame.json ids), reload and save, remask
  const M = new Masker();
  const renum = M.mask(save(o.value), 1);
  const back = openText(renum);
  check("renumbered model loads", back.status === 200, back.text);
  eq("frame_reimport.json", M.remask(save(back.value)), read("ifc/frame_reimport.json"));
  X.close(back.value), X.close(o.value);
  X.guid_seed(0n);
  // export_ifc: frame.json as IFC4 = golden/ifc/frame.ifc (the version masked, golden/README.md)
  const f = openText(read("frame.json"));
  const ex = call(X.export_ifc, ["frame.ifc", "2026-01-01T00:00:00"], f.value, 0);
  check("export_ifc IFC4 -> 200", ex.status === 200, ex.text.slice(0, 200));
  eq("export_ifc IFC4 = golden/ifc/frame.ifc", ex.text.replace("'bimble 0.1.0-ccore'", "'bimble <version>'"), read("ifc/frame.ifc"));
  const ex3 = call(X.export_ifc, ["frame.ifc", "2026-01-01T00:00:00"], f.value, 1);
  check("export_ifc IFC2x3 -> 200", ex3.status === 200 && ex3.text.includes("FILE_SCHEMA(('IFC2X3'));"), ex3.text.slice(0, 200));
  X.close(f.value);
  const bad = openIfc("ISO-10303-21;\nHEADER;\nENDSEC;\n");
  check("not an IFC file -> 400", bad.status === 400, `${bad.status} ${bad.text}`);
  end();
}

// ---- mesh ----
const CAT = ["Concrete", "Steel", "Timber", "Masonry", "Insulation", "Earth", "Glass", "Other"];
function parseBmsh(b) {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const u32 = (o) => dv.getUint32(o, true);
  const h = { magic: dec.decode(b.subarray(0, 4)), version: u32(4), rev: u32(8), nE: u32(12), nV: u32(16), nI: u32(20), nS: u32(24) };
  h.origin = [0, 1, 2].map((k) => dv.getBigUint64(28 + 8 * k, true).toString(16).padStart(16, "0"));
  h.rows = [];
  let o = 52;
  for (let e = 0; e < h.nE; e++, o += 48)
    h.rows.push({ id: dec.decode(b.subarray(o, o + 22)), cat: b[o + 22], pad: b[o + 23], v0: u32(o + 24), nv: u32(o + 28), i0: u32(o + 32), ni: u32(o + 36), s0: u32(o + 40), ns: u32(o + 44) });
  const f32 = (n) => { const a = new Float32Array(n); for (let i = 0; i < n; i++, o += 4) a[i] = dv.getFloat32(o, true); return a; };
  h.pos = f32(3 * h.nV);
  h.idx = new Uint32Array(h.nI); for (let i = 0; i < h.nI; i++, o += 4) h.idx[i] = u32(o);
  h.edges = f32(6 * h.nS);
  h.normals = new Int8Array(b.buffer.slice(b.byteOffset + o, b.byteOffset + o + 8 * h.nS)); o += 8 * h.nS;
  if (h.version === 2) {
    h.place = [];
    for (let e = 0; e < h.nE; e++) { const inst = u32(o); o += 4; h.place.push({ inst, cols: f32(12) }); }
  }
  h.end = o;
  return h;
}
const f32hex = (x) => { const v = new DataView(new ArrayBuffer(4)); v.setFloat32(0, x); return v.getUint32(0).toString(16).padStart(8, "0"); };
/** golden/mesh/<k>.txt of element k of a local BMSH */
function elementText(h, k) {
  const r = h.rows[k], p = h.place[k];
  let s = "";
  if (p.inst) s += "t " + [...p.cols].map(f32hex).join(" ") + "\n";
  for (let i = r.v0; i < r.v0 + r.nv; i++) s += "v " + [...h.pos.subarray(3 * i, 3 * i + 3)].map(f32hex).join(" ") + "\n";
  for (let i = r.i0; i + 2 < r.i0 + r.ni; i += 3) s += `i ${h.idx[i] - r.v0} ${h.idx[i + 1] - r.v0} ${h.idx[i + 2] - r.v0}\n`;
  for (let i = r.s0; i < r.s0 + r.ns; i++) s += "e " + [...h.edges.subarray(6 * i, 6 * i + 6)].map(f32hex).join(" ") + "\n";
  for (let i = r.s0; i < r.s0 + r.ns; i++) s += "n " + [...h.normals.subarray(8 * i, 8 * i + 8)].join(" ") + "\n";
  return s;
}
// cc_scene_world_mesh in f32 (Math.fround after each operation, no fused multiply-add)
const F = Math.fround;
function placeLocal(h, k) {
  const r = h.rows[k], p = h.place[k];
  const pos = [...h.pos.subarray(3 * r.v0, 3 * (r.v0 + r.nv))], edges = [...h.edges.subarray(6 * r.s0, 6 * (r.s0 + r.ns))];
  const idx = [...h.idx.subarray(r.i0, r.i0 + r.ni)], normals = [...h.normals.subarray(8 * r.s0, 8 * (r.s0 + r.ns))];
  if (!p.inst) return { pos, idx, edges, normals };
  const c = p.cols, x = [c[0], c[1], c[2]], y = [c[3], c[4], c[5]], z = [c[6], c[7], c[8]], t = [c[9], c[10], c[11]];
  const cr = [F(F(y[1] * z[2]) - F(y[2] * z[1])), F(F(y[2] * z[0]) - F(y[0] * z[2])), F(F(y[0] * z[1]) - F(y[1] * z[0]))];
  const mirrors = F(F(F(x[0] * cr[0]) + F(x[1] * cr[1])) + F(x[2] * cr[2])) < 0;
  if (mirrors) for (let i = 0; i + 2 < idx.length; i += 3) [idx[i + 1], idx[i + 2]] = [idx[i + 2], idx[i + 1]];
  const xf = (a, o) => { const q = [a[o], a[o + 1], a[o + 2]]; for (let j = 0; j < 3; j++) a[o + j] = F(F(F(F(x[j] * q[0]) + F(y[j] * q[1])) + F(z[j] * q[2])) + t[j]); };
  for (let i = 0; i < pos.length; i += 3) xf(pos, i);
  for (let i = 0; i < edges.length; i += 3) xf(edges, i);
  const q8 = (v) => { const a = F(Math.min(1, Math.max(-1, v)) * 127); const r = Math.abs(a) < 8388608 ? Math.sign(a) * Math.floor(Math.abs(a) + 0.5) : a; return r | 0; };
  for (let i = 0; i < normals.length; i += 4) {
    const nq = normals.slice(i, i + 4);
    if (!nq[0] && !nq[1] && !nq[2] && !nq[3]) continue;
    const u = [F(nq[0] / 127), F(nq[1] / 127), F(nq[2] / 127)];
    const v = [0, 1, 2].map((j) => F(F(F(x[j] * u[0]) + F(y[j] * u[1])) + F(z[j] * u[2])));
    let l = F(Math.sqrt(F(F(F(v[0] * v[0]) + F(v[1] * v[1])) + F(v[2] * v[2]))));
    l = Math.max(l, F(1e-12));
    for (let j = 0; j < 3; j++) normals[i + j] = q8(F(v[j] / l));
    normals[i + 3] = 0;
  }
  return { pos, idx, edges, normals };
}
function meshSuite(modelFile, dir) {
  const o = openText(read(modelFile));
  const model = JSON.parse(save(o.value)).model;
  const loc = call(X.mesh, [], o.value, 1), wor = call(X.mesh, [], o.value, 0);
  check(`${modelFile}: mesh -> 200`, loc.status === 200 && wor.status === 200, `${loc.status} ${wor.status}`);
  const L = parseBmsh(loc.bytes), W = parseBmsh(wor.bytes);
  check("local: BMSH version 2, length = layout", L.magic === "BMSH" && L.version === 2 && L.end === loc.bytes.length);
  check("world: BMSH version 1, length = layout", W.magic === "BMSH" && W.version === 1 && W.end === wor.bytes.length);
  // scene.txt
  let scene = `origin ${L.origin.join(" ")}\nelements ${L.nE}\n`;
  L.rows.forEach((r, k) => {
    const e = model.elements[r.id];
    scene += `${String(k).padStart(3, "0")} ${r.id} ${e.ifc_entity ?? e.class} ${r.cat} ${CAT[r.cat]} v=${r.nv} i=${r.ni / 3} e=${r.ns} n=${r.ns}\n`;
  });
  eq(`${dir}/scene.txt`, scene, read(`${dir}/scene.txt`));
  for (let k = 0; k < L.nE; k++) eq(`${dir}/${String(k).padStart(3, "0")}.txt`, elementText(L, k), read(`${dir}/${String(k).padStart(3, "0")}.txt`));
  // world = local placed (positions, indices, edges, normals: bits)
  check("world header = local header", ["rev", "nE", "nV", "nI", "nS"].every((f) => W[f] === L[f]) && W.origin.join() === L.origin.join());
  check("world rows = local rows", JSON.stringify(W.rows) === JSON.stringify(L.rows));
  let bad = "";
  for (let k = 0; k < L.nE && !bad; k++) {
    const want = placeLocal(L, k), r = W.rows[k];
    const got = {
      pos: [...W.pos.subarray(3 * r.v0, 3 * (r.v0 + r.nv))], idx: [...W.idx.subarray(r.i0, r.i0 + r.ni)],
      edges: [...W.edges.subarray(6 * r.s0, 6 * (r.s0 + r.ns))], normals: [...W.normals.subarray(8 * r.s0, 8 * (r.s0 + r.ns))],
    };
    for (const f of ["pos", "idx", "edges", "normals"])
      if (got[f].some((v, i) => !Object.is(v, want[f][i]))) bad ||= `element ${k} ${f}: ${got[f].slice(0, 6)} vs ${want[f].slice(0, 6)}`;
  }
  check(`${modelFile}: world mesh = local meshes placed`, !bad, bad);
  X.close(o.value);
}
suite("mesh");
meshSuite("frame.json", "mesh");
meshSuite("frame2.json", "frame2/mesh");
end();

// ---- memory: no growth when a model is loaded and freed again and again ----
suite("memory");
{
  const frameText = put(read("frame.json")), ifcText = put(read("ifc/frame.ifc"));
  // one round: open, mesh, a verb, undo, close; every input and answer freed
  const roundClean = () => {
    const a = put("place"), b = put('{"kind":"column","storey":"1uAZA0Yfn05f$xRd1k4uPR","a":[0,0]}');
    X.open_text(frameText[0], frameText[1], res);
    const dv = () => new DataView(X.memory.buffer);
    const h = dv().getUint32(res + 8, true);
    X.cc_free(dv().getUint32(res, true));
    X.mesh(h, 0, res);
    X.cc_free(dv().getUint32(res, true));
    X.verb(h, a[0], a[1], b[0], b[1], res);
    X.cc_free(dv().getUint32(res, true));
    X.undo(h, res);
    X.cc_free(dv().getUint32(res, true));
    X.close(h);
    X.cc_free(a[0]), X.cc_free(b[0]);
  };
  for (let i = 0; i < 3; i++) roundClean();
  const pages0 = X.memory.buffer.byteLength / 65536, heap0 = X.cc_heap_used();
  t0 = performance.now();
  for (let i = 0; i < 1000; i++) roundClean();
  const loopMs = performance.now() - t0;
  const pages1 = X.memory.buffer.byteLength / 65536, heap1 = X.cc_heap_used();
  check("1000 x (open, mesh, verb, undo, close): no new pages", pages1 === pages0, `${pages0} -> ${pages1}`);
  check("1000 x (open, mesh, verb, undo, close): heap unchanged", heap1 === heap0, `${heap0} -> ${heap1}`);
  const ifcRound = () => {
    X.open_ifc(ifcText[0], ifcText[1], res);
    const dv = new DataView(X.memory.buffer);
    const h = dv.getUint32(res + 8, true);
    X.cc_free(dv.getUint32(res, true));
    X.close(h);
  };
  for (let i = 0; i < 2; i++) ifcRound();
  const heap2 = X.cc_heap_used();
  for (let i = 0; i < 100; i++) ifcRound();
  check("100 x open_ifc + close: heap unchanged", X.cc_heap_used() === heap2, `${heap2} -> ${X.cc_heap_used()}`);
  console.log(`memory: ${pages1} pages (${(pages1 / 16).toFixed(1)} MiB), heap ${heap1} bytes; 1000 rounds in ${loopMs.toFixed(0)} ms`);
  end();
}

// ---- SLAB1: openings cut in the wasm build (src/prism_cut.h: no kernel in wasm) against the
// native host (the manifold kernel when built in): volume and box of every element within 1e-6 ----
const HOST_BIN_SLAB1 = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
suite("openings");
{
  const ids = (c) => `0Slab1Wasm${c.repeat(12)}`; // 22 characters
  const box = (x0, y0, z0, x1, y1, z1) => ({
    kind: "mesh",
    positions: [[x0, y0, z0], [x1, y0, z0], [x1, y1, z0], [x0, y1, z0], [x0, y0, z1], [x1, y0, z1], [x1, y1, z1], [x0, y1, z1]],
    indices: [0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7],
  });
  const at = (o) => ({ origin: o, z_axis: [0, 0, 1], x_axis: [1, 0, 0] });
  const el = (c, cls, origin, generator, openings) => [ids(c), {
    id: ids(c), class: cls, name: cls + c, storey: ids("S"), material: ids("M"), placement: at(origin), generator,
    openings: openings.map((o, k) => ({ id: ids(c).slice(0, 21) + "abcdefgh"[k], placement: at([0, 0, 0]), generator: o })),
  }];
  const model = { format: "bimble", version: 1, model: {
    project_id: ids("P"), project_name: "slab1 openings",
    storeys: { [ids("S")]: { id: ids("S"), name: "L0", elevation: 0 } },
    materials: { [ids("M")]: { id: ids("M"), name: "C30/37", category: "concrete" } },
    elements: Object.fromEntries([
      el("A", "IfcWall", [0, 0, 0], { kind: "wall", length: 6000, thickness: 200, height: 3000 },
        [box(500, -300, 900, 1700, 300, 2100), box(4500, -300, 0, 5400, 300, 2200)]),
      el("B", "IfcSlab", [0, 1000, 3000], { kind: "slab", outline: [[0, 0], [6000, 0], [6000, 2000], [2000, 2000], [2000, 5000], [0, 5000]], thickness: 250 },
        [box(4000, 500, -10, 4500, 1500, 260)]),
      el("C", "IfcSlab", [0, 8000, 3000], { kind: "slab", outline: [[0, 0], [3000, 0], [3000, 1000], [1000, 1000], [1000, 3000], [0, 3000]], thickness: 250 },
        [box(200, 200, 100, 600, 600, 400)]), // a recess in an L: not a special case, uncut in wasm
    ]),
  } };
  const text = JSON.stringify(model);
  /** Per element id: [volume, xmin, ymin, zmin, xmax, ymax, zmax] of a world BMSH (positions + origin). */
  const solids = (bytes) => {
    const M = parseBmsh(bytes), out = {};
    const org = M.origin.map((h) => { const dv = new DataView(new ArrayBuffer(8)); dv.setBigUint64(0, BigInt("0x" + h)); return dv.getFloat64(0); });
    for (const r of M.rows) {
      let v = 0;
      const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
      const P = (i) => [0, 1, 2].map((k) => M.pos[3 * (r.v0 + i) + k] + org[k]);
      for (let i = 0; i < r.nv; i++) { const p = P(i); for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); } }
      for (let t = r.i0; t + 2 < r.i0 + r.ni; t += 3) {
        const [a, b, c] = [P(M.idx[t] - r.v0), P(M.idx[t + 1] - r.v0), P(M.idx[t + 2] - r.v0)]; // indices are absolute
        v += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6;
      }
      out[r.id] = [v, ...lo, ...hi];
    }
    return out;
  };
  const o = openText(text);
  check("openings model opens in wasm", o.status === 200, o.text);
  const W = solids(call(X.mesh, [], o.value, 0).bytes);
  const tmpd = mkdtempSync(join(tmpdir(), "ccore-wasm-slab1-"));
  const file = join(tmpd, "openings.json");
  writeFileSync(file, text);
  const P = port("wasm_run_slab1"), TOKEN = "slab1-token";
  const host = spawn(HOST_BIN_SLAB1, [String(P), file], { env: hostEnv({ BIMBLE_TOKEN: TOKEN }), stdio: ["ignore", "ignore", "pipe"] });
  let err = "";
  host.stderr.on("data", (d) => (err += d));
  try {
    const until = Date.now() + 5000;
    while (!err.includes("listening")) {
      if (Date.now() > until || host.exitCode !== null) throw new Error(`native host did not start: ${err}`);
      await new Promise((r) => setTimeout(r, 20));
    }
    const N = solids(new Uint8Array(await (await fetch(`http://127.0.0.1:${P}/mesh?t=${TOKEN}`)).arrayBuffer()));
    const rel = (a, b) => Math.abs(a - b) <= 1e-6 * Math.max(1, Math.abs(b));
    const same = (id) => W[id] && N[id] && W[id].every((x, k) => rel(x, N[id][k]));
    const hand = { A: 6000 * 200 * 3000 - 1200 * 200 * 1200 - 900 * 200 * 2200, B: (6000 * 2000 + 2000 * 3000) * 250 - 500 * 1000 * 250 };
    for (const c of ["A", "B"]) {
      check(`opening cut in wasm: ${c === "A" ? "wall, window + door" : "L slab, shaft"}: volume and box = native's (1e-6)`, same(ids(c)),
        `wasm ${W[ids(c)]} native ${N[ids(c)]}`);
      check(`  ${c}: native's volume is the hand's ${hand[c]}`, N[ids(c)] && rel(N[ids(c)][0], hand[c]), `${N[ids(c)]?.[0]}`);
    }
    // The recess: native cuts it (kernel), wasm draws it uncut (not a special case) and says so.
    console.log(`  recess: wasm volume ${W[ids("C")]?.[0]}, native ${N[ids("C")]?.[0]}`);
    check("the recess in an L slab: uncut in wasm (its whole volume), cut natively", W[ids("C")] && rel(W[ids("C")][0], 5000000 * 250) &&
      N[ids("C")] && rel(N[ids("C")][0], 5000000 * 250 - 400 * 400 * 150));
  } finally {
    host.kill();
    X.close(o.value);
  }
  end();
}

// ---- host: the call layer in the module against the native host ----
const HOST_BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const HOST_PORT = port("wasm_run"), HOST_TOKEN = "wasmrun-token";
/** One call of the module's host layer: {status, text, bytes, value}. */
const hcall = (op, name = "-", body = "") => call(X.call, [op, name, body]);
const change = () => call(X.change_json, []).text;
suite("host");
{
  const tmpd = mkdtempSync(join(tmpdir(), "ccore-wasm-run-"));
  const model = join(tmpd, "frame.json");
  copyFileSync(join(G, "frame.json"), model);
  const host = spawn(HOST_BIN, [String(HOST_PORT), model], {
    env: hostEnv({ BIMBLE_TOKEN: HOST_TOKEN, CCORE_SEED: "1" }), stdio: ["ignore", "ignore", "pipe"],
  });
  let hostErr = "";
  host.stderr.on("data", (d) => (hostErr += d));
  try {
    const base = `http://127.0.0.1:${HOST_PORT}`;
    const until = Date.now() + 5000;
    while (!hostErr.includes("listening")) {
      if (Date.now() > until || host.exitCode !== null) throw new Error(`native host did not start: ${hostErr}`);
      await new Promise((r) => setTimeout(r, 20));
    }
    const ncall = async (op, name = "-", body = "") => {
      const r = await fetch(`${base}/${op}/${encodeURIComponent(name)}?t=${HOST_TOKEN}`,
        { method: "POST", body, headers: { "Content-Type": "text/plain" } });
      return { status: r.status, text: await r.text() };
    };
    const nchange = async () => JSON.stringify((await (await fetch(`${base}/change?since=0&t=${HOST_TOKEN}`)).json()).change);
    const nmesh = async () => new Uint8Array(await (await fetch(`${base}/mesh?t=${HOST_TOKEN}`)).arrayBuffer());

    // Boot: no model yet (the page asks before it opens one).
    check("boot: change_json without a model", JSON.parse(change()).file === null, change());
    // FS1: the file dialog's queries are the native host's; the module has no folder, no recent files.
    const fsl = hcall("query", "fs_list", JSON.stringify({ dir: "/" }));
    check("FS1: query fs_list in the module -> 400 no folder in the browser", fsl.status === 400 && fsl.text === "fs_list: no folder in the browser", `${fsl.status} ${fsl.text}`);
    check("FS1: query recent_files in the module -> []", hcall("query", "recent_files", "{}").text === "[]");
    const st0 = hcall("query", "state", "{}");
    check("boot: query state -> 200, no file", st0.status === 200 && JSON.parse(st0.text).file === null, st0.text);
    const ac0 = hcall("query", "actions", "{}");
    check("boot: query actions -> 65 rows (Rust's 54 + ISS72's 2 + ISS71's 2 + SETOUT's 3 + VIEWSHEET's 4)", ac0.status === 200 && JSON.parse(ac0.text).length === 65, ac0.text.slice(0, 200));
    check("unknown op -> 404", hcall("nope").status === 404);
    check("host_saved before a save -> 404", call(X.host_saved, []).status === 404);

    const frameText = read("frame.json");
    X.guid_seed(1n);
    const op = call(X.host_open_text, ["frame.json", frameText]);
    check("host_open_text frame.json -> 200, rev 1", op.status === 200 && op.value === 1, `${op.status} ${op.text} ${op.value}`);
    check("a second host_open_text -> 400", call(X.host_open_text, ["frame.json", frameText]).status === 400);

    const STOREY = "1uAZA0Yfn05f$xRd1k4uPR";
    // Rust's view ids (golden/verbs_all.txt), and a plan id not open (the Fondations' up: any other).
    const V3D = "1v7Vtm2FfafmYPKIQEXnlY", PLAN_N1 = "3w8zGYnP$Ay1lyvMZjDnXq", PLAN_N2 = "0Kvmz3DcY9zKW4N4hq88bE";
    const firstId = Object.keys(frame.elements).sort()[0];
    const COL00 = "2P97ZHH3v3XutfaphHA_Tu", COL60 = "2KmXk9zn1FAPxjTPfLpwLP"; // columns of Niveau 1 at (0, 0), (6000, 0)
    let firstAnn = "";
    const steps = [
      ["query", "state", "{}"], ["query", "actions", "{}"], ["query", "storeys", "{}"], ["query", "status", "null"],
      ["query", "open_views", "{}"], ["query", "visibility", "{}"], ["query", "placing", "{}"],
      ["select", "-", JSON.stringify([firstId])], ["query", "element", JSON.stringify({ id: firstId })],
      ["set_placing", "-", JSON.stringify({ kind: "beam" })], ["query", "placing", "{}"],
      ["verb", "place", JSON.stringify({ a: [1500.0, 1500.0], kind: "column", storey: STOREY })],
      ["query", "history", "{}"], ["query", "state", "{}"], "mesh", ["action", "save", ""], "saved",
      ["undo", "-", ""], ["query", "history", "{}"], ["query", "state", "{}"], ["action", "save", ""], "saved", "saved=open",
      ["query", "state", "{}"], ["redo", "-", ""], "mesh", ["action", "undo", ""], ["action", "redo", ""],
      ["apply", "-", "not json"], ["verb", "nope", "{}"], ["query", "nope", "{}"], ["action", "nope", ""],
      ["tab", "-", JSON.stringify({ op: "activate", id: "x" })], ["dismiss_belongs", "-", ""], ["query", "status", "null"],
      // ticket E1: verbs first (ADR 0068): the verb table's queries, dry_run, if_rev, rename
      ["query", "verbs", ""], ["query", "describe", JSON.stringify({ name: "rotate" })], ["query", "describe", JSON.stringify({ name: "rename" })],
      ["verb", "copy", JSON.stringify({ d: [7000.0, 0.0, 0.0], ids: [firstId], dry_run: true })],
      ["verb", "rename", JSON.stringify({ id: firstId, name: "P-x", dry_run: true })], ["query", "history", "{}"],
      ["verb", "rename", JSON.stringify({ id: firstId, name: "P-x", if_rev: 999 })],
      ["verb", "rename", JSON.stringify({ if_rev: "REV", id: firstId, name: "P-x" })], ["query", "history", "{}"], ["undo", "-", ""],
      // ticket B4: the place tool (op tool, query ghost): snap, ghost, one step per gesture, cancel
      ["set_placing", "-", JSON.stringify({ kind: "column" })], ["tool", "pointer", JSON.stringify({ at: [6037, 4958], tol_mm: 100 })],
      ["query", "ghost", "{}"], ["tool", "click", JSON.stringify({ at: [3004, 2533], tol_mm: 50 })], ["query", "history", "{}"],
      ["set_placing", "-", JSON.stringify({ kind: "beam" })], ["tool", "click", JSON.stringify({ at: [1000, 7500] })],
      ["query", "placing", "{}"], ["tool", "pointer", JSON.stringify({ at: [9000, 7520], ortho: true })], ["tool", "click", ""],
      ["query", "history", "{}"], ["tool", "click", JSON.stringify({ at: [0, 0] })], ["tool", "cancel", ""], ["query", "placing", "{}"],
      ["tool", "nope", ""], ["tool", "click", "{}"], ["undo", "-", ""], ["query", "ghost", "{}"],
      // ticket B3: visibility and the section box (session state: rev unchanged), the box tool
      ["tool", "cancel", ""], ["select", "-", JSON.stringify([firstId])], ["action", "hide_now", ""], ["query", "visibility", "{}"],
      ["query", "visibility_sets", "{}"], ["query", "actions", "{}"], ["action", "section_box", ""], ["query", "visibility", "{}"],
      ["select", "-", JSON.stringify([firstId])], ["action", "unhide", ""], ["action", "isolate_in_view", ""], ["action", "box_to_selection", ""],
      ["query", "visibility", "{}"], ["action", "show_all_in_view", ""], ["action", "reveal_hidden", ""], ["query", "actions", "{}"],
      // ISS72: show_previous (back before show_all_in_view), show_all, show_previous again
      ["action", "show_previous", ""], ["query", "visibility_sets", "{}"], ["action", "show_all", ""], ["query", "visibility_sets", "{}"],
      ["action", "show_previous", ""], ["query", "visibility_sets", "{}"], ["query", "actions", "{}"], ["action", "show_all", ""],
      // ISS71: the box's orientation (refused alike: frame.json has no turned georeference)
      ["action", "box_north_true", ""], ["action", "box_north_project", ""], ["query", "visibility", "{}"],
      ["tool", "box_set", JSON.stringify({ min: [0, 0, 0], max: [8000.5, 6000.25, 3000.1], on: true })],
      ["tool", "box_press", JSON.stringify({ ray: [-5000, 3000, 9000, 0.8320502943378437, 0, -0.5547001962252291], ray_tol: [-5000, 3150, 9000, 0.8320502943378437, 0, -0.5547001962252291] })],
      ["tool", "box_move", JSON.stringify({ ray: [-5000, 3000, 9000, 0.8, 0, -0.6] })], ["query", "visibility", "{}"],
      ["tool", "box_release", ""], ["query", "visibility", "{}"], ["tool", "box_cancel", ""], ["tool", "box_set", JSON.stringify({ min: [0, 0, 0], max: [1, 1, 1] })],
      ["action", "section_box", ""], ["query", "visibility", "{}"],
      // ticket C1: views and tabs (session/views.h): a plan's tab, its clip, the placing storey, the
      // place tool with an orthographic ray (straight down), activate, close, Home, open again
      ["query", "open_views", "{}"], ["tab", "-", JSON.stringify({ op: "open", tab: { type: "view", id: PLAN_N1 } })],
      ["query", "open_views", "{}"], ["query", "view_clip", "{}"], ["query", "placing", "{}"],
      ["set_placing", "-", JSON.stringify({ kind: "column" })],
      ["tool", "click", JSON.stringify({ ray: [2470, 1530, 90000, 0, 0, -1], ray_tol: [2620, 1530, 90000, 0, 0, -1], free: true })],
      ["tab", "-", JSON.stringify({ op: "activate", tab: { type: "view", id: V3D } })], ["query", "view_clip", "{}"],
      ["tab", "-", JSON.stringify({ op: "activate", tab: { type: "view", id: PLAN_N2 } })],
      ["tab", "-", JSON.stringify({ op: "close", tab: { type: "view", id: PLAN_N1 } })],
      ["tab", "-", JSON.stringify({ op: "close", tab: { type: "view", id: V3D } })], ["query", "open_views", "{}"], ["query", "view_clip", "{}"],
      ["tab", "-", JSON.stringify({ op: "open", tab: { type: "view", id: V3D } })], ["query", "open_views", "{}"],
      // ticket C4: dimensions and texts (src/annot.h): the verbs (a default plan stored on its first
      // annotation), view_annotations (anchors on column corners follow a moved column), undo, the
      // annotate tool (2 snapped clicks + the offset; Esc; a text), move, delete, errors
      ["verb", "add_dimension", JSON.stringify({ view: PLAN_N1, a: { element: COL00, feature: "corner:0" }, b: { element: COL60, feature: "corner:1" }, offset: -800 })],
      ["query", "views", "{}"], ["query", "view_annotations", JSON.stringify({ view: PLAN_N1 })],
      ["verb", "add_text", JSON.stringify({ view: PLAN_N1, at: [1000.5, 2000], text: "Poteau \u00e9" })],
      ["verb", "move", JSON.stringify({ ids: [COL60], d: [500, 250, 0] })], ["query", "view_annotations", JSON.stringify({ view: PLAN_N1, unit: "cm" })],
      ["undo", "-", ""], ["query", "view_annotations", JSON.stringify({ view: PLAN_N1, unit: "m" })],
      ["tab", "-", JSON.stringify({ op: "open", tab: { type: "view", id: PLAN_N1 } })], ["tool", "annotate", JSON.stringify({ mode: "dimension" })],
      ["tool", "pointer", JSON.stringify({ at: [-142, -155], tol_mm: 40 })], ["tool", "click", JSON.stringify({ at: [-142, -155], tol_mm: 40 })],
      ["tool", "pointer", JSON.stringify({ at: [6148, 147], tol_mm: 40 })], ["tool", "click", JSON.stringify({ at: [6148, 147], tol_mm: 40 })],
      ["tool", "pointer", JSON.stringify({ at: [3000, 2600], tol_mm: 40 })], ["query", "annotate", "{}"],
      ["tool", "click", JSON.stringify({ at: [3000, 2600], tol_mm: 40 })], ["query", "history", "{}"],
      ["tool", "click", JSON.stringify({ at: [0, 5000] })], ["tool", "cancel", ""], ["query", "annotate", "{}"],
      ["tool", "annotate", JSON.stringify({ mode: "text", text: "Axe A", size_mm: 5 })], ["tool", "click", JSON.stringify({ at: [2004, 3006], free: true })],
      ["query", "view_annotations", "{}"], ["verb", "move_annotation", JSON.stringify({ view: PLAN_N1, id: "FIRST_ANN", by: [0, -200] })],
      ["verb", "delete_annotation", JSON.stringify({ view: PLAN_N1, id: "FIRST_ANN" })], ["query", "view_annotations", "{}"], ["undo", "-", ""],
      ["verb", "add_dimension", JSON.stringify({ view: PLAN_N1, a: [0, 0], b: [0, 0], offset: 0 })],
      ["verb", "add_text", JSON.stringify({ view: V3D, at: [0, 0], text: "x" })], ["tool", "annotate", JSON.stringify({ mode: "bad" })],
      ["action", "save", ""], "saved", ["tab", "-", JSON.stringify({ op: "activate", tab: { type: "view", id: V3D } })],
    ];
    let rev = 1, placedRev = 0, toolPlaced = 0, toolAdded = 0;
    for (const s of steps) {
      if (s === "mesh") {
        const w = call(X.host_mesh, [], 0), n = await nmesh();
        check(`host_mesh = GET /mesh at rev ${w.value}`, w.status === 200 && w.bytes.length === n.length && w.bytes.every((b, i) => b === n[i]),
          `${w.status} ${w.bytes.length} vs ${n.length} bytes`);
        continue;
      }
      if (s === "saved=open") { // place, undo, save: the text cc_model_save gives frame.json (export save)
        const h = openText(frameText);
        eq("saved after undo = save of frame.json as opened", call(X.host_saved, []).text, save(h.value));
        X.close(h.value);
        continue;
      }
      if (s === "saved") {
        const w = call(X.host_saved, []);
        eq("action save: host_saved = the native host's saved file", w.status === 200 ? w.text : `${w.status}`, readFileSync(model, "utf8"));
        continue;
      }
      const [o, name] = s, body = s[2].replace('"if_rev":"REV"', `"if_rev":${rev}`).replaceAll("FIRST_ANN", firstAnn); // the rev now
      const w = hcall(o, name, body), n = await ncall(o, name, body);
      // The load time is a clock reading (the module has none: 0): masked in query status.
      const mask = (t) => o === "query" && name === "status" ? t.replace(/"load_ms":[0-9.e+-]+/, '"load_ms":_').replace(/loaded in [^·]*·/, "loaded in _ ·") : t;
      eq(`${o} ${name} ${body}`, mask(`${w.status} ${w.text}`), mask(`${n.status} ${n.text}`));
      const wc = change();
      eq(`change after ${o} ${name}`, JSON.stringify(JSON.parse(wc)), await nchange());
      const r = JSON.parse(wc).rev;
      if (o === "verb" && name === "place") placedRev = r, check("verb place: rev + 1", r === rev + 1, `${rev} -> ${r}`);
      if (o === "tool") { // B4: a completed gesture is one step (rev + 1), anything else no change
        const ans = w.status === 200 ? JSON.parse(w.text) : {};
        const added = (ans.added ?? null) !== null; // C4: the annotate tool's click that adds
        const placed = (ans.placed ?? null) !== null || added; // B3: the box tool's answers have no "placed"
        if ((ans.placed ?? null) !== null) toolPlaced++;
        if (added) toolAdded++;
        check(`tool ${name} ${body}: ${placed ? "placed, rev + 1" : "rev unchanged"}`, r === rev + (placed ? 1 : 0), `${w.status} ${w.text.slice(0, 120)}, rev ${rev} -> ${r}`);
      }
      if (o === "verb" && name === "rename") { // E1: dry run and conflict change nothing; if_rev = rev runs
        const real = !body.includes("dry_run") && !body.includes("999");
        check(`verb rename ${body}: ${real ? "200, rev + 1" : "no change"}`, real ? w.status === 200 && r === rev + 1 : r === rev && (w.status === 200) === body.includes("dry_run"), `${w.status} ${w.text}, rev ${rev} -> ${r}`);
      }
      if (o === "verb" && name === "add_dimension" && !firstAnn && w.status === 200) {
        const vs = JSON.parse(hcall("query", "view_annotations", JSON.stringify({ view: PLAN_N1 })).text);
        firstAnn = vs.items[0].id;
      }
      if (o === "query" && name === "view_annotations" && body === JSON.stringify({ view: PLAN_N1 }) && w.status === 200 && JSON.parse(w.text).items.length === 1)
        check("C4: the anchored dimension measures 6300 (corner 0 at x -150 to corner 1 at x 6150)", JSON.parse(w.text).items[0].value_mm === 6300, w.text);
      rev = r;
    }
    check("C4: the annotate tool added a dimension and a text", toolAdded === 2, String(toolAdded));
    check("the place happened", placedRev === 2, String(placedRev));
    check("the tool placed a column and a beam, and a column in a plan (C1)", toolPlaced === 3, String(toolPlaced));
    // ticket B5: linked models (session/links.h). The native host links a path, the module the
    // page's bytes (host_open flag 4) under the same name; then the same calls on both: every
    // answer, the change JSON and /mesh (with the LNKS trailer) byte for byte.
    {
      const LINKS = [["mapped_item.ifc", "ifc/fixtures/mapped_item.ifc"], ["frame2.bimbleview", "view/frame2.bimbleview"]];
      for (const [name, src] of LINKS) {
        copyFileSync(join(G, src), join(tmpd, name));
        const n = await ncall("action", "link_model", JSON.stringify({ path: join(tmpd, name) }));
        const w = call(X.host_open, [name, readFileSync(join(G, src))], 4);
        check(`link_model ${name}: native and host_open flag 4 -> 200`, n.status === 200 && w.status === 200, `${n.status} ${n.text} / ${w.status} ${w.text}`);
        eq(`change after link_model ${name}`, JSON.stringify(JSON.parse(change())), await nchange());
      }
      const wm = call(X.host_mesh, [], 0).bytes, nm = await nmesh();
      check("links: host_mesh = GET /mesh (with the LNKS trailer)", wm.length === nm.length && wm.every((b, i) => b === nm[i]), `${wm.length} vs ${nm.length} bytes`);
      // The linked ids from the trailer: the first element of each link.
      const dv = new DataView(nm.buffer, nm.byteOffset, nm.byteLength);
      const size = (o) => 52 + 48 * dv.getUint32(o + 12, true) + 12 * dv.getUint32(o + 16, true) + 4 * dv.getUint32(o + 20, true) + 32 * dv.getUint32(o + 24, true);
      let o = size(0);
      check("links: the trailer is LNKS with 2 links", dec.decode(nm.subarray(o, o + 4)) === "LNKS" && dv.getUint32(o + 8, true) === 2);
      o += 12;
      const first = [];
      for (let k = 0; k < 2; k++) {
        const L = dv.getUint32(o, true);
        o += 4 + L + ((4 - (L % 4)) % 4) + 8;
        first.push(dec.decode(nm.subarray(o + 52, o + 74)));
        o += dv.getUint32(o - 4, true);
      }
      const lsteps = [
        ["query", "links", "{}"], ["query", "actions", "{}"],
        ["select", "-", JSON.stringify({ link: "mapped_item.ifc", ids: [first[0]] })],
        ["query", "element", JSON.stringify({ id: first[0], link: "mapped_item.ifc" })],
        ["select", "-", JSON.stringify({ link: "frame2.bimbleview", ids: [first[1]] })],
        ["query", "element", JSON.stringify({ id: first[1], link: "frame2.bimbleview" })],
        ["verb", "rename", JSON.stringify({ id: first[1], name: "x" })],
        ["select", "-", JSON.stringify({ link: "nope", ids: [first[1]] })], ["select", "-", "[]"],
        ["action", "manage_links", JSON.stringify({ op: "hide", name: "mapped_item.ifc" })], ["action", "manage_links", ""], "mesh",
        ["action", "manage_links", JSON.stringify({ op: "show", name: "mapped_item.ifc" })],
        ["action", "manage_links", JSON.stringify({ op: "remove", name: "frame2.bimbleview" })], "mesh", ["query", "links", "{}"],
        ["action", "manage_links", JSON.stringify({ op: "remove", name: "mapped_item.ifc" })], "mesh", ["query", "links", "{}"],
      ];
      for (const s of lsteps) {
        if (s === "mesh") {
          const w = call(X.host_mesh, [], 0).bytes, n = await nmesh();
          check("links: host_mesh = GET /mesh", w.length === n.length && w.every((b, i) => b === n[i]), `${w.length} vs ${n.length} bytes`);
          continue;
        }
        const [op, name, body] = s;
        const w = hcall(op, name, body), n = await ncall(op, name, body);
        eq(`links: ${op} ${name} ${body}`, `${w.status} ${w.text}`, `${n.status} ${n.text}`);
        eq(`links: change after ${op} ${name}`, JSON.stringify(JSON.parse(change())), await nchange());
      }
    }
    const vl = hcall("query", "verbs", "");
    check("query verbs in the module = golden/verbs_list.json", vl.status === 200 && vl.text === JSON.stringify(JSON.parse(read("verbs_list.json"))), vl.text.slice(0, 120));
    const lg = call(X.host_log, []).text;
    check("host_log has the Saved lines (2 saves, 1 in the C4 block)", (lg.match(/bimble-ccore: Saved frame\.json/g) ?? []).length === 3, JSON.stringify(lg));
    const local = call(X.host_mesh, [], 1);
    check("host_mesh local -> BMSH version 2", local.status === 200 && new DataView(local.bytes.buffer).getUint32(4, true) === 2);
  } catch (e) {
    check("no exception", false, e.stack ?? String(e));
  } finally {
    X.guid_seed(0n);
    host.kill("SIGKILL");
    rmSync(tmpd, { recursive: true, force: true });
  }
  end();
}

// ---- files (ticket A5): the file actions through a fresh module ----
suite("files");
{
  const { instance: I2 } = await WebAssembly.instantiate(bytes, {});
  const Y = I2.exports;
  const yres = Y.cc_alloc(12);
  const yput = (data) => {
    const b = typeof data === "string" ? enc.encode(data) : data;
    const p = Y.cc_alloc(b.length || 1);
    new Uint8Array(Y.memory.buffer).set(b, p);
    return [p, b.length];
  };
  const ycall = (f, args, ...rest) => {
    const ins = args.map(yput);
    const status = f(...rest, ...ins.flat(), yres);
    for (const [p] of ins) Y.cc_free(p);
    const dv = new DataView(Y.memory.buffer);
    const ptr = dv.getUint32(yres, true), len = dv.getUint32(yres + 4, true), value = dv.getUint32(yres + 8, true);
    const out = new Uint8Array(Y.memory.buffer).slice(ptr, ptr + len);
    Y.cc_free(ptr);
    return { status, bytes: out, get text() { return new TextDecoder().decode(out); }, value };
  };
  const act = (name, body) => ycall(Y.call, ["action", name, body === undefined ? "" : JSON.stringify(body)]);
  const q = (name, args = {}) => JSON.parse(ycall(Y.call, ["query", name, JSON.stringify(args)]).text);
  const saved = () => ycall(Y.host_saved, []);
  const savedPath = () => ycall(Y.host_saved_path, []).text;
  const open = (name, data, discard = 0) => ycall(Y.host_open, [name, data], discard);
  const ids = (els) => JSON.stringify(els.map((e) => e.id).sort());
  const isV2 = (b) => dec.decode(b.subarray(0, 7)) === "bimble\0";
  const UNSAVED = 'unsaved changes: save them first, or send {"discard": true}';
  Y.guid_seed(1n);

  const nw = act("new");
  const s1 = q("state");
  check("new -> 200: Untitled.bimble, unsaved", nw.status === 200 && s1.file === "Untitled.bimble" && s1.unsaved, `${nw.text} ${JSON.stringify(s1)}`);
  const sto = q("storeys");
  check("new: one storey 'Level 0' at 0", sto.length === 1 && sto[0].name === "Level 0" && sto[0].elevation === 0, JSON.stringify(sto));
  const pl = ycall(Y.call, ["verb", "place", JSON.stringify({ a: [0, 0], kind: "column", storey: sto[0].id })]);
  check("place -> 200", pl.status === 200, pl.text);
  const un = open("x.bimble", new Uint8Array(0));
  check("host_open with unsaved changes -> 400", un.status === 400 && un.text === UNSAVED, un.text);
  // Save: in the module a new model's Save writes under its name (Rust's wasm Save downloads).
  const sv = act("save");
  const b1 = saved().bytes;
  check("save of the new model -> 200: v2 bytes named Untitled.bimble", sv.status === 200 && isV2(b1) && savedPath() === "Untitled.bimble", `${sv.text} ${savedPath()}`);
  const sa = act("save_as", { path: "x.bimble" });
  const b2 = saved().bytes;
  check("save_as x.bimble -> 200: v2, the same bytes, state file x.bimble", sa.status === 200 && isV2(b2) && savedPath() === "x.bimble" &&
    b2.length === b1.length && b2.every((b, i) => b === b1[i]) && q("state").file === "x.bimble", sa.text);
  const e1 = q("elements", { limit: 10000 });
  const r1 = JSON.parse(ycall(Y.change_json, []).text).rev;
  const o1 = open("x.bimble", b2);
  const e2 = q("elements", { limit: 10000 });
  check("host_open x.bimble (v2 bytes) -> 200, the same elements, rev + 1", o1.status === 200 && e1.length === 1 && ids(e1) === ids(e2) && o1.value === r1 + 1,
    `${o1.status} ${o1.text} ${e1.length} ${e2.length} rev ${r1} -> ${o1.value}`);

  // Ticket A5c: an IFC opens read-only (as the Rust viewer); flags 2 = action import_ifc.
  const o2 = open("frame.ifc", read("ifc/frame.ifc"), 2);
  const s2 = q("state");
  const golden = Object.keys(JSON.parse(read("ifc/frame_reimport.json")).model.elements).length;
  check("host_open import frame.ifc -> 200: the copy frame.bimble, unsaved, elements = the import golden's",
    o2.status === 200 && s2.file === "frame.bimble" && s2.unsaved && q("elements", { limit: 10000 }).length === golden, `${o2.text} ${JSON.stringify(s2)}`);

  // Ticket D5: sheet_svg in the module = the golden sheet/S-101.svg once the file is named frame.json
  // (the project), the clock is 2026-01-01 (the date) and the frame's version notes are cut out
  // (the C core's own version; the note's place follows its width). The native answer is checked by
  // the suite draw_sheet.
  Y.host_clock(Date.UTC(2026, 0, 1) / 1000);
  const od = open("frame.json", read("drawing/model.json"), 1);
  const s101 = od.status === 200 ? q("sheets").find((x) => x.number === "S-101") : null;
  const cutNote = (t) => t.replace(/<text [^>]*>Bimble [^<]*<\/text>\n/, "");
  const ss = s101 ? ycall(Y.call, ["query", "sheet_svg", JSON.stringify({ id: s101.id })]) : { status: 0, text: od.text };
  eq("sheet_svg S-101 (frame.json, clock 2026-01-01) = golden/drawing/sheet/S-101.svg (version note cut)",
    ss.status === 200 ? cutNote(JSON.parse(ss.text).svg) : `${ss.status} ${ss.text}`, cutNote(read("drawing/sheet/S-101.svg")));
  const o3 = open("frame.json", read("frame.json"), 1);
  check("host_open frame.json, discard -> 200", o3.status === 200 && q("state").file === "frame.json", o3.text);
  Y.host_clock(Date.UTC(2026, 0, 1) / 1000);
  const ex = act("export_ifc4");
  eq("export_ifc4 (clock 2026-01-01) = golden/ifc/frame.ifc (version masked)", dec.decode(saved().bytes).replace("'bimble 0.1.0-ccore'", "'bimble <version>'"), read("ifc/frame.ifc"));
  check("export_ifc4 -> 200, named frame.ifc", ex.status === 200 && savedPath() === "frame.ifc", `${ex.text} ${savedPath()}`);
  const ex3 = act("export_ifc2x3", { path: "f3.ifc" });
  check("export_ifc2x3 {path} -> IFC2X3 text named f3.ifc", ex3.status === 200 && savedPath() === "f3.ifc" && dec.decode(saved().bytes).includes("FILE_SCHEMA(('IFC2X3'));"));
  const sj = act("save_as", { path: "y.json" });
  eq("save_as y.json: the v1 text (= frame.json)", sj.status === 200 ? dec.decode(saved().bytes) : `${sj.status} ${sj.text}`, read("frame.json"));
  const sb = act("save_as", { path: "f.bimble" });
  const fb = saved().bytes;
  check("save_as f.bimble: v2, gzip of golden/bin/frame.cbor", sb.status === 200 && isV2(fb) &&
    Buffer.compare(gunzipSync(fb.subarray(7)), readFileSync(join(G, "bin/frame.cbor"))) === 0);
  // Read-only files (ticket A5c).
  const READ_ONLY = "this file is read only: import it to edit";
  const meshOf = () => {
    const m = ycall(Y.host_mesh, [], 0);
    const dv = new DataView(m.bytes.buffer, m.bytes.byteOffset, m.bytes.byteLength);
    return { status: m.status, elements: m.bytes.length >= 24 ? dv.getUint32(12, true) : -1, triangles: m.bytes.length >= 24 ? dv.getUint32(20, true) / 3 : -1 };
  };
  const fm = meshOf();
  const sh = act("share_for_viewing");
  let raw = new Uint8Array(0);
  try { raw = gunzipSync(saved().bytes); } catch {}
  check("share_for_viewing of f.bimble -> 200: f.bimbleview, inflated = golden/view/frame.bimbleview.raw",
    sh.status === 200 && savedPath() === "f.bimbleview" && Buffer.compare(raw, readFileSync(join(G, "view/frame.bimbleview.raw"))) === 0, `${sh.text} ${savedPath()} ${raw.length}`);
  const view = saved().bytes;
  const ov = open("f.bimbleview", view);
  const svw = q("state");
  const vm = meshOf();
  check("host_open f.bimbleview -> 200, read-only, mesh elements and triangles = frame's",
    ov.status === 200 && svw.editable === false && svw.file === "f.bimbleview" && vm.status === 200 && vm.elements === fm.elements && fm.elements === 72 && vm.triangles === fm.triangles,
    `${ov.text} ${JSON.stringify(svw)} ${JSON.stringify(vm)} vs ${JSON.stringify(fm)}`);
  // Ticket V1: the opened view file shared again: the same raw stream.
  const shv = act("share_for_viewing", { path: "again.bimbleview" });
  let rawAgain = new Uint8Array(0);
  try { rawAgain = gunzipSync(saved().bytes); } catch {}
  check("share_for_viewing of the open f.bimbleview -> 200: again.bimbleview, inflated = golden/view/frame.bimbleview.raw",
    shv.status === 200 && savedPath() === "again.bimbleview" && Buffer.compare(rawAgain, readFileSync(join(G, "view/frame.bimbleview.raw"))) === 0, `${shv.text} ${savedPath()} ${rawAgain.length}`);
  const mi = open("mapped_item.ifc", read("ifc/fixtures/mapped_item.ifc"));
  const sm = q("state");
  const [, gEls, gTris] = /^elements (\d+) triangles (\d+)/m.exec(read("ifc/read/mapped_item/scene.txt")).map(Number);
  const mm = meshOf();
  check("host_open mapped_item.ifc -> 200, read-only, schema IFC4; mesh = golden/ifc/read/mapped_item/scene.txt's counts",
    mi.status === 200 && sm.editable === false && sm.schema === "IFC4" && mm.status === 200 && mm.elements === gEls && gEls === 5 && mm.triangles === gTris,
    `${mi.text} ${JSON.stringify(sm)} ${JSON.stringify(mm)} vs ${gEls} ${gTris}`);
  const vp = ycall(Y.call, ["verb", "place", JSON.stringify({ a: [0, 0], kind: "column" })]);
  check("read-only: verb place -> 400 Rust's read-only text", vp.status === 400 && vp.text === READ_ONLY, vp.text);
  const acts = Object.fromEntries(q("actions").map((a) => [a.name, a.enabled]));
  check("read-only in the module: place, save, export off; convert_open off (no files)", !acts.place_column && !acts.save && !acts.export_ifc4 && !acts.convert_open && acts.open, JSON.stringify(acts));
  // Ticket V1: the read-only IFC shared for viewing (ribbon.rs:575), its view file opens with 5 elements.
  const shi = act("share_for_viewing");
  const miv = saved().bytes;
  check("read-only IFC: share_for_viewing on -> 200: mapped_item.bimbleview (gzip)",
    acts.share_for_viewing === true && shi.status === 200 && savedPath() === "mapped_item.bimbleview" && miv[0] === 0x1f, `${shi.text} ${savedPath()}`);
  const omv = open("mapped_item.bimbleview", miv);
  const mmv = meshOf();
  check("host_open mapped_item.bimbleview -> 200, read-only, mesh 5 elements and the read scene's triangles",
    omv.status === 200 && q("state").editable === false && mmv.elements === 5 && mmv.triangles === gTris, `${omv.text} ${JSON.stringify(mmv)}`);
  const im = open("mapped_item.ifc", read("ifc/fixtures/mapped_item.ifc"), 2);
  check("host_open import mapped_item.ifc -> editable copy", im.status === 200 && q("state").editable === true && q("state").file === "mapped_item.bimble", im.text);
  open("f.bimble", fb, 1);
  const bad = open("bad.bimble", "not a model", 1);
  check("host_open of a broken file -> 400, the open model kept", bad.status === 400 && q("state").file === "f.bimble", bad.text);
  const cl = act("close");
  const s3 = q("state");
  check("close -> 200, no model", cl.status === 200 && s3.file === null, `${cl.text} ${JSON.stringify(s3)}`);
  Y.guid_seed(0n);
  end();
}

// ---- bounds (ticket FIXLIB): a CBOR count whose DOM size wraps on wasm32 ----
// 89,478,486 items x 48 bytes (sizeof(JrVal) on wasm32) is 2^32 + 32: the old cbor.c asked the
// arena for 32 bytes, then wrote 89 M items past them. Now refused (the DOM budget, json_r.h) before
// any allocation, the module still usable.
suite("bounds");
{
  const cnt = 89478486;
  const body = Buffer.alloc(5 + cnt);
  body[0] = 0x9a;
  body.writeUInt32BE(cnt, 1);
  const file = Buffer.concat([Buffer.from("bimble\0", "latin1"), gzipSync(body, { level: 1 })]);
  const o = openText(file);
  check("a count that wraps on wasm32 -> 400, the value budget", o.status === 400 && /too many values/.test(o.text), `${o.status} ${o.text}`);
  const again = openText(read("frame.json"));
  check("frame.json still opens after it", again.status === 200, again.text);
  if (again.status === 200) X.close(again.value);
  end();
}

// ---- numbers ----
{
  const frameText = read("frame.json");
  const runs = [];
  for (let i = 0; i < 30; i++) {
    const a = performance.now();
    const o = openText(frameText);
    const b = performance.now();
    const m = call(X.mesh, [], o.value, 0);
    const c = performance.now();
    X.close(o.value);
    runs.push([b - a, c - b, m.bytes.length]);
  }
  const med = (j) => runs.map((r) => r[j]).sort((x, y) => x - y)[runs.length >> 1];
  console.log(`ccore.wasm: ${bytes.length} bytes, ${gzipSync(bytes, { level: 9 }).length} gzip -9; instantiate ${instantiateMs.toFixed(2)} ms`);
  console.log(`frame.json (${frameText.length} bytes): first open ${runs[0][0].toFixed(2)} ms + scene+mesh ${runs[0][1].toFixed(2)} ms; ` +
    `median of 30: open ${med(0).toFixed(3)} ms, scene+mesh ${med(1).toFixed(3)} ms (BMSH ${runs[0][2]} bytes)`);
}

const failed = suites.reduce((a, s) => a + s.failed, 0), passed = suites.reduce((a, s) => a + s.passed, 0);
console.log(`wasm_run: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
