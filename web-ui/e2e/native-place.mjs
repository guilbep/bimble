// Native twin end to end: place a slab through the HTTP API of a native Bimble process, then undo it.
// node e2e/native-place.mjs --endpoint http://127.0.0.1:<port> --token <t> [--no-b] [--n 1000]
//   --no-b  omit the second corner (proves the check fails: exit 1), as --one-click in place-slab.mjs
// The same steps as place-slab.mjs, with API verbs instead of clicks (the native process has no canvas
// in the browser). Exit 0 when one IfcSlab is added with corner (1000, 1000), Undo removes it, and the
// p95 of a query round trip is under 5 ms; 1 otherwise. Node globals only: fetch, performance.
const argv = process.argv.slice(2);
const opt = (name) => {
  const i = argv.indexOf(name);
  return i >= 0 ? argv[i + 1] : undefined;
};
const endpoint = (opt("--endpoint") ?? "").replace(/\/+$/, "");
const token = opt("--token");
const noB = argv.includes("--no-b");
const n = Number(opt("--n") ?? 1000);
if (!endpoint || !token || !Number.isInteger(n) || n < 1) {
  console.error("usage: native-place.mjs --endpoint http://127.0.0.1:<port> --token <t> [--no-b] [--n 1000]");
  process.exit(2);
}
const t = encodeURIComponent(token);
const port = new URL(endpoint).port || "<port>";
const HINT =
  `cannot reach a native Bimble process at ${endpoint}.\n` +
  `Start one with: BIMBLE_SERVE=${port} BIMBLE_TOKEN=<t> bimble view <file>   (the file must be frame.bimble)`;

class ApiError extends Error {}

/** POST /<op>/<name or ->?t=; 200 gives the text body, anything else throws ApiError with the body. */
async function call(op, name, body) {
  const url = `${endpoint}/${op}/${name === null ? "-" : encodeURIComponent(name)}?t=${t}`;
  const r = await fetch(url, { method: "POST", headers: { "Content-Type": "text/plain" }, body: body ?? "" });
  const text = await r.text();
  if (r.status !== 200) throw new ApiError(text || `${op}: HTTP ${r.status}`);
  return text;
}
const q = async (name, args = {}) => JSON.parse(await call("query", name, JSON.stringify(args)));
const slabs = () => q("elements", { class: "IfcSlab" });
const isConn = (e) => !(e instanceof ApiError) && (e?.name === "TypeError" || e?.cause?.code);

function fail(reason) {
  console.log(`FAIL: ${reason}`);
  process.exit(1);
}

function stats(ms) {
  const s = [...ms].sort((x, y) => x - y);
  const at = (p) => s[Math.min(s.length - 1, Math.ceil(p * s.length) - 1)];
  return { p50: at(0.5), p95: at(0.95), max: s[s.length - 1] };
}

async function main() {
  // 1. Wait for the process and its model.
  let state = null;
  let lastErr = null;
  const until = performance.now() + 20000;
  while (performance.now() < until) {
    try {
      state = await q("state");
      if (state.file === "frame.bimble") break;
      lastErr = new Error(`state.file is ${JSON.stringify(state.file)}, expected "frame.bimble"`);
    } catch (e) {
      lastErr = e;
    }
    state = null;
    await new Promise((res) => setTimeout(res, 200));
  }
  if (!state) {
    if (lastErr && isConn(lastErr)) {
      console.error(HINT);
      console.error(`(${lastErr.cause?.code ?? lastErr.message})`);
      fail(`no server at ${endpoint}`);
    }
    fail(`not ready after 20 s: ${lastErr?.message}`);
  }
  console.log(`file ${state.file}, version ${state.version}`);

  // 2. Two slabs in the default demo (one per storey).
  const before = await slabs();
  console.log(`IfcSlab before ${before.length}`);
  if (before.length !== 2) fail(`expected 2 IfcSlab, got ${before.length}`);
  const beforeIds = new Set(before.map((e) => e.id));

  // 3. Place a slab on the lowest storey ("storeys" is sorted by elevation).
  const storeys = await q("storeys");
  if (!storeys.length) fail("no storeys");
  const storey = storeys[0].id;
  const args = { kind: "slab", storey, a: [1000, 1000] };
  if (!noB) args.b = [5000, 4000];
  console.log(`place on ${storeys[0].name} (${storey}):`, JSON.stringify(args));
  try {
    await call("verb", "place", JSON.stringify(args));
  } catch (e) {
    if (!(e instanceof ApiError)) throw e;
    fail(`verb place refused: ${e.message}`);
  }

  // 4. One new slab with the corner (1000, 1000).
  const after = await slabs();
  const added = after.filter((e) => !beforeIds.has(e.id));
  console.log(`IfcSlab after ${after.length}, new ${added.length}`);
  if (after.length !== 3 || added.length !== 1) fail(`expected 3 IfcSlab and 1 new, got ${after.length} and ${added.length}`);
  const el = await q("element", { id: added[0].id });
  console.log("new slab:", JSON.stringify({ id: el.id, storey: el.storey_name, placement: el.placement, params: el.generator_params }));
  const [ox, oy] = el.placement.origin;
  const near = ([x, y]) => Math.abs(x - 1000) < 1 && Math.abs(y - 1000) < 1;
  const outline = (el.generator_params?.outline ?? []).map(([x, y]) => [x + ox, y + oy]);
  // ponytail: corner match within 1 mm in plan, as place-slab.mjs; z is the storey's, not checked
  if (!near([ox, oy]) && !outline.some(near)) fail("the new slab has no corner at (1000, 1000)");

  // 5. The change feed has moved.
  const r = await fetch(`${endpoint}/change?since=0&t=${t}`);
  if (!r.ok) fail(`change: HTTP ${r.status}: ${await r.text()}`);
  const ch = await r.json();
  console.log(`change seq ${ch.seq}`);
  if (!(typeof ch.seq === "number" && ch.seq >= 1)) fail(`expected change seq >= 1, got ${JSON.stringify(ch.seq)}`);

  // 6. Undo.
  await call("undo", null);
  const undone = await slabs();
  console.log(`IfcSlab after undo ${undone.length}`);
  if (undone.length !== 2) fail(`expected 2 IfcSlab after undo, got ${undone.length}`);

  // 7. Latency of one query round trip.
  const time = async (name, a) => {
    const ms = [];
    for (let i = 0; i < n; i++) {
      const t0 = performance.now();
      await call("query", name, JSON.stringify(a));
      ms.push(performance.now() - t0);
    }
    return stats(ms);
  };
  const lat = { pointer: await time("pointer", {}), elements: await time("elements", { class: "IfcSlab" }) };
  for (const [k, s] of Object.entries(lat)) {
    console.log(`${k} x${n}: p50 ${s.p50.toFixed(3)} ms, p95 ${s.p95.toFixed(3)} ms, max ${s.max.toFixed(3)} ms`);
  }
  const slow = Object.entries(lat).filter(([, s]) => s.p95 >= 5);
  if (slow.length) fail(`p95 >= 5 ms for ${slow.map(([k]) => k).join(", ")}`);
  console.log("PASS");
}

try {
  await main();
  process.exit(0);
} catch (e) {
  if (isConn(e)) console.error(HINT);
  fail(e?.message ?? String(e));
}
