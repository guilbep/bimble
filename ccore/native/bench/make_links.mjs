// Spike NAT2: a folder of N synthetic linked IFC files for both native prototypes (and the web lab),
// through bench/links_synth.mjs (ticket LNK3D) and the tcc host. Synthetic only.
//   node ccore/native/bench/make_links.mjs <dir> [n=17] [proxies=400] [tris=600]
// Each link: golden/frame.json plus `proxies` unique closed meshes of ~`tris` triangles (no instancing).
import { spawn } from "node:child_process";
import { mkdirSync, mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { makeLinkFolder } from "../../bench/links_synth.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const [dir, n = "17", proxies = "400", tris = "600"] = process.argv.slice(2);
if (!dir) { console.error("usage: make_links.mjs <dir> [n] [proxies] [tris]"); process.exit(2); }
mkdirSync(resolve(dir), { recursive: true });
const work = mkdtempSync(join(process.env.NAT2_TMP ?? tmpdir(), "nat2-links-"));
const port = 47000 + Math.floor(Math.random() * 900), token = "nat2";
const host = spawn(resolve(here, "../../out/tcc/bimble-ccore"), [String(port)], { env: { ...process.env, BIMBLE_TOKEN: token }, stdio: ["ignore", "ignore", "pipe"] });
let err = "";
host.stderr.on("data", (d) => (err += d));
for (let i = 0; i < 100 && !err.includes("listening"); i++) await new Promise((r) => setTimeout(r, 100));
const hcall = async (op, name, body = {}) => {
  const r = await fetch(`http://127.0.0.1:${port}/${op}/${name}?t=${token}`, { method: "POST", body: JSON.stringify(body) });
  return { status: r.status, text: await r.text() };
};
try {
  const t0 = Date.now();
  const files = await makeLinkFolder({ hcall, dir: resolve(dir), work, n: Number(n), proxies: Number(proxies), tris: Number(tris), unique: true });
  console.log(`make_links: ${files.length} files in ${resolve(dir)} in ${Date.now() - t0} ms`);
} finally {
  host.kill();
  rmSync(work, { recursive: true, force: true });
}
