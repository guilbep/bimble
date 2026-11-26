// The dev loop (ccore/dev.sh) end to end: node ccore/test/dev_test.mjs
// Starts dev.sh on test ports (host <port>, Vite <port>), waits for the lab URL, checks the host
// answers; then edits session/calls.c (a trailing comment, then without it) five times and checks
// each save gives one "rebuilt in" line and a host with a new epoch; a syntax error prints the
// compiler output while the old host still answers; the revert rebuilds. Every save is waited for
// by dev.sh's cycle lines (one "changed, building", then one end: rebuilt, build failed, or the host
// did not start), not by a time of quiet: with a build slower than that time (920 ms on a GitHub
// runner) the restart of one save was seen as the next one's (ticket K2). Each save must give
// exactly one cycle. SIGTERM to dev.sh must leave no process behind. The edited file is restored byte for byte. Needs tcc, inotifywait and
// web-ui/node_modules. Prints the cycle times and "N passed, M failed"; exits non-zero on a failure.
import { execFileSync, spawn } from "node:child_process";
import { readFileSync, writeFileSync, renameSync } from "node:fs";
import http from "node:http";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port, vitePort } from "./ports.mjs";
import { hostEnv } from "./lib/host_env.mjs"; // FIXSES: never the caller's BIMBLE_CONFIG_DIR

/** Write a source file atomically (temp + rename): a background compile reading the old file must
 *  never see it truncated (clang mmaps sources; an in-place rewrite gave it SIGBUS). */
function writeAtomic(p, data) {
  const t = `${p}.tmp${process.pid}`;
  writeFileSync(t, data);
  renameSync(t, p);
}

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const PORT = port("dev_test");
const VITE = vitePort("dev_test");
const EDITED = join(CCORE, "session/calls.c");
const ORIGINAL = readFileSync(EDITED);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};

async function waitFor(what, ms, fn) {
  const until = Date.now() + ms;
  while (Date.now() < until) {
    const v = await fn();
    if (v) return v;
    await sleep(20);
  }
  throw new Error(`timeout waiting for ${what}`);
}

function req(method, path, token) {
  return new Promise((res) => {
    const r = http.request({ host: "127.0.0.1", port: PORT, method, path: `${path}${path.includes("?") ? "&" : "?"}t=${token}`,
      headers: { "Content-Type": "text/plain" } }, (rs) => {
      let b = "";
      rs.setEncoding("utf8");
      rs.on("data", (d) => (b += d));
      rs.on("end", () => res({ status: rs.statusCode, body: b }));
    });
    r.on("error", (e) => res({ status: 0, body: String(e) }));
    r.end();
  });
}
const epochOf = async (token) => {
  const r = await req("GET", "/change?since=0", token);
  try { return r.status === 200 ? JSON.parse(r.body).epoch : null; } catch { return null; }
};

/** Every process in dev.sh's session and below it (pid, command line), from /proc. */
function descendants(root) {
  const kids = new Map();
  const cmd = new Map();
  for (const line of execFileSync("ps", ["-e", "-o", "pid=,ppid=,args="], { encoding: "utf8" }).split("\n")) {
    const m = /^\s*(\d+)\s+(\d+)\s+(.*)$/.exec(line);
    if (!m) continue;
    cmd.set(+m[1], m[3]);
    if (!kids.has(+m[2])) kids.set(+m[2], []);
    kids.get(+m[2]).push(+m[1]);
  }
  const out = [];
  const walk = (p) => { for (const k of kids.get(p) ?? []) { out.push({ pid: k, cmd: cmd.get(k) }); walk(k); } };
  walk(root);
  return out;
}
const alive = (pid) => { try { process.kill(pid, 0); return true; } catch { return false; } };

const dev = spawn(join(CCORE, "dev.sh"), [], {
  env: hostEnv({ CCORE_DEV_PORT: String(PORT), CCORE_DEV_VITE: String(VITE) }),
  stdio: ["ignore", "pipe", "pipe"], detached: true,
});
let out = "";
dev.stdout.on("data", (d) => (out += d));
dev.stderr.on("data", (d) => (out += d));
const exited = new Promise((r) => dev.on("exit", (code, sig) => r({ code, sig })));
const count = (re) => (out.match(re) ?? []).length;
// dev.sh's cycles: started, ended (rebuilt, build failed, or the host did not start), restarts.
const REBUILT = /^rebuilt in \d+ ms/gm;
const starts = () => count(/^dev\.sh: \S+ changed, building$/gm);
const restarts = () => count(REBUILT);
const ends = () => restarts() + count(/^dev\.sh: build failed/gm) + count(/^dev\.sh: the host did not start/gm);
/** Write `data` to the edited file and wait until dev.sh has started a cycle for it and every cycle
 *  has ended, then 300 ms more (six debounces) without a new one. Returns the number of cycles the
 *  write gave (the caller checks it is 1). */
async function save(what, data) {
  const s0 = starts();
  writeAtomic(EDITED, data);
  await waitFor(`the cycle of ${what}`, 30000, async () => {
    if (starts() <= s0 || ends() !== starts()) return false;
    const n = starts();
    await sleep(300);
    return starts() === n && ends() === n;
  });
  return starts() - s0;
}

