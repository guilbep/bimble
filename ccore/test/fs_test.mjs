// The file dialog's queries and the recent files of the native host (ticket FS1): node ccore/test/fs_test.mjs
// Starts out/tcc/bimble-ccore (or $HOST_BIN) with HOME and BIMBLE_CONFIG_DIR in a temp folder
// (never the user's), one --root, and checks over HTTP:
// - fs_roots and fs_home; fs_list of the home root (names sorted, hidden left out and counted,
//   sizes, kinds, a 255-byte name, a non-UTF-8 name skipped and counted, `ext` filtering, parent);
// - the path rule (host/fs.h): a relative path, a path with NUL, ".." out of a root, an absolute
//   path outside the roots and a symbolic link that leaves them are refused, each with one text;
//   a link inside the roots is listed as a link with its target, one outside as "outside";
//   the text is checked before the file system (FIXDOC: a missing folder outside is "outside",
//   ".." resolved lexically; Windows network and device paths refused, POSIX "//" is "/");
//   an unreadable folder is refused (not as root); a missing token is 403;
// - 10,000 entries: 5,000 listed, "truncated": true;
// - recent files: a settings.json in Rust's schema (settings.rs to_json, a hand-written fixture:
//   no Rust build here) keeps every other field when an open adds the file first; the list is
//   12 at most, newest first, once; a save_as adds the new path; recent_remove drops one; a
//   missing settings.json is created with "recent" only; a file that is not JSON is left alone;
//   a restarted host reads the list back; the wasm-only "no folder" answer is not here (fs_* are
//   the native host's, wasm_run.mjs checks the module's answer);
// - POST /stage (a file dropped on the page): the token, a plain name only, binary bytes kept
//   exactly, 3 MB accepted (over the 1 MiB of a call), over FS_STAGE_MAX 413 and other POSTs still
//   413 over 1 MiB; `open {path, dropped: true}` opens it under its name, Save asks where (as a new
//   model's), no recent file.
// Prints "N passed, M failed"; exits non-zero on a failure. Kills what it starts.
import { spawn } from "node:child_process";
import net from "node:net";
import { chmodSync, copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { port } from "./ports.mjs";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const G = join(CCORE, "golden");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const TOKEN = "fs1-test-token";
const PORT = port("fs_test"), PORT2 = port("fs_test2");
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const tmp = mkdtempSync(join(tmpdir(), "ccore-fs1-"));
const home = join(tmp, "home"), extra = join(tmp, "extra"), outside = join(tmp, "outside"), conf = join(tmp, "config");
for (const d of [home, extra, outside]) mkdirSync(d);
const hosts = [];

async function startHost(p, flags = [], env = {}) {
  const h = spawn(BIN, [...flags, String(p)], {
    env: { ...process.env, HOME: home, USERPROFILE: home, BIMBLE_CONFIG_DIR: conf, BIMBLE_TOKEN: TOKEN, CCORE_SEED: "1", ...env },
    stdio: ["ignore", "ignore", "pipe"],
  });
  h.log = "";
  h.stderr.on("data", (d) => (h.log += d));
  hosts.push(h);
  for (let i = 0; i < 400 && !h.log.includes("listening"); i++) {
    if (h.exitCode !== null) break;
    await sleep(10);
  }
  if (!h.log.includes("listening")) throw new Error(`host on ${p} did not start: ${h.log}`);
  const call = async (op, name = "-", body = "", t = TOKEN) => {
    const r = await fetch(`http://127.0.0.1:${p}/${op}/${encodeURIComponent(name)}${t === null ? "" : `?t=${t}`}`,
      { method: "POST", body, headers: { "Content-Type": "text/plain" } });
    return { status: r.status, text: await r.text() };
  };
  const q = async (name, args = {}) => {
    const r = await call("query", name, JSON.stringify(args));
    if (r.status !== 200) throw new Error(`query ${name}: ${r.status} ${r.text}`);
    return JSON.parse(r.text);
  };
  const ls = (dir, ext) => call("query", "fs_list", JSON.stringify(ext ? { dir, ext } : { dir }));
  const act = (name, body) => call("action", name, body === undefined ? "" : JSON.stringify(body));
  const stop = async () => {
    h.kill("SIGTERM");
    for (let i = 0; i < 200 && h.exitCode === null && h.signalCode === null; i++) await sleep(10);
  };
  return { h, call, q, ls, act, stop };
}

const statMode = (p) => statSync(p).mode;
/** A POST with a Content-Length and no body: the status line the host answers. */
const rawPost = (p, path, len) => new Promise((res) => {
  const sock = net.connect(p, "127.0.0.1", () => sock.write(`POST ${path} HTTP/1.1\r\nHost: 127.0.0.1:${p}\r\nContent-Type: text/plain\r\nContent-Length: ${len}\r\n\r\n`));
  let got = "";
  sock.on("data", (d) => { got += d; sock.end(); });
  sock.on("close", () => res(got));
  sock.on("error", () => res(got));
  setTimeout(() => sock.destroy(), 3000);
});
const OUTSIDE = "fs_list: outside the folders Bimble lists (query fs_roots)";
const NETWORK = "fs_list: dir: network and device paths are not listed (\\\\server, \\\\.\\, \\\\?\\)"; // host/fs.h FS_NETWORK

try {
  // The tree: files, a folder, a hidden file, a 255-byte name, a non-UTF-8 name (Linux only),
  // a link inside the roots and one leaving them, an unreadable folder, a big folder.
  writeFileSync(join(home, "b.ifc"), "ISO-10303-21;");
  copyFileSync(join(G, "frame.json"), join(home, "a.bimble.json"));
  copyFileSync(join(G, "frame.json"), join(home, "frame.json"));
  writeFileSync(join(home, "C.PDF"), "%PDF");
  writeFileSync(join(home, ".hidden"), "x");
  mkdirSync(join(home, "sub"));
  writeFileSync(join(home, "sub", "in.dxf"), "0\nEOF\n");
  const long = "L".repeat(251) + ".ifc"; // 255 bytes: the longest name Linux allows
  writeFileSync(join(home, long), "x");
  let nonUtf8 = false;
  if (process.platform === "linux") {
    try {
      writeFileSync(Buffer.concat([Buffer.from(home + "/bad-"), Buffer.from([0xff, 0xfe]), Buffer.from(".ifc")]), "x");
      nonUtf8 = true;
    } catch { /* a file system that refuses it */ }
  }
  writeFileSync(join(outside, "secret.ifc"), "x");
  symlinkSync(join(home, "sub"), join(home, "inlink"));
  symlinkSync(outside, join(home, "outlink"));
  symlinkSync(join(outside, "secret.ifc"), join(home, "outfile.ifc"));
  mkdirSync(join(home, "locked"));
  chmodSync(join(home, "locked"), 0o000);
  const big = join(extra, "big");
  mkdirSync(big);
  for (let i = 0; i < 10000; i++) writeFileSync(join(big, `f${String(i).padStart(5, "0")}.ifc`), "");

  const H = await startHost(PORT, ["--root", extra]);
  const roots = await H.q("fs_roots");
  check("fs_roots: home first, then --root", roots.length === 2 && roots[0].kind === "home" && roots[0].path === home && roots[1].kind === "root" && roots[1].path === extra, JSON.stringify(roots));
  const fh = await H.q("fs_home");
  check("fs_home: HOME", fh.path === home, JSON.stringify(fh));

  const r0 = await H.ls(home);
  const l0 = JSON.parse(r0.text);
  const names = l0.entries.map((e) => e.name);
  check("fs_list home -> 200", r0.status === 200, r0.text.slice(0, 200));
  check("fs_list: sorted by name, hidden left out", JSON.stringify(names) === JSON.stringify([...names].sort((a, b) => Buffer.compare(Buffer.from(a), Buffer.from(b)))) && !names.includes(".hidden") && l0.hidden === 1, JSON.stringify(names));
  const by = Object.fromEntries(l0.entries.map((e) => [e.name, e]));
  check("fs_list: a file's kind and size", by["b.ifc"]?.kind === "file" && by["b.ifc"].size === 13 && by["b.ifc"].mtime > 1e9, JSON.stringify(by["b.ifc"]));
  check("fs_list: a folder", by.sub?.kind === "dir", JSON.stringify(by.sub));
  check("fs_list: the 255-byte name", by[long]?.kind === "file");
  check("fs_list: a link inside the roots is a link to a dir", by.inlink?.kind === "link" && by.inlink.target === "dir" && !by.inlink.outside, JSON.stringify(by.inlink));
  check("fs_list: a link leaving the roots: outside, no target", by.outlink?.kind === "link" && by.outlink.target === null && by.outlink.outside === true, JSON.stringify(by.outlink));
  check("fs_list: a file link leaving the roots: outside", by["outfile.ifc"]?.outside === true && by["outfile.ifc"].target === null, JSON.stringify(by["outfile.ifc"]));
  if (nonUtf8) check("fs_list: a non-UTF-8 name skipped and counted", l0.skipped === 1 && !names.some((n) => n.startsWith("bad-")), `skipped ${l0.skipped}`);
  check("fs_list: names and metadata only (no bytes)", !r0.text.includes("ISO-10303") && !r0.text.includes("%PDF"));
  check("fs_list home: the parent is not listed (outside)", l0.parent === null && l0.dir === home && l0.truncated === false, JSON.stringify({ parent: l0.parent, dir: l0.dir }));
  const l1 = JSON.parse((await H.ls(home, [".ifc", ".pdf"])).text);
  const n1 = l1.entries.map((e) => e.name);
  check("fs_list ext [.ifc, .pdf]: those files (any case) and the folders", n1.includes("b.ifc") && n1.includes("C.PDF") && n1.includes("sub") && !n1.includes("frame.json") && n1.includes(long) && n1.includes("inlink"), JSON.stringify(n1));
  const ls2 = JSON.parse((await H.ls(join(home, "sub"))).text);
  check("fs_list sub: in.dxf, parent = home", ls2.entries.length === 1 && ls2.entries[0].name === "in.dxf" && ls2.parent === home, JSON.stringify(ls2));
  const ls3 = await H.ls(join(home, "inlink"));
  check("fs_list of a link inside: resolved, listed", ls3.status === 200 && JSON.parse(ls3.text).dir === join(home, "sub"), ls3.text);

  // The rule: each refusal with one text.
  const rel = await H.ls("home/sub");
  check("fs_list relative -> 400", rel.status === 400 && rel.text === "fs_list: dir: an absolute path, without NUL, at most 4096 bytes", rel.text);
  const nul = await H.call("query", "fs_list", JSON.stringify({ dir: home + "\u0000/sub" }));
  check("fs_list with a NUL -> 400", nul.status === 400 && nul.text.startsWith("fs_list: dir: an absolute path"), nul.text);
  const up = await H.ls(join(home, "..", "outside"));
  check("fs_list .. out of a root -> 400 outside", up.status === 400 && up.text === OUTSIDE, up.text);
  const up2 = await H.ls(home + "/..");
  check("fs_list home/.. -> 400 outside", up2.status === 400 && up2.text === OUTSIDE, up2.text);
  const abs = await H.ls(outside);
  check("fs_list an absolute path outside -> 400 outside", abs.status === 400 && abs.text === OUTSIDE, abs.text);
  const etc = await H.ls("/");
  check("fs_list / -> 400 outside", etc.status === 400 && etc.text === OUTSIDE, etc.text);
  const lk = await H.ls(join(home, "outlink"));
  check("fs_list a link that leaves the roots -> 400 outside", lk.status === 400 && lk.text === OUTSIDE, lk.text);
  // FIXDOC: the lexical rule (host/fs_rule.h) comes before the file system: a missing folder
  // outside the roots gets the outside text, not "no such folder" (the old order said whether it
  // existed); ".." in the text is resolved before the check; "//" is not a network path on POSIX.
  const goneOut = await H.ls(join(outside, "nothing", "deeper"));
  check("fs_list a missing folder outside -> 400 outside (checked before the file system)", goneOut.status === 400 && goneOut.text === OUTSIDE, goneOut.text);
  const dots = await H.ls(home + "/sub/../../outside");
  check("fs_list home/sub/../../outside (text not normalised by the client) -> 400 outside", dots.status === 400 && dots.text === OUTSIDE, dots.text);
  const dotsIn = await H.ls(home + "/sub/./../sub");
  check("fs_list home/sub/./../sub -> 200, resolved", dotsIn.status === 200 && JSON.parse(dotsIn.text).dir === join(home, "sub"), dotsIn.text);
  const above = await H.ls(process.platform === "win32" ? home.slice(0, 3) + ".." : "/../..");
  check("fs_list .. above the file system's root -> 400 absolute path", above.status === 400 && above.text.startsWith("fs_list: dir: an absolute path"), above.text);
  if (process.platform !== "win32") {
    const two = await H.ls("/" + outside);
    check("fs_list //<outside> (POSIX: not a network path) -> 400 outside", two.status === 400 && two.text === OUTSIDE, two.text);
    const twoIn = await H.ls("/" + home);
    check("fs_list //<home> (POSIX: the same folder) -> 200", twoIn.status === 200 && JSON.parse(twoIn.text).dir === home, twoIn.text);
  } else {
    const unc = await H.ls("\\\\127.0.0.1\\c$");
    check("fs_list \\\\server\\share -> 400 network (never opened)", unc.status === 400 && unc.text === NETWORK, unc.text);
    const dev = await H.ls("\\\\.\\PhysicalDrive0");
    check("fs_list \\\\.\\device -> 400 network", dev.status === 400 && dev.text === NETWORK, dev.text);
  }
  const gone = await H.ls(join(home, "nothing"));
  check("fs_list a missing folder -> 400", gone.status === 400 && gone.text === "fs_list: no such folder", gone.text);
  const file = await H.ls(join(home, "b.ifc"));
  check("fs_list a file -> 400 not a folder", file.status === 400 && file.text === "fs_list: not a folder", file.text);
  if (process.getuid?.() !== 0) {
    const lock = await H.ls(join(home, "locked"));
    check("fs_list an unreadable folder -> 400", lock.status === 400 && lock.text === "fs_list: the folder cannot be read", lock.text);
  }
  const notok = await H.call("query", "fs_list", JSON.stringify({ dir: home }), null);
  check("fs_list without the token -> 403", notok.status === 403, `${notok.status}`);
  const bad = await H.call("query", "fs_list", JSON.stringify({ dir: home }), "wrong");
  check("fs_list with a wrong token -> 403", bad.status === 403, `${bad.status}`);
  const badext = await H.call("query", "fs_list", JSON.stringify({ dir: home, ext: ".ifc" }));
  check("fs_list ext not an array -> 400", badext.status === 400 && badext.text.startsWith("fs_list: ext:"), badext.text);

  // The cap.
  const t0 = performance.now();
  const bg = JSON.parse((await H.ls(big)).text);
  const ms = performance.now() - t0;
  check("fs_list 10,000 entries: 5,000 listed, truncated", bg.entries.length === 5000 && bg.truncated === true && bg.entries[0].name === "f00000.ifc" && bg.entries[4999].name === "f04999.ifc", `${bg.entries.length} ${bg.truncated}`);
  console.log(`fs_list of 10,000 entries: ${ms.toFixed(1)} ms`);

  // ---- POST /stage: a dropped file ----
  const stage = (name, body, t = TOKEN) => fetch(`http://127.0.0.1:${PORT}/stage?name=${encodeURIComponent(name)}${t === null ? "" : `&t=${t}`}`,
    { method: "POST", body, headers: { "Content-Type": "text/plain" } }).then(async (r) => ({ status: r.status, text: await r.text() }));
  const bin = Buffer.alloc(3 << 20);
  for (let i = 0; i < bin.length; i++) bin[i] = (i * 2654435761) >>> 24;
  check("stage without the token -> 403", (await stage("a.bimble", "x", null)).status === 403);
  // FIXSES: DOS device names (on every system: the name would open a device on Windows).
  for (const bad of ["a/b.ifc", "../x.ifc", ".hidden", "", "a\\b.ifc", "x".repeat(256), "CON", "nul.ifc", "Com1 .bimble", "aux..", "CONOUT$"]) {
    const r = await stage(bad, "x");
    check(`stage name ${JSON.stringify(bad.slice(0, 20))} -> 400`, r.status === 400 && r.text.startsWith("stage: name"), `${r.status} ${r.text}`);
  }
  const sg = await stage("big drop.bin", bin);
  const sp = sg.status === 200 ? JSON.parse(sg.text).path : "";
  check("stage 3 MB of binary -> 200 {path}, the bytes exact", sg.status === 200 && existsSync(sp) && Buffer.compare(readFileSync(sp), bin) === 0, sg.text);
  check("stage: the host's own folder, 0700, not under the roots", sp && (statMode(dirname(sp)) & 0o777) === 0o700 && !sp.startsWith(home), sp);
  const huge = await rawPost(PORT, `/stage?name=x.ifc&t=${TOKEN}`, (256 << 20) + 1);
  check("stage over 256 MB -> 413", huge.startsWith("HTTP/1.1 413"), huge.split("\r\n")[0]);
  const bigq = await rawPost(PORT, `/query/state?t=${TOKEN}`, (1 << 20) + 1);
  check("a call over 1 MiB is still 413", bigq.startsWith("HTTP/1.1 413"), bigq.split("\r\n")[0]);
  const sf = await stage("dropped.json", readFileSync(join(G, "frame.json")));
  const dp = JSON.parse(sf.text).path;
  const od = await H.act("open", { path: dp, dropped: true });
  const sd = await H.q("state");
  check("open {path, dropped}: the dropped name, editable", od.status === 200 && sd.file === "dropped.json" && sd.editable, `${od.text} ${JSON.stringify(sd)}`);
  await H.call("verb", "place", JSON.stringify({ a: [0, 0], kind: "column", storey: (await H.q("storeys"))[0].id }));
  const svd = await H.act("save");
  check("a dropped model: Save asks where (no file of its own)", svd.status === 400 && svd.text.startsWith("a new model has no file yet"), svd.text);
  check("a dropped model is no recent file", !(await H.q("recent_files")).some((f) => f.path === dp));
  await H.act("close", { discard: true });

  // ---- Recent files ----
  check("recent_files: none yet (no settings.json)", JSON.stringify(await H.q("recent_files")) === "[]");
  await H.stop();
  check("stage: the host removes its folder when it stops", !existsSync(dirname(sp)), dirname(sp));
  // A settings.json as Rust's to_json writes it (serde_json, keys sorted): a hand-written fixture.
  mkdirSync(conf);
  const RUST = '{"coordinates":"survey","gpu_cull":false,"memory_share":0.8,"moving_fps":30,"moving_proxies":true,"recent":["/m/old.ifc"],"text_size":14.5,"theme":"bimble-light","ui_font":"roboto","user_email":"","user_name":"Ana \\"A\\" é"}';
  writeFileSync(join(conf, "settings.json"), RUST);
  const H2 = await startHost(PORT2, []);
  const rf0 = await H2.q("recent_files");
  check("recent_files: Rust's list read", rf0.length === 1 && rf0[0].path === "/m/old.ifc" && rf0[0].exists === false && rf0[0].name === "old.ifc" && rf0[0].folder === "/m" && rf0[0].kind === "ifc" && rf0[0].mtime === null && rf0[0].opened_unix === null, JSON.stringify(rf0));
  const fj = join(home, "frame.json");
  const op = await H2.act("open", { path: fj });
  const after = readFileSync(join(conf, "settings.json"), "utf8");
  check("open adds the file first in settings.json", op.status === 200 && JSON.parse(after).recent.join() === [fj, "/m/old.ifc"].join(), after);
  check("settings.json: every other field kept byte for byte", after === RUST.replace('"recent":["/m/old.ifc"]', `"recent":[${JSON.stringify(fj)},"/m/old.ifc"]`), after);
  const rf1 = await H2.q("recent_files");
  check("recent_files: frame.json first, exists, mtime", rf1[0].path === fj && rf1[0].exists && rf1[0].kind === "json" && rf1[0].mtime > 1e9, JSON.stringify(rf1[0]));
  const x = join(home, "x.bimble");
  await H2.act("save_as", { path: x });
  const rf2 = await H2.q("recent_files");
  check("save_as adds the new file first", rf2.map((f) => f.path).join() === [x, fj, "/m/old.ifc"].join() && rf2[0].schema === ".bimble" && rf2[0].kind === "bimble", JSON.stringify(rf2.map((f) => f.path)));
  await H2.act("open", { path: fj });
  check("open again: moved first, once", (await H2.q("recent_files")).map((f) => f.path).join() === [fj, x, "/m/old.ifc"].join());
  for (let i = 0; i < 14; i++) {
    const p = join(home, `m${i}.json`);
    copyFileSync(join(G, "frame.json"), p);
    await H2.act("open", { path: p, discard: true });
  }
  const rf3 = await H2.q("recent_files");
  check("12 at most, newest first (settings.rs RECENT)", rf3.length === 12 && rf3[0].path === join(home, "m13.json") && rf3[11].path === join(home, "m2.json"), JSON.stringify(rf3.map((f) => f.name)));
  const rm = await H2.act("recent_remove", { path: join(home, "m13.json") });
  const rf4 = await H2.q("recent_files");
  check("recent_remove drops one", rm.status === 200 && rf4.length === 11 && rf4[0].path === join(home, "m12.json"), rm.text);
  const rm2 = await H2.act("recent_remove", { path: "/not/there" });
  check("recent_remove of a path not in the list -> 400", rm2.status === 400, rm2.text);
  const kept = JSON.parse(readFileSync(join(conf, "settings.json"), "utf8"));
  check("settings.json: other fields still there after many writes", kept.user_name === 'Ana "A" é' && kept.text_size === 14.5 && kept.recent.length === 11, JSON.stringify(kept));
  // The Rust app changes the file meanwhile: the next open reads it first.
  writeFileSync(join(conf, "settings.json"), JSON.stringify({ ...kept, recent: ["/rust/opened.bimble"], theme: "dark" }));
  await H2.act("open", { path: fj, discard: true });
  const merged = JSON.parse(readFileSync(join(conf, "settings.json"), "utf8"));
  check("the list is read again before a write (the Rust app's change kept)", merged.recent.join() === [fj, "/rust/opened.bimble"].join() && merged.theme === "dark", JSON.stringify(merged));
  await H2.stop();
  // A restarted host reads it back.
  const H3 = await startHost(PORT2, []);
  check("restart: the list read back", (await H3.q("recent_files")).map((f) => f.path).join() === [fj, "/rust/opened.bimble"].join());
  // Not JSON: left alone, the list in memory.
  writeFileSync(join(conf, "settings.json"), "not json {");
  await H3.act("open", { path: x, discard: true });
  check("a settings.json that is not JSON is left alone", readFileSync(join(conf, "settings.json"), "utf8") === "not json {");
  check("... and the list lives in memory", (await H3.q("recent_files"))[0]?.path === x);
  check("... with a log line", H3.h.log.includes("not a JSON object"), H3.h.log.slice(-300));
  await H3.stop();
  // Missing: created with "recent" only (and its folder).
  rmSync(conf, { recursive: true });
  const H4 = await startHost(PORT2, []);
  await H4.act("open", { path: fj });
  const fresh = existsSync(join(conf, "settings.json")) ? readFileSync(join(conf, "settings.json"), "utf8") : "(missing)";
  check("a missing settings.json is created with recent only", fresh === `{"recent":[${JSON.stringify(fj)}]}`, fresh);
  await H4.stop();
  // No config folder at all (the dev host, the other tests): in memory, nothing written.
  rmSync(conf, { recursive: true });
  const H5 = await startHost(PORT2, [], { BIMBLE_CONFIG_DIR: "" });
  await H5.act("open", { path: fj });
  check("no BIMBLE_CONFIG_DIR (plain host): the list in memory, no file written", (await H5.q("recent_files"))[0]?.path === fj && !existsSync(conf) && !existsSync(join(home, ".config")));
  await H5.stop();
  // --config-dir wins over the variable.
  const conf2 = join(tmp, "config2");
  const H6 = await startHost(PORT2, ["--config-dir", conf2]);
  await H6.act("open", { path: fj });
  check("--config-dir: the settings file there", existsSync(join(conf2, "settings.json")) && !existsSync(conf));
  await H6.stop();
} catch (e) {
  check("no exception", false, e.stack ?? String(e));
} finally {
  for (const h of hosts) if (h.exitCode === null) h.kill("SIGKILL");
  try { chmodSync(join(home, "locked"), 0o700); } catch { /* gone */ }
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
