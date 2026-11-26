// The MCP server over stdio (ticket M1, ADR 0068 rule 5): node ccore/test/mcp_test.mjs
// Runs out/tcc/bimble-ccore mcp (or $HOST_BIN mcp) on a temporary copy of golden/frame.json named
// model.json, CCORE_SEED=1, and checks:
// - Rust's transcript: golden/mcp/requests.jsonl in, every answer line = golden/mcp/responses.jsonl
//   (new GlobalIds and the server version masked, golden/README.md "mcp/"), and the file `save`
//   wrote = golden/mcp/saved.json byte for byte;
// - the C additions' transcript (rename, dry_run, if_rev): additions_requests.jsonl ->
//   additions_responses.jsonl, the C core's own golden (golden/DEVIATIONS.md "MCP");
// - --read-only: queries and dry runs answer, run_verb, undo, redo and save are refused, the file
//   keeps its bytes;
// - a last line without its newline is answered; a model that does not load exits 1.
// Request lines starting with "#" are made here, not sent as written (golden/README.md "mcp/"):
//   #bytes N        N bytes of "x" (a line over the 1 MiB limit when N > 1048576)
//   #ping ID N      a ping with id ID padded to exactly N bytes
// Prints "N passed, M failed"; exits non-zero on a failure.
//   node test/mcp_test.mjs --generate <transcript> <server> [args…]: runs `<server> [args…] mcp
//   model.json` on that transcript ("rust" or "additions") and writes its masked answers (and, for
//   "rust", saved.json) into golden/mcp/. tools/mcp_golden.sh runs it with Rust's `bimble`.
import { spawnSync } from "node:child_process";
import { copyFileSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { hostEnv } from "./lib/host_env.mjs"; // FIXSES: never the caller's BIMBLE_CONFIG_DIR

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const GOLDEN = join(CCORE, "golden");
const DIR = join(GOLDEN, "mcp");
const BIN = process.env.HOST_BIN ?? join(CCORE, "out/tcc/bimble-ccore");
const TRANSCRIPTS = { rust: "", additions: "additions_" };

let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};

// The request lines to send, the "#" lines made.
function expand(text) {
  const lines = text.split("\n");
  if (lines.at(-1) === "") lines.pop();
  return lines.map((l) => {
    let m = /^#bytes (\d+)$/.exec(l);
    if (m) return "x".repeat(Number(m[1]));
    m = /^#ping (\d+) (\d+)$/.exec(l);
    if (m) {
      const head = `{"jsonrpc":"2.0","id":${m[1]},"method":"ping","pad":"`;
      return head + "x".repeat(Number(m[2]) - head.length - 2) + '"}';
    }
    if (l.startsWith("#")) throw new Error(`unknown request marker ${l}`);
    return l;
  });
}

// golden/README.md "Masking of new GlobalIds": a token of the IFC alphabet that is a GlobalId and
// not an id the frame already gives becomes <new-N>, N by first appearance (one numbering per
// transcript); the all-zero id stays. The ids the frame gives: every GlobalId token of frame.json
// and of query.txt (unmasked: the project's derived building and site ids are there). The
// server's version in initialize becomes <version>.
const isGid = (t) => t.length === 22 && "0123".includes(t[0]);
const known = new Set(["frame.json", "query.txt"].flatMap((f) =>
  (readFileSync(join(GOLDEN, f), "utf8").match(/[0-9A-Za-z_$]+/g) ?? []).filter(isGid)));
