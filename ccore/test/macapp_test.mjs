// The macOS disk image of the native app (ticket MACAPP, ADR 0088): node ccore/test/macapp_test.mjs [image.dmg]
// after native/macos/bundle.sh (default: the one .dmg in ccore/out/macapp). On macOS only; elsewhere "skipped:".
// Mounts the image read-only and checks: the layout (bimble.app, an Applications link, README.txt with the first
// launch's steps), Info.plist (bundle id, ccore/VERSION, the oldest macOS, Retina, the three file types), the
// signature (codesign --verify --strict: ad hoc or better), the executable's libraries (otool -L: only /usr/lib and
// /System: SDL, FreeType, brotli are inside), its architectures (MACAPP_ARCH: arm64, x86_64 or universal) and its
// minimum macOS (LC_BUILD_VERSION), and that `bimble --version` runs and prints the version (under Rosetta 2 for
// x86_64 on an arm64 Mac). The image is detached at the end, whatever happened.
import { execFileSync, spawnSync } from "node:child_process";
import { existsSync, lstatSync, mkdtempSync, readFileSync, readdirSync, readlinkSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const CCORE = resolve(dirname(fileURLToPath(import.meta.url)), "..");
if (process.platform !== "darwin") { console.log("skipped: macapp_test needs macOS (hdiutil, codesign, otool)"); process.exit(0); }
const V = readFileSync(join(CCORE, "VERSION"), "utf8").trim();
const MINOS = process.env.MACOSX_DEPLOYMENT_TARGET || "11.0";
let dmg = process.argv[2];
if (!dmg) {
  const d = join(CCORE, "out/macapp");
  const all = existsSync(d) ? readdirSync(d).filter((f) => f.endsWith(".dmg")) : [];
  if (all.length !== 1) { console.log(`FAIL macapp_test: ${all.length} .dmg in ${d} (name one)`); process.exit(1); }
  dmg = join(d, all[0]);
}
const arch = process.env.MACAPP_ARCH || (dmg.match(/-macos-(arm64|x86_64|universal)\.dmg$/) || [])[1];
let passed = 0, failed = 0;
const check = (name, ok, detail = "") => {
  ok ? passed++ : failed++;
  console.log(`${ok ? "ok  " : "FAIL"} ${name}${detail ? `: ${detail}` : ""}`);
};
const sh = (cmd, args) => {
  const r = spawnSync(cmd, args, { encoding: "utf8" });
  return { code: r.status, out: (r.stdout ?? "") + (r.stderr ?? "") };
};

check("image name", new RegExp(`^bimble-${V.replace(/\./g, "\\.")}-macos-(arm64|x86_64|universal)\\.dmg$`).test(dmg.split("/").pop()), dmg);
const mnt = mkdtempSync(join(tmpdir(), "macapp-mnt-"));
const att = sh("hdiutil", ["attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mnt, dmg]);
check("hdiutil attach", att.code === 0, att.code === 0 ? mnt : att.out.trim());
try {
  if (att.code === 0) {
    const app = join(mnt, "bimble.app"), exe = join(app, "Contents/MacOS/bimble");
    check("bimble.app/Contents/MacOS/bimble", existsSync(exe));
    check("bimble.app/Contents/Resources/bimble.icns", existsSync(join(app, "Contents/Resources/bimble.icns")));
    check("licences inside", existsSync(join(app, "Contents/Resources/LICENSE")) && existsSync(join(app, "Contents/Resources/licences/DearImGui-LICENSE.txt")));
    const apps = join(mnt, "Applications");
    check("Applications link", existsSync(apps) && lstatSync(apps).isSymbolicLink() && readlinkSync(apps) === "/Applications");
    const readme = existsSync(join(mnt, "README.txt")) ? readFileSync(join(mnt, "README.txt"), "utf8") : "";
    check("README.txt: the first launch (Open Anyway, xattr)", readme.includes("Open Anyway") && readme.includes("xattr -dr com.apple.quarantine"));
    const plist = join(app, "Contents/Info.plist");
    const key = (k) => sh("/usr/libexec/PlistBuddy", ["-c", `Print :${k}`, plist]).out.trim();
    check("CFBundleIdentifier", key("CFBundleIdentifier") === "ch.bimble.bimble", key("CFBundleIdentifier"));
    check("BimbleVersion", key("BimbleVersion") === V, key("BimbleVersion"));
    check("CFBundleShortVersionString", key("CFBundleShortVersionString") === V.replace(/^(\d+\.\d+\.\d+).*/, "$1"));
    check("LSMinimumSystemVersion", key("LSMinimumSystemVersion") === MINOS, key("LSMinimumSystemVersion"));
    check("NSHighResolutionCapable", key("NSHighResolutionCapable") === "true");
    const xml = execFileSync("plutil", ["-convert", "xml1", "-o", "-", plist], { encoding: "utf8" });
    for (const ext of ["bimble", "bimbleview", "ifc"]) check(`file type .${ext}`, xml.includes(`<string>${ext}</string>`));
    const cs = sh("codesign", ["--verify", "--strict", "--deep", "--verbose=2", app]);
    check("codesign --verify --strict", cs.code === 0, cs.out.trim().split("\n").pop());
    const dv = sh("codesign", ["-dv", app]).out;
    check("signature", /Signature=adhoc|Authority=Developer ID Application/.test(dv), (dv.match(/Signature=\S+|Authority=Developer ID[^\n]*/) || ["?"])[0]);
    const libs = sh("otool", ["-L", exe]).out.split("\n").slice(1).map((l) => l.trim().split(" ")[0]).filter(Boolean);
    const outside = libs.filter((l) => !l.startsWith("/usr/lib/") && !l.startsWith("/System/Library/"));
    check("otool -L: only the system's libraries", outside.length === 0, outside.length ? outside.join(", ") : `${new Set(libs).size} entries`);
    const archs = sh("lipo", ["-archs", exe]).out.trim();
    const want = arch === "universal" ? "x86_64 arm64" : arch;
    check("architectures", archs.split(" ").sort().join(" ") === want.split(" ").sort().join(" "), `${archs} (want ${want})`);
    const lc = sh("otool", ["-l", exe]).out;
    const minos = [...lc.matchAll(/minos (\S+)/g)].map((m) => m[1]);
    check("minimum macOS (LC_BUILD_VERSION)", minos.length > 0 && minos.every((m) => m === MINOS), minos.join(", "));
    const host = execFileSync("uname", ["-m"], { encoding: "utf8" }).trim();
    const run = arch === "x86_64" && host === "arm64" ? sh("arch", ["-x86_64", exe, "--version"]) : sh(exe, ["--version"]);
    check("bimble --version", run.code === 0 && run.out.includes(`bimble ${V} (native)`), run.out.trim());
  }
} finally {
  if (att.code === 0) sh("hdiutil", ["detach", "-quiet", mnt]);
  rmSync(mnt, { recursive: true, force: true });
}
console.log(`macapp_test: ${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
