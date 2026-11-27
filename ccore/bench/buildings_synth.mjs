// Synthetic buildings for "draw only what can be seen" (ticket VIS1, ADR 0078 amendment 7). Synthetic only.
//   import { makeBuildingFolder, buildingModel } from "../bench/buildings_synth.mjs";
// A building as a linked IFC file of an imported project looks: an outer shell (facade panels, a slab per
// storey and a roof) around MANY small inner elements per storey (inner walls, pipes, ducts, sprinkler heads,
// furniture, fittings), so that from outside almost everything is hidden by the shell and from inside a room
// almost everything else is hidden by its walls and slabs. Every element is a closed mesh (generator "mesh",
// outward triangles); pipes and fittings are each a mesh of their own (an imported folder's geometry is mostly
// unique), sprinkler heads and furniture are copies of a few shapes (drawn instanced). Each building is opened in
// a running host and exported as IFC4 (action export_ifc4): building_<k>.ifc in `dir`. Deterministic but for
// the IFC header's timestamp. Per storey about 2,500 elements and 300,000 triangles with the defaults.
import { readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { derivedId } from "./links_synth.mjs";

const BENCH = dirname(fileURLToPath(import.meta.url));
export const W = 36000, D = 24000, H = 3200, PANEL = 3000, WALL = 300, SLAB = 300;

/** A box [x0, x1] x [y0, y1] x [z0, z1] (mm, local), 8 vertices, 12 outward triangles. */
function box(x0, y0, z0, x1, y1, z1) {
  const p = [];
  for (let k = 0; k < 8; k++) p.push([k & 1 ? x1 : x0, k & 2 ? y1 : y0, k & 4 ? z1 : z0]);
  // faces (outward, counter-clockwise seen from outside)
  const F = [[0, 2, 3, 1], [4, 5, 7, 6], [0, 1, 5, 4], [2, 6, 7, 3], [0, 4, 6, 2], [1, 3, 7, 5]];
  const idx = [];
  for (const f of F) idx.push(f[0], f[1], f[2], f[0], f[2], f[3]);
  return { pos: p, idx };
}
/** A cylinder along x from 0 to len, radius r, `seg` sides, capped: 4 seg triangles. */
function cylinder(len, r, seg) {
  const pos = [], idx = [];
  for (let i = 0; i < seg; i++) {
    const a = (2 * Math.PI * i) / seg, y = r * Math.cos(a), z = r * Math.sin(a);
    pos.push([0, y, z], [len, y, z]);
  }
  const c0 = pos.length, c1 = c0 + 1;
  pos.push([0, 0, 0], [len, 0, 0]);
  for (let i = 0; i < seg; i++) {
    const j = (i + 1) % seg, a0 = 2 * i, a1 = 2 * i + 1, b0 = 2 * j, b1 = 2 * j + 1;
    idx.push(a0, b0, b1, a0, b1, a1); // side (outward)
    idx.push(c0, b0, a0, c1, a1, b1); // caps
  }
  return { pos, idx };
}
/** A UV sphere of about `tris` triangles, radius r, centred at (0, 0, r). */
function sphere(tris, r) {
  const seg = Math.max(4, Math.floor(Math.sqrt(tris))), rings = Math.max(2, Math.floor(tris / (2 * seg)) + 1);
  const pos = [[0, 0, 2 * r]];
  for (let i = 1; i < rings; i++) {
    const t = (Math.PI * i) / rings;
    for (let j = 0; j < seg; j++) { const p = (2 * Math.PI * j) / seg; pos.push([r * Math.sin(t) * Math.cos(p), r * Math.sin(t) * Math.sin(p), r + r * Math.cos(t)]); }
  }
  pos.push([0, 0, 0]);
  const last = pos.length - 1, idx = [];
  for (let j = 0; j < seg; j++) idx.push(0, 1 + j, 1 + ((j + 1) % seg));
  for (let i = 0; i < rings - 2; i++) {
    const a = 1 + i * seg, b = 1 + (i + 1) * seg;
    for (let j = 0; j < seg; j++) { const k = (j + 1) % seg; idx.push(a + j, b + j, b + k, a + j, b + k, a + k); }
  }
  const a = 1 + (rings - 2) * seg;
  for (let j = 0; j < seg; j++) idx.push(a + j, last, a + ((j + 1) % seg));
  return { pos, idx };
}
/** A desk: a top and four legs (five boxes merged). */
function desk(w, d, h) {
  const parts = [box(0, 0, h - 40, w, d, h), box(0, 0, 0, 50, 50, h - 40), box(w - 50, 0, 0, w, 50, h - 40), box(0, d - 50, 0, 50, d, h - 40), box(w - 50, d - 50, 0, w, d, h - 40)];
  const pos = [], idx = [];
  for (const p of parts) { const o = pos.length; pos.push(...p.pos); idx.push(...p.idx.map((i) => i + o)); }
  return { pos, idx };
}
const r3 = (v) => Math.round(v * 1000) / 1000;

/** A deterministic pseudo-random generator (mulberry32). */
function rng(seed) {
  let a = seed >>> 0;
  return () => { a = (a + 0x6d2b79f5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}

/** The model JSON of one building (golden/frame.json's storeys and materials, its elements replaced). */
export function buildingModel({ k = 0, storeys = 4, scale = 1 } = {}) {
  const doc = JSON.parse(readFileSync(join(BENCH, "../golden/frame.json"), "utf8"));
  const m = doc.model;
  const storey = Object.values(m.storeys).sort((a, b) => a.elevation - b.elevation)[0].id;
  const material = Object.keys(m.materials)[0];
  m.elements = {};
  const R = rng(1234 + k);
  let n = 0, tris = 0;
  const add = (cls, name, g, at) => {
    const id = derivedId(`vis1/${k}/${n++}`);
    m.elements[id] = { id, class: cls, name, storey, material,
      placement: { origin: at.map((v) => r3(v)), z_axis: [0, 0, 1], x_axis: [1, 0, 0] },
      generator: { kind: "mesh", positions: g.pos.map((p) => p.map(r3)), indices: g.idx }, properties: {} };
    tris += g.idx.length / 3;
  };
  const sprinkler = sphere(200, 40), chair = sphere(120, 250), deskG = desk(1600, 800, 740);
  const per = Math.max(1, Math.round(scale));
  for (let s = 0; s < storeys; s++) {
    const z = s * H;
    // the shell: a slab, facade panels on the four sides (300 mm thick, a storey high)
    add("IfcSlab", `Slab ${s}`, box(0, 0, 0, W, D, SLAB), [0, 0, z]);
    for (let x = 0; x < W; x += PANEL) {
      add("IfcWall", `Facade S ${s}.${x}`, box(0, 0, 0, Math.min(PANEL, W - x), WALL, H - SLAB), [x, 0, z + SLAB]);
      add("IfcWall", `Facade N ${s}.${x}`, box(0, 0, 0, Math.min(PANEL, W - x), WALL, H - SLAB), [x, D - WALL, z + SLAB]);
    }
    for (let y = WALL; y < D - WALL; y += PANEL) {
      const l = Math.min(PANEL, D - WALL - y);
      add("IfcWall", `Facade W ${s}.${y}`, box(0, 0, 0, WALL, l, H - SLAB), [0, y, z + SLAB]);
      add("IfcWall", `Facade E ${s}.${y}`, box(0, 0, 0, WALL, l, H - SLAB), [W - WALL, y, z + SLAB]);
    }
    // inner walls: rooms of 6 m x 6 m along a corridor (y 10.5-13.5 m), each wall with its own length
    for (let x = 6000; x < W; x += 6000) {
      add("IfcWall", `Inner ${s}.${x}a`, box(0, 0, 0, 150, 10500 - WALL, H - SLAB), [x, WALL, z + SLAB]);
      add("IfcWall", `Inner ${s}.${x}b`, box(0, 0, 0, 150, D - 13500 - WALL, H - SLAB), [x, 13500, z + SLAB]);
    }
    add("IfcWall", `Corridor ${s}a`, box(0, 0, 0, W - 2 * WALL, 150, H - SLAB), [WALL, 10500, z + SLAB]);
    add("IfcWall", `Corridor ${s}b`, box(0, 0, 0, W - 2 * WALL, 150, H - SLAB), [WALL, 13350, z + SLAB]);
    for (let rep = 0; rep < per; rep++) {
      // pipes under the ceiling: each its own length (a unique mesh)
      for (let i = 0; i < 600; i++) {
        const len = 1500 + Math.floor(R() * 6000), r = 20 + Math.floor(R() * 60);
        add("IfcBuildingElementProxy", `Pipe ${s}.${rep}.${i}`, cylinder(len, r, 12), [WALL + R() * (W - 2 * WALL - len), WALL + R() * (D - 2 * WALL - 2 * r), z + H - 250 - R() * 300]);
      }
      // ducts
      for (let i = 0; i < 100; i++) {
        const len = 2000 + Math.floor(R() * 8000);
        add("IfcBuildingElementProxy", `Duct ${s}.${rep}.${i}`, box(0, 0, 0, len, 400, 300), [WALL + R() * (W - 2 * WALL - len), WALL + R() * (D - 2 * WALL - 400), z + H - 650]);
      }
      // sprinkler heads: copies of one shape
      for (let i = 0; i < 300; i++) add("IfcBuildingElementProxy", `Sprinkler ${s}.${rep}.${i}`, sprinkler, [WALL + 500 + (i % 30) * 1150, WALL + 500 + Math.floor(i / 30) * 2250, z + H - 120]);
      // furniture: desks and chairs, copies
      for (let i = 0; i < 200; i++) {
        const x = WALL + 300 + R() * (W - 2 * WALL - 2000), y = WALL + 300 + R() * (D - 2 * WALL - 1200);
        add("IfcBuildingElementProxy", `Desk ${s}.${rep}.${i}`, deskG, [x, y, z + SLAB]);
        add("IfcBuildingElementProxy", `Chair ${s}.${rep}.${i}`, chair, [x + 500, y + 900, z + SLAB]);
      }
      // fittings: small unique meshes (a sphere, its vertices moved by their own few mm)
      for (let i = 0; i < 1000; i++) {
        const g = sphere(100, 60 + (i % 7) * 10), j = i + 1000 * s + 100000 * rep;
        g.pos = g.pos.map((p, v) => p.map((c, a) => c + ((5 * j + 3 * v + a) % 7) + (v === 0 ? j % 13 : v === 1 ? Math.floor(j / 13) % 13 : 0)));
        add("IfcBuildingElementProxy", `Fitting ${s}.${rep}.${i}`, g, [WALL + R() * (W - 2 * WALL - 200), WALL + R() * (D - 2 * WALL - 200), z + SLAB + R() * (H - 2 * SLAB - 200)]);
      }
    }
  }
  add("IfcSlab", "Roof", box(0, 0, 0, W, D, SLAB), [0, 0, storeys * H]);
  m.elements = Object.fromEntries(Object.entries(m.elements).sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0)));
  return { doc, elements: n, tris };
}

/** Writes `n` buildings building_0.ifc .. into `dir` through the host (`hcall(op, name, body)` -> {status, text}). */
export async function makeBuildingFolder({ hcall, dir, work, n, storeys = 4, scale = 1 }) {
  const files = [];
  let elements = 0, tris = 0;
  for (let k = 0; k < n; k++) {
    const b = buildingModel({ k, storeys, scale });
    elements += b.elements, tris += b.tris;
    const src = join(work, `vis1-building-${k}.json`), out = resolve(dir, `building_${k}.ifc`);
    writeFileSync(src, JSON.stringify(b.doc));
    try {
      let r = await hcall("action", "open", { path: src, discard: true });
      if (r.status !== 200) throw new Error(`open ${src}: ${r.text}`);
      r = await hcall("action", "export_ifc4", { path: out });
      if (r.status !== 200) throw new Error(`export_ifc4: ${r.text}`);
    } finally {
      rmSync(src, { force: true });
    }
    files.push(out);
  }
  return { files, elements, tris };
}
