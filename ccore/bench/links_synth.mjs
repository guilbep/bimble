// Synthetic linked IFC files for the 3D of links (ticket LNK3D, ADR 0078 amendment 3). Synthetic only.
//   import { makeLinkFolder } from "../bench/links_synth.mjs";
// Each link is bench/mesh_heavy_model.py's model (golden/frame.json plus `proxies` closed meshes of about
// `tris` triangles), its GlobalIds rewritten per link (SHA-256 of "lnk3d/<k>/<id>" in the IFC
// alphabet, first character 0-3) so no two links share an id, opened in a running host and exported
// as IFC4 (action export_ifc4): link_<k>.ifc in `dir`. Deterministic but for the IFC header's timestamp.
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";
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
export async function makeLinkFolder({ hcall, dir, work, n, proxies = 96, tris = 600 }) {
  const base = join(work, `lnk3d-heavy-${proxies}-${tris}.json`);
  execFileSync("python3", [join(BENCH, "mesh_heavy_model.py"), String(proxies), String(tris), base], { stdio: ["ignore", "pipe", "pipe"] });
  const text = readFileSync(base, "utf8");
  const files = [];
  for (let k = 0; k < n; k++) {
    const map = new Map();
    const t = text.replace(/"([0-3][0-9A-Za-z_$]{21})"/g, (_, id) => {
      if (!map.has(id)) map.set(id, derivedId(`lnk3d/${k}/${id}`));
      return `"${map.get(id)}"`;
    });
    const src = join(work, `lnk3d-link-${k}.json`), out = resolve(dir, `link_${k}.ifc`);
    writeFileSync(src, t);
    let r = await hcall("action", "open", { path: src, discard: true });
    if (r.status !== 200) throw new Error(`open ${src}: ${r.text}`);
    r = await hcall("action", "export_ifc4", { path: out });
    if (r.status !== 200) throw new Error(`export_ifc4: ${r.text}`);
    files.push(out);
  }
  return files;
}
