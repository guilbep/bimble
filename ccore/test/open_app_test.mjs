// `bimble-ccore open --app` (ticket GPU2): an app window of a Chromium-family browser with its own profile and
// the flags that give WebGPU on Linux; the setting open_app does the same; --no-app and no browser found fall
// back to the system's opener.
//   node ccore/test/open_app_test.mjs          (after ccore/build.sh tcc; HOST_BIN overrides the program)
// A fake browser (BIMBLE_BROWSER, a shell script) and a fake xdg-open first on PATH write their arguments to a
// file; no browser is started. Linux and macOS (the flags are Linux's); Windows: skipped. Prints "N passed, M
// failed"; exits non-zero on a failure.
import { spawn } from "node:child_process";
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => { if (ok) passed++; else failed++; console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`); };
if (process.platform === "win32") { console.log("SKIP open --app: not on Windows yet"); process.exit(0); }
if (!existsSync(BIN)) { console.log(`FAIL no host program at ${BIN}: ccore/build.sh tcc`); process.exit(1); }

const work = mkdtempSync(join(tmpdir(), "gpu2-open-app-"));
const web = join(work, "web"), bin = join(work, "bin"), conf = join(work, "config"), rec = join(work, "args");
mkdirSync(web), mkdirSync(bin), mkdirSync(conf);
writeFileSync(join(web, "index.html"), "<!doctype html><title>lab</title>");
const fake = (name) => { const p = join(bin, name); writeFileSync(p, `#!/bin/sh\nfor a in "$@"; do printf '%s\\n' "$a"; done > "${rec}.${name}"\n`); chmodSync(p, 0o755); return p; };
const browser = fake("fake-chromium");
fake("xdg-open"), fake("open"); // (macOS's opener is `open`)

/** Starts `open` with args and env, waits for the URL on stdout and for a fake to write its arguments; returns both. */
async function run(args, env) {
  for (const n of ["fake-chromium", "xdg-open", "open"]) rmSync(`${rec}.${n}`, { force: true });
  const p = spawn(BIN, ["open", "--port", "0", "--web", web, "--config-dir", conf, ...args], { env: { ...process.env, PATH: bin /* only the fakes: a real browser is never found nor started */, ...env }, stdio: ["ignore", "pipe", "pipe"] });
  let out = "", err = "";
  p.stdout.on("data", (d) => (out += d)), p.stderr.on("data", (d) => (err += d));
  const until = Date.now() + 15000;
  let who = null;
  while (Date.now() < until && !who) {
    await new Promise((r) => setTimeout(r, 100));
    who = ["fake-chromium", "xdg-open", "open"].find((n) => existsSync(`${rec}.${n}`) && readFileSync(`${rec}.${n}`, "utf8").endsWith("\n")) ?? null;
  }
  await new Promise((r) => setTimeout(r, 100));
  p.kill("SIGTERM");
  await new Promise((r) => p.on("exit", r));
  const url = (/http:\/\/127\.0\.0\.1:\d+\/\S+/.exec(out) ?? [""])[0];
  return { url, who, args: who ? readFileSync(`${rec}.${who}`, "utf8").trim().split("\n") : [], err };
}

try {
  const LINUX = process.platform === "linux";
  const GPU = LINUX ? ["--use-angle=vulkan", "--enable-features=Vulkan", "--enable-unsafe-webgpu"] : [];
  // 1. --app with a Chromium-family browser: an app window, its own profile beside the settings, the WebGPU flags
  const a = await run(["--app"], { BIMBLE_BROWSER: browser });
  check("--app: the fake browser is started, not the opener", a.who === "fake-chromium", `${a.who}; ${a.err.trim().split("\n").pop()}`);
  check("--app: --app=<the lab's URL>", a.url !== "" && a.args.includes(`--app=${a.url}`), a.args.join(" "));
  check("--app: its own profile, <config dir>/browser", a.args.includes(`--user-data-dir=${join(conf, "browser")}`));
  check(`--app: the flags that give WebGPU here (${GPU.join(" ") || "none on this platform"}) and no other GPU flag`,
    GPU.every((f) => a.args.includes(f)) && a.args.filter((x) => !/^--(app|user-data-dir)=/.test(x) && /angle|vulkan|webgpu|gpu/i.test(x)).length === GPU.length, a.args.join(" "));
  // 2. the setting open_app (settings.json in the config dir), no flag
  writeFileSync(join(conf, "settings.json"), JSON.stringify({ open_app: true }));
  const b = await run([], { BIMBLE_BROWSER: browser });
  check("the setting open_app: the app window without --app", b.who === "fake-chromium" && b.args.includes(`--app=${b.url}`), b.args.join(" "));
  // 3. --no-app over the setting: the system's opener with the URL alone
  const c = await run(["--no-app"], { BIMBLE_BROWSER: browser });
  const opener = process.platform === "darwin" ? "open" : "xdg-open";
  check("--no-app: the system's opener, the URL alone", c.who === opener && c.args.length === 1 && c.args[0] === c.url, `${c.who}: ${c.args.join(" ")}`);
  // 4. --app with no Chromium-family browser found (none on PATH, no BIMBLE_BROWSER): the usual opening, said
  writeFileSync(join(conf, "settings.json"), "{}");
  const d = await run(["--app"], { BIMBLE_BROWSER: "" });
  check("--app, no Chromium found: the usual opening", d.who === opener && d.args[0] === d.url, `${d.who}: ${d.args.join(" ")}`);
  check("--app, no Chromium found: said on stderr", /no Chromium-family browser found/.test(d.err), d.err.trim().split("\n").pop());
} catch (e) {
  check("the run", false, e?.stack ?? String(e));
} finally {
  rmSync(work, { recursive: true, force: true });
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
