// The host's session file survives a restart (ticket T12): node ccore/test/session_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) with --session on 47341, on a temporary copy of
// golden/frame.json. Places a column (verb place), selects it, stops the host (SIGTERM) and starts
// it again on the same port and file; then checks: rev and selection as before (GET /change),
// history as before, 73 elements, `undo` works and removes the column (72), redo brings it back.
// Also: a SIGKILL right after a change keeps it (the file is written after each call, not only
// at exit); a corrupted session file and a model file changed since are ignored, logged, and the
// host starts from the model file (rev 1, 72 elements, no history).
// Prints "N passed, M failed"; exits non-zero on a failure. Kills all it starts.
import { spawn } from "node:child_process";
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync, utimesSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = 47341;
const TOKEN = "session-test";
const BASE = `http://127.0.0.1:${PORT}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-session-"));
const MODEL = join(tmp, "frame.bimble");
const SESSION = `${MODEL}.ccore-session`;
const STOREY = "1uAZA0Yfn05f$xRd1k4uPR"; // golden/frame.json, as host_test.mjs places
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};

async function startHost() {
  const t0 = performance.now();
  const h = spawn(BIN, ["--session", String(PORT), MODEL], { stdio: ["ignore", "pipe", "pipe"], env: { ...process.env, BIMBLE_TOKEN: TOKEN } });
  h.out = "";
  h.stdout.on("data", (d) => (h.out += d));
  h.stderr.on("data", (d) => (h.out += d));
  procs.push(h);
  for (let i = 0; i < 1000 && !h.out.includes("listening"); i++) {
    if (h.exitCode !== null) throw new Error(`host exited: ${h.out}`);
    await sleep(5);
  }
  if (!h.out.includes("listening")) throw new Error(`host not listening: ${h.out}`);
  h.startMs = performance.now() - t0;
  return h;
}

async function stop(h, sig = "SIGTERM") {
  if (h.exitCode !== null || h.signalCode !== null) return;
  const gone = new Promise((r) => h.once("exit", r));
  h.kill(sig);
  await Promise.race([gone, sleep(3000)]);
}

const call = async (op, name, body = "") => {
  const r = await fetch(`${BASE}/${op}/${name}?t=${TOKEN}`, { method: "POST", body, headers: { "Content-Type": "text/plain" } });
  return { status: r.status, body: await r.text() };
};
const q = async (name, args) => {
  const r = await call("query", name, args === undefined ? "" : JSON.stringify(args));
  if (r.status !== 200) throw new Error(`query ${name}: ${r.status} ${r.body}`);
  return JSON.parse(r.body);
};
const change = async () => (await (await fetch(`${BASE}/change?since=0&t=${TOKEN}`)).json()).change;
const elements = async () => (await q("elements")).length;
const ids = async () => new Set((await q("elements")).map((e) => e.id));

try {
  copyFileSync(join(CCORE, "golden/frame.json"), MODEL);
  let h = await startHost();
  console.log(`host start (spawn to 'listening'), no session file yet: ${h.startMs.toFixed(1)} ms`);
  const n0 = await elements();
  check("fresh start: 72 elements, rev 1", n0 === 72 && (await change()).rev === 1, `${n0}, rev ${(await change()).rev}`);
  const before = await ids();
  const pl = await call("verb", "place", JSON.stringify({ a: [1500.0, 1500.0], kind: "column", storey: STOREY }));
  check("place a column -> 200", pl.status === 200, pl.body);
  const col = [...(await ids())].find((id) => !before.has(id));
  check("the new column has an id", typeof col === "string" && col.length === 22, String(col));
  const sel = await call("select", "-", JSON.stringify([col]));
  check("select it -> 200", sel.status === 200, sel.body);
  const c1 = await change();
  const h1 = await q("history");
  const st1 = await q("state");
  console.log(`before restart: rev ${c1.rev}, selection ${JSON.stringify(c1.selection)}, history ${JSON.stringify(h1)}, unsaved ${st1.unsaved}`);
  check("session file written after the change", existsSync(SESSION));

  await stop(h);
  check("host stopped by SIGTERM", h.exitCode !== null || h.signalCode !== null, `exit ${h.exitCode} ${h.signalCode}`);
  h = await startHost();
  console.log(`host restart with the session file (spawn to 'listening'): ${h.startMs.toFixed(1)} ms`);
  check("restart logs the restore", /session restored/.test(h.out), h.out.trim());
  const c2 = await change();
  check("same rev after restart", c2.rev === c1.rev, `${c1.rev} -> ${c2.rev}`);
  check("same selection after restart", JSON.stringify(c2.selection) === JSON.stringify(c1.selection) && c2.selection[0] === col,
    `${JSON.stringify(c1.selection)} -> ${JSON.stringify(c2.selection)}`);
  check("same unsaved after restart", c2.unsaved === c1.unsaved && (await q("state")).unsaved === st1.unsaved, `${c1.unsaved} -> ${c2.unsaved}`);
  const h2 = await q("history");
  check("same history after restart", JSON.stringify(h2) === JSON.stringify(h1), JSON.stringify(h2));
  const n2 = await elements();
  check("73 elements after restart", n2 === 73, String(n2));
  const un = await call("undo", "-");
  const n3 = await elements();
  check("undo after restart -> 200, the column is gone (72)", un.status === 200 && n3 === 72 && !(await ids()).has(col), `${un.status} ${un.body}, ${n3}`);
  const c3 = await change();
  check("undo: rev + 1, the selection dropped, redo offered", c3.rev === c1.rev + 1 && c3.selection.length === 0 &&
    (await q("history")).redo[0] === h1.undo[0], `rev ${c3.rev}, sel ${JSON.stringify(c3.selection)}`);

  // SIGKILL right after a change: the file was written after the call, not at exit.
  const re = await call("redo", "-");
  check("redo -> 200, 73 elements", re.status === 200 && (await elements()) === 73, re.body);
  const c4 = await change();
  await stop(h, "SIGKILL");
  h = await startHost();
  const c5 = await change();
  check("after SIGKILL: the redo is kept (rev, 73 elements, undo offered)", c5.rev === c4.rev && (await elements()) === 73 &&
    (await q("history")).undo[0] === h1.undo[0], `rev ${c4.rev} -> ${c5.rev}`);
  await stop(h);

  // A corrupted session file: ignored and logged, the host starts from the model file.
  const good = readFileSync(SESSION, "utf8");
  const corruptions = [
    ["truncated", good.slice(0, Math.floor(good.length / 2))],
    ["not JSON", "\u0000\u0001garbage"],
    ["a selection that is not an element", good.replace(/"primary":(null|"[^"]*")/, '"primary":"0000000000000000000000"')],
    ["a command that does not parse", good.replace('"cmds":"[', '"cmds":"[{\\"op\\":\\"nope\\"},')],
  ];
  for (const [what, text] of corruptions) {
    if (text === good) throw new Error(`corruption "${what}" changed nothing`);
    writeFileSync(SESSION, text);
    h = await startHost();
    const c = await change();
    const n = await elements();
    const hi = await q("history");
    check(`corrupted session (${what}): starts from the model file (rev 1, 72 elements, no history) and logs`,
      c.rev === 1 && n === 72 && hi.undo.length === 0 && c.selection.length === 0 && /session .* ignored: /.test(h.out),
      `rev ${c.rev}, ${n} elements, log: ${h.out.trim().split("\n").filter((l) => /session/.test(l)).join(" | ")}`);
    await stop(h);
  }

  // The model file changed since the session was written (another program saved it): ignored.
  h = await startHost();
  await call("verb", "place", JSON.stringify({ a: [2500.0, 1500.0], kind: "column", storey: STOREY }));
  check("one more column placed (73)", (await elements()) === 73);
  await stop(h);
  const later = new Date(Date.now() + 5000);
  utimesSync(MODEL, later, later);
  h = await startHost();
  check("model file changed since: the session is ignored, logged, 72 elements", (await elements()) === 72 &&
    /changed since/.test(h.out), h.out.trim().split("\n").filter((l) => /session/.test(l)).join(" | "));
  await stop(h);
} catch (e) {
  check("no exception in the test", false, e.stack ?? String(e));
  for (const p of procs) console.log(`--- host output ---\n${p.out.slice(-2000)}`);
} finally {
  for (const p of procs) if (p.exitCode === null && p.signalCode === null) p.kill("SIGKILL");
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
