// What a frame would draw from the tile pyramids (ticket TILE1, ADR 0078 amendment 6; ccore/src/tiles.h): per set, its
// pyramid (tiles, levels, triangles per level, bytes, build time and peak memory) and, for a far (site), a close and an
// inside camera at 1 px and 2 px, what the reference walker (cc_tiles_select, through GET /mesh?tiles_frame) selects:
// tiles, triangles, bytes to fetch. Also the leaf formats compared (the leaves' answers as served against deflate and a
// raw 21-bit layout; decode times) and, with --png, per-level pictures (the selected tiles rasterised in software).
//   node ccore/tools/tiles.mjs [--sets synth17,heavy,large,buildings] [--png DIR] [--out FILE]
//   node ccore/tools/tiles.mjs --folder <dir of IFC> --cache-dir <dir> [--project <file.bimble>]   (a local folder: the
//        caches go to --cache-dir only, never beside the files; the report to --out, default ccore/out/tiles/local.md)
// A native host (out/tcc/bimble-ccore, or HOST_BIN): build it first. Scratch under ccore/out/tiles/ (on disk).
// Synthetic sets: synth17 (bench/links_synth.mjs: 17 links of 160 unique closed meshes, placed 30 m apart, as
// frames.mjs --links 17), heavy (bench/mesh_heavy_model.py 700 x 1000), large (bench/large_model.py 20026), buildings
// (test/tiles_test.c's generator: 18 buildings of 10 storeys, slabs and facades around 400 small parts a storey).
import { execFileSync, spawn } from "node:child_process";
import { deflateRawSync } from "node:zlib";
import { existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { hostEnv } from "../test/lib/host_env.mjs";
import { port } from "../test/ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const T = await import(pathToFileURL(join(CCORE, "../web-ui/src/gl/tiles.ts")).href);
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const args = process.argv.slice(2);
const opt = (k) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : undefined; };
const SHELL = args.includes("--shell"); // SHELL1: the exterior shell's numbers (scripted outside paths, the truth renders)
const SETS = (opt("--sets") ?? (SHELL ? "synth17,buildings" : "synth17,heavy,large,buildings")).split(",");
const COMPARE = (opt("--shell-compare") ?? "").split(",").filter(Boolean).map(Number); // e.g. 64,256,1024: views against the last
const SHELL_RES = Number(opt("--shell-res") ?? 1024);
const XCOMPARE = (opt("--cross-compare") ?? "").split(",").filter(Boolean).map(Number); // the cross-link pass's views, against the last
const TRUTH = !args.includes("--no-truth");
const FOLDER = opt("--folder"), CACHE_DIR = opt("--cache-dir"), PROJECT = opt("--project"), PNG = opt("--png");
const WORK = join(CCORE, "out/tiles");
const OUT = opt("--out") ?? join(WORK, (SHELL ? "shell1-" : "") + (FOLDER ? "local.md" : "tiles.md"));
const TOKEN = "tiles-tool";
mkdirSync(WORK, { recursive: true });
const log = (...a) => console.error("tiles:", ...a);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const hosts = [];
process.on("exit", () => { for (const h of hosts) try { h.kill("SIGKILL"); } catch { /* gone */ } });

async function startHost(name, env = {}, argv = []) {
  const h = spawn(BIN, [String(port(name)), ...argv], { env: hostEnv({ BIMBLE_TOKEN: TOKEN, ...env }), stdio: ["ignore", "ignore", "pipe"] });
  h.log = "";
  h.stderr.on("data", (d) => (h.log += d));
  hosts.push(h);
  for (let i = 0; i < 2400 && !h.log.includes("listening"); i++) await sleep(25);
  const p = Number(h.log.match(/listening (\d+)/)?.[1]);
  if (!p) throw new Error(`host ${name}: ${h.log.slice(-400)}`);
  const call = async (op, nm, body = {}) => {
    const r = await fetch(`http://127.0.0.1:${p}/${op}/${encodeURIComponent(nm)}?t=${TOKEN}`, { method: "POST", body: JSON.stringify(body) });
    return { status: r.status, text: await r.text() };
  };
  const get = async (q) => {
    const r = await fetch(`http://127.0.0.1:${p}/mesh?t=${TOKEN}&${q}`, { cache: "no-store" });
    return { status: r.status, bytes: new Uint8Array(await r.arrayBuffer()) };
  };
  const rss = () => { try { return Number(/VmHWM:\s+(\d+)/.exec(readFileSync(`/proc/${h.pid}/status`, "utf8"))[1]) * 1024; } catch { return 0; } };
  return { h, call, get, rss, kill: () => h.kill("SIGKILL") };
}
const json = (b) => JSON.parse(Buffer.from(b).toString("utf8"));
const MB = (b) => (b / 1e6).toFixed(2) + " MB";
const fmt = (n) => Math.round(n).toLocaleString("en-US");

/** The pyramid of every link (and the open scene) the host serves, its numbers. */
async function pyramids(H, nlinks, open) {
  const out = [];
  const one = async (who, name) => {
    const g = await H.get(`${who}tiles=toc`);
    if (g.status !== 200) return;
    const toc = T.parseToc(g.bytes);
    const lv = new Array(toc.levels).fill(0), lvTiles = new Array(toc.levels).fill(0), lvErr = new Array(toc.levels).fill(0);
    let leaves = 0, proxies = 0, occ = 0, proxyTris = 0;
    for (let i = 0; i < toc.ntiles; i++) {
      lv[toc.level[i]] += toc.tris[i], lvTiles[toc.level[i]]++;
      if (toc.flags[i] & T.TILE_PROXY) lvErr[toc.level[i]] = Math.max(lvErr[toc.level[i]], toc.error[i]);
      if (toc.flags[i] & T.TILE_LEAF) leaves++; else if (toc.flags[i] & T.TILE_PROXY) proxies++, (proxyTris += toc.tris[i]);
      occ += toc.nocc_[i];
    }
    out.push({ who, name, toc, lv, lvTiles, lvErr, leaves, proxies, occ, proxyTris, sectionBytes: toc.blobAt + toc.blobBytes, tocBytes: toc.tocBytes, occBytes: 36 * toc.nocc });
  };
  if (open) await one("", "(open)");
  for (let k = 0; k < nlinks; k++) await one(`link=${k}&`, `link ${k}`);
  return out;
}

