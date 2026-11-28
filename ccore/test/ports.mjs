// The network ports of ccore's tests and benches (tickets P1, TEST1):
//   import { port, vitePort } from "./ports.mjs";   // or "../test/ports.mjs" from bench/
//   const PORT = port("lab_gl"), VITE = vitePort("lab_gl");
// A new test needs NO row anywhere: the name is enough. Every chosen port is reserved for the life
// of the process by a lock file (<tmpdir>/bimble-ports/<port>, holding the pid; a dead pid's lock
// is taken over) and checked free by a bind test, so two tests running at once on one machine never
// pick the same port, whatever their names. Each choice is printed on stderr ("ports: host lab_gl
// 23456 (auto)"), which lands in the test's log.
// CCORE_PORT_BASE decides where the search starts:
//   auto     (ci/run.sh's default) a random start in 20000..31999: under the Linux ephemeral range
//            (32768..60999, where the OS takes outgoing ports), so never 47400 or 5173 (dev.sh's
//            ports) nor a fixed port of the table below.
//   unset    a stable port per name: 20000 + a hash of kind and name, the next free one if taken.
//   <number> base + a hash of kind and name in 0..SPAN-1 (base 1024..65535-SPAN), the next free
//            offset if taken: two worktrees with bases SPAN apart never share a port.
// The second argument (a default port) is DEPRECATED and optional: kept so tests written against
// the old table still run unchanged. With a default and a matching row below, unset mode tries that
// fixed default first and a number base tries base + the row's offset first; auto ignores both.
// A clash in the table no longer throws (rows are hints, the locks decide). The rows kept are the
// human-facing fixed ports (tools/perf.mjs, tools/parity.mjs: one host at a time, a URL people
// open) and a few rows tests still in flight append after; a new row is never needed.
// Also CHROMIUM, the browser every lab test starts, and RUN_TAG (below).
import { spawnSync } from "node:child_process";
import { closeSync, mkdirSync, openSync, readFileSync, rmSync, unlinkSync, writeSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

// [kind, name, default, offset]
const TABLE = [
  ["host", "parity", 47440, 140], // PAR2 (tools/parity.mjs: one host at a time, Rust twin then C)
  ["host", "perf", 47367, 67], // PERF1 (tools/perf.mjs: the static server of the built lab)
  ["host", "lab_settings", 47303, 3], // SET1
  ["host", "lab_materials", 47387, 87], // MAT1
  ["host", "lab_visibility", 47372, 72], // ISS72
  ["host", "lab_section_box", 47389, 89], // ISS71
  ["host", "lab_history", 47304, 4], // H2a
  ["vite", "lab_boot", 5191, 129],
  ["vite", "lab_settings", 5204, 142], // SET1
  ["vite", "lab_materials", 5150, 88], // MAT1
  ["vite", "lab_visibility", 5135, 73], // ISS72
  ["vite", "lab_section_box", 5152, 90], // ISS71
  ["vite", "lab_history", 5133, 71], // H2a
];

export const SPAN = 160; // offsets 0..SPAN-1 of a number base
const LO = 20000, HI = 32000; // the auto and unset range: under the ephemeral ports, above 1024
const NEVER = new Set([47400, 5173]); // dev.sh's documented ports (the owner's dev loop)
for (const r of TABLE) NEVER.add(r[2]); // and every fixed port of the table: never a hashed pick

const env = process.env.CCORE_PORT_BASE;
let BASE = null, MODE = "unset";
if (env !== undefined && env !== "") {
  if (env === "auto") MODE = "auto";
  else if (/^[0-9]+$/.test(env) && Number(env) >= 1024 && Number(env) + SPAN <= 65536) {
    BASE = Number(env);
    MODE = "base";
  } else {
    throw new Error(`ports.mjs: CCORE_PORT_BASE=${env}: a number 1024..${65536 - SPAN} or auto`);
  }
}
// The numeric base, or null (unset, auto).
export const PORT_BASE = BASE;

// The browser of the lab tests (headless, over CDP): $CHROMIUM when set, else /usr/bin/chromium on
// Linux and Google Chrome's binary on macOS (ticket MAC1; the GitHub macOS image has Chrome).
export const CHROMIUM = process.env.CHROMIUM ||
  (process.platform === "darwin" ? "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" : "/usr/bin/chromium");
// A suffix for fixed scratch paths under out/: "" unset, -<base> with a number, -p<pid> with auto,
// so two runs at once in one tree never share a scratch directory.
export const RUN_TAG = MODE === "unset" ? "" : MODE === "base" ? `-${BASE}` : `-p${process.pid}`;

// VIS2: every test and tool that imports this module gives the hosts it starts (process.env) a state folder of its own,
// so the page's web session log (ccore/host/server.c /session_log: $XDG_STATE_HOME/bimble/web-sessions, the last 20 kept)
// never lands in, nor prunes, the user's own sessions. Removed at exit.
if (!process.env.BIMBLE_TEST_STATE_KEEP) {
  const st = join(tmpdir(), `bimble-test-state-${process.pid}`);
  process.env.XDG_STATE_HOME = st;
  process.on("exit", () => { try { rmSync(st, { recursive: true, force: true }); } catch { /* gone */ } });
}

// FNV-1a 32 of a string.
function hash(s) {
  let h = 0x811c9dc5;
  for (const c of Buffer.from(s)) h = Math.imul(h ^ c, 0x01000193) >>> 0;
  return h;
}

// Locks: one file per port, created exclusively; ours are removed at exit.
const LOCKS = join(process.env.CCORE_PORT_LOCKS || tmpdir(), "bimble-ports");
const held = new Set();
process.on("exit", () => { for (const p of held) try { unlinkSync(join(LOCKS, String(p))); } catch {} });
const alive = (pid) => {
  if (!(pid > 0)) return false;
  try { process.kill(pid, 0); return true; } catch (e) { return e.code === "EPERM"; }
};
function lock(p) {
  if (held.has(p)) return false; // another name of this process holds it
  mkdirSync(LOCKS, { recursive: true });
  const f = join(LOCKS, String(p));
  for (let tries = 0; tries < 2; tries++) {
    try {
      const fd = openSync(f, "wx");
      writeSync(fd, String(process.pid));
      closeSync(fd);
      held.add(p);
      return true;
    } catch (e) {
      if (e.code !== "EEXIST") throw e;
      let pid = 0;
      try { pid = Number(readFileSync(f, "utf8")); } catch {}
      if (alive(pid)) return false;
      // A stale lock (its process died without its exit hook): take it over, once. A pid not yet
      // written (0, a lock being made right now) counts as dead only after a second look.
      if (pid === 0 && tries === 0) { Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 50); continue; }
      // Remove it only if it still names that dead pid (another process may have just taken it over).
      try { if (Number(readFileSync(f, "utf8")) === pid) unlinkSync(f); } catch {}
    }
  }
  return false;
}
function unlock(p) {
  held.delete(p);
  try { unlinkSync(join(LOCKS, String(p))); } catch {}
}

