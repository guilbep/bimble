// Open a model from a link (ticket URL1, ADR 0086): `<site>/#open=<url>` (canonical: the fragment
// never reaches the site's server, its logs or a Referer) or `<site>/?open=<url>`, with `name=<display
// name>` optional and `?view=1` for the viewer-only wasm. The page fetches the file IN THE BROWSER (the
// site is static: GitHub Pages) and opens it through the same path as a dropped file: the shell's
// `open {url}` (shell/machine.ts), its picker (files.ts pickUrl -> `fetchLink` here), then
// ccore.ts ccoreOpenFile (wasm) or POST /stage and `open {path, dropped}` (the native host: it has no
// HTTP client and gets none: no TLS stack in the host).
// - The address: https: only; http: only on localhost, 127.0.0.1, [::1] or the page's own origin (development);
//   a relative address resolves against the page (the site's samples/). data:, blob:, file:,
//   javascript: and the rest are refused before anything is fetched.
// - Another origin than the page's asks first (`ask`, drawn by ShellNotice in the `opening` state):
//   "Open a model from <host>?"; Open is remembered per origin for this tab's session (sessionStorage).
// - fetch: credentials omitted (no cookie, no HTTP auth), no referrer, redirects followed (the
//   browser's 20 at most), streamed with progress (`progress`), aborted by the shell's Cancel, 30 s
//   for the answer to start and 60 s at most between two chunks, the size limit `BROWSER_MAX`
//   before (Content-Length) and during the download (bytes counted as they come).
// - What came is decided by its first bytes, never by the URL's extension or the Content-Type:
//   ISO-10303-21 (an IFC), BIMBLEVW (a view file), bimble\0 (a .bimble v2), `{` (a .bimble v1 /
//   .json model), gzip (inflated to look: a .bimbleview stays as it is, a gzipped IFC or JSON is
//   inflated, bounded). An HTML page, a zip (.ifczip) or anything else is refused and named.
// - Each failure has its own message (i18n parts/openurl.ts); the shell shows it with Try again,
//   Back to Home and Copy link (ShellNotice).
// - A model opened from a link: File -> Copy link (`modelLink`), and Home's list of links opened
//   (`recentLinks`, localStorage: the address and a name, never the bytes).
import { computed, shallowRef, watch } from "vue";
import { t } from "./i18n";
import { ready, state } from "./bimble";
import { here, send } from "./shell/send";

/** The largest file the browser tab opens from a link: 100 MB (ADR 0086, measured: a 56 MB IFC took
 *  the tab's wasm memory to 515 MB and the renderer 0.6 GB above an empty page; a 229 MB IFC to 1.57 GB
 *  and 2.5 GB, and no picture after 7 min on the 16 GB dev machine shared with other builds). */
export const BROWSER_MAX = 100_000_000;
const HEAD_MS = 30000, STALL_MS = 60000;
const ALLOWED_KEY = "bimble.openurl.allowed", RECENT_KEY = "bimble.recentLinks", RECENT_MAX = 8;

// ---- the link in the address ----

/** The link of the address: `#open=` first (canonical), else `?open=`; with its `name=`. */
export function linkInAddress(): { url: string; name: string | null } | null {
  const h = new URLSearchParams(location.hash.slice(1)), q = new URLSearchParams(location.search);
  const url = h.get("open") ?? q.get("open");
  if (!url) return null;
  return { url, name: h.get("name") ?? q.get("name") };
}

const LOCAL = new Set(["localhost", "127.0.0.1", "[::1]"]);
/** The absolute address of `raw` (relative: against the page), or the reason it is refused. */
export function resolveLink(raw: string): URL | string {
  let u: URL;
  try { u = new URL(raw.trim(), document.baseURI); } catch { return t("openurl.err.invalid"); }
  if (u.protocol === "https:" || (u.protocol === "http:" && (LOCAL.has(u.hostname) || u.origin === location.origin))) {
    if (u.username || u.password) return t("openurl.err.credentials"); // never sent, never kept
    return u;
  }
  return t("openurl.err.scheme", { scheme: u.protocol.replace(/:$/, "") });
}

/** The address of `raw` as the shell keeps it: absolute when it resolves, else as typed (its error comes later). */
const absolute = (raw: string): string => { const u = resolveLink(raw); return typeof u === "string" ? raw.trim() : u.href; };

/** A display name given with the link (`name=`), per resolved address. */
const names = new Map<string, string>();

/** Open `raw` through the shell (Home's field, a recent link, the address). False: refused now. */
export function openLink(raw: string, name: string | null = null): boolean {
  const url = absolute(raw);
  if (name) names.set(url, name);
  return send({ do: "open", url });
}