let tree = [];
try {
  const url = await waitFor("the lab URL", 60000, () => {
    if (dev.exitCode !== null) throw new Error(`dev.sh exited: ${out}`);
    return /lab: (http:\/\/localhost:\d+\/\S+)/.exec(out)?.[1];
  });
  const u = new URL(url);
  const token = u.searchParams.get("token");
  check("lab URL: Vite port, native, gl, host port, token", u.port === String(VITE) && u.searchParams.get("native") === "1" &&
    u.searchParams.get("gl") === "1" && u.searchParams.get("port") === String(PORT) && /^[0-9a-f]{32}$/.test(token ?? ""), url);
  const page = await fetch(url);
  check("Vite serves the lab page", page.ok, String(page.status));
  const st = await req("POST", "/query/state", token);
  let state = null;
  try { state = JSON.parse(st.body); } catch {}
  check("query state answers (the scratch model is open)", st.status === 200 && state !== null && state.file !== null, st.body.slice(0, 120));
  tree = descendants(dev.pid);

  // Five edit cycles: trailing comment on, then off. Each must rebuild and give a new epoch.
  const times = [];
  let epoch = await epochOf(token);
  check("first epoch", /^[0-9a-f]{32}$/.test(epoch ?? ""), String(epoch));
  for (let i = 0; i < 5; i++) {
    const n = restarts();
    const c = await save(`edit ${i + 1}`, i % 2 === 0 ? Buffer.concat([ORIGINAL, Buffer.from("/* dev_test */\n")]) : ORIGINAL);
    const m = [...out.matchAll(/rebuilt in (\d+) ms, host up in (\d+) ms/g)].at(-1);
    if (m) times.push([+m[1], +m[2]]);
    const e = await epochOf(token);
    check(`cycle ${i + 1}: one build, one restart, a new epoch`, c === 1 && restarts() === n + 1 && e !== null && e !== epoch,
      `${c} build(s), ${restarts() - n} restart(s), ${epoch} -> ${e}`);
    epoch = e;
  }
  // Back to the original (the loop ended on the comment): a cycle of its own, ended before the
  // syntax error below is written.
  {
    const n = restarts();
    const c = await save("the restore", ORIGINAL);
    const e = await epochOf(token);
    check("restore: one build, one restart, a new epoch, before the next save", c === 1 && restarts() === n + 1 && e !== null && e !== epoch,
      `${c} build(s), ${restarts() - n} restart(s), ${epoch} -> ${e}`);
    epoch = e;
  }

  // A syntax error: compiler output, no restart, the old host still answers.
  const rebuilt = restarts();
  const c1 = await save("the syntax error", Buffer.concat([ORIGINAL, Buffer.from("int dev_test_broken( {\n")]));
  const failOut = out.slice(out.lastIndexOf("dev.sh: build failed"));
  check("syntax error: dev.sh prints the compiler output", c1 === 1 && out.includes("dev.sh: build failed") && /calls\.c:\d+/.test(failOut),
    `${c1} build(s): ${failOut.split("\n").slice(0, 3).join(" | ")}`);
  check("syntax error: no restart", restarts() === rebuilt, `${restarts() - rebuilt} restart(s)`);
  const eOld = await epochOf(token);
  const st2 = await req("POST", "/query/state", token);
  check("syntax error: the old host still answers, same epoch", st2.status === 200 && eOld === epoch, `${st2.status} ${epoch} -> ${eOld}`);

  // Revert: the build passes again and the host restarts, once: the restart is the revert's own.
  const c2 = await save("the revert", ORIGINAL);
  const eNew = await epochOf(token);
  check("revert: rebuild OK, new epoch", eNew !== null && eNew !== epoch, `${epoch} -> ${eNew}`);
  check("revert: one build, one restart since the syntax error", c2 === 1 && restarts() === rebuilt + 1,
    `${c2} build(s), ${restarts() - rebuilt} restart(s)`);

  console.log("cycle  rebuild ms  host up ms");
  times.forEach(([b, h], i) => console.log(`${String(i + 1).padStart(5)}  ${String(b).padStart(10)}  ${String(h).padStart(10)}`));
} catch (e) {
  check("dev loop", false, e.message);
  console.log(out);
} finally {
  writeAtomic(EDITED, ORIGINAL);
  tree = [...tree, ...descendants(dev.pid)];
  const pids = [...new Set(tree.map((t) => t.pid))];
  process.kill(dev.pid, "SIGTERM");
  const ex = await Promise.race([exited, sleep(10000).then(() => null)]);
  check("SIGTERM stops dev.sh", ex !== null, JSON.stringify(ex));
  await sleep(200);
  const left = tree.filter((t) => alive(t.pid));
  check("no process left (host, Vite, watcher)", pids.length > 0 && left.length === 0, left.map((t) => `${t.pid} ${t.cmd}`).join("; "));
  check("the host was among them", tree.some((t) => new RegExp(`bimble-ccore (--session )?(--warm )?${PORT}`).test(t.cmd)) && tree.some((t) => /vite/.test(t.cmd)));
  // By listening port, not by command line: a pgrep pattern also matched other shells that
  // merely mention it (false positive), and missed the host once it took --session.
  // TEST1: dev.sh has exited, but under load its children may still be closing their sockets (one
  // FAIL at load 25, 200 ms after the exit): the ports must be free within 5 s, and the time it took
  // is printed. (A test of another run on these ports is ruled out: ports.mjs locks them.)
  const listening = () => { try { return execFileSync("ss", ["-ltnpH", `( sport = :${PORT} or sport = :${VITE} )`], { encoding: "utf8" }).trim(); } catch { return ""; } };
  const tl = Date.now();
  let ls = listening();
  while (ls !== "" && Date.now() - tl < 5000) { await sleep(50); ls = listening(); }
  console.log(`ports ${PORT}, ${VITE} free ${Date.now() - tl} ms after the check began`);
  check(`nothing listens on ${PORT} or ${VITE} (within 5 s of dev.sh's exit)`, ls === "", ls);
  for (const t of left) try { process.kill(t.pid, "SIGKILL"); } catch {}
  try { process.kill(-dev.pid, "SIGKILL"); } catch {}
  check("session/calls.c restored byte for byte", readFileSync(EDITED).equals(ORIGINAL));
  console.log(`${passed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
}
