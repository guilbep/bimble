// Synthetic linked IFC files for the 3D of links (ticket LNK3D, ADR 0078 amendment 3). Synthetic only.
//   import { makeLinkFolder } from "../bench/links_synth.mjs";
// Each link is bench/mesh_heavy_model.py's model (golden/frame.json plus `proxies` closed meshes of about
// `tris` triangles), its GlobalIds rewritten per link (SHA-256 of "lnk3d/<k>/<id>" in the IFC
// alphabet, first character 0-3) so no two links share an id, opened in a running host and exported
// as IFC4 (action export_ifc4): link_<k>.ifc in `dir`; the intermediate models in `work` are removed (also on failure). Deterministic but for the IFC header's timestamp.
// `unique`: each closed mesh's vertices moved by their own few mm ((5 j + 3 i + axis) mod 7, j the mesh, i
// the vertex; its first two vertices by j mod 13 and j / 13 mod 13 more: not an affine map of another), so no two are copies of one shape (an imported folder's
// geometry is mostly unique: no instancing, every triangle in the GPU's buffers).
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const BENCH = dirname(fileURLToPath(import.meta.url));
const ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$";

/** A GlobalId derived from `seed` (22 characters, the first 0-3). */
export function derivedId(seed) {
  const h = createHash("sha256").update(seed).digest();
  let s = ALPHABET[h[0] & 3];
  for (let i = 1; i < 22; i++) s += ALPHABET[h[i] & 63];
  return s;
}

/** Writes `n` IFC files link_0.ifc .. into `dir` through the host (`hcall(op, name, body)` -> {status, text}). */
export async function makeLinkFolder({ hcall, dir, work, n, proxies = 96, tris = 600, unique = false }) {
  const base = join(work, `lnk3d-heavy-${proxies}-${tris}.json`);
  execFileSync("python3", [join(BENCH, "mesh_heavy_model.py"), String(proxies), String(tris), base], { stdio: ["ignore", "pipe", "pipe"] });
  let text = readFileSync(base, "utf8");
  if (unique) {
    const doc = JSON.parse(text);
    let j = 0;
    for (const el of Object.values(doc.model.elements)) {
      if (el.generator?.kind !== "mesh") continue;
      j++;
      const off = (i, a) => ((5 * j + 3 * i + a) % 7) + (i === 0 ? j % 13 : i === 1 ? Math.floor(j / 13) % 13 : 0);
      el.generator.positions = el.generator.positions.map((p, i) => p.map((v, a) => Math.round((v + off(i, a)) * 1000) / 1000));
    }
    text = JSON.stringify(doc);
  }
  const files = [];
  for (let k = 0; k < n; k++) {
    const map = new Map();
    const t = text.replace(/"([0-3][0-9A-Za-z_$]{21})"/g, (_, id) => {
      if (!map.has(id)) map.set(id, derivedId(`lnk3d/${k}/${id}`));
      return `"${map.get(id)}"`;
    });
    const src = join(work, `lnk3d-link-${k}.json`), out = resolve(dir, `link_${k}.ifc`);
    writeFileSync(src, t);
    try {
      let r = await hcall("action", "open", { path: src, discard: true });
      if (r.status !== 200) throw new Error(`open ${src}: ${r.text}`);
      r = await hcall("action", "export_ifc4", { path: out });
      if (r.status !== 200) throw new Error(`export_ifc4: ${r.text}`);
    } finally {
      rmSync(src, { force: true }); // its model only fed the export (the IFC stays, in `dir`)
    }
    files.push(out);
  }
  rmSync(base, { force: true });
  return files;
}
