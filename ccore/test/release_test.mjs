// Checks the Linux release archive (ticket R1): node ccore/test/release_test.mjs, after
// `ccore/build.sh release`. Unpacks out/release/bimble-ccore-<VERSION>-<arch>-linux.tar.gz in a
// temp folder (no repo there: the archive alone), then:
//   - the archive holds bimble-ccore, web/index.html, LICENSE and README.txt in one folder;
//     `--version` prints ccore/VERSION; ldd names no libmanifold (the kernel is static);
//   - `bimble-ccore open --no-browser frame.json` prints the lab's URL; GET / is web/index.html;
//     every file of web/ comes back byte for byte with its content type, without the token;
//   - path traversal, an unknown path, a symbolic link and a hidden file in web/ are 404; a wrong
//     Host or a foreign Origin is 403; the calls and /change still need the token;
//   - a query answers (the model is loaded), from the page's own origin too; SIGTERM exits 0;
//   - ticket SIZE1: no font file in the archive (no .ttf, no web/fonts/); `query view_pdf` of a plan
//     answers a Helvetica PDF (nothing embedded) and `pdf_fonts` says roboto false (BIMBLE_FONTS: an
//     empty folder of the test, so the real ~/.config is never read); with BIMBLE_FONTS holding the
//     two Roboto files (copied from the repository), roboto true and `{"font":"roboto"}` embeds them;
//   - FIXDOC: THIRD-PARTY.txt and every licences/ file it names; readelf -d: BIND_NOW (Linux);
//     the Roboto files of the user's Bimble folder (settings.rs's spelling) under a non-ASCII profile;
//   - `--port N` serves on N; a web/ without index.html or over 64 MiB refuses to start;
//   - without --no-browser, xdg-open (a stand-in first on PATH) gets the URL and no socket.
//   - NUI1 (Linux, when the archive holds the native app `bimble`): its third parties' licences, ldd names
//     no libstdc++ and BIND_NOW, `--version`, and `bimble --offscreen --quit-after 3 --shot` on the fixture
//     exits 0 with a picture that is not blank (no GPU device here: exit 3 and a SKIP line, said); an
//     archive without it (SDL3 absent where it was built) says so in a SKIP line.
// The ports: the system's free ones (open picks one; --port gets one from node), not test/ports.mjs.
// Prints "N passed, M failed"; exits non-zero on a failure. Needs tar and ldd.
// macOS (ticket MAC1): bimble-ccore-<VERSION>-<arch>-macos.tar.gz (RELEASE_ARCH=x86_64: the Intel
// one on an arm64 Mac, run by Rosetta 2), otool -L for ldd (libc++ is the system's), the stand-in
// is named `open`, and it lists its files with lsof (no /proc).
// Windows (ticket WIN2): bimble-ccore-<VERSION>-x86_64-windows.zip, listed and unpacked by the
// system's tar.exe (bsdtar reads zip), bimble-ccore.exe; llvm-objdump -p for ldd (no libc++,
// libunwind, winpthreads DLL: linked static); the web/ links are a file link and a junction to
// C:\Windows; no SIGTERM and no stand-in for ShellExecuteW: those two checks print a skip line.
import { execFileSync, spawn } from "node:child_process";
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, symlinkSync, truncateSync, writeFileSync } from "node:fs";
import net from "node:net";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { readPng, inkShare } from "./lib/png.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const VERSION = readFileSync(join(CCORE, "VERSION"), "utf8").trim();
const MAC = process.platform === "darwin";
const WIN = process.platform === "win32"; // WIN2
const ARCH = process.env.RELEASE_ARCH || (WIN ? "x86_64" : execFileSync("uname", ["-m"], { encoding: "utf8" }).trim());
const NAME = `bimble-ccore-${VERSION}-${ARCH}-${MAC ? "macos" : WIN ? "windows" : "linux"}`;
const OPENER = MAC ? "open" : "xdg-open";
const EXT = WIN ? ".zip" : ".tar.gz";
const ARCHIVE = process.env.RELEASE_ARCHIVE ?? join(CCORE, "out/release", `${NAME}${EXT}`);
const EXE = WIN ? "bimble-ccore.exe" : "bimble-ccore";
// WIN2: the system's bsdtar (MSYS2's GNU tar first on PATH reads no zip)
const TAR = WIN ? join(process.env.SystemRoot || "C:\\Windows", "System32", "tar.exe") : "tar";

