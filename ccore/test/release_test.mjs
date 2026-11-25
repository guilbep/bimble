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
//   - `--port N` serves on N; a web/ without index.html or over 64 MiB refuses to start;
//   - without --no-browser, xdg-open (a stand-in first on PATH) gets the URL and no socket.
// The ports: the system's free ones (open picks one; --port gets one from node), not test/ports.mjs.
// Prints "N passed, M failed"; exits non-zero on a failure. Needs tar and ldd.
// macOS (ticket MAC1): bimble-ccore-<VERSION>-<arch>-macos.tar.gz (RELEASE_ARCH=x86_64: the Intel
// one on an arm64 Mac, run by Rosetta 2), otool -L for ldd (libc++ is the system's), the stand-in
// is named `open`, and it lists its files with lsof (no /proc).
import { execFileSync, spawn } from "node:child_process";
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, symlinkSync, truncateSync, writeFileSync } from "node:fs";
import net from "node:net";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const VERSION = readFileSync(join(CCORE, "VERSION"), "utf8").trim();
const MAC = process.platform === "darwin";
const ARCH = process.env.RELEASE_ARCH || execFileSync("uname", ["-m"], { encoding: "utf8" }).trim();
const NAME = `bimble-ccore-${VERSION}-${ARCH}-${MAC ? "macos" : "linux"}`;
const OPENER = MAC ? "open" : "xdg-open";
const ARCHIVE = process.env.RELEASE_ARCHIVE ?? join(CCORE, "out/release", `${NAME}.tar.gz`);

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
  const proc = spawn(bin, args, { cwd, env: { ...process.env, BIMBLE_TOKEN: "", ...env }, stdio: ["ignore", "pipe", "pipe"] });
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
const CTYPE = { html: "text/html", js: "text/javascript", css: "text/css", woff2: "font/woff2", json: "application/json", svg: "image/svg+xml", png: "image/png", wasm: "application/wasm" };

const tmp = mkdtempSync(join(tmpdir(), "ccore-release-"));
let host = null;
try {
  check(`archive ${NAME}.tar.gz exists`, existsSync(ARCHIVE), ARCHIVE);
  console.log(`archive bytes: ${statSync(ARCHIVE).size}`);
  const list = execFileSync("tar", ["tzf", ARCHIVE], { encoding: "utf8" }).split("\n").filter(Boolean);
  const top = new Set(list.map((p) => p.split("/")[0]));
  check("one top folder named after the version and arch", top.size === 1 && top.has(NAME), [...top].join(" "));
  for (const f of ["bimble-ccore", "web/index.html", "LICENSE", "README.txt"])
    check(`archive holds ${f}`, list.includes(`${NAME}/${f}`));
  const rest = list.filter((p) => !/^[^/]+\/(bimble-ccore|LICENSE|README\.txt|web\/.*)?$/.test(p));
  check("nothing else in the archive", rest.length === 0, rest.join(" "));

  execFileSync("tar", ["xzf", ARCHIVE, "-C", tmp]);
  const root = join(tmp, NAME), bin = join(root, "bimble-ccore"), web = join(root, "web");
  const readme = readFileSync(join(root, "README.txt"), "utf8").trimEnd().split("\n");
  check("README.txt has at most ten lines", readme.length <= 10, `${readme.length} lines`);
  const ver = execFileSync(bin, ["--version"], { encoding: "utf8" }).trim();
  check("--version prints ccore/VERSION", ver === `bimble-ccore ${VERSION}`, ver);
  const ldd = execFileSync(MAC ? "otool" : "ldd", MAC ? ["-L", bin] : [bin], { encoding: "utf8" });
  check("ldd names no libmanifold", !/manifold/i.test(ldd) && (MAC ? /libSystem/ : /libc\.so/).test(ldd), ldd.replace(/\s+/g, " ").trim());
  check("ldd names no libstdc++ (static)", !/libstdc\+\+/.test(ldd));

  // Untrusted web/ content added after unpacking: a link out of the tree and a hidden file.
  const webFiles = files(web);
  symlinkSync("/etc/passwd", join(web, "passwd"));
  symlinkSync("/etc", join(web, "etc"));
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
  check("SIGTERM -> exits 0", ex && ex.code === 0, JSON.stringify(ex));

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