/** The leaves' answers of a pyramid: bytes as served, deflated per tile, a raw 21-bit layout; decode times. */
async function leafFormats(H, p) {
  const r = { served: 0, deflated: 0, raw21: 0, proxies: 0, proxiesDeflated: 0, decodeMs: 0, copyMs: 0, leaves: 0, edgesFree: 0 };
  const ids = [...Array(p.toc.ntiles).keys()];
  for (let i = 0; i < ids.length; i += 256) {
    const g = await H.get(`${p.who}tile=${ids.slice(i, i + 256).join(",")}`);
    if (g.status !== 200) continue;
    for (const a of T.parseTils(g.bytes)) {
      if (a.kind === 2) { r.proxies += a.bytes.length, r.proxiesDeflated += deflateRawSync(a.bytes).length; continue; }
      if (a.kind !== 1) continue;
      r.leaves++, r.served += a.bytes.length, r.deflated += deflateRawSync(a.bytes).length;
      const t0 = performance.now();
      const leaf = T.decodeLeaf(a.bytes);
      r.decodeMs += performance.now() - t0;
      // the raw layout: 8 bytes a vertex (21-bit lattice), u16/u32 indices, 4 bytes an element run, 52 an instance
      const ib = (nv) => (nv <= 65536 ? 2 : 4);
      let raw = leaf.pos.length / 3 * 8 + leaf.idx.length * ib(leaf.pos.length / 3) + leaf.runs.length / 3 * 12;
      for (const s of leaf.shapes) raw += s.pos.length / 3 * 8 + s.idx.length * ib(s.pos.length / 3) + s.instances.length * 52;
      r.raw21 += raw;
      const buf = new Uint8Array(raw), t1 = performance.now();
      buf.slice(); // a raw layout's "decode": a copy
      r.copyMs += performance.now() - t1;
    }
  }
  return r;
}

/** The cameras of a world box: site (all of it in view), close (a fifth of that distance), inside (a third in, at
 *  eye height above the lowest floor, looking at the centre). */
function cameras(b) {
  const c = [0, 1, 2].map((k) => (b[k] + b[3 + k]) / 2), d = Math.hypot(b[3] - b[0], b[4] - b[1], b[5] - b[2]) / 2;
  const site = d / Math.tan(Math.PI / 6) * 1.1;
  const dir = [-0.6, -0.6, 0.53];
  const at = (s) => [c[0] + dir[0] * s, c[1] + dir[1] * s, c[2] + dir[2] * s];
  const z = b[2] + Math.min(1700, (b[5] - b[2]) / 4);
  return {
    far: { eye: at(site), at: c },
    close: { eye: at(site / 5), at: c },
    inside: { eye: [b[0] + (b[3] - b[0]) / 3, b[1] + (b[4] - b[1]) / 3, z], at: [c[0], c[1], z] },
  };
}

async function frames(H, bounds) {
  const cams = cameras(bounds), out = {};
  for (const [name, cam] of Object.entries(cams))
    for (const err of [1, 2])
      for (const occ of [0, 1]) {
        const q = `tiles_frame=1&eye=${cam.eye.join(",")}&at=${cam.at.join(",")}&fov=60&w=1920&h=1080&err=${err}&leaf_bytes=1&occ=${occ}`;
        const t0 = performance.now(), g = await H.get(q), ms = performance.now() - t0;
        out[`${name}@${err}${occ ? "o" : ""}`] = { ...json(g.bytes), ms, cam };
      }
  return out;
}

/** First picture: every table and each pyramid's root tile. */
function firstPicture(pys) {
  let b = 0, tris = 0;
  for (const p of pys) b += p.tocBytes + (p.toc.flags[0] & T.TILE_PROXY ? p.toc.len[0] : 0), (tris += p.toc.tris[0]);
  return { bytes: b, tris };
}