let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** One raw request (the target as is, no normalisation): {status, head, body}. */
function raw(port, text, ms = 3000) {
  return new Promise((res) => {
    const s = net.connect(port, "127.0.0.1");
    const chunks = [];
    let done = false;
    const end = () => {
      if (done) return;
      done = true;
      const b = Buffer.concat(chunks), i = b.indexOf("\r\n\r\n");
      const head = i < 0 ? b.toString("latin1") : b.subarray(0, i).toString("latin1");
      res({ status: Number(/^HTTP\/1\.1 (\d{3})/.exec(head)?.[1] ?? 0), head, body: i < 0 ? Buffer.alloc(0) : b.subarray(i + 4) });
    };
    s.on("data", (d) => chunks.push(d));
    s.on("error", end);
    s.on("close", end);
    s.write(text);
    setTimeout(() => { s.destroy(); end(); }, ms);
  });
}
const get = (port, target, headers = {}) => {
  const h = { Host: `127.0.0.1:${port}`, Connection: "close", ...headers };
  return raw(port, `GET ${target} HTTP/1.1\r\n${Object.entries(h).map(([k, v]) => `${k}: ${v}\r\n`).join("")}\r\n`);
};
const post = (port, target, body = "", headers = {}) => {
  const h = { Host: `127.0.0.1:${port}`, Connection: "close", "Content-Type": "text/plain", "Content-Length": Buffer.byteLength(body), ...headers };
  return raw(port, `POST ${target} HTTP/1.1\r\n${Object.entries(h).map(([k, v]) => `${k}: ${v}\r\n`).join("")}\r\n${body}`);
};
const header = (head, name) => new RegExp(`^${name}: (.*)$`, "im").exec(head)?.[1]?.trim();

/** A free port from the system (listen on 0, close). */
const freePort = () => new Promise((res, rej) => {
  const s = net.createServer();
  s.once("error", rej);
  s.listen(0, "127.0.0.1", () => { const p = s.address().port; s.close(() => res(p)); });
});

/** Starts the unpacked host: {proc, out (stdout), log (stderr), exited}. */
function start(bin, args, cwd, env = {}) {
  // SIZE1: the font folder and the config dir are the test's (never the user's ~/.config/bimble); FS1: BIMBLE_CONFIG_DIR too.
  const proc = spawn(bin, args, { cwd, env: { ...process.env, BIMBLE_TOKEN: "", BIMBLE_FONTS: join(tmp, "nofonts"), XDG_CONFIG_HOME: join(tmp, "config"), BIMBLE_CONFIG_DIR: join(tmp, "config", "bimble"), ...env }, stdio: ["ignore", "pipe", "pipe"] });
  const st = { proc, out: "", log: "" };
  proc.stdout.on("data", (d) => (st.out += d));
  proc.stderr.on("data", (d) => (st.log += d));
  st.exited = new Promise((r) => proc.on("exit", (code, sig) => r({ code, sig })));
  return st;
}
async function waitUrl(st) {
  for (let i = 0; i < 400 && !/\n/.test(st.out) && st.proc.exitCode === null; i++) await sleep(5);
  return st.out.split("\n")[0];
}
async function stop(st) {
  st.proc.kill("SIGTERM");
  return Promise.race([st.exited, sleep(3000).then(() => null)]);
}