// The bind test, synchronous (port() is): a child Node listens on each port (all interfaces, as
// the earlier helper did, so an IPv4 or IPv6 listener both count) and prints those it could bind.
const PROBE = `const net=require("net");const ps=process.argv.slice(1).map(Number),ok=[];let i=0;
(function next(){if(i>=ps.length){process.stdout.write(ok.join(" "));return;}const p=ps[i++],s=net.createServer();
s.once("error",next);s.listen({port:p,exclusive:true},()=>{ok.push(p);s.close(next);});})();`;
function bindable(ports) {
  const r = spawnSync(process.execPath, ["-e", PROBE, ...ports.map(String)], { encoding: "utf8" });
  if (r.status !== 0) throw new Error(`ports.mjs: the bind test failed: ${r.stderr}`);
  return new Set(r.stdout.split(" ").filter(Boolean).map(Number));
}

const chosen = new Map(); // "kind name" -> port: one port per name and process
function pick(kind, name, def) {
  const key = `${kind} ${name}`;
  if (chosen.has(key)) return chosen.get(key);
  const row = def === undefined ? undefined : TABLE.find((r) => r[0] === kind && r[1] === name && r[2] === def);
  // The candidates, in order of preference.
  const cands = [];
  if (MODE === "base") {
    if (row) cands.push(BASE + row[3]);
    const h = hash(key);
    for (let i = 0; i < SPAN; i++) cands.push(BASE + ((h + i) % SPAN));
    for (let i = cands.length - 1; i >= 0; i--) if (cands[i] === 47400 || cands[i] === 5173) cands.splice(i, 1); // dev.sh's
  } else {
    if (MODE === "unset" && def !== undefined && def !== 47400 && def !== 5173) cands.push(def); // the old fixed default
    const h = MODE === "auto" ? hash(`${key} ${process.pid} ${process.hrtime.bigint()}`) : hash(key);
    for (let i = 0; i < HI - LO; i++) {
      const p = LO + ((h + i) % (HI - LO));
      if (!NEVER.has(p)) cands.push(p);
    }
  }
  // Lock up to 8 candidates, bind-test them in one child, keep the first free one.
  let at = 0;
  while (at < cands.length) {
    const batch = [];
    while (at < cands.length && batch.length < 8) {
      const p = cands[at++];
      if (lock(p)) batch.push(p);
    }
    if (batch.length === 0) continue;
    const ok = bindable(batch);
    const got = batch.find((p) => ok.has(p));
    for (const p of batch) if (p !== got) unlock(p);
    if (got !== undefined) {
      chosen.set(key, got);
      process.stderr.write(`ports: ${kind} ${name} ${got} (${MODE}${MODE === "base" ? ` ${BASE}` : ""})\n`);
      return got;
    }
  }
  throw new Error(`ports.mjs: no free ${kind} port for ${name} (${MODE})`);
}

// A host (or other non-Vite) port. The second argument is deprecated and optional (see above).
export const port = (name, def) => pick("host", name, def);
// A Vite dev-server port. The second argument is deprecated and optional (see above).
export const vitePort = (name, def) => pick("vite", name, def);
