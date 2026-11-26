// The environment of a host (or a tool that starts one) a node test spawns (ticket FIXSES):
//   import { hostEnv } from "./lib/host_env.mjs";
//   spawn(BIN, args, { env: hostEnv({ BIMBLE_TOKEN: TOKEN }) });
// The caller's environment, with BIMBLE_CONFIG_DIR a temporary folder of this test process (made
// on import, removed at exit), so a test never reads or writes the settings and the recent files
// of whoever runs it. `extra` comes last: a test that wants its own folder (or "" for none) passes
// BIMBLE_CONFIG_DIR itself.
// Lives in test/lib/, not test/: ci/run.sh runs every test/*.mjs as a suite.
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const dir = mkdtempSync(join(tmpdir(), "ccore-test-config-"));
process.on("exit", () => {
  try { rmSync(dir, { recursive: true, force: true }); } catch { /* gone already */ }
});

/** process.env with BIMBLE_CONFIG_DIR a temporary folder, then `extra`. */
export const hostEnv = (extra = {}) => ({ ...process.env, BIMBLE_CONFIG_DIR: join(dir, "config"), ...extra });