// ---- software pictures of the selected tiles (flat shading, z-buffer), per level ----
async function picture(H, pys, cam, file, opts = {}) {
  const W = 640, Hh = 400, z = new Float32Array(W * Hh).fill(Infinity), img = new Uint8Array(W * Hh * 3).fill(255);
  const f = [0, 1, 2].map((k) => cam.at[k] - cam.eye[k]), fl = Math.hypot(...f);
  for (let k = 0; k < 3; k++) f[k] /= fl;
  let up = [0, 0, 1];
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const r = cross(f, up), rl = Math.hypot(...r); for (let k = 0; k < 3; k++) r[k] /= rl;
  const u = cross(r, f), th = Math.tan(Math.PI / 6), kpx = Hh / 2 / th;
  const PAL = [[176, 176, 168], [120, 140, 170], [190, 150, 100], [180, 100, 80], [230, 220, 120], [140, 110, 80], [150, 200, 220], [160, 160, 160]];
  let drawn = 0;
  const tri = (a, b, c, col) => {
    const P = [a, b, c].map((p) => { const d = [p[0] - cam.eye[0], p[1] - cam.eye[1], p[2] - cam.eye[2]]; const zc = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
      return [W / 2 + kpx * (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / zc, Hh / 2 - kpx * (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) / zc, zc]; });
    if (P.some((p) => p[2] < 10)) return;
    const n = cross([b[0] - a[0], b[1] - a[1], b[2] - a[2]], [c[0] - a[0], c[1] - a[1], c[2] - a[2]]), nl = Math.hypot(...n) || 1;
    const light = 0.55 + 0.45 * Math.abs((n[0] * 0.3 + n[1] * 0.5 + n[2] * 0.81) / nl);
    const x0 = Math.max(0, Math.floor(Math.min(P[0][0], P[1][0], P[2][0]))), x1 = Math.min(W - 1, Math.ceil(Math.max(P[0][0], P[1][0], P[2][0])));
    const y0 = Math.max(0, Math.floor(Math.min(P[0][1], P[1][1], P[2][1]))), y1 = Math.min(Hh - 1, Math.ceil(Math.max(P[0][1], P[1][1], P[2][1])));
    const ar = (P[1][0] - P[0][0]) * (P[2][1] - P[0][1]) - (P[2][0] - P[0][0]) * (P[1][1] - P[0][1]);
    if (!ar || (x1 - x0) * (y1 - y0) > W * Hh) return;
    drawn++;
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) {
      const px = x + 0.5, py = y + 0.5;
      const w0 = ((P[1][0] - px) * (P[2][1] - py) - (P[2][0] - px) * (P[1][1] - py)) / ar, w1 = ((P[2][0] - px) * (P[0][1] - py) - (P[0][0] - px) * (P[2][1] - py)) / ar, w2 = 1 - w0 - w1;
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      const zz = w0 * P[0][2] + w1 * P[1][2] + w2 * P[2][2], o = y * W + x;
      if (zz >= z[o]) continue;
      z[o] = zz;
      for (let k = 0; k < 3; k++) img[3 * o + k] = Math.min(255, col[k] * light);
    }
  };
  for (const p of pys) {
    const sel = opts.level !== undefined ? [...Array(p.toc.ntiles).keys()].filter((i) => p.toc.level[i] === Math.min(opts.level, p.toc.levels - 1) || (p.toc.flags[i] & T.TILE_LEAF && p.toc.level[i] < opts.level))
      : opts.ids?.[p.name] ?? [];
    for (let i = 0; i < sel.length; i += 128) {
      const g = await H.get(`${p.who}tile=${sel.slice(i, i + 128).join(",")}`);
      if (g.status !== 200) continue;
      const xf = p.xf ?? ((q) => q);
      for (const a of T.parseTils(g.bytes)) {
        if (a.kind === 2) {
          const pr = T.decodeProxy(a.bytes), pos = T.proxyPositions(pr, p.toc.box.subarray(6 * a.id, 6 * a.id + 6));
          for (const part of pr.parts) for (let t = part.first; t < part.first + part.count; t += 3) {
            const v = [0, 1, 2].map((q) => xf([pos[3 * pr.idx[t + q]], pos[3 * pr.idx[t + q] + 1], pos[3 * pr.idx[t + q] + 2]]));
            tri(v[0], v[1], v[2], PAL[part.category] ?? PAL[7]);
          }
        } else if (a.kind === 1) {
          const lf = T.decodeLeaf(a.bytes);
          for (let t = 0; t < lf.idx.length; t += 3) tri(...[0, 1, 2].map((q) => xf([lf.pos[3 * lf.idx[t + q]], lf.pos[3 * lf.idx[t + q] + 1], lf.pos[3 * lf.idx[t + q] + 2]])), PAL[0]);
          for (const s of lf.shapes) for (const ins of s.instances) {
            const m = ins.placement, pl = (i) => [m[0] * s.pos[3 * i] + m[3] * s.pos[3 * i + 1] + m[6] * s.pos[3 * i + 2] + m[9],
              m[1] * s.pos[3 * i] + m[4] * s.pos[3 * i + 1] + m[7] * s.pos[3 * i + 2] + m[10], m[2] * s.pos[3 * i] + m[5] * s.pos[3 * i + 1] + m[8] * s.pos[3 * i + 2] + m[11]];
            for (let t = 0; t < s.idx.length; t += 3) tri(xf(pl(s.idx[t])), xf(pl(s.idx[t + 1])), xf(pl(s.idx[t + 2])), PAL[2]);
          }
        }
      }
    }
  }
  // PPM, then PNG through python's zlib (no dependency)
  const ppm = Buffer.concat([Buffer.from(`P6 ${W} ${Hh} 255\n`), Buffer.from(img)]);
  writeFileSync(file.replace(/\.png$/, ".ppm"), ppm);
  try {
    execFileSync("python3", ["-I", "-c", `
import zlib,struct,sys
d=open(sys.argv[1],'rb').read(); h=d.index(b'255\\n')+4; w,hh=${W},${Hh}; px=d[h:]
raw=b''.join(b'\\x00'+px[y*w*3:(y+1)*w*3] for y in range(hh))
def ch(t,b): return struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b)&0xffffffff)
open(sys.argv[2],'wb').write(b'\\x89PNG\\r\\n\\x1a\\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,hh,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw,6))+ch(b'IEND',b''))
`, file.replace(/\.png$/, ".ppm"), file]);
    rmSync(file.replace(/\.png$/, ".ppm"));
  } catch { /* the PPM stays */ }
  return drawn;
}

// ---- SHELL1: the exterior shell ----
/** The shell table of a pyramid (tiles=shell): per tile its exterior triangles and its shell proxy. */
function parseShell(b, ntiles) {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  if (b.length < 32 || String.fromCharCode(...b.subarray(0, 4)) !== "TSH1" || dv.getUint32(8, true) !== ntiles) throw new Error("shell table");
  const ext = new Uint32Array(ntiles), flags = new Uint32Array(ntiles), tris = new Uint32Array(ntiles), len = new Uint32Array(ntiles);
  for (let i = 0; i < ntiles; i++) { const o = 32 + 32 * i; ext[i] = dv.getUint32(o, true), flags[i] = dv.getUint32(o + 4, true), len[i] = dv.getUint32(o + 16, true), tris[i] = dv.getUint32(o + 20, true); }
  return { views: dv.getUint32(12, true), res: dv.getUint32(16, true), tol: dv.getUint32(20, true), exterior: dv.getUint32(24, true), ext, flags, tris, len };
}

/** The scripted outside paths of a world box: orbits at three distances (8 azimuths, 25 deg up) and an approach down to a
 *  face of the box (from the site distance to 2 m outside it, at eye height), and one inside camera (the shell must not apply). */
function shellCameras(b) {
  const c = [0, 1, 2].map((k) => (b[k] + b[3 + k]) / 2), r = Math.hypot(b[3] - b[0], b[4] - b[1], b[5] - b[2]) / 2;
  const site = r / Math.tan(Math.PI / 6) * 1.1, cams = [];
  for (const [nm, d] of [["orbit1", site], ["orbit1/2", site / 2], ["orbit1/4", Math.max(site / 4, r * 1.05)]])
    for (let a = 0; a < 8; a++) {
      const az = a * Math.PI / 4 + 0.3, el = 25 * Math.PI / 180;
      cams.push({ name: `${nm}@${a * 45}`, path: nm, eye: [c[0] + d * Math.cos(el) * Math.cos(az), c[1] + d * Math.cos(el) * Math.sin(az), c[2] + d * Math.sin(el)], at: c });
    }
  const z = b[2] + Math.min(1700, (b[5] - b[2]) / 4);
  for (const d of [site, site / 3, site / 10, 20000, 5000, 2000])
    cams.push({ name: `approach ${Math.round(d / 1000)} m`, path: "approach", eye: [c[0], b[1] - d, z], at: [c[0], b[1] + (b[4] - b[1]) / 2, z] });
  cams.push({ name: "inside", path: "inside", eye: [b[0] + (b[3] - b[0]) / 3, b[1] + (b[4] - b[1]) / 3, z], at: [c[0], c[1], z] });
  return cams;
}

