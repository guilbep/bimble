// The lab page's timings (make perf-web): serves web-ui/dist with vite preview,
// runs Lighthouse's mobile run (a slow 4G and a 4x slower CPU, simulated) on a
// small and a large synthetic model, and prints each run as a Go benchmark line,
// which perf/check.mjs holds to perf/budgets.txt:
//   node perf/lighthouse.mjs [runs]   (from web-ui/; also writes ../perf-results/web.txt)
// WasmReady and ModelShown are the page's own marks (src/bimble.ts), from the
// user-timings audit. Chrome: $CHROME_PATH, else the installed one.
import { spawn, execFileSync } from "node:child_process";
import { appendFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const runs = Number(process.argv[2] ?? "3");
const port = "18492";
const base = `http://127.0.0.1:${port}`;
const results = new URL("../../perf-results/", import.meta.url).pathname;
mkdirSync(results, { recursive: true });
const txt = join(results, "web.txt");
writeFileSync(txt, "");
const print = (line) => {
  console.log(line);
  appendFileSync(txt, line + "\n");
};

const server = spawn("npx", ["vite", "preview", "--host", "127.0.0.1", "--port", port, "--strictPort"], {
  stdio: ["ignore", "ignore", "inherit"],
  detached: true,
});
// Kill the whole group: npx starts vite as a child.
const stop = () => {
  try {
    process.kill(-server.pid);
  } catch {}
};
process.on("exit", stop);
let failed = false;
try {
  for (let i = 0; ; i++) {
    try {
      if ((await fetch(base)).ok) break;
    } catch {}
    if (i > 600) throw new Error(`vite preview didn't answer on ${base}`);
    await new Promise((r) => setTimeout(r, 100));
  }
  const pages = { Small: "/?url=/perf-small.bimble", Large: "/?url=/perf-large.bimble" };
  const out = mkdtempSync(join(tmpdir(), "perf-web-"));
  print("pkg: bimble/web-ui");
  for (const [name, path] of Object.entries(pages)) {
    for (let r = 0; r < runs; r++) {
      const file = join(out, `${name}-${r}.json`);
      execFileSync(
        "npx",
        ["-y", "lighthouse@12.6.1", base + path, "--only-categories=performance", "--output=json", `--output-path=${file}`, "--chrome-flags=--headless=new", "--quiet"],
        { stdio: ["ignore", "ignore", "inherit"] },
      );
      const a = JSON.parse(readFileSync(file, "utf8")).audits;
      const ns = (ms) => `${Math.round(ms * 1e6)} ns/op`;
      print(`Benchmark${name}FCP\t1\t${ns(a["first-contentful-paint"].numericValue)}`);
      print(`Benchmark${name}LCP\t1\t${ns(a["largest-contentful-paint"].numericValue)}`);
      print(`Benchmark${name}TBT\t1\t${ns(a["total-blocking-time"].numericValue)}`);
      const timings = a["user-timings"]?.details?.items ?? [];
      for (const [metric, mark] of [["WasmReady", "bimble:wasm-ready"], ["ModelShown", "bimble:model-shown"]]) {
        const m = timings.find((t) => t.timingType === "Measure" && t.name === mark);
        if (m) print(`Benchmark${name}${metric}\t1\t${ns(m.startTime + m.duration)}`);
        else {
          console.error(`error: ${name} run ${r}: no ${mark} measure in Lighthouse's user-timings`);
          failed = true;
        }
      }
      print(`Benchmark${name}Bytes\t1\t${Math.round(a["total-byte-weight"].numericValue)} B/op`);
    }
  }
  rmSync(out, { recursive: true, force: true });
} finally {
  stop();
}
if (failed) process.exit(1);
