// Holds benchmark lines to a budgets file (make perf-web), like git-board's perfcheck:
//   node perf/check.mjs <results.txt> <budgets.txt> [tolerance]
// The median per benchmark; fails when a median is over its budget by more than the
// tolerance, or a budgeted benchmark did not run. Writes report.md next to the results.
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { pathToFileURL } from "node:url";

const UNITS = { ns: 1, us: 1e3, µs: 1e3, ms: 1e6, s: 1e9, B: 1, KB: 1e3, MB: 1e6 };

/** "BenchmarkX\t1\t123 ns/op" lines → { X: [ns or bytes, ...] }. */
export function parseResults(text) {
  const out = {};
  for (const line of text.split("\n")) {
    const m = line.match(/^Benchmark(\S+)\s+\d+\s+([\d.]+) (ns|B)\/op/);
    if (m) (out[m[1]] ??= []).push(Number(m[2]));
  }
  return out;
}

/** "name budget why" lines → { name: { budget (ns or bytes), text, why } }. */
export function parseBudgets(text) {
  const out = {};
  for (const line of text.split("\n")) {
    if (!line.trim() || line.startsWith("#")) continue;
    const [name, b, ...why] = line.trim().split(/\s+/);
    const m = b?.match(/^([\d.]+)(ns|us|µs|ms|s|B|KB|MB)$/);
    if (!m) throw new Error(`bad budget line: ${line}`);
    out[name] = { budget: Number(m[1]) * UNITS[m[2]], text: b, why: why.join(" ") };
  }
  return out;
}

export function median(xs) {
  const s = [...xs].sort((a, b) => a - b);
  const h = s.length >> 1;
  return s.length % 2 ? s[h] : (s[h - 1] + s[h]) / 2;
}

/** Rows { name, median, budget, ratio, ok }; a budgeted benchmark that did not run fails. */
export function check(results, budgets, tolerance) {
  return Object.entries(budgets).map(([name, { budget, text }]) => {
    const xs = results[name];
    if (!xs) return { name, median: null, budget: text, ratio: null, ok: false };
    const med = median(xs);
    const ratio = med / budget;
    return { name, median: med, budget: text, ratio, ok: ratio <= 1 + tolerance };
  });
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [res, bud, tol = "0.15"] = process.argv.slice(2);
  if (!res || !bud) {
    console.error("usage: check.mjs <results.txt> <budgets.txt> [tolerance]");
    process.exit(2);
  }
  const results = parseResults(readFileSync(res, "utf8"));
  const rows = check(results, parseBudgets(readFileSync(bud, "utf8")), Number(tol));
  const unit = (name, v) => (name.endsWith("Bytes") ? `${v} B` : `${(v / 1e6).toFixed(0)} ms`);
  const lines = [
    `# Web performance (tolerance ${Math.round(Number(tol) * 100)} %)`,
    "",
    "| name | median | budget | ratio | result |",
    "|---|---|---|---|---|",
    ...rows.map((r) =>
      `| ${r.name} | ${r.median === null ? "did not run" : unit(r.name, r.median)} | ${r.budget} | ${r.ratio === null ? "-" : r.ratio.toFixed(2)} | ${r.ok ? "pass" : "FAIL"} |`,
    ),
  ];
  const unbudgeted = Object.keys(results).filter((n) => !rows.some((r) => r.name === n));
  if (unbudgeted.length) lines.push("", `Not budgeted: ${unbudgeted.join(", ")}`);
  writeFileSync(join(dirname(res), "report.md"), lines.join("\n") + "\n");
  console.log(lines.join("\n"));
  process.exit(rows.every((r) => r.ok) ? 0 : 1);
}
