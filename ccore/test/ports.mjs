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
  ["vite", "lab_boot", 5191, 129],
  ["vite", "lab_gl", 5193, 131],
  ["vite", "lab_place", 5195, 133],
  ["vite", "lab_loop", 5197, 135],
  ["vite", "dev_test", 5199, 137],
  ["vite", "lab_wasm", 5201, 139],
  ["vite", "large", 5207, 145],
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
