# ADR 0086 — Open a model from a link in the web viewer

- Status: Accepted, 27 Nov 2026
- Decided by: Pierre asked for it ("you're right we should be able to open an ifc url in the web
  viewer") and delegated the design on 27 Nov 2026; the choices below by Claude on ticket URL1.
- Concerns: ADR 0020 (the web version, GitHub Pages), ADR 0069 (the viewer-only build), ADR 0073
  (Pages previews on the production origin), ADR 0074 (200 ms), files as untrusted input (CLAUDE.md).
- Reversal cost: low (page code: `web-ui/src/openurl.ts`, rows in `shell/machine.ts`, Home's field,
  File → Copy link; no file format, no core action, no golden).

## Context

The product split of 27 Nov: the native app is the tool for big folders; the web lab stays for what a
browser is uniquely good at: opening a model from a link with nothing to install, sharing a view,
small and medium models. The site is static (GitHub Pages): no server can fetch for the page, so the
visitor's browser fetches the file itself, under the browser's rules (CORS), and the link is untrusted
input made by someone else.

## Decision

1. **The link.** `https://<site>/#open=<url>` is canonical; `https://<site>/?open=<url>` works too.
   The fragment is never sent to the site's server: a private file's address (a signed URL with a
   token, an intranet path) stays out of the request line, GitHub's logs and any `Referer`. The query
   form stays for tools that drop fragments, and for the site's own samples (README:
   `…/?open=samples/frame.ifc`). Both take `name=<display name>` (sanitised, shown as text). `?view=1`
   (the viewer-only wasm, ADR 0069) stays a query parameter: it is not private and the page's preload
   reads it before any script. File → Copy link writes `<site>/[?view=1]#open=<encoded url>`, relative
   when the file is on the site. The value is URL-encoded (`encodeURIComponent`): a raw address with its
   own `&` must be encoded, which Copy link does.
2. **One path for every model.** A link is the shell's `open {url}` (`shell/machine.ts`: `opening`,
   `confirmClose` and `error` carry the address); its picker is `files.ts pickUrl`, which fetches
   (`openurl.ts fetchLink`) and hands the file on exactly as a dropped one: its bytes to
   `ccoreOpenFile` (wasm: read → parse → scene → mesh, cancellable before the parse), or, on the
   native host build, `POST /stage` then `open {path, dropped}`. A link in the address is sent at start
   (`installFiles`), before the wasm is loaded: the `opening` card is the first thing drawn, the
   download runs while the wasm loads, and the start opens no demo model (no `frame.json` fetched).
3. **The fetch.** `credentials: "omit"` (no cookie, no HTTP auth; an address with `user:pass@` is
   refused), `referrerPolicy: "no-referrer"`, `mode: "cors"`, redirects followed (the browser's 20 at
   most; the file is named after the final address). Only `https:`; `http:` only on `localhost`,
   `127.0.0.1`, `[::1]` and the page's own origin (development); `data:`, `blob:`, `file:`,
   `javascript:`, `ftp:` and the rest are refused before anything is fetched. Streamed
   (`ReadableStream`): progress from `Content-Length` when given ("x of y"), else bytes so far; the
   shell's Cancel aborts the request (the server sees the connection closed). 30 s for the answer to
   start, 60 s at most between two chunks. `Content-Encoding` is the browser's (then the bar counts
   bytes so far).
4. **What came is decided by its first bytes**, never by the URL's extension or the `Content-Type`
   (the test serves an IFC as `text/plain` and a view file as `text/html`): `ISO-10303-21` an IFC,
   `BIMBLEVW` a view file, `bimble\0` a `.bimble` v2, `{` a `.bimble` v1 / `.json` model (the core says
   whether it is one), gzip inflated to look (a `.bimbleview` stays gzipped for the core; a gzipped
   IFC or JSON, `.ifc.gz`, is inflated in the page, bounded by the same limit). Refused and named: an
   HTML page ("a sign-in page or an error page?"), a zip (`.ifczip`: not read in the browser yet;
   unzip it and drop the `.ifc`), an empty file, anything else (its `Content-Type` and first 60
   characters). The core then reads the bytes as any untrusted file (its own budgets).