async function shellReport(H, pys, bounds) {
  const per = [];
  for (const p of pys) { // per pyramid: the share marked exterior
    const g = await H.get(`${p.who}tiles=shell`);
    let sh = null;
    if (g.status === 200) sh = parseShell(g.bytes, p.toc.ntiles);
    let extEl = 0;
    for (let i = 0; i < p.toc.nlist; i++) extEl += (p.toc.elFlags[i] & 4) ? 1 : 0;
    per.push({ name: p.name, elements: p.toc.nlist, exterior: extEl, detail: p.toc.detail[0], extTris: sh ? sh.ext[0] : 0, baked: !!(p.toc.bake & 1),
      tiles: p.toc.ntiles, extTiles: [...Array(p.toc.ntiles).keys()].filter((i) => p.toc.flags[i] & 16).length, shellProxyBytes: sh ? sh.len.reduce((a, x, i) => a + (sh.flags[i] & 1 && x && p.toc.len[i] !== x ? x : 0), 0) : 0 });
  }
  // the cross-link pass (session/xshell.h): started by the first frame asking the shell, waited for
  let xs = null;
  const cams = shellCameras(bounds), q0 = `tiles_frame=1&eye=${cams[0].eye.join(",")}&at=${cams[0].at.join(",")}&fov=60&w=1920&h=1080&err=1&shell=priority`;
  const t0x = performance.now();
  for (let i = 0; i < 36000; i++) {
    xs = json((await H.get(q0)).bytes).xshell;
    if (!xs || xs.state !== 1) break;
    await sleep(100);
  }
  log(`cross-link pass: state ${xs?.state}, ${xs?.groups} groups of overlapping links, ${fmt(xs?.exterior ?? 0)} of ${fmt(xs?.elements ?? 0)} elements exterior, ${fmt(xs?.ms ?? 0)} ms (waited ${fmt(performance.now() - t0x)} ms), buffers ${MB(xs?.peak ?? 0)}`);
  const rows = [];
  for (const cam of cams) {
    const base = `tiles_frame=1&eye=${cam.eye.join(",")}&at=${cam.at.join(",")}&fov=60&w=1920&h=1080&err=1&leaf_bytes=1`;
    const r = { name: cam.name, path: cam.path };
    // priority (the default) selects what off selects, exterior first: its truth must miss nothing; the experimental filter
    // (the links' own shells, then the cross-link marks) for the record
    for (const [k, q] of [["off", "shell=off&occ=0"], ["offOcc", "shell=off&occ=1"], ["prio", `shell=priority&occ=0${TRUTH ? "&truth=1" : ""}`],
      ["on", "shell=filter&occ=0&xshell=0"], ["onOcc", "shell=filter&occ=1&xshell=0"], ["x", "shell=filter&occ=0"], ["xOcc", `shell=filter&occ=1${TRUTH ? "&truth=1" : ""}`]]) {
      const t0 = performance.now(), g = await H.get(`${base}&${q}`), ms = performance.now() - t0;
      if (g.status !== 200) throw new Error(`tiles_frame ${q}: ${g.status}`);
      const j = json(g.bytes);
      r[k] = { triangles: j.triangles, bytes: j.bytes, hidden: j.hidden_triangles, shellLinks: j.shell_links, xLinks: j.links.filter((l) => l.xshell).length, ms };
      if (k === "xOcc" && TRUTH) Object.assign(r, { seen: j.seen, missing: j.missing, missingOcc: j.missing_occ, missingTris: j.missing_triangles, missingList: j.missing_list });
      if (k === "prio" && TRUTH) Object.assign(r, { prioSeen: j.seen, prioMissing: j.missing, first: j.first_picture });
    }
    log(`${cam.name}: off ${fmt(r.off.triangles)}, off+occ ${fmt(r.offOcc.triangles)}, own shells ${fmt(r.on.triangles)} (${r.on.shellLinks} links), +occ ${fmt(r.onOcc.triangles)}; cross ${fmt(r.x.triangles)} (${r.x.xLinks} links), +occ ${fmt(r.xOcc.triangles)}${TRUTH ? `; seen ${fmt(r.seen)}, filter missing ${r.missing} (with occ ${r.missingOcc}); priority missing ${r.prioMissing}, first picture plain ${fmt(r.first?.plain ?? 0)} / priority ${fmt(r.first?.priority ?? 0)} of ${fmt(r.prio.triangles)} [${r.xOcc.ms.toFixed(0)} ms]` : ""}`);
    rows.push(r);
  }
  return { per, rows, xshell: xs };
}

/** Each link indexed again with shell=<views>x<res> for each count asked, its exterior elements against the largest count's. */
async function shellCompare(label, files, cache, place) {
  const out = [], sets = [];
  for (const v of COMPARE) {
    const dir = `${cache}-shell${v}`;
    rmSync(dir, { recursive: true, force: true });
    mkdirSync(dir, { recursive: true });
    const conv = convert(files, dir, { BIMBLE_TILES: `shell=${v}x${SHELL_RES}` });
    const H = await startHost(`tiles_tool_${label}_cmp${v}`, { BIMBLE_LINK_CACHE_MIN: "0", BIMBLE_CACHE_DIR: dir, BIMBLE_TILES: `shell=${v}x${SHELL_RES}` });
    await H.call("action", "new");
    await H.call("action", "link_model", { paths: files });
    for (let i = 0; i < 24000; i++) {
      const r = JSON.parse((await H.call("query", "links_panel", {})).text).models;
      if (r.length >= files.length && r.every((m) => m.state === "ready" && !m.indexing) || r.some((m) => m.state === "failed") || (i > 3000 && r.some((m) => m.state === "refused"))) break;
      await sleep(100);
    }
    const ext = [];
    for (let k = 0; k < files.length; k++) {
      const g = await H.get(`link=${k}&tiles=toc`);
      const set = new Set();
      if (g.status === 200) { const toc = T.parseToc(g.bytes); for (let i = 0; i < toc.nlist; i++) if (toc.elFlags[i] & 4) set.add(toc.list[i]); }
      ext.push(set);
    }
    H.kill();
    sets.push(ext);
    out.push({ views: v, ms: conv.reduce((n, c) => n + (c.shell?.ms ?? 0), 0), classifyMs: conv.reduce((n, c) => n + (c.shell?.classifyMs ?? 0), 0), exterior: ext.reduce((n, x) => n + x.size, 0), peak: Math.max(...conv.map((c) => c.shell?.peak ?? 0)) });
    rmSync(dir, { recursive: true, force: true });
  }
  const ref = sets[sets.length - 1];
  for (let i = 0; i < out.length; i++) {
    let missed = 0, extra = 0;
    const perLink = [];
    for (let k = 0; k < ref.length; k++) {
      let m = 0;
      for (const e of ref[k]) if (!sets[i][k].has(e)) m++;
      for (const e of sets[i][k]) if (!ref[k].has(e)) extra++;
      missed += m, perLink.push(m);
    }
    Object.assign(out[i], { missed, extra, perLink });
  }
  return out;
}

/** The cross-link pass computed again with <views>x<res> for each count asked (a host each), its marks against the last's;
 *  the outside walk's triangles for the site and half-site orbits at 0 deg. */