let started = false;
/** At start (files.ts installFiles): the address's link, opened once. */
export function openFromAddress(): void {
  if (started) return;
  started = true;
  const l = linkInAddress();
  if (l) openLink(l.url, l.name);
}

// ---- the question for another origin ----

export interface Ask { url: string; host: string; answer: (ok: boolean) => void }
/** The question shown (ShellNotice), or null. */
export const ask = shallowRef<Ask | null>(null);

function allowedOrigins(): string[] {
  try { return JSON.parse(sessionStorage.getItem(ALLOWED_KEY) ?? "[]") as string[]; } catch { return []; }
}
function allow(origin: string): void {
  try { sessionStorage.setItem(ALLOWED_KEY, JSON.stringify([...new Set([...allowedOrigins(), origin])])); } catch { /* asked again next time */ }
}
/** Whether `u` opens without the question: the page's own origin, or one allowed in this session. */
export const trusted = (u: URL): boolean => u.origin === location.origin || allowedOrigins().includes(u.origin);

function askFor(u: URL, current: () => boolean): Promise<boolean> {
  return new Promise((resolve) => {
    let stop: (() => void) | null = null;
    const a: Ask = { url: u.href, host: u.host, answer: (ok) => {
      stop?.();
      if (ask.value === a) ask.value = null;
      if (ok) allow(u.origin);
      resolve(ok);
    } };
    ask.value = a;
    stop = watch(here, () => { if (!current()) a.answer(false); }); // Cancel, or the shell moved on
  });
}

// ---- the download ----

export interface Progress { url: string; host: string; loaded: number; total: number | null }
/** The download under way (ShellNotice's bar), or null. */
export const progress = shallowRef<Progress | null>(null);

const MB = (n: number) => (n < 1e3 ? `${n} bytes` : n < 1e6 ? `${Math.round(n / 1e3)} kB` : `${(n / 1e6).toFixed(n < 1e7 ? 1 : 0)} MB`);
export const formatBytes = MB;

class LinkError extends Error {}

/** Why a fetch failed with no answer (the browser says nothing more): offline; the server answers a
 *  request that does not ask to read (no-cors, HEAD: an opaque answer) -> it refuses other sites
 *  (CORS); no answer at all -> unreachable. */
async function whyNoAnswer(u: URL): Promise<string> {
  if (navigator.onLine === false) return t("openurl.err.offline");
  try {
    await fetch(u.href, { method: "HEAD", mode: "no-cors", credentials: "omit", referrerPolicy: "no-referrer", signal: AbortSignal.timeout(10000) });
    return t("openurl.err.cors", { host: u.host, origin: location.origin });
  } catch {
    return t("openurl.err.network", { host: u.host });
  }
}

function statusMessage(u: URL, r: Response): string {
  const st = `${r.status}${r.statusText ? ` ${r.statusText}` : ""}`;
  const k = r.status === 404 || r.status === 410 ? "notFound" : r.status === 401 || r.status === 403 ? "denied" : r.status >= 500 ? "server" : "other";
  return t(`openurl.err.status.${k}` as "openurl.err.status.other", { host: u.host, status: st });
}

type Kind = "ifc" | "view" | "bimble" | "json" | "gzip" | "zip" | "html" | "xml" | "empty" | "other";
const ascii = (b: Uint8Array, n: number) => String.fromCharCode(...b.subarray(0, n));
/** What the first bytes are (a BOM and leading white space skipped for the text forms). */
export function sniff(b: Uint8Array): Kind {
  if (b.length === 0) return "empty";
  if (b[0] === 0x1f && b[1] === 0x8b) return "gzip";
  if (ascii(b, 4) === "PK\x03\x04") return "zip";
  if (ascii(b, 8) === "BIMBLEVW") return "view";
  if (ascii(b, 7) === "bimble\0") return "bimble";
  let i = b[0] === 0xef && b[1] === 0xbb && b[2] === 0xbf ? 3 : 0;
  while (i < b.length && (b[i] === 0x20 || b[i] === 0x09 || b[i] === 0x0a || b[i] === 0x0d)) i++;
  const head = ascii(b.subarray(i), Math.min(64, b.length - i));
  if (head.startsWith("ISO-10303-21")) return "ifc";
  if (head.startsWith("{")) return "json";
  if (/^<(!doctype html|html|head|body|!--)/i.test(head)) return "html";
  if (head.startsWith("<")) return "xml";
  return "other";
}

/** The first bytes as text a person can read (what was received), 60 characters at most. */
function excerpt(b: Uint8Array): string {
  return ascii(b, Math.min(60, b.length)).replace(/[^\x20-\x7e]/g, "·");
}

