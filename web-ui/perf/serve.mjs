// A static server that gzips like GitHub Pages does, for make perf-web:
//   node perf/serve.mjs <dir> <port>
// .wasm/.js/.css/.html/.bimble go out gzipped (Content-Encoding: gzip), the rest as is.
import { createServer } from "node:http";
import { readFileSync, statSync } from "node:fs";
import { extname, join, normalize, resolve } from "node:path";
import { gzipSync } from "node:zlib";

const [dir, port] = process.argv.slice(2);
if (!dir || !port) {
  console.error("usage: serve.mjs <dir> <port>");
  process.exit(2);
}
const root = resolve(dir);
const TYPES = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript",
  ".mjs": "text/javascript",
  ".css": "text/css",
  ".wasm": "application/wasm",
  ".json": "application/json",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".woff2": "font/woff2",
  ".bimble": "application/octet-stream",
};
const GZIP = new Set([".html", ".js", ".mjs", ".css", ".wasm", ".json", ".svg", ".bimble"]);
// ponytail: gzip once per file and keep it; the files don't change during a run.
const cache = new Map();

createServer((req, res) => {
  let path = normalize(decodeURIComponent(new URL(req.url, "http://x").pathname));
  if (path.endsWith("/")) path += "index.html";
  const file = join(root, path);
  if (!file.startsWith(root)) return res.writeHead(403).end();
  try {
    if (!statSync(file).isFile()) throw new Error();
  } catch {
    return res.writeHead(404).end();
  }
  const ext = extname(file);
  const headers = { "Content-Type": TYPES[ext] ?? "application/octet-stream", "Cache-Control": "no-cache" };
  let body;
  if (GZIP.has(ext) && /\bgzip\b/.test(req.headers["accept-encoding"] ?? "")) {
    if (!cache.has(file)) cache.set(file, gzipSync(readFileSync(file)));
    body = cache.get(file);
    headers["Content-Encoding"] = "gzip";
    headers["Vary"] = "Accept-Encoding";
  } else body = readFileSync(file);
  res.writeHead(200, headers).end(body);
}).listen(Number(port), "127.0.0.1");
