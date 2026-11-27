// The shared Chromium launcher's TMPDIR (ticket VIS1, test/lib/chromium.mjs): node ccore/test/chromium_test.mjs
// Chromium aborted at start when its TMPDIR was a deep scratch folder (its singleton socket's path over the 107
// bytes of a Unix socket). Checks: chromiumEnv gives a new short TMPDIR that fits (and leaves the rest of the
// environment), two launches get two folders, a long TMPDIR is refused with a message naming the variable.
// No browser is started.
import { existsSync } from "node:fs";
import { checkTmpdir, chromiumEnv, SOCKET_PATH_MAX } from "./lib/chromium.mjs";

let passed = 0, failed = 0;
const check = (name, ok, detail = "") => { if (ok) passed++; else failed++; console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`); };
const a = chromiumEnv({ HOME: "/home/x", PATH: "/bin", TMPDIR: "/home/x/.cache/very/deep/scratch/folder/run-123456/and/more" });
const b = chromiumEnv();
check("chromiumEnv: a new folder under the system's temp folder", existsSync(a.TMPDIR) && /\/bbc-/.test(a.TMPDIR), a.TMPDIR);
check("chromiumEnv: its path leaves room for the singleton socket", a.TMPDIR.length + 60 < SOCKET_PATH_MAX, `${a.TMPDIR.length} characters`);
check("chromiumEnv: the rest of the environment kept", a.HOME === "/home/x" && a.PATH === "/bin");
check("chromiumEnv: one folder per launch", a.TMPDIR !== b.TMPDIR);
let msg = "";
try { checkTmpdir("/home/x/.cache/bimble-vis1/ccore-lab-visible-HolA6w/run-pq3p9f"); } catch (e) { msg = e.message; }
check("a TMPDIR too long for the socket is refused, naming the variable", /TMPDIR=.*too long/.test(msg), msg.slice(0, 120));
console.log(`chromium: ${passed} passed, ${failed} failed`);
process.exitCode = failed ? 1 : 0;