/** Inflates gzip bytes, `max` bytes out at most (a gzip bomb stops here); `head`: only the first chunk. */
async function gunzip(blob: Blob, max: number, head = false): Promise<Uint8Array> {
  const reader = blob.stream().pipeThrough(new DecompressionStream("gzip")).getReader();
  const parts: Uint8Array[] = [];
  let n = 0;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      parts.push(value);
      n += value.length;
      if (head) break;
      if (n > max) throw new LinkError(t("openurl.err.tooLargeInflated", { limit: MB(max) }));
    }
  } catch (e) {
    if (e instanceof LinkError) throw e;
    throw new LinkError(t("openurl.err.badGzip"));
  } finally {
    void reader.cancel().catch(() => undefined);
  }
  const out = new Uint8Array(n);
  let at = 0;
  for (const p of parts) { out.set(p, at); at += p.length; }
  return out;
}

const EXT: Record<"ifc" | "view" | "bimble" | "json", string> = { ifc: ".ifc", view: ".bimbleview", bimble: ".bimble", json: ".json" };
/** The file's name: `name=`, else the address's last segment; sanitised, with the extension of what it is. */
export function fileName(u: URL, kind: keyof typeof EXT, given: string | null): string {
  let base = given ?? "";
  if (!base) { try { base = decodeURIComponent(u.pathname.split("/").pop() ?? ""); } catch { base = u.pathname.split("/").pop() ?? ""; } }
  base = base.replace(/[\u0000-\u001f\u007f/\\:*?"<>|]/g, "_").trim().slice(0, 100);
  base = base.replace(/(\.ifc)?\.(gz|ifczip|zip)$/i, "").replace(/\.(ifc|bimbleview|bimble|json)$/i, "");
  if (kind === "json" && /\.bimble$/i.test(u.pathname)) return `${base || "model"}.bimble`; // a v1 text .bimble keeps its name
  return `${base || "model"}${EXT[kind]}`;
}

/** The answer of the shell's picker for `open {url}`: the file as a dropped one (wasm: its bytes,
 *  then ccoreOpenFile), or null when the question was answered Cancel. Throws the message to show.
 *  `current()`: whether the shell still waits for this open (false: Cancel, aborts the download). */
export async function fetchLink(raw: string, current: () => boolean): Promise<File | null> {
  const u = resolveLink(raw);
  if (typeof u === "string") throw new LinkError(u);
  if (!trusted(u) && !(await askFor(u, current))) return null;
  const ctl = new AbortController();
  const stop = watch(here, () => { if (!current()) ctl.abort(); });
  let timer = 0, timedOut = false;
  const arm = (ms: number) => { clearTimeout(timer); timer = window.setTimeout(() => { timedOut = true; ctl.abort(); }, ms); };
  const p: Progress = { url: u.href, host: u.host, loaded: 0, total: null };
  progress.value = p;
  try {
    arm(HEAD_MS);
    let r: Response;
    try {
      r = await fetch(u.href, { credentials: "omit", referrerPolicy: "no-referrer", redirect: "follow", mode: "cors", signal: ctl.signal });
    } catch (e) {
      if (!current()) throw e;
      if (timedOut) throw new LinkError(t("openurl.err.timeout", { host: u.host, s: HEAD_MS / 1000 }));
      throw new LinkError(await whyNoAnswer(u));
    }
    if (!r.ok) throw new LinkError(statusMessage(u, r));
    const declared = Number(r.headers.get("content-length") ?? "");
    if (declared > BROWSER_MAX) {
      ctl.abort();
      throw new LinkError(t("openurl.err.tooLarge", { size: MB(declared), limit: MB(BROWSER_MAX) }));
    }
    // Content-Length counts the bytes sent: a compressed answer (Content-Encoding, decoded by the
    // browser) reads more. The bar then shows the bytes so far.
    const enc = (r.headers.get("content-encoding") ?? "identity").toLowerCase();
    const total = declared > 0 && enc === "identity" ? declared : null;
    progress.value = { ...p, total };
    const chunks: Uint8Array[] = [];
    let loaded = 0, kind: Kind | null = null;
    const head = () => { const h = new Uint8Array(Math.min(loaded, 64)); let at = 0; for (const c of chunks) { if (at >= h.length) break; h.set(c.subarray(0, h.length - at), at); at += Math.min(c.length, h.length - at); } return h; };
    const refuse = (k: Kind, first: Uint8Array): never => {
      ctl.abort();
      const type = r.headers.get("content-type") ?? "?";
      if (k === "html") throw new LinkError(t("openurl.err.html", { host: u.host }));
      if (k === "zip") throw new LinkError(t("openurl.err.zip", { host: u.host }));
      if (k === "empty") throw new LinkError(t("openurl.err.empty", { host: u.host }));
      throw new LinkError(t("openurl.err.notModel", { host: u.host, type, start: excerpt(first) }));
    };
    const reader = r.body?.getReader();
    arm(STALL_MS);
    for (;;) {
      let step: ReadableStreamReadResult<Uint8Array>;
      try { step = reader ? await reader.read() : { done: true, value: undefined }; } catch (e) {
        if (!current()) throw e;
        if (timedOut) throw new LinkError(t("openurl.err.stalled", { host: u.host, s: STALL_MS / 1000 }));
        throw new LinkError(t("openurl.err.cut", { host: u.host, got: MB(loaded) }));
      }
      if (step.done) break;
      arm(STALL_MS);
      loaded += step.value.length;
      if (loaded > BROWSER_MAX) {
        ctl.abort();
        throw new LinkError(t("openurl.err.tooLargeRead", { limit: MB(BROWSER_MAX) }));
      }
      chunks.push(step.value);
      if (kind === null && loaded >= 64) {
        kind = sniff(head());
        if (kind !== "ifc" && kind !== "view" && kind !== "bimble" && kind !== "json" && kind !== "gzip") refuse(kind, head());
      }
      progress.value = { ...p, total, loaded };
    }
    clearTimeout(timer);
    if (kind === null) kind = sniff(head());
    let blob = new Blob(chunks as BlobPart[]);
    chunks.length = 0;
    if (kind === "gzip") {
      const inner = await gunzip(blob, BROWSER_MAX, true);
      const k = sniff(inner);
      if (k === "ifc" || k === "json") blob = new Blob([await gunzip(blob, BROWSER_MAX) as BlobPart]);
      else if (k !== "view") refuse(k === "other" || k === "empty" ? "other" : k, inner);
      kind = k;
    }
    if (kind !== "ifc" && kind !== "view" && kind !== "bimble" && kind !== "json") return refuse(kind, new Uint8Array(await blob.slice(0, 64).arrayBuffer()));
    let named = u; // after redirects: the file's own address names it (a short link's target)
    try { if (r.url) named = new URL(r.url); } catch { /* the link's */ }
    const file = new File([blob], fileName(named, kind, names.get(raw) ?? names.get(u.href) ?? null));
    await ready; // the module is loaded (a link opened at start downloads while the wasm loads)
    return file;
  } finally {
    clearTimeout(timer);
    stop();
    if (progress.value?.url === u.href) progress.value = null;
  }
}

// ---- a model opened from a link: Copy link, the recent links ----

/** The link that opens `fileUrl` on this site: `#open=` (canonical), relative when on this site,
 *  `?view=1` kept when the page is the viewer-only build. */
export function shareLink(fileUrl: string): string {
  const page = new URL(location.pathname, location.origin);
  if (new URLSearchParams(location.search).get("view") === "1") page.search = "?view=1";
  let target = fileUrl;
  try {
    const f = new URL(fileUrl), dir = new URL(".", page.href);
    if (f.origin === location.origin && f.pathname.startsWith(dir.pathname)) target = f.pathname.slice(dir.pathname.length) + f.search;
  } catch { /* kept as it is */ }
  page.hash = `open=${encodeURIComponent(target)}`;
  return page.href;
}

export interface RecentLink { url: string; name: string; at: number }
function readRecent(): RecentLink[] {
  try {
    const v = JSON.parse(localStorage.getItem(RECENT_KEY) ?? "[]") as RecentLink[];
    return Array.isArray(v) ? v.filter((x) => typeof x?.url === "string" && typeof x.name === "string").slice(0, RECENT_MAX) : [];
  } catch { return []; }
}
/** Home's links opened, newest first (this browser only; the address and a name, never the bytes). */
export const recentLinks = shallowRef<RecentLink[]>(readRecent());
function keep(list: RecentLink[]): void {
  recentLinks.value = list;
  try { localStorage.setItem(RECENT_KEY, JSON.stringify(list)); } catch { /* this session only */ }
}
export const forgetLink = (url: string): void => keep(recentLinks.value.filter((x) => x.url !== url));

/** The model open now came from this link (its address and the file name it opened under). */
const opened = shallowRef<{ url: string; file: string } | null>(null);
watch(here, (s, prev) => {
  if (prev.at !== "opening" || s.at !== "model") return;
  if (prev.url === undefined) { opened.value = null; return; }
  const file = state.value.file ?? "";
  opened.value = { url: prev.url, file };
  keep([{ url: prev.url, name: file.split(/[\\/]/).pop() || prev.url, at: Date.now() }, ...recentLinks.value.filter((x) => x.url !== prev.url)].slice(0, RECENT_MAX));
});
/** File -> Copy link: the link of the model open now, or null when it did not come from a link. */
export const modelLink = computed(() => (opened.value && state.value.file === opened.value.file ? shareLink(opened.value.url) : null));

/** Copies `text` (Copy link); false when the browser refused. */
export async function copyText(text: string): Promise<boolean> {
  try { await navigator.clipboard.writeText(text); return true; } catch { return false; }
}