5. **The size limit: 100 MB in the browser** (`BROWSER_MAX`, decimal), checked against
   `Content-Length` before the download and against the bytes counted during it (and after a gzip
   is inflated); above it: "Too large for the browser: <size> (at most 100 MB here). Open it in the
   Bimble app." with Copy the file's address. The core's own wasm32 limits are higher (IFC and view
   file 1 GiB, `.bimble` 256 MiB); the tab's memory decides (measured below: about 9 bytes of wasm
   memory per IFC byte, and the page's copies on top). The native host build stages at most 256 MB
   (`FS_STAGE_MAX`): the browser's 100 MB comes first there too.
6. **Honest errors**, each its own message (FR/EN, `i18n/parts/openurl.ts`), shown by the shell's
   `error` state with Copy the file's address, Back to Home and Try again (an open of the same link
   from the screen under the error, never nested in an earlier question): CORS refusal (the browser
   says nothing more: a second request that does not ask to read, `no-cors` `HEAD`, gets an opaque
   answer → the server is there and refuses other sites: "<host> does not allow opening its files
   from another site", with what the file's owner can do: allow this origin with
   `Access-Control-Allow-Origin`, or download the file and drop it); no answer at all → "<host> cannot
   be reached"; offline; a timeout; a broken download; 404/410, 401/403 (a sign-in link cannot open
   from here), 5xx and other statuses with the status line; too large; not a model; the core's own
   parse error (`<file>: <message>`, as a dropped file's).
7. **Another site asks first.** For an origin other than the page's, the `opening` state shows
   "Open a model from <host>?" with the full address, [Cancel] [Open], before any request; Open is
   remembered per origin for the tab's session (`sessionStorage`); the site's own files (the samples)
   open without it. Cancel is the shell's (back where it came from).
   - **What it protects against:** a link someone else made cannot make a visitor's browser send a
     request to an arbitrary address (another site, an intranet host the visitor can reach) without a
     click that names the host; a GET with side effects on a careless server, or a probe of the
     visitor's network, needs the visitor's consent. Even then the request carries no cookie, no
     credential and no referrer, and the answer is readable only if that server allows other sites.
   - **What it does not:** a visitor who clicks Open without reading; the error message tells the
     visitor (only the visitor: the page sends nothing anywhere) whether the host answered, so a link
     plus a click is a one-host probe; a public site's server sees the visitor's IP address and the
     time; Chrome's Private Network Access rules (a preflight from a public site to a private address)
     are the browser's, not ours; the answer, once read, is parsed by the core like any dropped file.
8. **The rest of the address cannot act** (the audit of 26 Nov, C9): in a built page (the published
   site, the desktop build) `?action=` is ignored, and `?edit=` too when the page opens a model by
   address (`open=`, `model=`); they stay on the dev server for the lab tests, and `edit=` on the
   built-in demo for `lab_pages` (the viewer-only build refuses the verb). In a built page
   `?model=` and `?ccore_wasm=` name a file of the site only (another origin is ignored with a
   warning: the old `?model=<url>` fetched any address without a click), and the preload script in
   `index.html` preloads only files of the site, and no demo model when a link opens one. The display
   name is text (Vue escapes it); nothing from the address is evaluated.
9. **Sharing from the app.** A model opened from a link: File → Copy link (shown only then). Home has
   a field "Open from a link…" (paste, Enter) and a list "Opened from a link" (`localStorage`
   `bimble.recentLinks`, 8 at most: the address and a name, never the bytes; a click fetches again;
   × removes). The address is kept in this browser; a signed address keeps its token there, and every
   page of the origin `guilbep.github.io` can read that storage (ADR 0073's risk): × removes it.
10. **The native host build** (the desktop app, `?native=1`): the host is a server and has no HTTP
    client; it gets none (no TLS stack in the host). The field and the link work exactly as on the
    web: the page fetches (the same rules, the same question, CORS applies to the page's origin),
    then stages the bytes as a dropped file (`POST /stage`, 256 MB at most) and the host opens the
    copy. Opening a URL with the host's own client (no CORS) is a follow-up if the desktop app needs
    it, with a TLS library chosen then.
11. **Samples on the site.** `samples/frame.ifc` and `samples/frame.bimbleview`, the 72-element demo
    frame (synthetic) copied from `ccore/golden/` by `copy-ccore.mjs` at build time (the Pages job
    checks they are there); `?open=samples/frame.ifc` is same-origin: no question.

## Evidence

`ccore/test/lab_open_url.mjs` (the production build served as Pages serves it, a second origin, one
headless Chromium, SwiftShader; 27 Nov, the dev machine, 16 GB aarch64, loaded by other builders):
54 checks pass; on the old code (`web-ui/` at `1e091bd`) it fails at once: no opening card, the
demo `frame.json` opens instead of the link's file. Numbers from four runs:

| What | Measured |
|---|---|
| Page start → the `opening` card (`#open=samples/frame.ifc`, page's part, `lib/input_time.mjs`) | 66, 110, 115, 172 ms (≤ 200 asserted; 172 with the machine loaded by other builds) |
| Page start → the question for another origin | 54 to 61 ms |
| Home's field, Enter → the `opening` card | 2 to 6 ms |
| Page start → first picture, `#open=samples/frame.ifc`, full wasm (a fresh browser: SwiftShader compiles its pipelines) | 2.3 to 2.8 s |
| Page start → first picture, `?view=1#open=samples/frame.ifc` | 0.80 to 1.15 s |
| A first-time visitor's transfer for `?view=1#open=samples/frame.ifc` (12 requests) | 1,565,642 bytes raw; 592,723 bytes gzip -9 as Pages serves it: `ccore-view.wasm` 563,533 (262,502), `index.js` 619,688 (176,309), `glviewport.js` 127,004 (44,753), CSS 101,374 (14,742), five fonts 76,228, `ccore.js` 8,542 (3,174), `frame.ifc` 68,059 (14,224), `index.html` 1,214 |

Identity: the model opened from the link equals the same file dropped on the page (status counts,
model bytes, the mesh's bytes and triangles drawn, the viewport's pixels hashed), and so do the
other origin's copy, the gzip-encoded answer, the `.ifc.gz`, the redirect, `?view=1` and the native
host's staged copy. Each failure reaches its message; Cancel and the size limit close the connection
(the server sees it: 64 KB sent of a declared 100 MB + 1; 104,857,600 bytes of an undeclared stream
the server would have sent to 300 MiB). No request reaches another origin before Open; a refused
scheme fetches nothing.

Memory of a link's open in a tab (headless Chromium, the full wasm, synthetic IFC files made with
`scripts/repeat_ifc.py` from `golden/ifc/frame.ifc`, served from localhost; peak while opening):
| File | Wasm memory | Renderer RSS peak (above an empty tab) | Page start → first picture |
|---|---|---|---|
| 55,611,620 bytes, 52,920 elements, 635,040 triangles | 515 MB | 1,451 MB (+0.6 GB); GPU process 266 MB | 16.0 s (SwiftShader), 23.8 s wall to ready |
| 228,593,506 bytes, 211,608 elements, 2,539,296 triangles | 1,566 MB | 2,503 MB (+1.67 GB); then swapped out (1.7 GB seen in swap) | none after 468 s: the machine (16 GB, shared with other builds, 4 GB free at the start) swapped the tab |

About 9 bytes of wasm memory per IFC byte, plus the page's copies (the download's chunks, the
`File`, the bytes handed to the module). The 200 MB file parses but does not reach a picture on the
dev machine under load; a phone or an 8 GB laptop has less. Hence 100 MB (decision 5): about 0.9 GB
of wasm memory and 1.5 GB in the tab, projected from the two points; larger files are the app's.
Measured with a scratch script (not committed); the files were deleted after.

## Revisit when

- A user needs `.ifczip` (a small zip reader over `DecompressionStream("deflate-raw")`), or larger
  files in the browser (a streaming parse: the page would not hold the bytes, the wasm would not copy
  them; or wasm64).
- Sharing a view (follow-up): `#open=<url>&camera=<eye,target,up>&section=<box>&hide=<ids|classes>&view=<view id>`
  needs the camera's state readable and settable by the page (the viewport's camera as numbers, in
  the scene's mm), the section box and visibility as view rules the core already keeps (ADR 0081: hide
  and isolate as view rules) applied after the open, a canonical short form (element ids can be many:
  a class list, or a hash of the hidden set with the list in the fragment), and the same guard as
  here: the parameters are data (numbers and ids parsed, never evaluated), and a shared view runs no
  action.
- The desktop app needs to open a URL without CORS: the host's own HTTP client (TLS, redirects ≤ 5,
  a timeout, the same limits).
