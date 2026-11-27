// Copy the C core's wasm build (ccore/build.sh wasm) and its demo model into public/ccore/, where
// Vite serves them as is for the lab's `?ccore=1` (src/ccore.ts). With --view: the viewer-only
// build instead (ccore/build.sh wasm-view, ADR 0069 rule 3), served as public/ccore/ccore-view.wasm.
// `npm run build:ccore` (the Pages lab under c/, ticket PG1) runs it twice: both files in one build.
// SIZE1: no font is copied (the PDF export is Helvetica by default; Roboto is optional and not
// shipped): a public/ccore/fonts/ left by an older copy is removed, so no build ships it.
// URL1 (ADR 0086): the site's samples, public/samples/frame.ifc and frame.bimbleview (the 72-element
// demo frame, synthetic, from ccore/golden/), opened by `?open=samples/frame.ifc` (same origin: no question).
import { copyFileSync, existsSync, mkdirSync, rmSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const ccore = join(root, "..", "ccore");
const view = process.argv.includes("--view");
const wasm = view ? "ccore-view.wasm" : "ccore.wasm";
const files = [[`out/wasm/${wasm}`, wasm], ["golden/frame.json", "frame.json"]];
const dst = join(root, "public", "ccore");

const missing = files.filter(([f]) => !existsSync(join(ccore, f))).map(([f]) => f);
if (missing.length) {
  console.error(`ccore/ is missing ${missing.join(", ")}: run "ccore/build.sh ${view ? "wasm-view" : "wasm"}" first.`);
  process.exit(1);
}
mkdirSync(dst, { recursive: true });
for (const [f, to] of files) copyFileSync(join(ccore, f), join(dst, to));
// ---- URL1: the samples ----
const samples = [["golden/ifc/frame.ifc", "frame.ifc"], ["golden/view/frame.bimbleview", "frame.bimbleview"]];
mkdirSync(join(root, "public", "samples"), { recursive: true });
for (const [f, to] of samples) copyFileSync(join(ccore, f), join(root, "public", "samples", to));
// ---- end URL1 ----
rmSync(join(dst, "fonts"), { recursive: true, force: true }); // SIZE1
console.log(`copied ${files.length} files to public/ccore/, ${samples.length} to public/samples/`);