/** Every file under dir, as "/a/b" paths. */
function files(dir, pre = "") {
  const out = [];
  for (const e of readdirSync(join(dir, pre), { withFileTypes: true })) {
    const p = `${pre}/${e.name}`;
    if (e.isDirectory()) out.push(...files(dir, p));
    else if (e.isFile()) out.push(p);
  }
  return out.sort();
}
const CTYPE = { html: "text/html", js: "text/javascript", css: "text/css", woff2: "font/woff2", json: "application/json", svg: "image/svg+xml", png: "image/png", wasm: "application/wasm", ttf: "font/ttf" };

const tmp = mkdtempSync(join(tmpdir(), "ccore-release-"));
let host = null;
try {
  check(`archive ${NAME}${EXT} exists`, existsSync(ARCHIVE), ARCHIVE);
  console.log(`archive bytes: ${statSync(ARCHIVE).size}`);
  const list = execFileSync(TAR, WIN ? ["tf", ARCHIVE] : ["tzf", ARCHIVE], { encoding: "utf8" }).split(/\r?\n/).filter(Boolean);
  const top = new Set(list.map((p) => p.split("/")[0]));
  check("one top folder named after the version and arch", top.size === 1 && top.has(NAME), [...top].join(" "));
  for (const f of [EXE, "web/index.html", "LICENSE", "README.txt", "THIRD-PARTY.txt"])
    check(`archive holds ${f}`, list.includes(`${NAME}/${f}`));
  // FIXDOC: every licence file THIRD-PARTY.txt names under licences/ is in the archive, and it
  // names the parts built in and the fonts of web/.
  const tp = execFileSync(TAR, WIN ? ["xOf", ARCHIVE, `${NAME}/THIRD-PARTY.txt`] : ["xzOf", ARCHIVE, `${NAME}/THIRD-PARTY.txt`], { encoding: "utf8" });
  const NATIVE = !WIN && !MAC && list.includes(`${NAME}/bimble`); // NUI1
  const NATIVE_LIC = ["licences/DearImGui-LICENSE.txt", "licences/cimgui-LICENSE.txt", "licences/nanosvg-LICENSE.txt"];
  const named = [...tp.matchAll(/licences\/[\w.-]+/g)].map((m) => m[0]).filter((l) => NATIVE || !NATIVE_LIC.includes(l));
  const missing = named.filter((l) => !list.includes(`${NAME}/${l}`));
  check("THIRD-PARTY.txt: every licences/ file it names is in the archive", named.length >= 9 && missing.length === 0, `${named.length} named; missing ${missing.join(" ")}`);
  const parts = ["manifold", "Clipper2", "BLAKE3", "Ryu", "earcutr", "Sora", "Source Sans 3", "DM Mono"];
  const absent = parts.filter((p) => !tp.includes(p));
  check("THIRD-PARTY.txt names manifold, Clipper2, BLAKE3, Ryu, earcutr and the three UI fonts", absent.length === 0, absent.join(", "));
  const spdx = ["Apache-2.0", "BSL-1.0", "CC0-1.0", "ISC", "OFL-1.1"].filter((id) => !tp.includes(id));
  check("THIRD-PARTY.txt gives their SPDX identifiers", spdx.length === 0, spdx.join(", "));
  const ttf = list.filter((p) => /\.(ttf|otf)$/i.test(p) || /\/fonts\//.test(p));
  check("SIZE1: no font file in the archive", ttf.length === 0, ttf.join(" "));
  const rest = list.filter((p) => !(WIN ? /^[^/]+\/(bimble-ccore\.exe|LICENSE|README\.txt|THIRD-PARTY\.txt|licences\/.*|web\/.*)?$/ : /^[^/]+\/(bimble-ccore|bimble|LICENSE|README\.txt|THIRD-PARTY\.txt|licences\/.*|web\/.*)?$/).test(p));
  check("nothing else in the archive", rest.length === 0, rest.join(" "));

  execFileSync(TAR, [WIN ? "xf" : "xzf", ARCHIVE, "-C", tmp]);
  const root = join(tmp, NAME), bin = join(root, EXE), web = join(root, "web");
  const readme = readFileSync(join(root, "README.txt"), "utf8").trimEnd().split(/\r?\n/);
  check("README.txt has at most ten lines", readme.length <= 10, `${readme.length} lines`);
  const ver = execFileSync(bin, ["--version"], { encoding: "utf8" }).trim();
  check("--version prints ccore/VERSION", ver === `bimble-ccore ${VERSION}`, ver);
  if (WIN) {
    // WIN2: the imported DLLs (llvm-objdump -p: "DLL Name: X"): the system's and the UCRT only.
    const dlls = [...execFileSync("llvm-objdump", ["-p", bin], { encoding: "utf8" }).matchAll(/DLL Name: (\S+)/g)].map((x) => x[1]);
    check("imports name no manifold DLL, and KERNEL32.dll", !dlls.some((d) => /manifold/i.test(d)) && dlls.some((d) => /^kernel32\.dll$/i.test(d)), dlls.join(" "));
    check("imports name no libc++, libunwind, winpthreads, libgcc or libstdc++ DLL (static)",
      !dlls.some((d) => /^(libc\+\+|libunwind|libwinpthread|libgcc|libstdc\+\+)/i.test(d)), dlls.join(" "));
  } else {
    const ldd = execFileSync(MAC ? "otool" : "ldd", MAC ? ["-L", bin] : [bin], { encoding: "utf8" });
    check("ldd names no libmanifold", !/manifold/i.test(ldd) && (MAC ? /libSystem/ : /libc\.so/).test(ldd), ldd.replace(/\s+/g, " ").trim());
    check("ldd names no libstdc++ (static)", !/libstdc\+\+/.test(ldd));
    if (!MAC) { // FIXDOC: the hardened link (build.sh release: -z relro -z now)
      const dyn = execFileSync("readelf", ["-d", bin], { encoding: "utf8" });
      check("readelf -d: BIND_NOW (-z now)", /\(BIND_NOW\)|Flags:.*\bNOW\b/.test(dyn), dyn.split("\n").filter((l) => /FLAGS|BIND/.test(l)).join(" | "));
      const seg = execFileSync("readelf", ["-lW", bin], { encoding: "utf8" });
      check("readelf -l: a GNU_RELRO segment", /GNU_RELRO/.test(seg));
    }
  }

  // NUI1: the native app
  if (NATIVE) {
    const nb = join(root, "bimble");
    const ldd = execFileSync("ldd", [nb], { encoding: "utf8" });
    check("bimble: ldd names no libstdc++ nor libmanifold (static)", !/libstdc\+\+|manifold/.test(ldd), ldd.replace(/\s+/g, " ").trim());
    check("bimble: readelf -d: BIND_NOW", /\(BIND_NOW\)|Flags:.*\bNOW\b/.test(execFileSync("readelf", ["-d", nb], { encoding: "utf8" })));
    const nv = execFileSync(nb, ["--version"], { encoding: "utf8" }).trim();
    check("bimble --version prints ccore/VERSION", nv === `bimble ${VERSION} (native)`, nv);
    const fixture = join(tmp, "native-frame.json"), shot = join(tmp, "native-shot.png");
    cpSync(join(CCORE, "golden/frame.json"), fixture);
    let rc = 0, err = "";
    try {
      execFileSync(nb, ["--offscreen", "--quit-after", "3", "--config-dir", join(tmp, "native-conf"), "--shot", shot, fixture], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"], timeout: 60000 });
    } catch (e) { rc = e.status ?? -1, err = String(e.stderr ?? e.message); }
    if (rc === 3) console.log(`SKIP native app offscreen run: no GPU device here (${err.split("\n").filter(Boolean)[0] ?? ""})`);
    else {
      check("bimble --offscreen --quit-after 3 --shot exits 0", rc === 0, err.trim().split("\n").slice(-3).join(" | "));
      let img = null;
      try { img = readPng(shot); } catch (e) { check("bimble's shot is a PNG", false, String(e)); }
      const ink = img ? inkShare(img, Math.floor(img.width * 0.3), Math.floor(img.height * 0.35), Math.floor(img.width * 0.9), Math.floor(img.height * 0.9)) : 0;
      if (img) check(`bimble's shot draws the fixture (${(ink * 100).toFixed(1)} % of the 3D view off its ground, over 2 %)`, ink > 0.02, `${img.width}x${img.height}`);
    }
  } else if (!WIN && !MAC) console.log("SKIP native app: the archive has no bimble (SDL3 >= 3.4, FreeType or glslc absent where it was built)");

  // Untrusted web/ content added after unpacking: a link out of the tree and a hidden file.
  const webFiles = files(web);
  console.log(`size: archive ${statSync(ARCHIVE).size} bytes; unpacked bimble-ccore ${statSync(bin).size} bytes, web/ ${webFiles.reduce((n, f) => n + statSync(join(web, f)).size, 0)} bytes in ${webFiles.length} files`);
  if (WIN) { // WIN2: a file link (the runner is an administrator) and a junction, out of the tree
    const sys = process.env.SystemRoot || "C:\\Windows";
    symlinkSync(join(sys, "win.ini"), join(web, "passwd"), "file");
    symlinkSync(sys, join(web, "etc"), "junction");
  } else {
    symlinkSync("/etc/passwd", join(web, "passwd"));
    symlinkSync("/etc", join(web, "etc"));
  }
  writeFileSync(join(web, ".hidden"), "secret");

  // The run of a user: a model next to the archive, no repo, no token in the environment.
  const work = join(tmp, "work");
  mkdirSync(work);
  cpSync(join(CCORE, "golden/frame.json"), join(work, "frame.json"));
  const t0 = performance.now();
  host = start(bin, ["open", "--no-browser", "frame.json"], work);
  const url = await waitUrl(host);
  console.log(`open to URL printed: ${(performance.now() - t0).toFixed(1)} ms`);
  const m = /^http:\/\/127\.0\.0\.1:(\d+)\/\?native=1&gl=1&port=(\d+)&token=([0-9a-f]{32})$/.exec(url);
  check("open prints http://127.0.0.1:<port>/?native=1&gl=1&port=<port>&token=<t>", !!m && m[1] === m[2], `${url} | ${host.log.trim()}`);
  if (!m) throw new Error("no URL");
  const port = Number(m[1]), token = m[3];
  check("stderr names the files served", /serving \d+ files/.test(host.log), host.log.trim());
  // FS1: the desktop keeps the model given at start in Rust's settings file (here BIMBLE_CONFIG_DIR).
  const sj = join(tmp, "config", "bimble", "settings.json");
  const recent = existsSync(sj) ? JSON.parse(readFileSync(sj, "utf8")).recent : null;
  check("FS1: open adds the model to settings.json's recent files", Array.isArray(recent) && recent.length === 1 && recent[0].endsWith("/work/frame.json"), JSON.stringify(recent));

  const index = readFileSync(join(web, "index.html"));
  const root1 = await get(port, "/");
  check("GET / is web/index.html (200, text/html)", root1.status === 200 && root1.body.equals(index) && /^text\/html/.test(header(root1.head, "Content-Type") ?? ""), `${root1.status} ${header(root1.head, "Content-Type")}`);
  check("GET / sends nosniff", header(root1.head, "X-Content-Type-Options") === "nosniff");
  check("GET with the page's query is index.html", (await get(port, url.slice(url.indexOf("/", 7)))).body.equals(index));
  let same = 0, bad = [];
  for (const f of webFiles) {
    const r = await get(port, f);
    const ext = f.slice(f.lastIndexOf(".") + 1);
    const ct = header(r.head, "Content-Type") ?? "";
    if (r.status === 200 && r.body.equals(readFileSync(join(web, f))) && (!CTYPE[ext] || ct.startsWith(CTYPE[ext]))) same++;
    else bad.push(`${f} ${r.status} ${ct}`);
  }
  check(`every file of web/ (${webFiles.length}) byte for byte with its content type`, same === webFiles.length && same > 1, bad.join("; "));
  const assets = webFiles.filter((f) => f.startsWith("/assets/") && f.endsWith(".js"));
  check("index.html names only files that are served", [...index.toString().matchAll(/(?:src|href)="(\/[^"]+)"/g)].every((x) => webFiles.includes(x[1])));
  check("web/ has the lab's script", assets.length > 0);

  for (const t of ["/../etc/passwd", "/..%2f..%2fetc/passwd", "/%2e%2e/%2e%2e/etc/passwd", "/assets/../index.html", "//etc/passwd",
    "/passwd", "/etc/passwd", "/.hidden", "/web/index.html", "/nope.js", "/index.html/", "/assets", "/assets/"]) {
    const r = await get(port, t);
    check(`GET ${t} -> 404`, r.status === 404 && !r.body.toString().includes("root:"), `${r.status}`);
  }
  check("GET /../etc/passwd with the token -> 404", (await get(port, `/../etc/passwd?t=${token}`)).status === 404);
  check("wrong Host -> 403 (static too)", (await get(port, "/", { Host: `evil.example:${port}` })).status === 403);
  check("foreign Origin -> 403 (static too)", (await get(port, "/", { Origin: "http://evil.example" })).status === 403);

  // The API keeps its token.
  check("POST /query/state without the token -> 403", (await post(port, "/query/state")).status === 403);
  check("GET /change without the token -> 403", (await get(port, "/change?since=0")).status === 403);
  check("GET /mesh without the token -> 403", (await get(port, "/mesh")).status === 403);
  const q = await post(port, `/query/state?t=${token}`);
  let state = null;
  try { state = JSON.parse(q.body.toString()); } catch { /* checked below */ }
  check("a query answers: state has the model", q.status === 200 && state && state.file !== null, q.body.toString().slice(0, 200));
  const self = `http://127.0.0.1:${port}`;
  const qs = await post(port, `/query/state?t=${token}`, "", { Origin: self });
  check("a call from the page's own origin -> 200 with Access-Control-Allow-Origin", qs.status === 200 && header(qs.head, "Access-Control-Allow-Origin") === self, `${qs.status}`);
  check("Origin http://localhost:<port> -> 200", (await post(port, `/query/state?t=${token}`, "", { Origin: `http://localhost:${port}`, Host: `localhost:${port}` })).status === 200);
  check("GET /change with the token -> 200", (await get(port, `/change?since=0&t=${token}`)).status === 200);
  // SIZE1: the PDF export is Helvetica (no font shipped, none in BIMBLE_FONTS); Roboto not offered.
  let plan = null;
  try { plan = JSON.parse((await post(port, `/query/open_views?t=${token}`)).body.toString()).views.find((v) => v.auto_name?.kind === "plan"); } catch { /* checked below */ }
  const pdf = plan ? await post(port, `/query/view_pdf?t=${token}`, JSON.stringify({ view: plan.id })) : null;
  check("view_pdf of a plan: a Helvetica PDF, nothing embedded", pdf && pdf.status === 200 && pdf.body.subarray(0, 5).toString() === "%PDF-" &&
    pdf.body.subarray(-5).toString() === "%%EOF" && !pdf.body.includes("/FontFile2") && pdf.body.includes("/BaseFont /Helvetica"), pdf ? `${pdf.status} ${pdf.body.length} bytes` : "no plan view");
  const pf = await post(port, `/query/pdf_fonts?t=${token}`);
  check("pdf_fonts without Roboto files: roboto false", pf.status === 200 && pf.body.toString() === '{"helvetica":true,"roboto":false}', `${pf.status} ${pf.body}`);
  const mesh = await get(port, `/mesh?t=${token}`);
  check("GET /mesh with the token -> 200 BMSH", mesh.status === 200 && mesh.body.subarray(0, 4).toString() === "BMSH", `${mesh.status}`);

  // Read once at start: a file written now is not served.
  writeFileSync(join(web, "late.js"), "x");
  check("a file added after the start -> 404", (await get(port, "/late.js")).status === 404);

  // Keep-alive: two static GETs on one connection.
  const two = await raw(port, `GET /index.html HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\n\r\nGET / HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nConnection: close\r\n\r\n`);
  check("two GETs on one connection", (two.head + two.body.toString()).split("HTTP/1.1 200").length === 3);

  const ex = await stop(host);
  host = null;
  if (WIN) console.log("skip SIGTERM -> exits 0 (win32: no SIGTERM; node ends the process with TerminateProcess)");
  else check("SIGTERM -> exits 0", ex && ex.code === 0, JSON.stringify(ex));

  // SIZE1: Roboto from the user's folder (BIMBLE_FONTS), as a user who adds the files would.
  if (plan) {
    const fonts = join(tmp, "userfonts");
    mkdirSync(fonts);
    for (const f of ["Roboto-Regular.ttf", "Roboto-Bold.ttf"]) cpSync(join(CCORE, "fonts", f), join(fonts, f));
    host = start(bin, ["open", "--no-browser", "frame.json"], work, { BIMBLE_FONTS: fonts });
    const mu = /port=(\d+)&token=([0-9a-f]{32})$/.exec(await waitUrl(host));
    const pf2 = mu ? await post(Number(mu[1]), `/query/pdf_fonts?t=${mu[2]}`) : null;
    check("BIMBLE_FONTS with the files: pdf_fonts roboto true", pf2 && pf2.body.toString() === '{"helvetica":true,"roboto":true}', pf2 ? pf2.body.toString() : "no URL");
    const rp = mu ? await post(Number(mu[1]), `/query/view_pdf?t=${mu[2]}`, JSON.stringify({ view: plan.id, font: "roboto" })) : null;
    check('view_pdf {"font":"roboto"}: Roboto embedded', rp && rp.status === 200 && rp.body.includes("/FontFile2") && rp.body.includes("/BaseFont /Roboto"), rp ? `${rp.status} ${rp.body.length} bytes` : "");
    const hp = mu ? await post(Number(mu[1]), `/query/view_pdf?t=${mu[2]}`, JSON.stringify({ view: plan.id })) : null;
    check("view_pdf without font, the files there: still Helvetica", hp && hp.status === 200 && !hp.body.includes("/FontFile2"), hp ? `${hp.status}` : "");
    await stop(host);
    host = null;
  }

  // FIXDOC: no BIMBLE_FONTS: the fonts of the user's Bimble folder, the settings.rs spelling on
  // every system (macOS .../Application Support/Bimble, Windows %APPDATA%\Bimble, Linux
  // $XDG_CONFIG_HOME/bimble), under a profile whose name is not ASCII (Windows: read with
  // _wgetenv, not the ANSI code page).
  if (plan) {
    const prof = join(tmp, "Jérôme");
    const cfg = MAC ? join(prof, "Library", "Application Support", "Bimble") : WIN ? join(prof, "Bimble") : join(prof, "bimble");
    mkdirSync(join(cfg, "fonts"), { recursive: true });
    for (const f of ["Roboto-Regular.ttf", "Roboto-Bold.ttf"]) cpSync(join(CCORE, "fonts", f), join(cfg, "fonts", f));
    const env = { BIMBLE_FONTS: "", ...(MAC ? { HOME: prof } : WIN ? { APPDATA: prof } : { XDG_CONFIG_HOME: prof }) };
    host = start(bin, ["open", "--no-browser", "frame.json"], work, env);
    const mu = /port=(\d+)&token=([0-9a-f]{32})$/.exec(await waitUrl(host));
    const pf3 = mu ? await post(Number(mu[1]), `/query/pdf_fonts?t=${mu[2]}`) : null;
    check("no BIMBLE_FONTS: the fonts of <the user's Bimble folder>/fonts, a non-ASCII profile: roboto true", pf3 && pf3.body.toString() === '{"helvetica":true,"roboto":true}', pf3 ? pf3.body.toString() : "no URL");
    await stop(host);
    host = null;
  }

  // --port N, no model.
  const p2 = await freePort();
  host = start(bin, ["open", "--no-browser", "--port", String(p2)], work);
  const url2 = await waitUrl(host);
  check("--port N: the URL names N", url2.startsWith(`http://127.0.0.1:${p2}/?native=1&gl=1&port=${p2}&token=`), url2);
  check("--port N, no model: GET / -> 200", (await get(p2, "/")).status === 200);
  await stop(host);
  host = null;

  // The browser: xdg-open gets the URL, detached, with none of the host's sockets (a browser it
  // starts would keep the port). A stand-in records its argument and open files.
  const fake = join(tmp, "fakebin"), rec = join(tmp, "xdg-open.txt");
  if (WIN) console.log("skip xdg-open gets the printed URL, holds no socket (win32: ShellExecuteW, no stand-in on PATH)");
  else {
  mkdirSync(fake);
  // Its open files, one line each: /proc's link targets ("socket:[n]" for a socket), or lsof's
  // file types on macOS ("tIPv4" for a TCP socket), after the URL.
  const fds = MAC ? `/usr/sbin/lsof -a -p $$ -d 0-1023 -F t | grep '^t'` : `for f in /proc/$$/fd/*; do readlink "$f"; done`;
  writeFileSync(join(fake, OPENER), `#!/bin/sh\n{ printf '%s\\n' "$1"; ${fds}; } > "${rec}.tmp"\nmv "${rec}.tmp" "${rec}"\n`, { mode: 0o755 });
  host = start(bin, ["open", "frame.json"], work, { PATH: `${fake}:${process.env.PATH}` });
  const url3 = await waitUrl(host);
  for (let i = 0; i < 1000 && !existsSync(rec); i++) await sleep(5); // lsof (macOS) takes longer than /proc
  const got = existsSync(rec) ? readFileSync(rec, "utf8").split("\n").filter(Boolean) : [];
  check("xdg-open gets the printed URL", url3.startsWith("http://127.0.0.1:") && got[0] === url3, `${url3} | ${got[0]}`);
  check("xdg-open holds no socket of the host", got.length > 1 && !got.slice(1).some((l) => /^(socket:|tIPv[46]$|tunix$)/.test(l)), got.slice(1).join(" "));
  await stop(host);
  host = null;
  }

  // A web/ that cannot be served refuses to start (exit 1, a message).
  const noIndex = join(tmp, "noindex");
  mkdirSync(noIndex);
  writeFileSync(join(noIndex, "a.js"), "x");
  const big = join(tmp, "big");
  mkdirSync(big);
  writeFileSync(join(big, "index.html"), "<!doctype html>");
  writeFileSync(join(big, "huge.bin"), "");
  truncateSync(join(big, "huge.bin"), 65 * 1024 * 1024); // sparse: no disk used
  for (const [what, dir, re] of [["no index.html", noIndex, /no index\.html/], ["over 64 MiB", big, /bytes in all/], ["missing folder", join(tmp, "none"), /No such file/]]) {
    const st = start(bin, ["open", "--no-browser", "--web", dir, "--port", "0"], work);
    const e = await Promise.race([st.exited, sleep(5000).then(() => null)]);
    if (!e) st.proc.kill("SIGKILL");
    check(`--web with ${what} -> exit 1 with a message`, e && e.code === 1 && re.test(st.log) && st.out === "", `${JSON.stringify(e)} ${st.log.trim()}`);
  }
  // The usage errors.
  const u = start(bin, ["open", "--bogus"], work);
  const ue = await Promise.race([u.exited, sleep(3000).then(() => null)]);
  check("open --bogus -> exit 2", ue && ue.code === 2, JSON.stringify(ue));
} catch (e) {
  check("no exception", false, e?.stack ?? String(e));
} finally {
  if (host) host.proc.kill("SIGKILL");
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