async function crossCompare(label, nlinks, bounds) {
  const out = [], sets = [];
  const cams = shellCameras(bounds).filter((c) => c.name === "orbit1@0" || c.name === "orbit1/2@0" || c.name === "approach 20 m");
  for (const v of XCOMPARE) {
    const env = { BIMBLE_LINK_CACHE_MIN: "0", BIMBLE_CACHE_DIR: CUR.cache, BIMBLE_TILES: `cross=${v}x${SHELL_RES}` };
    const H = await startHost(`tiles_tool_${label}_x${v}`, env, CUR.project ? [CUR.project] : []);
    if (!CUR.project) { await H.call("action", "new"); await H.call("action", "link_model", { paths: CUR.files }); }
    for (let i = 0; i < 24000; i++) {
      const r = JSON.parse((await H.call("query", "links_panel", {})).text).models;
      if (r.length >= nlinks && r.every((m) => m.state === "ready" && !m.indexing) || r.some((m) => m.state === "failed") || (i > 3000 && r.some((m) => m.state === "refused"))) break;
      await sleep(100);
    }
    if (CUR.place) for (let k = 0; k < nlinks; k++) {
      const nm = CUR.files[k].split("/").pop();
      await H.call("action", "manage_links", { op: "unpin", name: nm });
      await H.call("action", "manage_links", { op: "place", name: nm, move_mm: CUR.place(k) });
    }
    const q0 = `tiles_frame=1&eye=${cams[0].eye.join(",")}&at=${cams[0].at.join(",")}&fov=60&w=1920&h=1080&err=1&shell=priority`;
    let xs = null;
    const t0 = performance.now();
    for (let i = 0; i < 72000; i++) { xs = json((await H.get(q0)).bytes).xshell; if (!xs || xs.state !== 1) break; await sleep(100); }
    const marks = [];
    for (let k = 0; k < nlinks; k++) { const g = await H.get(`link=${k}&tiles=xshell`); marks.push(g.status === 200 ? g.bytes : null); }
    const tris = {};
    for (const c of cams) tris[c.name] = json((await H.get(`tiles_frame=1&eye=${c.eye.join(",")}&at=${c.at.join(",")}&fov=60&w=1920&h=1080&err=1&shell=filter`)).bytes).triangles;
    log(`cross ${v}x${SHELL_RES}: state ${xs?.state}, ${fmt(xs?.exterior ?? 0)} of ${fmt(xs?.elements ?? 0)} exterior, ${fmt(xs?.ms ?? 0)} ms (waited ${fmt(performance.now() - t0)}), triangles ${JSON.stringify(tris)}`);
    out.push({ views: v, state: xs?.state, ms: xs?.ms, exterior: xs?.exterior, elements: xs?.elements, peak: xs?.peak, tris });
    sets.push(marks);
    H.kill();
  }
  const ref = sets[sets.length - 1];
  for (let i = 0; i < out.length; i++) {
    let missed = 0, extra = 0;
    for (let k = 0; k < ref.length; k++) {
      const a = sets[i][k], b = ref[k];
      if (!a || !b || a.length !== b.length) continue;
      for (let j = 0; j < a.length; j++) { if (b[j] && !a[j]) missed++; if (a[j] && !b[j]) extra++; }
    }
    Object.assign(out[i], { missed, extra });
  }
  return out;
}
let CUR = {}; // the linked set being measured (crossCompare starts hosts of its own)

