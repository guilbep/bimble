// The session file as an append-only log (ticket T18): node ccore/test/session_log_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) with --session on 47382, on a temporary copy of
// golden/frame.json. Checks: the fresh snapshot has no model text (the model is the file); each
// change appends one line (the file is never rewritten for a change); a mix of places, undo, redo,
// select and a save survives SIGKILL with the same elements, history, rev and selection; after a
// Save the snapshot is without model text again; past 2,000 entries a new snapshot is written
// with the model text (the 4x-bytes rule may write one earlier), and a restore from it does not read the model file (its content replaced
// by garbage of the same size and mtime still restores). Prints "N passed, M failed".
import { execFileSync, spawn } from "node:child_process";
import { copyFileSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("session_log", 47382);
const TOKEN = "session-log-test";
const BASE = `http://127.0.0.1:${PORT}`;
const tmp = mkdtempSync(join(tmpdir(), "ccore-session-log-"));
const MODEL = join(tmp, "frame.bimble");
const SESSION = `${MODEL}.ccore-session`;
const STOREY = "1uAZA0Yfn05f$xRd1k4uPR";
const procs = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};

async function startHost() {
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
const q = async (name, args) => JSON.parse((await call("query", name, args === undefined ? "" : JSON.stringify(args))).body);
const change = async () => (await (await fetch(`${BASE}/change?since=0&t=${TOKEN}`)).json()).change;
const place = (i) => call("verb", "place", JSON.stringify({ a: [1000.0 + 37 * (i % 100), 1000.0 + 53 * Math.floor(i / 100)], kind: "column", storey: STOREY }));
const lines = () => readFileSync(SESSION, "utf8").split("\n").filter((l) => l);
const snapshot = () => JSON.parse(lines()[0]);
const state = async () => JSON.stringify({ els: await q("elements", { limit: 10000 }), hist: await q("history"), ch: await change(), st: (await q("state")).unsaved });

try {
  copyFileSync(join(CCORE, "golden/frame.json"), MODEL);
  let h = await startHost();
  check("fresh snapshot: format 2, no model text", snapshot().format === 2 && snapshot().model === null, lines()[0].slice(0, 120));
  for (let i = 0; i < 5; i++) await place(i);
  await call("undo", "-"), await call("undo", "-"), await call("redo", "-");
  const ids = (await q("elements", { limit: 10000 })).map((e) => e.id);
  await call("select", "-", JSON.stringify([ids[ids.length - 1], ids[0]]));
  const n1 = lines().length;
  if (n1 !== 10) console.log(lines().slice(1).map((l) => JSON.parse(l).op).join(" "));
  check("one line per change (5 place, 2 undo, 1 redo, 1 select)", n1 === 1 + 9, `${n1} lines`);
  const ino = statSync(SESSION).ino;
  await place(5);
  check("a change appends (same file, one more line)", statSync(SESSION).ino === ino && lines().length === n1 + 1);
  const s1 = await state();
  await stop(h, "SIGKILL");
  h = await startHost();
  check("SIGKILL then restart: same elements, history, rev, selection, unsaved", (await state()) === s1);
  // Save: a new snapshot, without model text (the model is the file again).
  const sv = await call("action", "save");
  check("save -> 200", sv.status === 200, sv.body);
  check("after save: one snapshot line, no model text", lines().length === 1 && snapshot().model === null, `${lines().length} lines`);
  await place(6);
  await call("undo", "-");
  const s2 = await state();
  await stop(h, "SIGKILL");
  h = await startHost();
  check("after save + changes + SIGKILL: same state", (await state()) === s2);
  // Past 2,000 entries: a new snapshot with the model text.
  for (let i = 7; i < 2010; i++) await place(i);
  const snap = snapshot();
  check("2,000 changes later: a newer snapshot with the model text, far fewer lines", typeof snap.model === "string" && lines().length < 2000,
    `${lines().length} lines, model ${typeof snap.model}`);
  const s3 = await state();
  await stop(h);
  // The restore reads the snapshot's model text, not the model file: garbage of the same size and mtime.
  const st = statSync(MODEL), ref = join(tmp, "stamp");
  writeFileSync(ref, "");
  execFileSync("touch", ["-r", MODEL, ref]); // touch keeps the nanoseconds; Node's utimes does not
  writeFileSync(MODEL, "x".repeat(st.size));
  execFileSync("touch", ["-r", ref, MODEL]);
  h = await startHost();
  check("restore from a snapshot with text does not read the model file", (await state()) === s3 && /session restored/.test(h.out),
    h.out.split("\n").filter((l) => /session/.test(l)).join(" | "));
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
