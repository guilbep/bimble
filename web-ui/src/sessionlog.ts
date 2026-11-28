// The web session log (ticket VIS2, the owner: "so that when I try the app you have observability"). With the NATIVE
// HOST only (?native=1: the page talks to a host on this machine): the page posts what it measures to the host, which
// writes it under $XDG_STATE_HOME/bimble/web-sessions/<UTC>-<pid>/ (ccore/host/server.c, POST /session_log: the
// native app's session folder's shape, ccore/native/ui/slog.c):
//   session.log   the page's start (build, browser, renderer, window, settings in force), errors and warnings
//   frames.csv    one row a second while frames are drawn: frames, frame ms median / p95 / max, input -> frame
//                 median / max, triangles and draws of the last frame, GPU MB, moving or still
//   inputs.csv    inputs answered in more than 100 ms (what, ms, at)
//   report.txt    the Performance report, every 10 s while frames are drawn (replaced)
// Local only (127.0.0.1, the host's token and Origin rules); the setting session_log (on by default with the host; off:
// the host writes nothing and answers 204); nothing at all in the pure web / wasm build (this module is not loaded).
import { shallowRef } from "vue";
import { onPerfFrame, onPerfInput, perfReport } from "./perf";

/** The folder the host writes this session into ("" until asked; null: off or not served). Settings shows it. */
export const sessionLogDir = shallowRef<string | null>("");

const SLOW_INPUT_MS = 100, REPORT_MS = 10000;
let base = "", token = "", on = false;
const queue: { file: string; text: string; replace?: boolean }[] = [];
let sending = false;

async function flush(): Promise<void> {
  if (sending || !on) return;
  sending = true;
  try {
    while (queue.length) {
      // one request a file: the queued lines of the same file together (replace: the last one alone)
      const head = queue[0]!, same = head.replace ? [queue.shift()!] : [];
      if (!head.replace) for (let i = 0; i < queue.length;) { if (queue[i]!.file === head.file && !queue[i]!.replace) same.push(queue.splice(i, 1)[0]!); else i++; }
      const r = await fetch(`${base}/session_log?t=${encodeURIComponent(token)}&file=${head.file}${head.replace ? "&mode=replace" : ""}`,
        { method: "POST", body: same.map((x) => x.text).join(""), headers: { "Content-Type": "text/plain" } }).catch(() => null);
      if (!r || r.status === 204) { on = false, queue.length = 0, sessionLogDir.value = null; break; } // off (or the host gone)
    }
  } finally { sending = false; }
}
const post = (file: string, text: string, replace = false) => {
  if (!on) return;
  if (queue.length > 2000) queue.splice(0, queue.length - 2000); // (bounded while the host is slow)
  queue.push({ file, text, replace });
  void flush();
};
const stamp = () => new Date().toISOString();
const line = (s: string) => post("session.log", `${stamp()} ${s.replace(/\s+$/, "")}\n`);

const q = (xs: number[], p: number) => { if (!xs.length) return 0; const s = [...xs].sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.floor(p * s.length))]!; };

/** Starts the log against the native host at `hostBase` (http://127.0.0.1:port) with its token. Once. */
export async function startSessionLog(hostBase: string, hostToken: string): Promise<void> {
  if (base) return;
  base = hostBase, token = hostToken;
  try {
    const r = await fetch(`${base}/session_log?t=${encodeURIComponent(token)}`, { cache: "no-store" });
    const j = r.status === 200 ? ((await r.json()) as { on: boolean; dir: string | null }) : null;
    on = !!j?.on, sessionLogDir.value = on ? j!.dir : null;
  } catch { on = false, sessionLogDir.value = null; }
  if (!on) return;
  const c = document.getElementById("bimble") as HTMLCanvasElement | null;
  line(`page start: build ${typeof __BB_COMMIT__ === "string" ? __BB_COMMIT__ : "?"}, ${navigator.userAgent}`);
  line(`window ${innerWidth}x${innerHeight} css px, dpr ${devicePixelRatio}, canvas ${c?.width ?? 0}x${c?.height ?? 0}; address ${location.pathname}${location.search.replace(/([?&])token=[^&]*/, "$1token=…")}`);
  post("frames.csv", "time,frames,frame_ms_median,frame_ms_p95,frame_ms_max,input_ms_median,input_ms_max,triangles,draws,gpu_mb,state\n");
  post("inputs.csv", "time,input,ms\n");
  // errors and warnings
  addEventListener("error", (e) => line(`error: ${e.message} (${e.filename}:${e.lineno}:${e.colno})`));
  addEventListener("unhandledrejection", (e) => line(`unhandled rejection: ${String((e.reason as Error)?.stack ?? e.reason)}`));
  for (const k of ["error", "warn"] as const) {
    const orig = console[k].bind(console);
    console[k] = (...a: unknown[]) => { orig(...a); line(`console.${k}: ${a.map((x) => (x instanceof Error ? x.stack ?? x.message : typeof x === "string" ? x : JSON.stringify(x))).join(" ").slice(0, 4000)}`); };
  }
  // the frames of each second, the inputs
  let sec: { ms: number[]; inp: number[]; moving: boolean; tris: number; draws: number } = { ms: [], inp: [], moving: false, tris: 0, draws: 0 };
  onPerfFrame((f) => { sec.ms.push(f.ms), sec.moving ||= f.moving, sec.tris = f.tris, sec.draws = f.draws; });
  onPerfInput((i) => { sec.inp.push(i.ms); if (i.ms > SLOW_INPUT_MS) post("inputs.csv", `${stamp()},${JSON.stringify(i.what)},${i.ms.toFixed(1)}\n`); });
  let lastReport = 0, drewSince = false;
  setInterval(() => {
    const s = sec;
    sec = { ms: [], inp: [], moving: false, tris: 0, draws: 0 };
    if (!s.ms.length) return;
    drewSince = true;
    const gpu = window.__bbGl?.gpuBytes ?? 0;
    post("frames.csv", `${stamp()},${s.ms.length},${q(s.ms, 0.5).toFixed(1)},${q(s.ms, 0.95).toFixed(1)},${Math.max(...s.ms).toFixed(1)},${s.inp.length ? q(s.inp, 0.5).toFixed(1) : ""},${s.inp.length ? Math.max(...s.inp).toFixed(1) : ""},${s.tris},${s.draws},${(gpu / 1048576).toFixed(1)},${s.moving ? "moving" : "still"}\n`);
    if (drewSince && performance.now() - lastReport > REPORT_MS) { lastReport = performance.now(), drewSince = false; post("report.txt", perfReport(true, null), true); }
  }, 1000);
  addEventListener("pagehide", () => { line("page closed"); post("report.txt", perfReport(true, null), true); });
}