function masker() {
  const seen = new Map();
  return (line) => line
    .replace(/[0-9A-Za-z_$]+/g, (t) => {
      if (!isGid(t) || known.has(t) || t === "0".repeat(22)) return t;
      if (!seen.has(t)) seen.set(t, `<new-${seen.size + 1}>`);
      return seen.get(t);
    })
    .replace(/("serverInfo":\{"name":"bimble","version":)"[^"]*"/, '$1"<version>"');
}

// Runs `cmd args… mcp [flags…] <model>` with `input`; {status, out: [lines], err}.
function serve(cmd, args, model, input, flags = []) {
  const r = spawnSync(cmd, [...args, "mcp", ...flags, model], {
    input, env: hostEnv({ CCORE_SEED: "1" }), maxBuffer: 64 << 20, timeout: 60000,
  });
  const out = r.stdout ? r.stdout.toString("utf8") : "";
  const lines = out.split("\n");
  if (lines.at(-1) === "") lines.pop();
  return { status: r.status, out: lines, err: r.stderr ? r.stderr.toString("utf8") : "", raw: out };
}

const tmp = mkdtempSync(join(tmpdir(), "ccore-mcp-"));
const fresh = () => {
  const model = join(tmp, "model.json");
  copyFileSync(join(GOLDEN, "frame.json"), model);
  return model;
};

// One transcript: {answers (masked), saved (the model file after), status, err}.
function transcript(name, cmd, args) {
  const model = fresh();
  const req = expand(readFileSync(join(DIR, `${TRANSCRIPTS[name]}requests.jsonl`), "utf8"));
  const r = serve(cmd, args, model, req.join("\n") + "\n");
  const mask = masker();
  return { answers: r.out.map(mask), saved: readFileSync(model, "utf8"), status: r.status, err: r.err };
}

if (process.argv[2] === "--generate") {
  const [name, cmd, ...args] = process.argv.slice(3);
  if (!(name in TRANSCRIPTS) || !cmd) {
    console.error("usage: mcp_test.mjs --generate rust|additions <server> [args…]");
    process.exit(2);
  }
  const t = transcript(name, cmd, args);
  if (t.status !== 0) {
    console.error(`server exited ${t.status}: ${t.err}`);
    process.exit(1);
  }
  writeFileSync(join(DIR, `${TRANSCRIPTS[name]}responses.jsonl`), t.answers.join("\n") + "\n");
  if (name === "rust") writeFileSync(join(DIR, "saved.json"), t.saved);
  console.log(`${name}: ${t.answers.length} answers written`);
  rmSync(tmp, { recursive: true, force: true });
  process.exit(0);
}

function compare(name, t) {
  const want = readFileSync(join(DIR, `${TRANSCRIPTS[name]}responses.jsonl`), "utf8").split("\n");
  if (want.at(-1) === "") want.pop();
  check(`${name}: server exit 0`, t.status === 0, `exit ${t.status} ${t.err.trim()}`);
  check(`${name}: ${want.length} answers`, t.answers.length === want.length, `got ${t.answers.length}`);
  let bad = 0;
  for (let i = 0; i < Math.max(want.length, t.answers.length); i++) {
    if (t.answers[i] === want[i]) continue;
    if (bad++ < 5) {
      const cut = (s) => (s === undefined ? "(none)" : s.length > 300 ? s.slice(0, 300) + "…" : s);
      console.log(`  answer ${i + 1}:\n    want ${cut(want[i])}\n    got  ${cut(t.answers[i])}`);
    }
  }
  check(`${name}: every answer line equals the golden`, bad === 0, `${bad} differ`);
}

try {
  // Rust's transcript.
  const rust = transcript("rust", BIN, []);
  compare("rust", rust);
  const saved = readFileSync(join(DIR, "saved.json"), "utf8");
  check("rust: save wrote Rust's bytes (golden/mcp/saved.json)", rust.saved === saved,
    `${rust.saved.length} vs ${saved.length} bytes`);
  check("rust: the log goes to stderr", /mcp: serving .*model\.json/.test(rust.err), rust.err.trim());

  // The C additions' transcript.
  compare("additions", transcript("additions", BIN, []));

  // --read-only.
  {
    const model = fresh();
    const before = readFileSync(model);
    const COL = "004z1Cm4z4hgNOg0ez4Id1";
    const call = (id, name, args) => JSON.stringify({ jsonrpc: "2.0", id, method: "tools/call", params: { name, arguments: args } });
    const req = [
      call(1, "query", { name: "element", args: { id: COL } }),
      call(2, "run_verb", { name: "move", args: { ids: [COL], d: [100, 0, 0] } }),
      call(3, "run_verb", { name: "move", args: { ids: [COL], d: [100, 0, 0], dry_run: true } }),
      call(4, "undo", {}),
      call(5, "redo", {}),
      call(6, "save", {}),
      call(7, "list_verbs", {}),
    ];
    const r = serve(BIN, [], model, req.join("\n") + "\n", ["--read-only"]);
    const res = r.out.map((l) => JSON.parse(l).result);
    const RO = "read only: the server was started with --read-only";
    check("read-only: 7 answers", res.length === 7, `got ${res.length}`);
    check("read-only: query answers", res[0] && !res[0].isError && res[0].content[0].text.includes(COL));
    check("read-only: run_verb refused", res[1]?.isError && res[1].content[0].text === RO, JSON.stringify(res[1]));
    const dry = res[2] && !res[2].isError ? JSON.parse(res[2].content[0].text) : null;
    check("read-only: a dry run answers its commands and rev 1",
      dry && dry.rev === 1 && dry.commands.length === 1 && dry.commands[0].op === "set_placement", JSON.stringify(res[2]));
    for (const [i, t] of [[3, "undo"], [4, "redo"], [5, "save"]])
      check(`read-only: ${t} refused`, res[i]?.isError && res[i].content[0].text === RO, JSON.stringify(res[i]));
    check("read-only: list_verbs answers", res[6] && !res[6].isError);
    check("read-only: the model file keeps its bytes", readFileSync(model).equals(before));
    check("read-only: the log says so", /\(read only\)/.test(r.err), r.err.trim());
  }

  // A last line without its newline (mcp.rs read_line: answered at the end of input).
  {
    const r = serve(BIN, [], fresh(), '{"jsonrpc":"2.0","id":1,"method":"ping"}\n{"jsonrpc":"2.0","id":2,"method":"ping"}');
    check("a last line without a newline is answered", r.out.length === 2 && r.out[1] === '{"id":2,"jsonrpc":"2.0","result":{}}',
      JSON.stringify(r.out));
    check("each answer ends with a newline", r.raw.endsWith("}\n"));
  }

  // Errors before serving.
  {
    const bad = join(tmp, "bad.json");
    writeFileSync(bad, "{not a model");
    const r = serve(BIN, [], bad, "");
    check("a model that does not load: exit 1, nothing on stdout", r.status === 1 && r.out.length === 0, `exit ${r.status}`);
    check("  and the reason on stderr", /bad\.json: parse/.test(r.err), r.err.trim());
    const u = spawnSync(BIN, ["mcp"], { input: "", env: hostEnv() });
    check("no model: usage, exit 2", u.status === 2 && /usage: bimble-ccore mcp/.test(u.stderr.toString()));
    const f = spawnSync(BIN, ["mcp", "--write", fresh()], { input: "", env: hostEnv() });
    check("an unknown flag: exit 2", f.status === 2 && /unknown flag --write/.test(f.stderr.toString()));
  }
} finally {
  rmSync(tmp, { recursive: true, force: true });
}

console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
