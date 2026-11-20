import { test } from "node:test";
import assert from "node:assert/strict";
import { check, parseBudgets, parseResults } from "./check.mjs";

const budgets = parseBudgets("# comment\nA  100ms  why\nB  1MB  bytes\n");

test("units parse", () => {
  assert.equal(budgets.A.budget, 100_000_000); // 100 ms in ns
  assert.equal(budgets.B.budget, 1_000_000);
  const r = parseResults("pkg: x\nBenchmarkA\t1\t5 ns/op\nBenchmarkB\t1\t7 B/op\n");
  assert.deepEqual(r, { A: [5], B: [7] });
});

test("over by 20 % fails, by 10 % passes (median of three)", () => {
  // medians: A = 120 ms (ratio 1.20), B = 1.1 MB (ratio 1.10)
  const r = parseResults(
    ["BenchmarkA\t1\t90000000 ns/op", "BenchmarkA\t1\t120000000 ns/op", "BenchmarkA\t1\t300000000 ns/op",
     "BenchmarkB\t1\t1100000 B/op", "BenchmarkB\t1\t1000000 B/op", "BenchmarkB\t1\t1200000 B/op"].join("\n"),
  );
  const [a, b] = check(r, budgets, 0.15);
  assert.equal(a.median, 120_000_000);
  assert.equal(a.ok, false);
  assert.equal(b.median, 1_100_000);
  assert.equal(b.ok, true);
});

test("a budgeted benchmark that did not run fails", () => {
  const rows = check(parseResults("BenchmarkA\t1\t1 ns/op\n"), budgets, 0.15);
  assert.deepEqual(rows.map((x) => x.ok), [true, false]);
});
