// The web session log's route on the native host (ticket VIS2, host/server.c GET|POST /session_log): node ccore/test/web_session_log_test.mjs
// A host with XDG_STATE_HOME and BIMBLE_CONFIG_DIR in a temporary folder: GET answers on and the session folder
// (<state>/bimble/web-sessions/<UTC>-<pid>); POST appends to session.log and frames.csv and replaces report.txt;
// another file name is refused (400), a request without the token or from another Origin is forbidden (403); with
// 25 old session folders only the last 20 remain; the setting session_log false: GET answers off, POST 204, nothing
// written. Fails on the code before VIS2: no /session_log route.
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const PORT = port("web_session_log_test"), TOKEN = "slog-token", BASE = `http://127.0.0.1:${PORT}`;
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => { if (ok) passed++; else failed++; console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`); };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const tmp = mkdtempSync(join(tmpdir(), "ccore-slog-"));
const state = join(tmp, "state"), config = join(tmp, "config"), sessions = join(state, "bimble/web-sessions");
mkdirSync(config, { recursive: true });
for (let i = 0; i < 25; i++) mkdirSync(join(sessions, `2020010${Math.floor(i / 10)}T0000${String(i % 10).padStart(2, "0")}Z-1${i}`), { recursive: true });

async function withHost(f) {
  const h = spawn(BIN, [String(PORT)], { env: { ...process.env, BIMBLE_TOKEN: TOKEN, XDG_STATE_HOME: state, BIMBLE_CONFIG_DIR: config }, stdio: ["ignore", "ignore", "pipe"] });
  let err = "";
  h.stderr.on("data", (d) => (err += d));
  for (let i = 0; i < 100 && !/listening/.test(err); i++) await sleep(50);
  try { await f(); } finally { h.kill("SIGTERM"); await new Promise((r) => h.once("exit", r)); }
}
const req = (path, opts = {}) => fetch(`${BASE}${path}`, opts);

try {
  await withHost(async () => {
    const g = await req(`/session_log?t=${TOKEN}`);
    const j = g.status === 200 ? await g.json() : null;
    check("GET: on by default, the session folder under $XDG_STATE_HOME/bimble/web-sessions", j?.on === true && typeof j.dir === "string" && j.dir.startsWith(sessions + "/") && existsSync(j.dir), `${g.status} ${JSON.stringify(j)}`);
    check("the last 20 sessions kept (25 old ones and this one: 20 remain)", existsSync(sessions) && readdirSync(sessions).length === 20, existsSync(sessions) ? String(readdirSync(sessions).length) : "none");
    if (!j?.dir) return;
    const p1 = await req(`/session_log?t=${TOKEN}&file=session.log`, { method: "POST", body: "a line\n" });
    await req(`/session_log?t=${TOKEN}&file=session.log`, { method: "POST", body: "another\n" });
    await req(`/session_log?t=${TOKEN}&file=frames.csv`, { method: "POST", body: "time,frames\n" });
    await req(`/session_log?t=${TOKEN}&file=report.txt&mode=replace`, { method: "POST", body: "first report" });
    await req(`/session_log?t=${TOKEN}&file=report.txt&mode=replace`, { method: "POST", body: "second report" });
    check("POST: appended to session.log, frames.csv; report.txt replaced", p1.status === 200 && readFileSync(join(j.dir, "session.log"), "utf8") === "a line\nanother\n"
      && readFileSync(join(j.dir, "frames.csv"), "utf8") === "time,frames\n" && readFileSync(join(j.dir, "report.txt"), "utf8") === "second report");
    const bad = await req(`/session_log?t=${TOKEN}&file=..%2F..%2Fx.txt`, { method: "POST", body: "x" });
    check("another file name refused (400), nothing written outside", bad.status === 400 && !existsSync(join(j.dir, "../../x.txt")), String(bad.status));
    const nt = await req(`/session_log?file=session.log`, { method: "POST", body: "x" });
    const og = await req(`/session_log?t=${TOKEN}&file=session.log`, { method: "POST", body: "x", headers: { Origin: "https://example.com" } });
    check("without the token, or from another Origin: 403", nt.status === 403 && og.status === 403, `${nt.status} ${og.status}`);
    const off = await req(`/action/set_settings?t=${TOKEN}`, { method: "POST", body: JSON.stringify({ session_log: false }), headers: { "Content-Type": "text/plain" } });
    const g2 = await req(`/session_log?t=${TOKEN}`), j2 = g2.status === 200 ? await g2.json() : null;
    const p2 = await req(`/session_log?t=${TOKEN}&file=session.log`, { method: "POST", body: "not written\n" });
    check("the setting session_log false: GET off, POST 204, nothing written", off.status === 200 && j2?.on === false && p2.status === 204 && !readFileSync(join(j.dir, "session.log"), "utf8").includes("not written"), `${off.status} ${JSON.stringify(j2)} ${p2.status}`);
  });
} catch (e) {
  check("no exception", false, e.stack ?? String(e));
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`web_session_log: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
