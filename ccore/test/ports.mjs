// The network ports of ccore's tests and benches, from one place (ticket P1):
//   import { port, vitePort } from "./ports.mjs";   // or "../test/ports.mjs" from bench/
//   const PORT = port("lab_gl", 47323), VITE = vitePort("lab_gl", 5193);
// CCORE_PORT_BASE decides:
//   unset    each port is its default (the fixed ports below): runs behave as before.
//   <number> base + the name's offset (base 1024..65535-SPAN). Two worktrees with different bases
//            never share a port: CCORE_PORT_BASE=48000 in one, 49000 in another.
//   auto     a base from a hash of the worktree path (20000..31999, under the Linux ephemeral
//            range), checked free on every offset of the table; if a port is taken, the next slot.
//            The resolved number is written back to process.env, so child processes (a bench run
//            by a test, dev.sh) use the same base.
// Every name in the suite has its own offset (checked at load): no two ports collide in a base.
// Also CHROMIUM, the browser every lab test starts (below).
// Host offset = default - 47300; Vite offset = 128 + default - 5190. Never 47400 or 5173 (dev.sh).
import net from "node:net";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

// [kind, name, default, offset]
const TABLE = [
  ["host", "host_test", 47311, 11],
  ["host", "lab_boot", 47312, 12],
  ["host", "mesh_http", 47321, 21],
  ["host", "mesh_http_nomodel", 47322, 22],
  ["host", "lab_gl", 47323, 23],
  ["host", "lab_tool", 47324, 24],
  ["host", "host_test_model", 47331, 31],
  ["host", "host_test_model2", 47332, 32],
  ["host", "lab_place", 47333, 33],
  ["host", "lab_place_lab", 47334, 34],
  ["host", "session_test", 47341, 41],
  ["host", "lab_loop", 47342, 42],
  ["host", "wasm_run", 47351, 51],
  ["host", "large_test", 47375, 75],
  ["host", "session_log", 47382, 82],
  ["host", "large_host", 47384, 84],
  ["host", "large_session", 47385, 85],
  ["host", "large_lab", 47386, 86],
  ["host", "files_test", 47391, 91],
  ["host", "files_test2", 47392, 92],
  ["host", "verbs_session", 47393, 93],
  ["host", "dev_test", 47411, 111],
  ["host", "visibility_test", 47394, 94],
  ["host", "lab_section", 47395, 95],
  ["host", "links_test", 47396, 96],
  ["host", "lab_links", 47397, 97],
  ["host", "views_test", 47398, 98],
  ["host", "lab_plan", 47399, 99],
  ["host", "annotations_test", 47412, 112],
  ["host", "lab_dims", 47413, 113],
  ["host", "lab_viewcube", 47426, 126],
  ["host", "lab_sheets", 47427, 127],
  ["host", "lab_pages", 47365, 65],
  ["host", "lab_measure", 47428, 128],
  ["host", "lab_nav", 47417, 117],
  ["host", "versions_test", 47318, 18], // ticket H1
  ["host", "lab_refs", 47418, 118], // L1
  ["host", "lab_refs_dxf", 47438, 138], // L2
  ["host", "lab_export", 47306, 6], // D7
  ["host", "lab_ifc_plan", 47446, 146], // S1
  ["vite", "lab_boot", 5191, 129],
  ["vite", "lab_gl", 5193, 131],
  ["vite", "lab_place", 5195, 133],
  ["vite", "lab_loop", 5197, 135],
  ["vite", "dev_test", 5199, 137],
  ["vite", "lab_wasm", 5201, 139],
  ["vite", "large", 5207, 145],
  ["vite", "lab_tool", 5209, 147],
  ["vite", "lab_section", 5211, 149],
  ["vite", "lab_links", 5213, 151],
  ["vite", "lab_plan", 5215, 153],
  ["vite", "lab_dims", 5217, 155],
  ["vite", "lab_viewcube", 5219, 157],
  ["vite", "lab_sheets", 5221, 159],
  ["vite", "lab_measure", 5192, 130],
  ["vite", "lab_nav", 5205, 143],
  ["vite", "lab_refs", 5203, 141], // L1
  ["vite", "lab_refs_dxf", 5223, 156], // L2
  ["vite", "lab_export", 5196, 134], // D7
  ["vite", "lab_ifc_plan", 5210, 148], // S1
];
export const SPAN = 160; // offsets 0..SPAN-1
const AUTO_LO = 20000, AUTO_SLOTS = Math.floor((32000 - AUTO_LO) / SPAN);

{
  const seen = new Map();
  for (const [kind, name, def, off] of TABLE) {
    if (off < 0 || off >= SPAN) throw new Error(`ports.mjs: ${kind} ${name}: offset ${off} outside 0..${SPAN - 1}`);
    if (seen.has(off)) throw new Error(`ports.mjs: ${kind} ${name} and ${seen.get(off)} share offset ${off}`);
    if (def === 47400 || def === 5173) throw new Error(`ports.mjs: ${kind} ${name}: ${def} is dev.sh's port`);
    seen.set(off, `${kind} ${name}`);
  }
}

const free = (p) => new Promise((res) => {
  const s = net.createServer();
  s.once("error", () => res(false));
  s.listen({ port: p, exclusive: true }, () => s.close(() => res(true)));
});

async function autoBase() {
  const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
  let h = 0x811c9dc5; // FNV-1a 32 of the worktree path
  for (const c of Buffer.from(root)) h = Math.imul(h ^ c, 0x01000193) >>> 0;
  for (let i = 0; i < AUTO_SLOTS; i++) {
    const base = AUTO_LO + ((h + i) % AUTO_SLOTS) * SPAN;
    let ok = true;
    for (let o = 0; o < SPAN && ok; o++) ok = await free(base + o);
    if (ok) return base;
  }
  throw new Error("ports.mjs: CCORE_PORT_BASE=auto: no free range of " + SPAN + " ports");
}

const env = process.env.CCORE_PORT_BASE;
let BASE = null;
if (env !== undefined && env !== "") {
  if (env === "auto") {
    BASE = await autoBase();
    process.env.CCORE_PORT_BASE = String(BASE);
  } else if (/^[0-9]+$/.test(env) && Number(env) >= 1024 && Number(env) + SPAN <= 65536) {
    BASE = Number(env);
  } else {
    throw new Error(`ports.mjs: CCORE_PORT_BASE=${env}: a number 1024..${65536 - SPAN} or auto`);
  }
}
export const PORT_BASE = BASE;

// The browser of the lab tests (headless, over CDP): $CHROMIUM when set, else /usr/bin/chromium on
// Linux and Google Chrome's binary on macOS (ticket MAC1; the GitHub macOS image has Chrome).
export const CHROMIUM = process.env.CHROMIUM ||
  (process.platform === "darwin" ? "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" : "/usr/bin/chromium");
// A suffix for fixed scratch paths under out/ ("" without a base): two runs in one tree, two dirs.
export const RUN_TAG = BASE === null ? "" : `-${BASE}`;

function lookup(kind, name, def) {
  const row = TABLE.find((r) => r[0] === kind && r[1] === name);
  if (!row) throw new Error(`ports.mjs: unknown ${kind} port ${name}: add it to the table`);
  if (row[2] !== def) throw new Error(`ports.mjs: ${kind} ${name}: default ${def}, the table says ${row[2]}`);
  return BASE === null ? def : BASE + row[3];
}

// A host (or other non-Vite) port.
export const port = (name, def) => lookup("host", name, def);
// A Vite dev-server port.
export const vitePort = (name, def) => lookup("vite", name, def);
