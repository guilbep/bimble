// The WebGPU renderer's frame (tickets GPU1, GPU2; ADR 0083): the scene + camera + states the viewport gives
// (gpu/switch.ts GpuView) turned into pixels, as glviewport.ts draw() turns them with WebGL2, on a second
// canvas stacked over #bimble (under the overlay; no pointer events: the viewport's handlers, tools, picking
// and overlays stay as they are).
// GPU2: the viewport's models drawn from the viewport's own lists (its batches, instanced groups, per-element
// state texels, placements, link proxies; switch.ts GpuModels), each model in its own buffers taken over from
// WebGL2's (scene.ts), so the two renderers draw the same picture by construction and never hold the same
// model twice. The frame's choices are WebGL2's (glviewport.ts draw(), ADR 0078 decision 4 and amendments
// 2-3): a moving frame (the camera changed) draws, over the batches in view, the elements largest on screen
// first within the triangle budget, far cells as their proxies (3 px; at most half the budget), each shown
// link's baked proxies cut with the eye in its frame, a link whose detail is away (released by the budget,
// or not taken over yet) as its coarsest proxies, else its batches' outlines: never an empty picture. 150 ms
// after the camera stops (no button held) the rest is added in slices into the kept picture (an offscreen
// colour and depth target), then every edge once more; any new frame abandons the fill. Edges only where they
// read (a batch whose box spans 16 device px; a part drawn while moving: faces only), never a proxy's.
// GPU culling (the setting gpu_cull, ADR 0036 #10): the copies of each group are tested on the GPU (gpu/cull.ts)
// and drawn with one drawIndexedIndirect per group; off, each run of copies is a direct instanced draw.
import { boxInFrustum, budgetCut, cutEnd, frustumPlanes, MIN_MOVING_TRIS, type Cut } from "../gl/cull";
import { cutProxies, fitProxies, standIns, usableCells } from "../gl/proxies";
import { perfFrame, perfWaits, renderer as perfRenderer } from "../perf";
import { GpuCull } from "./cull";
import { BU, TU, type GpuDevice } from "./device";
import { DEPTH, DRAW_BYTES, Pipelines, type PipeKey } from "./pipelines";
import { xfRows, type ModelXf } from "../gl/cull";
import { BACKGROUND, BOX, EDGE, GHOST, GHOST_ALPHA, GHOST_EDGE, OUTLINE, REFERENCE_EDGE, Scene, SLOT } from "./scene";
import { GL_NAMES, type GlArrays, type GpuDb, type GpuLinkPrx, type GpuModels, type GpuRenderer, type GpuView } from "./switch";

const STILL_MS = 150, MOVING_PROXY_PX = 3, STILL_PROXY_PX = 1, FIRST_BUDGET_TRIS = 1000000, AMBIENT = 0.62, CUT_SHADE = 0.72, EDGE_MIN_PX = 16;
/** Taking WebGL2's buffers over: at most this long a frame (at least one model), the open model at once. */
const TAKE_MS = 24;
/** Picks' triangles read back from the GPU (a link's arrays dropped from the page): bounded. */
const PICK_CACHE_BYTES = 8 << 20;

/** The viewport's stats (glviewport.ts GlStats): the fields this renderer writes. */
interface Stats {
  frames: number; frameTris?: number; frameDraws?: number; gpuBytes?: number; fullFrameMs?: number | null; clip?: unknown;
  moving?: { moving: boolean; slice: boolean; budget: number; t: number; batches: number; culled: number; outlines: number; tris: number; proxied?: number };
  proxies?: { drawnCells: number; drawnTris: number; standIns: number; on: boolean };
  q2?: { edges: boolean; ortho: boolean; edgeSegs: number };
  linkProxies?: { name: string; cells: number; tris: number; bytes: number; drawnCells: number; drawnTris: number }[];
  /** GPU2: the GPU bytes each renderer holds now (WebGPU's buffers, WebGL2's left), the models taken over of those loaded. */
  gpuSplit?: { webgpu: number; webgl: number; taken: number; models: number };
  gpuPickReads?: number;
}