// ---- the sets ----
async function setSynth17() {
  const dir = join(WORK, "synth17"), cache = join(WORK, "synth17-cache");
  rmSync(dir, { recursive: true, force: true }), rmSync(cache, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  const G = await startHost("tiles_tool_gen", { BIMBLE_LINK_CACHE_MIN: "0", BIMBLE_CACHE_DIR: cache });
  const { makeLinkFolder } = await import("../bench/links_synth.mjs");
  const files = await makeLinkFolder({ hcall: G.call, dir, work: WORK, n: 17, proxies: 160, tris: 600, unique: true });
  G.kill();
  return { files, cache, place: (k) => [30000 * (k % 5), 30000 * Math.floor(k / 5), 0] };
}

/** link-convert --index of each file (one process each, timed; peak resident memory). */
function convert(files, cache, env = {}) {
  const rows = [];
  for (const f of files) {
    const t0 = Date.now();
    const o = execFileSync(BIN, ["link-convert", "--cache-dir", cache, "--index", f], { encoding: "utf8", maxBuffer: 1 << 24, env: { ...process.env, ...env } });
    const sh = /shell ms (\d+) classify_ms (\d+) exterior (\d+) elements (\d+) ext_tris (\d+) detail (\d+) bytes (\d+) peak (\d+) mixed (\d+)/.exec(o);
    const m = /indexed ok ms (\d+) hlod_ms (\d+) tiles (\d+) levels (\d+) proxy_tris (\d+) tiles_bytes (\d+) tiles_ms (\d+) cache_bytes (\d+) peak_rss_kB (\d+)/.exec(o);
    const c = /bytes (\d+) ms (\d+) elements (\d+) shapes (\d+) triangles (\d+)/.exec(o);
    const pk = Number(/peak_rss_kB (\d+)\n?$/m.exec(o)?.[1] ?? 0);
    rows.push({ file: f, ms: Date.now() - t0, convertMs: Number(c?.[2] ?? 0), elements: Number(c?.[3] ?? 0), triangles: Number(c?.[5] ?? 0), ifcBytes: Number(c?.[1] ?? 0),
      indexMs: Number(m?.[1] ?? 0), hlodMs: Number(m?.[2] ?? 0), tiles: Number(m?.[3] ?? 0), levels: Number(m?.[4] ?? 0), tilesBytes: Number(m?.[6] ?? 0), tilesMs: Number(m?.[7] ?? 0),
      cacheBytes: Number(m?.[8] ?? 0), indexPeak: Number(m?.[9] ?? 0) * 1024, peak: pk * 1024,
      shell: sh ? { ms: +sh[1], classifyMs: +sh[2], exterior: +sh[3], elements: +sh[4], extTris: +sh[5], detail: +sh[6], bytes: +sh[7], peak: +sh[8], mixed: +sh[9] } : null });
    log(`${f.split("/").pop()}: ${o.trim().split("\n").slice(-2).join(" | ")}`);
  }
  return rows;
}

async function linkedSet(label, files, cache, place, project) {
  CUR = { files, cache, place, project };
  const conv = convert(files, cache);
  if (SHELL && COMPARE.length) conv.compare = await shellCompare(label, files, cache, place);
  const H = await startHost(`tiles_tool_${label}`, { BIMBLE_LINK_CACHE_MIN: "0", BIMBLE_CACHE_DIR: cache }, project ? [project] : []);
  if (!project) {
    await H.call("action", "new");
    await H.call("action", "link_model", { paths: files });
  }
  const n = files.length;
  for (let i = 0; i < 24000; i++) {
    const r = JSON.parse((await H.call("query", "links_panel", {})).text).models;
    if (r.length >= n && r.every((m) => m.state === "ready" && !m.indexing) || r.some((m) => m.state === "failed") || (i > 3000 && r.some((m) => m.state === "refused"))) break; // refused: queued again by itself (SIZE2)
    await sleep(100);
  }
  { const r = JSON.parse((await H.call("query", "links_panel", {})).text).models; log(`${label}: links ${r.map((m) => `${m.state}${m.indexing ? "+indexing" : ""}`).join(" ")}`); }
  if (place) for (let k = 0; k < n; k++) {
    const nm = files[k].split("/").pop();
    await H.call("action", "manage_links", { op: "unpin", name: nm });
    await H.call("action", "manage_links", { op: "place", name: nm, move_mm: place(k) });
  }
  return report(label, H, n, false, conv);
}

async function openSet(label, model) {
  const H = await startHost(`tiles_tool_${label}`, {});
  const o = await H.call("action", "open", { path: model, discard: true });
  if (o.status !== 200) throw new Error(`open ${model}: ${o.text}`);
  const t0 = performance.now();
  let g;
  for (let i = 0; i < 100000; i++) { g = await H.get("tiles=toc&step=100000000"); if (g.status !== 202) break; }
  const buildMs = performance.now() - t0;
  return report(label, H, 0, true, null, buildMs);
}

async function report(label, H, nlinks, open, conv, buildMs) {
  const pys = await pyramids(H, nlinks, open);
  const bj = json((await H.get("tiles_frame=bounds")).bytes);
  const boxes = [];
  pys.forEach((p, i) => { // placements for the pictures (world = R p + o), in the order the frame walks them
    const d = bj.links[i];
    if (d) p.xf = (q) => [d.cos * q[0] - d.sin * q[1] + d.origin[0], d.sin * q[0] + d.cos * q[1] + d.origin[1], q[2] + d.origin[2]];
    const xf = p.xf ?? ((q) => q), b = [Infinity, Infinity, Infinity, -Infinity, -Infinity, -Infinity];
    for (let k = 0; k < 8; k++) {
      const w = xf([p.toc.box[k & 1 ? 3 : 0], p.toc.box[k & 2 ? 4 : 1], p.toc.box[k & 4 ? 5 : 2]]);
      for (let j = 0; j < 3; j++) b[j] = Math.min(b[j], w[j]), b[3 + j] = Math.max(b[3 + j], w[j]);
    }
    if (p.toc.ntiles) boxes.push(b);
  });
  // the main cluster: the links whose centre lies within 4 median sizes of the median centre (a link georeferenced far
  // away would make every camera far); those left out are counted
  const med = (a) => { const x = [...a].sort((u, v) => u - v); return x[Math.floor(x.length / 2)]; };
  const ctr = [0, 1, 2].map((k) => med(boxes.map((b) => (b[k] + b[3 + k]) / 2))), size = med(boxes.map((b) => Math.hypot(b[3] - b[0], b[4] - b[1], b[5] - b[2])));
  const main = boxes.filter((b) => Math.hypot(...[0, 1, 2].map((k) => (b[k] + b[3 + k]) / 2 - ctr[k])) <= 4 * size);
  const bounds = [0, 1, 2].map((k) => Math.min(...main.map((b) => b[k]))).concat([0, 1, 2].map((k) => Math.max(...main.map((b) => b[3 + k]))));
  const outliers = boxes.length - main.length;
  log(`${label}: cameras on ${main.length} of ${boxes.length} pyramids (${outliers} far away), box ${bounds.map((x) => Math.round(x / 1000)).join(",")} m`);
  const fr = SHELL ? {} : await frames(H, bounds);
  const shellRes = SHELL ? await shellReport(H, pys, bounds) : null;
  if (SHELL && XCOMPARE.length && nlinks) shellRes.xcompare = await crossCompare(label, nlinks, bounds, open);
  if (SHELL) { const pk = H.rss(); H.kill(); return { label, conv, shell: shellRes, hostPeak: pk, outliers, box: bounds.map((x) => Math.round(x / 1000)), buildMs }; }
  const formats = [];
  for (const p of pys) formats.push(await leafFormats(H, p));
  const sum = (f) => formats.reduce((n, x) => n + x[f], 0);
  const levels = [], errs = [];
  for (const p of pys) p.lv.forEach((t, i) => (levels[i] = (levels[i] ?? 0) + t)), p.lvErr.forEach((e, i) => (errs[i] = Math.max(errs[i] ?? 0, e)));
  const res = {
    label, links: nlinks, tiles: pys.reduce((n, p) => n + p.toc.ntiles, 0), leaves: pys.reduce((n, p) => n + p.leaves, 0), proxies: pys.reduce((n, p) => n + p.proxies, 0),
    maxLevels: Math.max(...pys.map((p) => p.toc.levels)), detail: pys.reduce((n, p) => n + p.toc.detail[0], 0), levels, errs,
    roots: pys.reduce((n, p) => n + p.toc.tris[0], 0), sectionBytes: pys.reduce((n, p) => n + p.sectionBytes, 0), tocBytes: pys.reduce((n, p) => n + p.tocBytes, 0),
    occ: pys.reduce((n, p) => n + p.occ, 0), occBytes: pys.reduce((n, p) => n + p.occBytes, 0), proxyTris: pys.reduce((n, p) => n + p.proxyTris, 0),
    leafServed: sum("served"), leafDeflated: sum("deflated"), leafRaw21: sum("raw21"), proxyBytes: sum("proxies"), proxyDeflated: sum("proxiesDeflated"),
    decodeMs: sum("decodeMs"), copyMs: sum("copyMs"), first: firstPicture(pys), frames: fr, conv, buildMs, hostPeak: H.rss(), outliers,
    box: bounds.map((x) => Math.round(x / 1000)),
  };
  if (PNG) {
    mkdirSync(PNG, { recursive: true });
    const cam = fr["far@1"].cam;
    res.pngs = [];
    for (let l = 0; l < res.maxLevels; l++) {
      let lt = 0; // the level's triangles (leaves above it too): pictures of at most 12 M triangles (a JS rasteriser)
      for (const p of pys) for (let i = 0; i < p.toc.ntiles; i++) if (p.toc.level[i] === Math.min(l, p.toc.levels - 1) || (p.toc.flags[i] & T.TILE_LEAF && p.toc.level[i] < l)) lt += p.toc.tris[i];
      if (lt > 12e6) { res.pngs.push(`level ${l}: ${fmt(lt)} triangles, not drawn`); continue; }
      const file = join(PNG, `tile1-${label}-level${l}.png`);
      const n = await picture(H, pys, cam, file, { level: l });
      res.pngs.push(`${file} (${fmt(n)} triangles)`);
    }
  }
  H.kill();
  log(`${label}: ${res.tiles} tiles, ${res.maxLevels} levels, ${fmt(res.detail)} detail triangles; site view at 1 px ${fmt(fr["far@1"].triangles)} triangles`);
  return res;
}

function md(rs) {
  const L = ["# Tile pyramids (TILE1)", "", `\`node ccore/tools/tiles.mjs\`, ${new Date().toISOString().slice(0, 10)}, native host (${BIN.split("/").slice(-2).join("/")}). Cameras 1920 x 1080, 60 deg; far = the whole set in view, close = a fifth of that distance, inside = a third into the box at eye height.`, ""];
  L.push("| set | detail tris | tiles (leaves / proxies) | levels | tris per level (root first) | largest proxy error per level, mm | pyramid bytes (tables) | occluders (bytes) | build |", "|---|---|---|---|---|---|---|---|---|");
  for (const r of rs) {
    const b = r.conv ? `index ${fmt(r.conv.reduce((n, c) => n + c.indexMs, 0))} ms (tiles ${fmt(r.conv.reduce((n, c) => n + c.tilesMs, 0))} ms), peak ${MB(Math.max(...r.conv.map((c) => c.indexPeak)))}` : `${fmt(r.buildMs)} ms in the host`;
    L.push(`| ${r.label} | ${fmt(r.detail)} | ${fmt(r.tiles)} (${fmt(r.leaves)} / ${fmt(r.proxies)}) | ${r.maxLevels} | ${r.levels.map(fmt).join(" / ")} | ${r.errs.map(fmt).join(" / ")} | ${MB(r.sectionBytes)} (${MB(r.tocBytes)}) | ${fmt(r.occ)} (${MB(r.occBytes)}) | ${b} |`);
  }
  for (const r of rs) if (r.outliers) L.push("", `${r.label}: the cameras frame ${r.box.join(", ")} m (lo xyz, hi xyz); ${r.outliers} pyramid(s) far from the others left out of that box (still walked).`);
  L.push("", "| set | view | 1 px: tiles, tris, bytes to fetch | 2 px: tiles, tris, bytes | 1 px with occlusion: tris (hidden), bytes | 2 px with occlusion | walk |", "|---|---|---|---|---|---|---|");
  for (const r of rs) for (const v of ["far", "close", "inside"]) {
    const a = r.frames[`${v}@1`], b = r.frames[`${v}@2`], ao = r.frames[`${v}@1o`], bo = r.frames[`${v}@2o`];
    L.push(`| ${r.label} | ${v} | ${fmt(a.tiles)}, ${fmt(a.triangles)}, ${MB(a.bytes)} | ${fmt(b.tiles)}, ${fmt(b.triangles)}, ${MB(b.bytes)} | ${fmt(ao.triangles)} (${fmt(ao.hidden_triangles)}), ${MB(ao.bytes)} | ${fmt(bo.triangles)} (${fmt(bo.hidden_triangles)}) | ${a.ms.toFixed(0)} / ${ao.ms.toFixed(0)} ms |`);
  }
  L.push("", "| set | first picture (tables + roots) | leaves served (BMG1 from ELEM) | deflated | raw 21-bit | proxies raw / deflated | leaf decode / copy |", "|---|---|---|---|---|---|---|");
  for (const r of rs) L.push(`| ${r.label} | ${MB(r.first.bytes)}, ${fmt(r.first.tris)} tris | ${MB(r.leafServed)} | ${MB(r.leafDeflated)} | ${MB(r.leafRaw21)} | ${MB(r.proxyBytes)} / ${MB(r.proxyDeflated)} | ${r.decodeMs.toFixed(0)} / ${r.copyMs.toFixed(1)} ms |`);
  for (const r of rs) if (r.pngs) L.push("", `${r.label} per level (far camera): ${r.pngs.join("; ")}`);
  for (const r of rs) if (r.conv) {
    const ifc = r.conv.reduce((n, c) => n + c.ifcBytes, 0), cb = r.conv.reduce((n, c) => n + c.cacheBytes, 0);
    L.push("", `${r.label}: IFC ${MB(ifc)}, caches ${MB(cb)} (${(100 * cb / ifc).toFixed(1)} %), of which TILE ${MB(r.conv.reduce((n, c) => n + c.tilesBytes, 0))}; conversion ${fmt(r.conv.reduce((n, c) => n + c.convertMs, 0))} ms, indexing ${fmt(r.conv.reduce((n, c) => n + c.indexMs, 0))} ms (HLOD ${fmt(r.conv.reduce((n, c) => n + c.hlodMs, 0))}, TILE ${fmt(r.conv.reduce((n, c) => n + c.tilesMs, 0))}), the largest process peak ${MB(Math.max(...r.conv.map((c) => c.peak)))}, indexing peak ${MB(Math.max(...r.conv.map((c) => c.indexPeak)))}`);
  }
  L.push("", "```json", JSON.stringify(rs.map((r) => ({ ...r, frames: Object.fromEntries(Object.entries(r.frames).map(([k, v]) => [k, { tiles: v.tiles, triangles: v.triangles, bytes: v.bytes }])) })), null, 0), "```", "");
  return L.join("\n");
}

function mdShell(rs) {
  const L = ["# The exterior shell (SHELL1)", "", `\`node ccore/tools/tiles.mjs --shell\`, ${new Date().toISOString().slice(0, 10)}, native host (${BIN.split("/").slice(-2).join("/")}). Cameras 1920 x 1080, 60 deg, 1 px of error; orbits at the site distance, a half and a quarter of it (8 azimuths, 25 deg up), an approach down to 2 m from the box's -y face at eye height, one inside camera. Triangles selected: shell off / on (auto: links whose box the eye is outside, 2 m margin) x occluders off / on. Missing: elements seen in a full-detail software render (glazing not occluding) that the shell walk does not depict (occ: or that the occluders hide).`, ""];
  for (const r of rs) {
    const per = r.shell.per;
    L.push(`## ${r.label}`, "", "| pyramid | elements | exterior | % | detail tris | exterior tris | % | tiles with exterior | shell proxies bytes |", "|---|---|---|---|---|---|---|---|---|");
    for (const p of per) L.push(`| ${p.name} | ${fmt(p.elements)} | ${fmt(p.exterior)} | ${(100 * p.exterior / Math.max(1, p.elements)).toFixed(1)} | ${fmt(p.detail)} | ${fmt(p.extTris)} | ${(100 * p.extTris / Math.max(1, p.detail)).toFixed(1)} | ${p.extTiles} / ${p.tiles} | ${fmt(p.shellProxyBytes)} |`);
    const te = per.reduce((n, p) => n + p.exterior, 0), tl = per.reduce((n, p) => n + p.elements, 0), tt = per.reduce((n, p) => n + p.extTris, 0), td = per.reduce((n, p) => n + p.detail, 0);
    L.push(`| all | ${fmt(tl)} | ${fmt(te)} | ${(100 * te / Math.max(1, tl)).toFixed(1)} | ${fmt(td)} | ${fmt(tt)} | ${(100 * tt / Math.max(1, td)).toFixed(1)} | | |`, "");
    if (r.conv) {
      const sh = r.conv.filter((c) => c.shell);
      L.push(`bake: shell ${fmt(sh.reduce((n, c) => n + c.shell.ms, 0))} ms in all (classification ${fmt(sh.reduce((n, c) => n + c.shell.classifyMs, 0))} ms), the largest link ${fmt(Math.max(0, ...sh.map((c) => c.shell.ms)))} ms; buffers at most ${MB(Math.max(0, ...sh.map((c) => c.shell.peak)))}; indexing peak ${MB(Math.max(...r.conv.map((c) => c.indexPeak)))}; bytes added to TILE ${MB(sh.reduce((n, c) => n + c.shell.bytes, 0))} (caches ${MB(r.conv.reduce((n, c) => n + c.cacheBytes, 0))})`, "");
      L.push("| link | elements | exterior | exterior tris / detail | shell ms | peak (process) |", "|---|---|---|---|---|---|");
      r.conv.forEach((c, i) => c.shell && L.push(`| #${i + 1} | ${fmt(c.shell.elements)} | ${fmt(c.shell.exterior)} | ${fmt(c.shell.extTris)} / ${fmt(c.shell.detail)} | ${fmt(c.shell.ms)} | ${MB(c.indexPeak)} |`));
      L.push("");
      if (r.shell.xcompare) {
        L.push("| cross-link views | exterior elements | missed against the last | extra | pass ms | buffers | outside walk triangles |", "|---|---|---|---|---|---|---|");
        for (const c of r.shell.xcompare) L.push(`| ${c.views} | ${fmt(c.exterior ?? 0)} | ${fmt(c.missed)} | ${fmt(c.extra)} | ${fmt(c.ms ?? 0)} | ${MB(c.peak ?? 0)} | ${Object.entries(c.tris).map(([k, t]) => `${k} ${fmt(t)}`).join(", ")} |`);
        L.push("");
      }
      if (r.conv.compare) {
        L.push("| views | exterior elements | missed against the last | extra | classification ms | buffers |", "|---|---|---|---|---|---|");
        for (const c of r.conv.compare) L.push(`| ${c.views} | ${fmt(c.exterior)} | ${fmt(c.missed)} | ${fmt(c.extra)} | ${fmt(c.classifyMs)} | ${MB(c.peak)} |`);
        L.push("");
      }
    }
    const xs = r.shell.xshell;
    if (xs) L.push(`cross-link pass: state ${xs.state} (2 ready), ${xs.groups} groups of overlapping links, ${fmt(xs.exterior)} of ${fmt(xs.elements)} elements exterior, ${fmt(xs.ms)} ms, buffers ${MB(xs.peak)}`, "");
    L.push("| camera | off (= priority) | off + occ | priority: missing; first picture plain / priority (tris) | filter, own shells (+ occ) | filter, cross-link (+ occ) | seen | filter missing (cross; + occ) |", "|---|---|---|---|---|---|---|---|");
    for (const x of r.shell.rows) L.push(`| ${x.name} | ${fmt(x.off.triangles)} | ${fmt(x.offOcc.triangles)} | ${x.prioMissing ?? "-"}; ${x.first ? `${fmt(x.first.plain)} / ${fmt(x.first.priority)}` : "-"} | ${fmt(x.on.triangles)} (${fmt(x.onOcc.triangles)}) | ${fmt(x.x.triangles)} (${fmt(x.xOcc.triangles)}) | ${x.seen !== undefined ? fmt(x.seen) : "-"} | ${x.missing ?? "-"}; ${x.missingOcc ?? "-"} |`);
    const ml = r.shell.rows.flatMap((x) => (x.missingList ?? []).map((m) => ({ ...m, cam: x.name })));
    if (ml.length) {
      L.push("", "Largest missing elements (shell): " + ml.sort((a, b) => b.size_mm - a.size_mm).slice(0, 20).map((m) => `${m.class} ${fmt(m.size_mm)} mm ${m.triangles} tris (link ${m.link + 1}, ${m.cam})`).join("; "));
    }
    L.push("");
  }
  L.push("```json", JSON.stringify(rs.map((r) => ({ label: r.label, per: r.shell.per, rows: r.shell.rows, xshell: r.shell.xshell, xcompare: r.shell.xcompare, compare: r.conv?.compare })), null, 0), "```", "");
  return L.join("\n");
}

const results = [];
if (FOLDER) {
  if (!CACHE_DIR) throw new Error("--folder needs --cache-dir (nothing is written beside the files)");
  const files = readdirSync(FOLDER).filter((f) => /\.ifc$/i.test(f)).sort().map((f) => join(FOLDER, f));
  results.push(await linkedSet("local", files, CACHE_DIR, null, PROJECT));
} else {
  for (const s of SETS) {
    if (s === "synth17") { const x = await setSynth17(); results.push(await linkedSet("synth17", x.files, x.cache, x.place)); }
    else if (s === "heavy") {
      const f = join(WORK, "heavy.json");
      execFileSync("python3", [join(CCORE, "bench/mesh_heavy_model.py"), "700", "1000", f]);
      results.push(await openSet("heavy", f));
    } else if (s === "large") {
      const f = join(WORK, "large.bimble");
      execFileSync("python3", [join(CCORE, "bench/large_model.py"), "20026", f], { stdio: ["ignore", "ignore", "inherit"] });
      results.push(await openSet("large", f));
    } else if (s === "buildings") {
      const f = join(WORK, "buildings.bimbleview");
      execFileSync(join(CCORE, "test/run.sh"), ["tiles"], { env: { ...process.env, CC: process.env.CC ?? "clang", TILES_SYNTH_OUT: f, TILES_SYNTH: "6,3,10,400" }, stdio: ["ignore", "ignore", "inherit"] });
      results.push(await openSet("buildings", f));
    }
  }
}
writeFileSync(OUT, SHELL ? mdShell(results) : md(results));
log(`wrote ${OUT}`);
process.exit(0);
