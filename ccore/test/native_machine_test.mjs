// The native app's shell machine (ccore/native/ui/machine.c, ticket NUI1) against the lab's machine.ts:
// node ccore/test/native_machine_test.mjs. Builds machine.c alone with a small main (CC, else clang, else
// tcc: no SDL needed), which prints, for every kind, event and session of the export (a saved model, a
// model with unsaved changes, nothing open), where it goes; compares row by row with the "dest" table that
// web-ui/scripts/export-shell-data.mjs writes from machine.ts itself (ccore/native/ui/data/machine.json),
// and checks the file is what the TypeScript gives now (export-shell-data.mjs --check).
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  if (ok) passed++;
  else failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const which = (c) => { try { execFileSync("sh", ["-c", `command -v ${c}`], { stdio: "ignore" }); return true; } catch { return false; } };

const tmp = mkdtempSync(join(tmpdir(), "native-machine-"));
try {
  let fresh = true;
  try { execFileSync("node", [join(CCORE, "../web-ui/scripts/export-shell-data.mjs"), "--check"], { stdio: "pipe" }); } catch (e) { fresh = false; console.log(String(e.stderr)); }
  check("machine.json is what machine.ts gives now (export-shell-data.mjs --check)", fresh);
  const M = JSON.parse(readFileSync(join(CCORE, "native/ui/data/machine.json"), "utf8"));
  writeFileSync(join(tmp, "main.c"), `#include <stdio.h>
#include "machine.h"
int main(void) {
  ShellCore cores[3] = {{1, 0, 1}, {1, 1, 1}, {0, 0, 0}};
  printf("{");
  for (int k = 0; k < SK_COUNT; k++) {
    printf("%s\\"%s\\":{", k ? "," : "", SHELL_KIND_NAME[k]);
    int first = 1;
    for (int e = 0; e < SE_COUNT; e++) {
      int to[3], any = 0;
      for (int c = 0; c < 3; c++) any |= (to[c] = shell_next(k, SK_VIEW, e, cores[c], 1)) != SK_REFUSED;
      if (!any) continue;
      printf("%s\\"%s\\":[", first ? "" : ",", SHELL_EVENT_NAME[e]);
      for (int c = 0; c < 3; c++) printf("%s\\"%s\\"", c ? "," : "", to[c] == SK_REFUSED ? "-" : SHELL_KIND_NAME[to[c]]);
      printf("]");
      first = 0;
    }
    printf("}");
  }
  printf("}\\n");
  return 0;
}
`);
  const cc = process.env.CC ?? (which("clang") ? "clang" : "tcc");
  execFileSync(cc, ["-I", join(CCORE, "native/ui"), join(CCORE, "native/ui/machine.c"), join(tmp, "main.c"), "-o", join(tmp, "m")], { stdio: "inherit" });
  const mine = JSON.parse(execFileSync(join(tmp, "m"), { encoding: "utf8" }));
  check("the same kinds", JSON.stringify(Object.keys(mine)) === JSON.stringify(M.kinds), Object.keys(mine).join(" "));
  let rows = 0;
  for (const k of M.kinds) {
    const want = M.dest[k] ?? {}, got = mine[k] ?? {};
    for (const e of new Set([...Object.keys(want), ...Object.keys(got)])) {
      rows++;
      const w = JSON.stringify(want[e] ?? null), g = JSON.stringify(got[e] ?? null);
      if (w !== g) check(`${k} --${e}--> (saved, unsaved, nothing open)`, false, `machine.ts ${w}, machine.c ${g}`);
    }
  }
  check(`every row of machine.ts has the same destinations in machine.c (${rows} rows x 3 sessions)`, failed === 0);
} catch (e) {
  check("no exception", false, e?.stack ?? String(e));
} finally {
  rmSync(tmp, { recursive: true, force: true });
}
console.log(`${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