export function createRenderer(glCanvas: HTMLCanvasElement, d: GpuDevice, statsObj: object): GpuRenderer {
  const { device, format } = d;
  const stats = statsObj as Stats;
  const canvas = document.createElement("canvas");
  canvas.setAttribute("aria-hidden", "true");
  canvas.dataset.bbGpu = "webgpu";
  canvas.style.cssText = "position:absolute;inset:0;width:100%;height:100%;pointer-events:none;display:block;visibility:hidden";
  glCanvas.insertAdjacentElement("afterend", canvas); // under the overlay (inserted after #bimble before it)
  const ctx = canvas.getContext("webgpu") as GPUCanvasContext | null;
  if (!ctx) { canvas.remove(); throw new Error("no webgpu context on a new canvas"); }
  ctx.configure({ device, format, alphaMode: "opaque", usage: TU.RENDER_ATTACHMENT | TU.COPY_DST });
  const pipes = new Pipelines(device, format);
  const scene = new Scene(device);
  const cull = new GpuCull(device);
  const frameU = device.createBuffer({ size: 144, usage: BU.UNIFORM | BU.COPY_DST });
  const cube: number[] = []; // the unit cube's 12 edges
  for (let i = 0; i < 8; i++) for (let k = 0; k < 3; k++) if (!((i >> k) & 1)) cube.push(i & 1, (i >> 1) & 1, (i >> 2) & 1, (i | (1 << k)) & 1, ((i | (1 << k)) >> 1) & 1, ((i | (1 << k)) >> 2) & 1);
  const cubeBuf = scene.buf(0, BU.VERTEX, new Float32Array(cube));
  const identity = scene.buf(0, BU.VERTEX, new Float32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]));
  const boxBuf = scene.buf(24 * 12, BU.VERTEX);
  let outlineBuf = scene.buf(48 * 64, BU.VERTEX), outlineCap = 64;
  let drawsBuf = scene.buf(SLOT * 64, BU.UNIFORM), drawsCap = 64;
  let ghost: { key: unknown; pos: GPUBuffer; idx: GPUBuffer; edges: GPUBuffer; marks: GPUBuffer; ni: number; ne: number; nm: number } | null = null;
  let color: GPUTexture | null = null, depth: GPUTexture | null = null;
  let group0: GPUBindGroup | null = null, group0Key: unknown[] = [];
  const pickCache = new Map<string, { pos: Float32Array; idx: Uint32Array }>();
  let pickBytes = 0;
  const pickAsked = new Set<string>();
  let handing = false, lastM: GpuModels | null = null, takenAll = false;

  // the frame policy's state (glviewport.ts draw() and its loop)
  let lastCamKey: string | null = null, lastFrameAt = 0, lastFrameTris = 0, trisPerMs = 0, lastBudgeted = false;
  let firstOfMesh = false, gen = 0, meshNow: unknown = null, usable = new Uint8Array(0), usableKey: unknown[] = [];
  const lusable = new Map<GpuLinkPrx, { key: unknown[]; u: Uint8Array }>();
  const last = { moving: false, t0: 0 };

  const frameBudget = (fps: number) => (fps <= 0 ? Infinity : Math.max(MIN_MOVING_TRIS, trisPerMs * (1000 / fps)));
  const targets = (w: number, h: number): boolean => {
    if (color && color.width === w && color.height === h) return false;
    color?.destroy(), depth?.destroy();
    color = device.createTexture({ size: [w, h], format, usage: TU.RENDER_ATTACHMENT | TU.COPY_SRC });
    depth = device.createTexture({ size: [w, h], format: DEPTH, usage: TU.RENDER_ATTACHMENT });
    canvas.width = w, canvas.height = h;
    return true;
  };
  const bind0 = (): GPUBindGroup => {
    const key = [scene.states, drawsBuf];
    if (group0 && key.every((b, i) => b === group0Key[i])) return group0;
    group0Key = key;
    return (group0 = device.createBindGroup({ layout: pipes.layout, entries: [
      { binding: 0, resource: { buffer: frameU } }, { binding: 1, resource: { buffer: scene.states } },
      { binding: 3, resource: { buffer: drawsBuf, size: DRAW_BYTES } },
    ] }));
  };
  /** Model k's copies' bind group (its placements, elements, compacted list), else the empty one. */
  const bind1 = (k: number): GPUBindGroup => {
    const g = k >= 0 ? scene.models[k] : undefined;
    if (g?.b.instMat && g.b.instEl) {
      const vis = g.visible ?? scene.empty, key = [g.b.instMat, g.b.instEl, vis];
      if (!g.group || key.some((b, i) => b !== g.key[i])) {
        g.key = key;
        g.group = device.createBindGroup({ layout: pipes.copies, entries: key.map((b, i) => ({ binding: i, resource: { buffer: b } })) });
      }
      return g.group;
    }
    return (scene.emptyGroup ??= device.createBindGroup({ layout: pipes.copies, entries: [0, 1, 2].map((i) => ({ binding: i, resource: { buffer: scene.empty } })) }));
  };

  // ---- GPU2: WebGL2's buffers taken over (and given back) ----
  const seenKeys = new Map<number, { l: unknown }>();
  /** One object per (a slot's bytes, the lists' key), compared by identity: changed, the slot is read again. */
  const seenKey = (bytes: number, lists: unknown): unknown => {
    let o = seenKeys.get(bytes);
    if (!o || o.l !== lists) seenKeys.set(bytes, (o = { l: lists }));
    return o;
  };
  /** Takes over what WebGL2 holds that WebGPU does not yet (the open model first and whole; then the links, within
   *  TAKE_MS a call); releases what the viewport released. True when the open model is WebGPU's (the picture can be). */
  const sync = (M: GpuModels): boolean => {
    lastM = M;
    if (handing) return true;
    const t0 = performance.now();
    let pending = 0;
    for (const x of M.slots) {
      const g = scene.models[x.k];
      if (!x.gl) { if (g) scene.release(x.k); continue; }
      if (g && g.gl === x.gl && g.seen === seenKey(x.bytes, M.listsKey)) continue;
      if (x.k > 0 && performance.now() - t0 > TAKE_MS) { pending++; continue; }
      const a = M.readGl(x.gl);
      scene.take(x.k, x.gl, a, seenKey(x.bytes, M.listsKey));
      M.emptyGl(x.gl);
    }
    for (let k = M.slots.length; k < scene.models.length; k++) scene.release(k);
    takenAll = pending === 0;
    const n = M.slots.filter((x) => x.gl).length;
    const pr = pending ? `WebGPU: ${n - pending} of ${n} models taken over from WebGL2 (the rest drawn as proxies meanwhile)` : null;
    if (perfRenderer.progress !== pr) perfRenderer.progress = pr;
    const s0 = M.slots[0];
    return !s0 || !s0.gl || !!scene.models[0];
  };
  /** Reads buffers back (a copy into a mappable buffer each, one submit): their bytes, in order. */
  const readBack = async (parts: { b: GPUBuffer; at: number; n: number }[]): Promise<ArrayBuffer[]> => {
    const enc = device.createCommandEncoder(), stage: GPUBuffer[] = [];
    for (const p of parts) {
      const n = Math.ceil(p.n / 4) * 4, s = device.createBuffer({ size: Math.max(4, n), usage: BU.MAP_READ | BU.COPY_DST });
      if (n) enc.copyBufferToBuffer(p.b, p.at, s, 0, Math.min(n, p.b.size - p.at));
      stage.push(s);
    }
    device.queue.submit([enc.finish()]);
    return Promise.all(stage.map(async (s, i) => { await s.mapAsync(1); const a = s.getMappedRange().slice(0, parts[i]!.n); s.unmap(); s.destroy(); return a; }));
  };
  // ---- end GPU2 ----

  type Kind = "moving" | "slice" | "edges" | "whole";
  const frameUniform = (v: GpuView, box: GpuView["clip"]) => {
    const e = v.eye, tg = v.target;
    const dir = [e[0]! - tg[0]!, e[1]! - tg[1]!, e[2]! - tg[2]!], dl = Math.hypot(dir[0]!, dir[1]!, dir[2]!) || 1;
    const nd = dir.map((x) => x / dl);
    const lt = [nd[0]! - 0.3, nd[1]! - 0.2, nd[2]! + 0.9], ll = Math.hypot(lt[0]!, lt[1]!, lt[2]!) || 1;
    const u = new Float32Array(36);
    u.set(v.mvp, 0);
    u.set(v.ortho ? [nd[0]!, nd[1]!, nd[2]!, 1] : [e[0]!, e[1]!, e[2]!, 0], 16);
    u.set([lt[0]! / ll, lt[1]! / ll, lt[2]! / ll, 0], 20);
    u.set([box?.min[0] ?? 0, box?.min[1] ?? 0, box?.min[2] ?? 0, box ? 1 : 0], 24);
    u.set([box?.max[0] ?? 0, box?.max[1] ?? 0, box?.max[2] ?? 0, CUT_SHADE], 28);
    u.set([Math.cos(box?.yaw ?? 0), Math.sin(box?.yaw ?? 0), v.plan ? v.zbias : 0, AMBIENT], 32);
    device.queue.writeBuffer(frameU, 0, u);
  };
  /** The per-draw uniforms (model, use_list, base, el_col; colour rgba; the model's rows and plain colour), SLOT apart,
   *  written before the submit. `rows(k)`: model k's placement and plain colour (sRGB 0..255 or null). */
  const ROW = new Float32Array(12);
  const writeSlots = (slots: number[], rows: (k: number) => { xf: ModelXf; plain: readonly number[] | null }) => {
    const n = Math.max(1, slots.length / 8);
    if (n > drawsCap) { scene.drop(drawsBuf); drawsCap = 2 * n; drawsBuf = scene.buf(SLOT * drawsCap, BU.UNIFORM); }
    const sb = new ArrayBuffer(SLOT * n);
    for (let i = 0; i < slots.length / 8; i++) {
      new Uint32Array(sb, SLOT * i, 4).set(slots.slice(8 * i, 8 * i + 4));
      const f = new Float32Array(sb, SLOT * i + 16, 20), r = rows(slots[8 * i]!);
      f.set(slots.slice(8 * i + 4, 8 * i + 8), 0);
      f.set(xfRows(r.xf, ROW), 4);
      if (r.plain) f.set([r.plain[0]! / 255, r.plain[1]! / 255, r.plain[2]! / 255, 1], 16);
    }
    device.queue.writeBuffer(drawsBuf, 0, sb);
  };
  const IDENT: ModelXf = { c: 1, s: 0, t: [0, 0, 0] };

  /** One frame: `kind` moving (the budget), slice (the fill's next part, into the kept picture), edges
   *  (the fill's last pass) or whole. */
  const draw = (v: GpuView, kind: Kind, fill: { t: number } | null): { t: number; outlines: number; proxied: number } => {
    const M = v.models!, m = v.mesh!, tNow = performance.now();
    const moving = kind === "moving", slice = kind === "slice" || kind === "edges";
    const box = v.clip, planes = v.noCull ? null : frustumPlanes(v.mvp);
    const e = v.eye;
    frameUniform(v, box);
    const pxK = v.mmPerPx(1); // mm a px at 1 mm (perspective), or mm a px (orthographic)
    const sizePx = (bx: Float32Array, dist: number) => Math.hypot(bx[3]! - bx[0]!, bx[4]! - bx[1]!, bx[5]! - bx[2]!) / (v.ortho ? pxK : dist * pxK);
    // ---- what each batch draws (glviewport.ts draw(): frustum, proxies, budget, outlines, stand-ins) ----
    const p = scene.prx;
    let prxCells: number[] = [], replaced: Uint8Array | null = null;
    const prxOn = p !== null && !v.noProxies && !box && !v.plan && v.proxiesOn;
    const budget0 = frameBudget(v.fps);
    if (prxOn) {
      const uk = [M.listsKey, v.shown, M.statesKey, p];
      if (uk.some((x, i) => x !== usableKey[i])) usableKey = uk, usable = new Uint8Array(usableCells(p, (el) => v.shown[el] === 1 && !v.selected(el)));
      ({ cells: prxCells, replaced } = cutProxies(p, usable, planes ? (b) => boxInFrustum(planes, b, 0) : null, e, v.mmPerPx, moving ? MOVING_PROXY_PX : STILL_PROXY_PX));
      if (moving && prxCells.length) prxCells = fitProxies(p, usable, prxCells, replaced, budget0 / 2);
    }
    // LNK3D (C): each shown link's proxies, cut with the eye in its frame; a link whose detail WebGPU does not hold
    // (released, or not taken over yet) by its coarsest usable proxies
    const linkCut: { x: GpuLinkPrx; cells: number[]; mdl: number; colour: number }[] = [];
    const linkRep: (Uint8Array | null)[] = [], linkNeed: (Uint8Array | null)[] = [];
    const held = (k: number) => !!scene.models[k] && !!M.slots[k]?.gl;
    if (m.links && !v.noProxies && !box && !v.plan && v.proxiesOn) {
      m.links.forEach((l, k) => {
        const x = M.linkPrx[k + 1];
        if (!x || !l.visible) return;
        const uk = [M.listsKey, v.shown, M.statesKey, M.linkedSel];
        let us = lusable.get(x);
        if (!us || us.key.some((y, i) => y !== uk[i])) {
          const sel = M.linkedSel;
          lusable.set(x, (us = { key: uk, u: usableCells(x.p, (el) => v.shown[l.e0 + el] === 1 && !(sel && sel.link === l.name && sel.id === m.ids[l.e0 + el])) }));
        }
        const normal = M.cutLink(x, l.xf, us.u, planes, e, moving ? MOVING_PROXY_PX : STILL_PROXY_PX);
        const { cells, replaced: r } = held(k + 1) ? normal : M.cutLink(x, l.xf, us.u, planes, e, Infinity);
        linkRep[k + 1] = r, linkNeed[k + 1] = normal.replaced;
        if (cells.length) linkCut.push({ x, cells, mdl: k + 1, colour: l.colour ?? -1 });
      });
    }
    for (const x of M.linkPrx) if (x) x.drawnCells = x.drawnTris = 0;
    const missing: Float32Array[] = [];
    let culled = 0, cutT = 0, budget = Infinity;
    const seen: { d: GpuDb; c: Cut }[] = [];
    for (const d of M.dbs) {
      if (d.b.shown === 0) { d.k0 = d.k1 = 0; continue; }
      if (!held(d.model)) { // its model's detail away: its proxies stand for it, else its box; asked back
        if (!planes || boxInFrustum(planes, d.b.box, 0)) {
          const nr = linkNeed[d.model], r2 = linkRep[d.model];
          if (!(nr && d.b.group >= 0 && nr[d.b.group])) { if (!M.slots[d.model]?.gl) M.want(d.model); M.need(d.model); }
          if (!(r2 && d.b.group >= 0 && r2[d.b.group])) missing.push(d.b.box);
        }
        d.k0 = d.k1 = 0;
        continue;
      }
      const rp = d.model === 0 ? replaced : linkRep[d.model] ?? null;
      if (rp && d.b.group >= 0 && rp[d.b.group]) { d.k0 = d.k1 = 0; continue; }
      if (planes && !boxInFrustum(planes, d.b.box, 0)) { d.k0 = d.k1 = 0; culled++; continue; }
      const bx = d.b.box, q = [0, 1, 2].map((a) => Math.min(Math.max(e[a]!, bx[a]!), bx[3 + a]!));
      const dist = v.ortho ? 1 : Math.max(1, Math.hypot(q[0]! - e[0]!, q[1]! - e[1]!, q[2]! - e[2]!));
      d.noEdge = sizePx(bx, dist) < EDGE_MIN_PX;
      seen.push({ d, c: { b: d.b, dist, k0: slice && fill ? cutEnd({ b: d.b, dist, k0: 0 }, fill.t) : 0 } });
      M.need(d.model);
    }
    if (kind === "edges") for (const { d } of seen) d.k0 = d.k1 = 0;
    else {
      if (moving || slice) budget = budget0;
      if (moving && prxCells.length && budget < Infinity) budget = Math.max(MIN_MOVING_TRIS, budget - prxCells.reduce((n, k) => n + p!.tris[k]!, 0));
      const cut = budgetCut(seen.map((x) => x.c), budget);
      cutT = cut.t;
      for (const { d, c } of seen) d.k0 = c.k0, d.k1 = cutEnd(c, cut.t);
    }
    if (moving) for (const { d } of seen) if (d.k1 < d.b.n) d.noEdge = true;
    let unDs = moving ? seen.filter(({ d }) => d.k1 < d.b.n) : [];
    let nStand = 0;
    if (prxOn && replaced && unDs.length) {
      const leaves = new Set<number>();
      for (const { d } of unDs) if (d.b.group >= 0 && d.model === 0) leaves.add(d.b.group);
      const used = prxCells.reduce((n, k) => n + p!.tris[k]!, 0);
      const extra = standIns(p!, usable, leaves, replaced, Math.max(0, budget0 / 2 - used));
      nStand = extra.length;
      if (extra.length) {
        prxCells.push(...extra);
        const r = replaced;
        for (const { d } of seen) if (d.model === 0 && d.b.group >= 0 && r[d.b.group]) d.k0 = d.k1 = 0;
        unDs = unDs.filter(({ d }) => !(d.model === 0 && d.b.group >= 0 && r[d.b.group]));
      }
    }
    const unfinished = [...unDs.map(({ d }) => d.b.box), ...missing];
    const drawPrx = prxCells.length > 0 && (!slice || fill?.t === Infinity);
    const runsOf = (parts: Iterable<{ i0: number; ni: number }>): [number, number][] => {
      const runs: [number, number][] = [];
      for (const q of parts) {
        const lr = runs[runs.length - 1];
        if (!q.ni) continue;
        if (lr && lr[0] + lr[1] === q.i0) lr[1] += q.ni;
        else runs.push([q.i0, q.ni]);
      }
      return runs;
    };
    // ---- encode ----
    const slots: number[] = [];
    const slotOf = (model: number, useList: number, base: number, elCol: number, rgb: readonly number[], alpha = 1) => {
      slots.push(model, useList, base, elCol, rgb[0]! / 255, rgb[1]! / 255, rgb[2]! / 255, alpha);
      return slots.length / 8 - 1;
    };
    const want = 16 + 4 * M.slots.length + 2 * M.groups.length + linkCut.length;
    if (want > drawsCap) { scene.drop(drawsBuf); drawsCap = 2 * want; drawsBuf = scene.buf(SLOT * drawsCap, BU.UNIFORM); }
    const enc = device.createCommandEncoder();
    const gpuCull = perfRenderer.gpuCull && M.groups.some((g) => held(g.model)) &&
      cull.run(enc, M, scene, planes, e, v.ortho, cutT, moving && cutT > 0, (d) => [d.k0, d.k1]);
    const clear = !slice || fill?.t === Infinity;
    const pass = enc.beginRenderPass({
      colorAttachments: [{ view: color!.createView(), loadOp: clear ? "clear" : "load", storeOp: "store",
        clearValue: [BACKGROUND[0] / 255, BACKGROUND[1] / 255, BACKGROUND[2] / 255, 1] }],
      depthStencilAttachment: { view: depth!.createView(), depthLoadOp: clear ? "clear" : "load", depthClearValue: 1, depthStoreOp: "store" },
    });
    const bg = bind0();
    let tris = 0, draws = 0, segs = 0, prxTris = 0;
    let curPipe: GPURenderPipeline | null = null, curSlot = -1, curModel = -2;
    const use = (k: PipeKey, slot: number, model: number) => {
      const pl = pipes.get(k);
      if (pl !== curPipe) pass.setPipeline(pl), curPipe = pl;
      if (slot !== curSlot) pass.setBindGroup(0, bg, [SLOT * slot]), curSlot = slot;
      if (model !== curModel) pass.setBindGroup(1, bind1(model)), curModel = model;
    };
    const baseFront: GPUFrontFace = box && v.mirror ? "cw" : "ccw";
    const flip = (f: GPUFrontFace): GPUFrontFace => (f === "ccw" ? "cw" : "ccw");
    const merged = (mdl: number, k: PipeKey, slot: number) => {
      const g = scene.models[mdl];
      if (!g?.b.pos || !g.b.el || !g.b.idx) return;
      let a = -1, z = -1, bound = false;
      const flush = () => {
        if (z <= a) return;
        if (!bound) { use({ ...k, kind: "merged" }, slot, mdl); pass.setVertexBuffer(0, g.b.pos!); pass.setVertexBuffer(1, g.b.el!); pass.setIndexBuffer(g.b.idx!, "uint32"); bound = true; }
        pass.drawIndexed(z - a, 1, a, 0, 0), draws++, tris += (z - a) / 3;
      };
      for (const d of M.dbs) {
        if (d.g || d.model !== mdl || d.k1 <= d.k0) continue;
        const s = d.f0 + 3 * d.b.cum[d.k0]!, t = d.f0 + 3 * d.b.cum[d.k1]!;
        if (s === z) z = t;
        else flush(), a = s, z = t;
      }
      flush();
    };
    const instFaces = (mdl: number, k: PipeKey, slotPlain: number, slotList: (gi: number) => number) => {
      const gm = scene.models[mdl];
      if (!gm?.b.pos || !gm.b.shapeIdx || !gm.b.instMat) return;
      M.groups.forEach((g, gi) => {
        if (g.model !== mdl || !g.ni) return;
        const front = g.mirrored ? flip(k.front ?? "ccw") : k.front ?? "ccw";
        let any = 0;
        for (const d of g.dbs) any += d.k1 - d.k0;
        if (!any) return;
        const bindInst = (slot: number) => { use({ ...k, kind: "inst", front }, slot, mdl); pass.setVertexBuffer(0, gm.b.pos!); pass.setIndexBuffer(gm.b.shapeIdx!, "uint32"); };
        if (gpuCull && cull.args) {
          bindInst(slotList(gi));
          pass.drawIndexedIndirect(cull.args, 20 * gi), draws++, tris += (g.ni / 3) * any; // at most: the GPU may keep fewer
          return;
        }
        bindInst(slotPlain);
        let a = -1, z = -1;
        const flush = () => { if (z > a) pass.drawIndexed(g.ni, z - a, g.i0, 0, a), draws++, tris += (g.ni / 3) * (z - a); };
        for (const d of g.dbs) {
          if (d.k1 <= d.k0) continue;
          const s = g.first + d.b.first + d.k0, t = g.first + d.b.first + d.k1;
          if (s === z) z = t;
          else flush(), a = s, z = t;
        }
        flush();
      });
    };
    // per model its slots: faces (the element's colour), edges (the edge colour), copies through the compacted list
    const memo = (f: (k: number) => number) => { const c = new Map<number, number>(); return (k: number) => { let s = c.get(k); if (s === undefined) c.set(k, (s = f(k))); return s; }; };
    const fSlot = memo((mdl) => slotOf(mdl, 0, 0, 1, EDGE));
    const eSlot = memo((mdl) => slotOf(mdl, 0, 0, 0, mdl ? REFERENCE_EDGE : EDGE));
    const lfSlot = memo((gi) => slotOf(M.groups[gi]!.model, 1, M.groups[gi]!.first, 1, EDGE));
    const leSlot = memo((gi) => slotOf(M.groups[gi]!.model, 1, M.groups[gi]!.first, 0, M.groups[gi]!.model ? REFERENCE_EDGE : EDGE));
    const prxFaces = (k: PipeKey) => {
      if (!drawPrx || !scene.prxPos) return;
      use({ ...k, kind: "proxy" }, fSlot(0), 0);
      pass.setVertexBuffer(0, scene.prxPos), pass.setVertexBuffer(1, scene.prxCol!), pass.setIndexBuffer(scene.prxIdx!, "uint32");
      for (const [a, n] of runsOf([...prxCells].sort((x, y) => x - y).flatMap((c) => p!.cells[c]!.parts))) pass.drawIndexed(n, 1, a, 0, 0), draws++, tris += n / 3, prxTris += n / 3;
    };
    const linkCells = (k: PipeKey) => {
      if (slice && fill?.t !== Infinity) return;
      for (const c of linkCut) {
        const b = scene.linkProxies(c.x);
        use({ ...k, kind: "lprx" }, slotOf(c.mdl, 0, 0, 0, M.linkColour(c.colour)), c.mdl);
        pass.setVertexBuffer(0, b.pos), pass.setIndexBuffer(b.idx, "uint32");
        let n3 = 0;
        for (const [a, n] of runsOf([...c.cells].sort((x, y) => x - y).flatMap((cell) => c.x.p.cells[cell]!.parts))) pass.drawIndexed(n, 1, a, 0, 0), draws++, n3 += n / 3;
        tris += n3, c.x.drawnCells = c.cells.length, c.x.drawnTris = n3;
      }
    };
    const faces = (push: number, cullMode: GPUCullMode) => {
      const main: PipeKey = { kind: "merged", cull: cullMode, front: baseFront, slope: 1.5 + push, bias: 2 + 2.4 * push };
      const lk: PipeKey = { kind: "merged", cull: cullMode, front: baseFront, slope: 2.5 + push, bias: 16 + 2.4 * push };
      merged(0, main, fSlot(0)), instFaces(0, main, fSlot(0), lfSlot), prxFaces(main);
      for (const x of M.slots) if (x.k > 0 && held(x.k)) merged(x.k, lk, fSlot(x.k)), instFaces(x.k, lk, fSlot(x.k), lfSlot);
      linkCells(lk);
    };
    if (!box) faces(0, "none");
    else faces(0, "back"), faces(v.plan ? -3 : 2.5, "front");
    const edgesOn = v.edges && !v.noEdges;
    if (kind === "edges") for (const { d } of seen) d.k0 = 0, d.k1 = d.b.n;
    if (edgesOn) {
      const mergedEdges = (mdl: number) => {
        const g = scene.models[mdl];
        if (!g?.b.edge || !g.b.edgeNrm || !g.b.edgeEl || !g.b.edgeIdx) return;
        let a = -1, z = -1, bound = false;
        const flush = () => {
          if (z <= a) return;
          if (!bound) { use({ kind: "edge" }, eSlot(mdl), mdl); pass.setVertexBuffer(0, g.b.edge!); pass.setVertexBuffer(1, g.b.edgeNrm!); pass.setVertexBuffer(2, g.b.edgeEl!); pass.setIndexBuffer(g.b.edgeIdx!, "uint32"); bound = true; }
          pass.drawIndexed(z - a, 1, a, 0, 0), draws++, segs += (z - a) / 2;
        };
        for (const d of M.dbs) {
          if (d.g || d.model !== mdl || d.k1 <= d.k0 || d.noEdge) continue;
          const s = d.e0 + 2 * d.b.ecum[d.k0]!, t = d.e0 + 2 * d.b.ecum[d.k1]!;
          if (s === z) z = t;
          else flush(), a = s, z = t;
        }
        flush();
      };
      const instEdges = (mdl: number) => {
        const gm = scene.models[mdl];
        if (!gm?.b.edge || !gm.b.edgeNrm || !gm.b.instMat) return;
        M.groups.forEach((g, gi) => {
          if (g.model !== mdl || !g.ns) return;
          let any = 0;
          for (const d of g.dbs) if (!d.noEdge) any += d.k1 - d.k0;
          if (!any) return;
          if (gpuCull && kind !== "edges" && cull.edgeArgs && g.dbs.every((d) => !d.noEdge || d.k1 <= d.k0)) {
            use({ kind: "edgeInst" }, leSlot(gi), mdl);
            pass.setVertexBuffer(0, gm.b.edge!), pass.setVertexBuffer(1, gm.b.edgeNrm!);
            pass.drawIndirect(cull.edgeArgs, 16 * gi), draws++, segs += g.ns * any;
            return;
          }
          use({ kind: "edgeInst" }, eSlot(mdl), mdl);
          pass.setVertexBuffer(0, gm.b.edge!), pass.setVertexBuffer(1, gm.b.edgeNrm!);
          let a = -1, z = -1;
          const flush = () => { if (z > a) pass.draw(2 * g.ns, z - a, 2 * g.s0, a), draws++, segs += g.ns * (z - a); };
          for (const d of g.dbs) {
            if (d.k1 <= d.k0 || d.noEdge) continue;
            const s = g.first + d.b.first + d.k0, t = g.first + d.b.first + d.k1;
            if (s === z) z = t;
            else flush(), a = s, z = t;
          }
          flush();
        });
      };
      mergedEdges(0), instEdges(0);
      for (const x of M.slots) if (x.k > 0 && held(x.k)) mergedEdges(x.k), instEdges(x.k);
    }
    if (unfinished.length) { // the outlines of what a moving frame could not draw, and of what is away
      if (unfinished.length > outlineCap) scene.drop(outlineBuf), outlineCap = 2 * unfinished.length, outlineBuf = scene.buf(48 * outlineCap, BU.VERTEX);
      const mat = new Float32Array(12 * unfinished.length);
      unfinished.forEach((b, i) => mat.set([b[3]! - b[0]!, 0, 0, 0, b[4]! - b[1]!, 0, 0, 0, b[5]! - b[2]!, b[0]!, b[1]!, b[2]!], 12 * i));
      device.queue.writeBuffer(outlineBuf, 0, mat);
      use({ kind: "box" }, slotOf(0, 0, 0, 0, OUTLINE), 0);
      pass.setVertexBuffer(0, cubeBuf), pass.setVertexBuffer(1, outlineBuf);
      pass.draw(24, unfinished.length), draws++;
    }
    if (box && !v.plan) { // the section box's 12 edges, over the model (turned by its yaw)
      const c = Math.cos(box.yaw ?? 0), s = Math.sin(box.yaw ?? 0);
      const corner = (i: number) => { const x = (i & 1 ? box.max[0] : box.min[0]) ?? 0, y = (i & 2 ? box.max[1] : box.min[1]) ?? 0, z = (i & 4 ? box.max[2] : box.min[2]) ?? 0; return [c * x - s * y, s * x + c * y, z]; };
      const vb: number[] = [];
      for (let i = 0; i < 8; i++) for (let k = 0; k < 3; k++) if (!((i >> k) & 1)) vb.push(...corner(i), ...corner(i | (1 << k)));
      device.queue.writeBuffer(boxBuf, 0, new Float32Array(vb));
      use({ kind: "box", depthTest: false }, slotOf(0, 0, 0, 0, BOX), 0);
      pass.setVertexBuffer(0, boxBuf), pass.setVertexBuffer(1, identity);
      pass.draw(24, 1), draws++;
    }
    // GPU2: the place tool's ghost and markers (WebGL2 drew them before: the frame switched canvas)
    const gh = M.ghost();
    if (gh && !slice) {
      if (!ghost || ghost.key !== gh.key) {
        if (ghost) for (const b of [ghost.pos, ghost.idx, ghost.edges, ghost.marks]) scene.drop(b);
        ghost = { key: gh.key, pos: scene.buf(0, BU.VERTEX, gh.pos), idx: scene.buf(0, BU.INDEX, gh.idx), edges: scene.buf(0, BU.VERTEX, gh.edges), marks: scene.buf(0, BU.VERTEX, gh.marks),
          ni: gh.idx.length, ne: gh.edges.length / 3, nm: gh.marks.length / 3 };
      }
      if (ghost.ni) { use({ kind: "ghost" }, slotOf(0, 0, 0, 0, GHOST, GHOST_ALPHA), 0); pass.setVertexBuffer(0, ghost.pos), pass.setIndexBuffer(ghost.idx, "uint32"); pass.drawIndexed(ghost.ni), draws++; }
      if (ghost.ne) { use({ kind: "ghostLine" }, slotOf(0, 0, 0, 0, GHOST_EDGE, 0.9), 0); pass.setVertexBuffer(0, ghost.edges); pass.draw(ghost.ne), draws++; }
      if (ghost.nm) { use({ kind: "ghostLine", depthTest: false }, slotOf(0, 0, 0, 0, GHOST_EDGE, 0.9), 0); pass.setVertexBuffer(0, ghost.marks); pass.draw(ghost.nm), draws++; }
    }
    pass.end();
    writeSlots(slots, (k) => ({ xf: M.xf[k] ?? IDENT, plain: M.plain[k] ?? null }));
    enc.copyTextureToTexture({ texture: color! }, { texture: ctx.getCurrentTexture() }, [color!.width, color!.height]);
    device.queue.submit([enc.finish()]);
    scene.flushOld();
    // ---- the counters (glviewport.ts's, read by frames.mjs, ux.mjs and the performance panel) ----
    stats.frameTris = tris, stats.frameDraws = draws, stats.clip = box;
    stats.moving = { moving, slice, budget, t: cutT, batches: M.dbs.length, culled, outlines: unfinished.length, tris, proxied: prxCells.length };
    if (stats.proxies) stats.proxies.drawnCells = drawPrx ? prxCells.length : 0, stats.proxies.drawnTris = prxTris, stats.proxies.standIns = nStand, stats.proxies.on = prxOn;
    stats.q2 = { edges: edgesOn, ortho: v.ortho && !v.plan, edgeSegs: edgesOn ? segs : 0 };
    stats.linkProxies = M.linkPrx.filter((x): x is GpuLinkPrx => !!x).map((x) => ({ name: x.name, cells: x.p.cells.length, tris: x.p.tris.reduce((a, b) => a + b, 0),
      bytes: x.p.pos.byteLength + x.p.idx.byteLength, drawnCells: x.drawnCells, drawnTris: x.drawnTris }));
    if ((moving || slice) && lastBudgeted && lastFrameAt && tNow - lastFrameAt < 250 && lastFrameTris > 0) {
      const r = lastFrameTris / Math.max(1, tNow - lastFrameAt);
      trisPerMs = trisPerMs ? 0.5 * trisPerMs + 0.5 * r : r;
    }
    lastFrameAt = tNow, lastFrameTris = tris, lastBudgeted = moving || slice;
    last.moving = moving, last.t0 = tNow;
    return { t: cutT, outlines: unfinished.length, proxied: prxCells.length };
  };

  /** After a moving frame: STILL_MS later (no button held), the rest in slices, one an animation frame. */
  const fillAfter = (v: GpuView, at: number, g: number) => {
    const step = (fill: { t: number } | null, edges: boolean) => {
      if (g !== gen) return;
      const r = draw(v, edges ? "edges" : "slice", fill);
      stats.frames++;
      perfFrame(false, last.t0, perfWaits() ? device.queue.onSubmittedWorkDone() : null);
      if (edges) { complete(); return; }
      const next = r.t === 0 ? null : { t: r.t };
      requestAnimationFrame(() => step(next, next === null));
    };
    const wait = () => {
      if (g !== gen) return;
      if (v.held() || performance.now() < at) { setTimeout(wait, Math.max(16, at - performance.now())); return; }
      requestAnimationFrame(() => step({ t: Infinity }, false));
    };
    setTimeout(wait, Math.max(0, at - performance.now()));
  };
  const complete = () => { if (stats.fullFrameMs === null && takenAll) stats.fullFrameMs = last.t0; };
  const counters = (M: GpuModels | null) => {
    stats.gpuSplit = { webgpu: r.bytes!(), webgl: M ? M.glHeld() : 0, taken: scene.models.filter(Boolean).length, models: M ? M.slots.filter((x) => x.gl).length : 0 };
  };

  /** The links' proxies before their mesh (the first picture of a project of links). */
  const drawPreview = (v: GpuView, M: GpuModels) => {
    const planes = frustumPlanes(v.mvp);
    frameUniform(v, null);
    const enc = device.createCommandEncoder();
    const pass = enc.beginRenderPass({
      colorAttachments: [{ view: color!.createView(), loadOp: "clear", storeOp: "store", clearValue: [BACKGROUND[0] / 255, BACKGROUND[1] / 255, BACKGROUND[2] / 255, 1] }],
      depthStencilAttachment: { view: depth!.createView(), depthLoadOp: "clear", depthClearValue: 1, depthStoreOp: "store" },
    });
    const slots: number[] = [];
    let tris = 0, draws = 0;
    const need = M.prePrx.length + 1;
    if (need > drawsCap) { scene.drop(drawsBuf); drawsCap = 2 * need; drawsBuf = scene.buf(SLOT * drawsCap, BU.UNIFORM); }
    const bg = bind0();
    M.prePrx.forEach((q, i) => {
      if (!q.visible) return;
      const us = Uint8Array.from(q.x.p.cells, (c) => (c.parts.length ? 1 : 0));
      const { cells } = M.cutLink(q.x, q.xf, us, planes, v.eye, STILL_PROXY_PX);
      const b = scene.linkProxies(q.x), c = M.linkColour(q.colour);
      slots.push(i + 1, 0, 0, 0, c[0]! / 255, c[1]! / 255, c[2]! / 255, 1);
      pass.setPipeline(pipes.get({ kind: "lprx", slope: 2.5, bias: 16 })), pass.setBindGroup(0, bg, [SLOT * (slots.length / 8 - 1)]), pass.setBindGroup(1, bind1(-1));
      pass.setVertexBuffer(0, b.pos), pass.setIndexBuffer(b.idx, "uint32");
      for (const cell of cells) for (const part of q.x.p.cells[cell]!.parts) if (part.ni) pass.drawIndexed(part.ni, 1, part.i0, 0, 0), draws++, tris += part.ni / 3;
    });
    pass.end();
    writeSlots(slots, (k) => ({ xf: k > 0 ? M.prePrx[k - 1]?.xf ?? IDENT : IDENT, plain: null }));
    enc.copyTextureToTexture({ texture: color! }, { texture: ctx.getCurrentTexture() }, [color!.width, color!.height]);
    device.queue.submit([enc.finish()]);
    scene.flushOld();
    stats.frameTris = tris, stats.frameDraws = draws;
    last.moving = false, last.t0 = performance.now();
  };

  const r: GpuRenderer = {
    last,
    prepare(v: GpuView) { return v.models ? sync(v.models) : true; },
    frame(v: GpuView) {
      gen++;
      const m = v.mesh, M = v.models;
      const resized = targets(Math.max(1, v.w), Math.max(1, v.h));
      if (!M || (!m && !M.prePrx.length) || (m && v.shown.length !== m.ids.length)) { // nothing to draw yet: the background
        const enc = device.createCommandEncoder();
        enc.beginRenderPass({ colorAttachments: [{ view: color!.createView(), loadOp: "clear", storeOp: "store", clearValue: [BACKGROUND[0] / 255, BACKGROUND[1] / 255, BACKGROUND[2] / 255, 1] }] }).end();
        enc.copyTextureToTexture({ texture: color! }, { texture: ctx.getCurrentTexture() }, [color!.width, color!.height]);
        device.queue.submit([enc.finish()]);
        last.moving = false, last.t0 = performance.now();
        return;
      }
      sync(M);
      scene.setStates(M.states, M.statesKey);
      scene.setProxies(v.prx && m && v.prx.nElements <= m.ids.length ? v.prx : null);
      scene.keepLinkProxies(new Set([...M.linkPrx, ...M.prePrx.map((q) => q.x)].filter((x): x is GpuLinkPrx => !!x).map((x) => x.p)));
      if (!m) { drawPreview(v, M); counters(M); return; }
      if (m !== meshNow) {
        meshNow = m;
        let t = 0;
        for (let e = 0; e < m.ids.length; e++) t += m.ni[e]! / 3;
        firstOfMesh = t >= FIRST_BUDGET_TRIS;
        pickCache.clear(), pickBytes = 0;
      }
      const first = firstOfMesh;
      firstOfMesh = false;
      const moving = (first || (lastCamKey !== null && v.camKey !== lastCamKey)) && v.fps > 0 && !resized && !M.ghost();
      lastCamKey = v.camKey;
      const res = draw(v, moving ? "moving" : "whole", null);
      counters(M);
      if (moving) {
        if (res.t > 0 || res.outlines > 0 || res.proxied > 0) fillAfter(v, first ? performance.now() : performance.now() + STILL_MS, gen);
        else complete();
      } else if (!res.outlines) complete();
    },
    done: () => device.queue.onSubmittedWorkDone(),
    show(on: boolean) { const v = on ? "visible" : "hidden"; if (canvas.style.visibility !== v) canvas.style.visibility = v; if (!on) gen++; },
    async pixels() {
      const tex = color!, w = tex.width, h = tex.height, bpr = Math.ceil((4 * w) / 256) * 256;
      const buf = device.createBuffer({ size: bpr * h, usage: BU.COPY_DST | BU.MAP_READ });
      const enc = device.createCommandEncoder();
      enc.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow: bpr }, [w, h]);
      device.queue.submit([enc.finish()]);
      await buf.mapAsync(1);
      const src = new Uint8Array(buf.getMappedRange()), out = new Uint8Array(4 * w * h), bgra = format.startsWith("bgra");
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        const s = y * bpr + 4 * x, o = 4 * (y * w + x);
        out[o] = src[s + (bgra ? 2 : 0)]!, out[o + 1] = src[s + 1]!, out[o + 2] = src[s + (bgra ? 0 : 2)]!, out[o + 3] = src[s + 3]!;
      }
      buf.unmap(), buf.destroy();
      return { w, h, rgba: out };
    },
    // GPU2: back to WebGL2: each model's buffers read back and given to WebGL2's (the links first, the open model last:
    // the picture keeps it until the switch), WebGPU drawing meanwhile (a model given back is drawn as its proxies)
    async handBack(v: GpuView) {
      const M = v.models;
      if (!M) return;
      handing = true;
      try {
        const ks = M.slots.filter((x) => x.gl && scene.models[x.k]?.gl === x.gl).map((x) => x.k).sort((a, b) => (a === 0 ? 1 : b === 0 ? -1 : a - b));
        for (const [i, k] of ks.entries()) {
          const g = scene.models[k], x = M.slots[k];
          if (!g || !x?.gl || g.gl !== x.gl) continue;
          const names = GL_NAMES.filter((n) => g.b[n]);
          const got = await readBack(names.map((n) => ({ b: g.b[n]!, at: 0, n: g.size[n] ?? 0 })));
          if (M.slots[k]?.gl !== g.gl) continue; // released meanwhile
          const a: Partial<GlArrays> = {};
          names.forEach((n, j) => { a[n] = got[j]!; });
          M.fillGl(g.gl, a);
          scene.release(k);
          perfRenderer.progress = `WebGL2: ${i + 1} of ${ks.length} models given back`;
        }
        M.fillPrx();
      } finally { handing = false; }
    },
    bytes: () => scene.bytes + cull.bytes + (ghost ? ghost.pos.size + ghost.idx.size + ghost.edges.size + ghost.marks.size : 0) + drawsBuf.size + outlineBuf.size,
    tris(k, key, vFirst, nv, iAt, ni, shape) {
      const g = scene.models[k];
      if (!g) return undefined;
      const ck = `${k}|${key}`, hit = pickCache.get(ck);
      if (hit) { pickCache.delete(ck), pickCache.set(ck, hit); return hit; }
      const ib = shape ? g.b.shapeIdx : g.b.idx;
      if (!g.b.pos || !ib || pickAsked.has(ck)) return null;
      pickAsked.add(ck);
      void readBack([{ b: g.b.pos, at: 12 * vFirst, n: 12 * nv }, { b: ib, at: 4 * iAt, n: 4 * ni }]).then(([pa, ia]) => {
        pickAsked.delete(ck);
        if (scene.models[k] !== g) return;
        const pos = new Float32Array(pa!), idx = new Uint32Array(ia!);
        for (let i = 0; i < idx.length; i++) idx[i] = idx[i]! - vFirst;
        pickCache.set(ck, { pos, idx }), pickBytes += pos.byteLength + idx.byteLength;
        stats.gpuPickReads = (stats.gpuPickReads ?? 0) + 1;
        for (const [q, x] of pickCache) { if (pickBytes <= PICK_CACHE_BYTES) break; pickCache.delete(q), pickBytes -= x.pos.byteLength + x.idx.byteLength; }
      }, () => pickAsked.delete(ck));
      return null;
    },
    destroy(keepDevice = false) {
      gen++;
      canvas.remove();
      if (ghost) for (const b of [ghost.pos, ghost.idx, ghost.edges, ghost.marks]) scene.drop(b);
      scene.destroy(), cull.destroy();
      for (const b of [frameU, cubeBuf, identity, boxBuf, outlineBuf, drawsBuf]) b.destroy();
      color?.destroy(), depth?.destroy();
      ctx.unconfigure();
      perfRenderer.progress = null;
      delete stats.gpuSplit;
      if (!keepDevice) device.destroy(); // GPU2: kept for the next switch
      delete window.__bbGpu;
    },
  };
  window.__bbGpu = { done: r.done, pixels: () => r.pixels(), adapter: d.adapter, fallback: d.fallback,
    scene: () => ({ batches: lastM?.dbs.length ?? 0, groups: lastM?.groups.length ?? 0, copies: lastM?.groups.reduce((n, g) => n + g.count, 0) ?? 0, bytes: r.bytes!(),
      models: scene.models.filter(Boolean).length }) };
  return r;
}

declare global {
  interface Window {
    /** GPU1: the WebGPU renderer while it draws (tests, frames.mjs --renderer webgpu). */
    __bbGpu?: { done: () => Promise<void>; pixels: () => Promise<{ w: number; h: number; rgba: Uint8Array }>; adapter: unknown; fallback: boolean;
      scene: () => { batches: number; groups: number; copies: number; bytes: number; models: number } };
  }
}
