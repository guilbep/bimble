// The web pages' timings (make perf-web): serves web-ui/dist (the Vue lab, editor build)
// and web/dist (the plain viewer) gzipped like GitHub Pages (perf/serve.mjs), runs
// Lighthouse 12.6.1 on them with DevTools throttling (4x slower CPU, 10 Mbit/s down,
// 40 ms RTT), and prints each run as a Go benchmark line, which perf/check.mjs holds
// to perf/budgets.txt:
//   node perf/lighthouse.mjs [runs]   (from web-ui/; also writes ../perf-results/web.txt)
// ponytail: DevTools throttling, not simulated: the page's own marks (src/bimble.ts,
// user-timings audit: WasmReady, ModelShown) must run on the slowed CPU.
// Chrome: $CHROME_PATH, else the installed one.
import { spawn, execFileSync } from "node:child_process";
import { appendFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const runs = Number(process.argv[2] ?? "5");
const here = new URL(".", import.meta.url).pathname;
const results = join(here, "../../perf-results");
mkdirSync(results, { recursive: true });
const txt = join(results, "web.txt");
writeFileSync(txt, "");
const print = (line) => {
  console.log(line);
  appendFileSync(txt, line + "\n");
};

const sites = [
  { name: "Lab", dir: join(here, "../dist"), port: 18492, marks: true, pages: { Small: "/?url=/perf-small.bimble", Large: "/?url=/perf-large.bimble" } },
  { name: "Viewer", dir: join(here, "../../web/dist"), port: 18493, marks: false, pages: { Large: "/index.html?url=/perf-large.bimble" } },
];
const servers = sites.map((s) => spawn(process.execPath, [join(here, "serve.mjs"), s.dir, String(s.port)], { stdio: ["ignore", "ignore", "inherit"] }));
const stop = () => servers.forEach((s) => s.kill());
process.on("exit", stop);

const ns = (ms) => `${Math.round(ms * 1e6)} ns/op`;
let failed = false;
try {
  for (const s of sites) {
    for (let i = 0; ; i++) {
      try {
        if ((await fetch(`http://127.0.0.1:${s.port}/`)).ok) break;
      } catch {}
      if (i > 600) throw new Error(`serve.mjs didn't answer on port ${s.port}`);
      await new Promise((r) => setTimeout(r, 100));
    }
  }
  const out = mkdtempSync(join(tmpdir(), "perf-web-"));
  print("pkg: bimble/web");
  for (const s of sites) {
    for (const [page, path] of Object.entries(s.pages)) {
      const name = s.name + page;
      for (let r = 0; r < runs; r++) {
        const file = join(out, `${name}-${r}.json`);
        execFileSync(
          "npx",
          [
            "-y", "lighthouse@12.6.1", `http://127.0.0.1:${s.port}${path}`,
            "--only-categories=performance", "--throttling-method=devtools",
            "--throttling.cpuSlowdownMultiplier=4", "--throttling.downloadThroughputKbps=10240",
            "--throttling.uploadThroughputKbps=5120", "--throttling.requestLatencyMs=40",
            "--max-wait-for-load=90000",
            "--output=json", `--output-path=${file}`, "--chrome-flags=--headless=new", "--quiet",
          ],
          { stdio: ["ignore", "ignore", "inherit"] },
        );
        const a = JSON.parse(readFileSync(file, "utf8")).audits;
        print(`Benchmark${name}FCP\t1\t${ns(a["first-contentful-paint"].numericValue)}`);
        print(`Benchmark${name}LCP\t1\t${ns(a["largest-contentful-paint"].numericValue)}`);
        print(`Benchmark${name}TBT\t1\t${ns(a["total-blocking-time"].numericValue)}`);
        if (a.interactive?.numericValue != null) print(`Benchmark${name}TTI\t1\t${ns(a.interactive.numericValue)}`);
        if (s.marks) {
          const timings = a["user-timings"]?.details?.items ?? [];
          for (const [metric, mark] of [["WasmReady", "bimble:wasm-ready"], ["ModelShown", "bimble:model-shown"]]) {
            const m = timings.find((t) => t.timingType === "Measure" && t.name === mark);
            if (m) print(`Benchmark${name}${metric}\t1\t${ns(m.startTime + m.duration)}`);
            else {
              console.error(`error: ${name} run ${r}: no ${mark} measure in Lighthouse's user-timings`);
              failed = true;
            }
          }
        }
        print(`Benchmark${name}Bytes\t1\t${Math.round(a["total-byte-weight"].numericValue)} B/op`);
      }
    }
  }
  rmSync(out, { recursive: true, force: true });
} finally {
  stop();
}
if (failed) process.exit(1);
