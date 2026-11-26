// The README's Status block, from the generated reports (ticket FIXDOC). An audit found 7 of the 9
// headline numbers of the hand-written block stale or overstated; this tool writes the block
// instead, so a number comes from one report line and says where it came from.
//
//   node ccore/tools/status.mjs            prints the block
//   node ccore/tools/status.mjs --write    writes it into README.md between the two markers
//                                          <!-- status:begin --> and <!-- status:end -->
//   node ccore/tools/status.mjs --check    exits 1 when README.md's block differs from what --write
//                                          would write (ci/run.sh unit runs it: run --write after
//                                          a report changes, and commit both)
//
// Sources (never edited by hand; each has its own generator): results/parity.md (tools/parity.mjs),
// results/weight.md (tools/weight.mjs), results/perf.md (tools/perf.mjs), results/ifc_corpus.md
// (bench/corpus.sh, its prose by the IFC tickets), results/bench.md and results/large.md (the loop
// benches, bench/lab_loop.sh and bench/large.mjs), results/ci.md (the CI tickets' runs). Each number
// is read with a pattern; a pattern that no longer matches stops the tool (exit 2) and names the
// report: the block is never written from a guess. Every performance number carries its conditions.
// No clock, no network: the same reports give the same block.
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const ROOT = resolve(CCORE, "..");
const README = join(ROOT, "README.md");
const BEGIN = "<!-- status:begin -->";
const END = "<!-- status:end -->";
const mode = process.argv.includes("--check") ? "check" : process.argv.includes("--write") ? "write" : "print";

const reports = {};
/** A report's text with every run of white space made one space (prose wraps anywhere). */
function report(name) {
  if (!reports[name]) reports[name] = readFileSync(join(CCORE, "results", name), "utf8").replace(/\s+/g, " ");
  return reports[name];
}
/** The groups of the LAST match of `re` in a report (the latest ticket's update comes last). */
function grab(name, re) {
  const all = [...report(name).matchAll(new RegExp(re.source, re.flags.includes("g") ? re.flags : re.flags + "g"))];
  if (!all.length) {
    console.error(`status: no match in ccore/results/${name} for ${re}: the report changed; update tools/status.mjs`);
    process.exit(2);
  }
  return all.at(-1);
}
const num = (s) => Number(String(s).replace(/,/g, ""));
const fmt = (n) => n.toLocaleString("en-US");
const pct = (a, b) => ((100 * a) / b).toFixed(1);

// ---------------------------------------------------------------- the numbers
const version = readFileSync(join(CCORE, "VERSION"), "utf8").trim();

// IFC (ifc_corpus.md)
const I = "ifc_corpus.md";
const corpus = grab(I, /(\d+) STEP files \((IFC2X3 \d+, IFC4 \d+, IFC4X3 \d+, other \d+)\), ([\d.]+ GB)/);
const step = num(corpus[1]);
const nonStep = num(grab(I, /The (\d+) non-STEP files \(([^)]*)\)/)[1]);
const lfs = num(grab(I, /(\d+) files of `Community-Sample-Test-Files` are git-lfs pointers/)[1]);
const sanAll = grab(I, /(\d+) files \((\d+) STEP, (\d+) not STEP; \d+ git-lfs pointers skipped as before\), \*\*no report\*\*/);
const outcome = grab(I, /every outcome equal to the -O2 build's \((\d+) ok, (\d+) refused\)/);
if (num(outcome[1]) !== step) { console.error(`status: ${I}: ${outcome[1]} ok of ${step} STEP files: the wording "read" needs a review`); process.exit(2); }
const drawn = num(grab(I, /Elements drawn \(\d+ STEP files\): \*\*[\d,]+ -> ([\d,]+)\*\*/)[1]);
const withBody = num(grab(I, /Products with a body drawn: \*\*[\d,]+ -> [\d,]+\*\* of ([\d,]+)/)[1]);
const ifc1Rust = grab(I, /Both ok: (\d+); identical \(scene\.txt and every mesh hash\): (\d+); different: (\d+)\./);
const ifc2 = grab(I, /\*\*828 files; (\d+) STEP files give the same scene, byte for byte\.\*\* (\d+) STEP files changed/);
const ifc3 = grab(I, /\*\*(\d+) files; (\d+) give the same scene, byte for byte; (\d+) changed, each explained\*\*/);
const rustDump = grab(I, /Rust: `ccore_golden --dump-read` \(release, rayon\)\. The binary in the shared target was built from the ([\d:]+) sources/);
const ios = grab(I, /\*\*IfcOpenShell ([\d,]+); C [\d,]+ -> ([\d,]+); equal files \d+ -> (\d+)\*\*/);
const iosVersion = grab(I, /IfcOpenShell (\d+\.\d+\.\d+) on (\d+) files/);

// Parity (parity.md)
const P = "parity.md";
const acts = grab(P, /\| Actions \(Rust (\d+), C (\d+)\) \| same (\d+) · enabled state differs (\d+) · pressed differs \(enabled same\) (\d+) · missing in C (\d+) · C-only (\d+) \|/);
const qs = grab(P, /\| Queries \(Rust (\d+), C (\d+)\) \| same shape (\d+) · differs (\d+) · stub (\d+) · missing in C (\d+) · C-only (\d+) \|/);
const keys = grab(P, /\| Shortcuts \(Rust (\d+)\) \| in the lab's key table (\d+) · of them inert in a browser tab (\d+) \(([^)]*)\) ·/);
const scen = grab(P, /Scenarios \((\d+)\):/);
const rustParity = grab(P, /Rust reference: `([^`]+)`/);
const fake = grab(P, /\| Fake UI \([^|]*\) \| (\d+) \|/);
const actsRustPresent = num(acts[1]) - num(acts[6]);

// Size (weight.md)
const W = "weight.md";
const total = grab(W, /\| \*\*total\*\* \| ([\d,]+) \| ([\d,]+) \| ([\d,]+) \| ([\d,]+) \| ([\d,]+) \|/);
const editor = grab(W, /\| ccore\.wasm \(editor\) \| ([\d,]+) \| ([\d,]+) \|/);
const viewer = grab(W, /\| ccore-view\.wasm \(viewer, shipped, after wasm-opt\) \| ([\d,]+) \| ([\d,]+) \| ([\d,]+) \(ADR 0069 as amended; ([\d.]+) % used\) \|/);
const native = grab(W, /\| bimble-ccore \(native, stripped\) \| ([\d,]+) \|/);
const archive = grab(W, /\| (bimble-ccore-[^|]*?-(\w+)-linux\.tar\.gz) \(release archive\) \| ([\d,]+) \|/);
const leftover = grab(W, /\| web\/ccore\/ in the archive \([^|]*\) \| \d+ \| \d+ \| \d+ \| \d+ \| ([\d,]+) \|/);

// Speed (perf.md, bench.md, large.md)
const F = "perf.md";
const perfWhen = grab(F, /- (\d{4}-\d\d-\d\d), commit (\w+), mode (\w+) \((\d+) Lighthouse runs/);
const perfLoad = grab(F, /load average at start ([\d.]+) \//);
const lhEditor = grab(F, /\| editor \(\/c\/\) \| (\d+) \/ (\d+) \|/);
const lhDeployed = grab(F, /\| deployed C lab \(https:\/\/guilbep\.github\.io\/bimble\/c\/\) \| (\d+) \/ (\d+) \|/);
const wasmLocal = grab(F, /\| editor \(\/c\/\) \| frame\.json \(sample\) \|.*?\| (\d{1,3},\d{3}) \/ [\d,]+ \/ [\d,]+ \|/);
const wasmDeployed = grab(F, /\| deployed C lab \(https:\/\/guilbep\.github\.io\/bimble\/c\/\) \| frame\.json \(sample\) \|.*?\| (\d{1,3},\d{3}) \/ [\d,]+ \/ [\d,]+ \|/);
const loop = grab("bench.md", /\| host \(dev\.sh\) median \| (\d+) \| \d+ \| \d+ \| [\d /]+ \| \*\*(\d+) \/ (\d+) \/ (\d+)\*\* \|/);
const loopRuns = grab("bench.md", /`RUNS=(\d+) MODES="host tcc" PROBE=0 bench\/lab_loop\.sh`/);
const large = grab("large.md", /Edit → pixels in the lab \(dev\.sh loop, tcc host\): ([\d.]+) s at 2k, ([\d.]+) s at 20k, ([\d.]+) s at 60k\*\* \(median of (\d+)\)/);

// CI (ci.md)
const ciFull = grab("ci.md", /Linux gate after these changes: `ci\/run\.sh full` (\d+) suites, (\d+) failed \((\d+) s\)/);

// ---------------------------------------------------------------- the block
const S = [];
const src = (n) => `[${n}]`;
S.push(BEGIN);
S.push("<!-- Written by `node ccore/tools/status.mjs --write` from ccore/results/. Do not edit between the markers: `ci/run.sh unit` checks them. -->");
S.push("");
S.push(`## Status (version \`${version}\`)`);
S.push("");
S.push("A preview. Each number is copied from a generated report under `ccore/results/`; the sources and");
S.push("the conditions of each number are listed under the list.");
S.push("");
S.push(`- **IFC reading** ${src(1)}: ${fmt(step)} public STEP files (${corpus[2]};`);
S.push(`  ${corpus[3]}) read. "Read" means not refused, no crash, no timeout, and no report under ASan and`);
S.push(`  UBSan; it does not mean every product is drawn: ${fmt(drawn)} of ${fmt(withBody)} products with a body`);
S.push(`  get a mesh (${pct(drawn, withBody)} %). Not counted: ${nonStep} non-STEP files (ifcZIP, ifcXML: refused, not`);
S.push(`  read yet) and ${lfs} git-lfs pointer files that were not fetched.`);
S.push(`- **IFC against Rust** ${src(1)}: before the C reader learnt new types, C and Rust gave the same scene`);
S.push(`  and meshes on ${ifc1Rust[2]} of ${ifc1Rust[1]} files. Since then the C reader draws types Rust does not, on purpose:`);
S.push(`  ${ifc2[2]} STEP files changed with IFC2 (${ifc2[1]} kept their scene byte for byte), ${ifc3[3]} more files with IFC3,`);
S.push(`  each holding a newly supported type. The Rust side was a release-mode build of the golden`);
S.push(`  dumper from the development sources of 25 Nov (${rustDump[1]}), not the v0.19.0 release binary.`);
S.push(`- **IFC against IfcOpenShell ${iosVersion[1]}** ${src(1)}: on a ${iosVersion[2]}-file sample, C gives a non-empty mesh to`);
S.push(`  ${ios[2]} products where IfcOpenShell triangulates ${ios[1]}; the same count on ${ios[3]} of the ${iosVersion[2]} files.`);
S.push(`- **Against the Rust app** ${src(2)}: the C core serves ${actsRustPresent} of the ${acts[1]} Rust action names (the name`);
S.push(`  only, not what the action does); ${acts[3]} of them have the same \`enabled\` and \`pressed\` flags in all`);
S.push(`  ${scen[1]} scenarios (synthetic fixtures, mainly a 72-element frame). ${qs[3]} of the ${qs[1]} Rust queries`);
S.push(`  answer with the same JSON key names and types (not checked: the values). ${keys[2]} of the ${keys[1]} Rust`);
S.push(`  shortcuts have a row in the lab's key table; ${keys[3]} of them cannot run in a browser tab`);
S.push(`  (${keys[4]}: the browser keeps it). The Rust answers were dumped from a development build that`);
S.push(`  reports \`${rustParity[1]}\`, not from v0.19.0.`);
S.push(`  ${fake[1]} controls still shown are not served by the C core; the report lists them.`);
S.push(`- **Size** ${src(3)}: ${fmt(num(total[1]))} lines of C; the viewer wasm ${viewer[1]} bytes (${viewer[2]} gzip, ${viewer[4]} % of`);
S.push(`  its ${viewer[3]}-byte limit), the editor wasm ${editor[1]} bytes (${editor[2]} gzip), the native program`);
S.push(`  ${native[1]} bytes (stripped), the Linux ${archive[2]} release archive ${archive[3]} bytes.`);
if (num(leftover[1]) > 0)
  S.push(`  That archive was built on the dev machine with a stale \`web/ccore/\` copy inside (${leftover[1]} bytes`,
         `  before compression) that the CI archive does not have.`);
S.push(`- **Speed** ${src(4)}: Lighthouse performance (desktop / mobile, median of ${perfWhen[4]} runs): ${lhEditor[1]} / ${lhEditor[2]} for the`);
S.push(`  current build served locally on the dev machine under load (load average ${perfLoad[1]} at the start);`);
S.push(`  ${lhDeployed[1]} / ${lhDeployed[2]} for the page then deployed on GitHub Pages, over the network, an older and smaller`);
S.push(`  build (wasm ${wasmDeployed[1]} bytes against ${wasmLocal[1]}). Both measured on ${perfWhen[1]}.`);
S.push(`- **Edit loop** ${src(5)}: a C edit written → new pixels in the lab in ${(num(loop[4]) / 1000).toFixed(2)} s (median of ${loopRuns[1]}) on the`);
S.push(`  72-element frame, host built by tcc; ${large[2]} s on a 20,000-element model. Conditions below.`);
S.push(`- **Checks** ${src(6)}: \`ccore/ci/run.sh full\`, ${ciFull[1]} suites, ${ciFull[2]} failed, ${fmt(num(ciFull[3]))} s on the dev machine`);
S.push(`  (one run).`);
S.push("");
S.push("Sources and conditions:");
S.push("");
S.push(`1. \`ccore/results/ifc_corpus.md\` (\`ccore/bench/corpus.sh\`): the public corpora at pinned commits;`);
S.push(`   the sanitizer run covered ${sanAll[1]} files (${sanAll[2]} STEP, ${sanAll[3]} not STEP). "Drawn" counts products`);
S.push(`   with a non-empty mesh; the IfcOpenShell count is products its iterator triangulates to at`);
S.push(`   least one face.`);
S.push(`2. \`ccore/results/parity.md\` (\`ccore/tools/parity.mjs\`): its header says what it supports and what it`);
S.push(`   does not.`);
S.push(`3. \`ccore/results/weight.md\` (\`ccore/tools/weight.mjs\`): the wasm files built by \`build.sh wasm\` and`);
S.push(`   \`wasm-view\`, the native program and the archive by \`build.sh release\` on the dev machine`);
S.push(`   (${archive[2]} Linux).`);
S.push(`4. \`ccore/results/perf.md\` (\`ccore/tools/perf.mjs\`, mode ${perfWhen[3]}, commit ${perfWhen[2]}): headless`);
S.push(`   Chrome, Lighthouse's simulated throttling; the local build is served by a static server that`);
S.push(`   answers as GitHub Pages.`);
S.push(`5. \`ccore/results/bench.md\` (\`ccore/bench/lab_loop.sh\`): 72 elements (73 after the placed column),`);
S.push(`   \`build.sh host\` (tcc, ${loop[1]} ms of the total), headless Chromium drawing with SwiftShader (CPU),`);
S.push(`   timed from the edit written to the first new canvas hash; \`dev.sh\`'s file watcher delay is not`);
S.push(`   in it.`);
S.push(`   \`ccore/results/large.md\` (\`ccore/bench/large.mjs\`): the same loop, median of ${large[4]}: ${large[1]} s at 2,000,`);
S.push(`   ${large[2]} s at 20,000 and ${large[3]} s at 60,000 elements (synthetic models).`);
S.push(`6. \`ccore/results/ci.md\`: the run after ticket WIN2, 16 GB aarch64 dev machine; CI runs the same`);
S.push(`   script in parallel jobs (\`.github/workflows/ccore.yml\`).`);
S.push(END);
const block = S.join("\n");

if (mode === "print") {
  console.log(block);
  process.exit(0);
}
const readme = readFileSync(README, "utf8");
const a = readme.indexOf(BEGIN), b = readme.indexOf(END);
if (a < 0 || b < a) {
  console.error(`status: README.md has no ${BEGIN} … ${END} block`);
  process.exit(2);
}
const now = readme.slice(a, b + END.length);
if (mode === "check") {
  if (now === block) {
    console.log("status: README.md's Status block matches the reports: 1 passed, 0 failed");
    process.exit(0);
  }
  const x = now.split("\n"), y = block.split("\n");
  for (let i = 0; i < Math.max(x.length, y.length); i++)
    if (x[i] !== y[i]) { console.log(`status: first difference at line ${i + 1} of the block:\n  README: ${x[i] ?? "(none)"}\n  report: ${y[i] ?? "(none)"}`); break; }
  console.log("FAIL status: README.md's Status block differs from the reports: run node ccore/tools/status.mjs --write and commit README.md");
  console.log("status: 0 passed, 1 failed");
  process.exit(1);
}
writeFileSync(README, readme.slice(0, a) + block + readme.slice(b + END.length));
console.log(now === block ? "status: README.md unchanged" : "status: wrote the Status block of README.md");
