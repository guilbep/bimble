// The WebGPU renderer's frame (ticket GPU1, ADR 0083): the scene + camera + states the viewport gives
// (gpu/switch.ts GpuView) turned into pixels, as glviewport.ts draw() turns them with WebGL2, on a
// second canvas stacked over #bimble (under the overlay; no pointer events: the viewport's handlers,
// tools, picking and overlays stay as they are).
// The frame (ADR 0078 decision 4 and amendment 2, unchanged): a moving frame (the camera changed)
// draws, over the batches in view, the elements largest on screen first within the triangle budget
// (moving_fps times the triangles a ms the last budgeted frames drew, at least MIN_MOVING_TRIS), far
// cells as their proxies (3 px; at most half the budget), the unfinished leaves as proxies or their
// batches' outlines: never an empty picture. 150 ms after the camera stops (no button held) the
// rest is added in slices of the same budget, one an animation frame, into the kept picture (an
// offscreen colour and depth target: a canvas texture does not keep its contents), then every edge
// once more; any new frame abandons the fill. A still frame for another reason draws the whole picture.
// GPU culling (the setting gpu_cull, ADR 0036 #10): the copies of each group are tested on the GPU
// (gpu/cull.ts) and drawn with one drawIndexedIndirect per group; off, each run of copies is a direct
// instanced draw (WebGPU's firstInstance: no attribute re-pointing as in WebGL2).
import { boxInFrustum, budgetCut, cutEnd, frustumPlanes, MIN_MOVING_TRIS, type Cut } from "../gl/cull";
import { cutProxies, fitProxies, standIns, usableCells } from "../gl/proxies";
import { perfFrame, perfWaits, renderer as perfRenderer } from "../perf";
import { GpuCull } from "./cull";
import { BU, TU, type GpuDevice } from "./device";
import { DEPTH, Pipelines, type PipeKey } from "./pipelines";
import { BACKGROUND, Scene, SLOT, SLOT_BOX, SLOT_GROUP0, SLOT_LINK, SLOT_MAIN, SLOT_OUTLINE, SLOT_PRX, type GBatch } from "./scene";
import type { GpuRenderer, GpuView } from "./switch";

const STILL_MS = 150, MOVING_PROXY_PX = 3, STILL_PROXY_PX = 1, FIRST_BUDGET_TRIS = 1000000, AMBIENT = 0.62, CUT_SHADE = 0.72;

/** The viewport's stats (glviewport.ts GlStats): the fields this renderer writes. */
interface Stats {
  frames: number; frameTris?: number; frameDraws?: number; gpuBytes?: number; fullFrameMs?: number | null; clip?: unknown;
  moving?: { moving: boolean; slice: boolean; budget: number; t: number; batches: number; culled: number; outlines: number; tris: number; proxied?: number };
  proxies?: { drawnCells: number; drawnTris: number; standIns: number; on: boolean };
  q2?: { edges: boolean; ortho: boolean; edgeSegs: number };
}

export function createRenderer(glCanvas: HTMLCanvasElement, d: GpuDevice, statsObj: object): GpuRenderer {
  const { device, format } = d;
  const stats = statsObj as Stats;
  const canvas = document.createElement("canvas");
  canvas.setAttribute("aria-hidden", "true");
  canvas.dataset.bbGpu = "webgpu";
  canvas.style.cssText = "position:absolute;inset:0;width:100%;height:100%;pointer-events:none;display:block";
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
  let color: GPUTexture | null = null, depth: GPUTexture | null = null;
  let group: GPUBindGroup | null = null, groupKey: unknown[] = [];

  // the frame policy's state (glviewport.ts draw() and its loop)
  let lastCamKey: string | null = null, lastFrameAt = 0, lastFrameTris = 0, trisPerMs = 0, lastBudgeted = false;
  let firstOfMesh = false, gen = 0, lastColour: Uint32Array | null = null, usable = new Uint8Array(0), usableKey: unknown[] = [];
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
  const bindGroup = (): GPUBindGroup => {
    const key = [scene.colours, scene.insts, scene.visible, scene.draws];
    if (group && key.every((b, i) => b === groupKey[i])) return group;
    groupKey = key;
    group = device.createBindGroup({ layout: pipes.layout, entries: [
      { binding: 0, resource: { buffer: frameU } }, { binding: 1, resource: { buffer: scene.colours } }, { binding: 2, resource: { buffer: scene.insts } },
      { binding: 3, resource: { buffer: scene.visible } }, { binding: 4, resource: { buffer: scene.draws, size: 32 } },
    ] });
    return group;
  };

  /** One frame: `kind` moving (the budget), slice (the fill's next part, into the kept picture), edges
   *  (the fill's last pass) or whole. */
  const draw = (v: GpuView, kind: "moving" | "slice" | "edges" | "whole", fill: { t: number } | null): { t: number; outlines: number; proxied: number } => {
    const m = v.mesh!, tNow = performance.now();
    const moving = kind === "moving", slice = kind === "slice" || kind === "edges";
    const box = v.clip, planes = v.noCull ? null : frustumPlanes(v.mvp);
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
    // ---- what each batch draws (glviewport.ts draw(): frustum, proxies, budget, outlines, stand-ins) ----
    const p = scene.prx;
    let prxCells: number[] = [], replaced: Uint8Array | null = null;
    const prxOn = p !== null && !v.noProxies && !box && !v.plan && v.proxiesOn;
    const budget0 = frameBudget(v.fps);
    if (prxOn) {
      const uk = [scene.listKey[1], v.colour, p];
      if (uk.some((x, i) => x !== usableKey[i])) usableKey = uk, usable = new Uint8Array(usableCells(p, (el) => v.shown[el] === 1 && !v.selected(el)));
      ({ cells: prxCells, replaced } = cutProxies(p, usable, planes ? (b) => boxInFrustum(planes, b, 0) : null, e, v.mmPerPx, moving ? MOVING_PROXY_PX : STILL_PROXY_PX));
      if (moving && prxCells.length) prxCells = fitProxies(p, usable, prxCells, replaced, budget0 / 2);
    }
    let culled = 0, cutT = 0, budget = Infinity;
    const seen: { d: GBatch; c: Cut }[] = [];
    for (const d of scene.batches) {
      if (replaced && d.b.group >= 0 && replaced[d.b.group]) { d.k0 = d.k1 = 0; continue; }
      if (planes && !boxInFrustum(planes, d.b.box, 0)) { d.k0 = d.k1 = 0; culled++; continue; }
      const bx = d.b.box, q = [0, 1, 2].map((a) => Math.min(Math.max(e[a]!, bx[a]!), bx[3 + a]!));
      const dist = v.ortho ? 1 : Math.max(1, Math.hypot(q[0]! - e[0]!, q[1]! - e[1]!, q[2]! - e[2]!));
      seen.push({ d, c: { b: d.b, dist, k0: slice && fill ? cutEnd({ b: d.b, dist, k0: 0 }, fill.t) : 0 } });
    }
    if (kind === "edges") for (const { d } of seen) d.k0 = d.k1 = 0;
    else {
      if (moving || slice) budget = budget0;
      if (moving && prxCells.length && budget < Infinity) budget = Math.max(MIN_MOVING_TRIS, budget - prxCells.reduce((n, k) => n + p!.tris[k]!, 0));
      const cut = budgetCut(seen.map((x) => x.c), budget);
      cutT = cut.t;
      for (const { d, c } of seen) d.k0 = c.k0, d.k1 = cutEnd(c, cut.t);
    }
    let unDs = moving ? seen.filter(({ d }) => d.k1 < d.b.n) : [];
    let nStand = 0;
    if (prxOn && replaced && unDs.length) {
      const leaves = new Set<number>();
      for (const { d } of unDs) if (d.b.group >= 0) leaves.add(d.b.group);
      const used = prxCells.reduce((n, k) => n + p!.tris[k]!, 0);
      const extra = standIns(p!, usable, leaves, replaced, Math.max(0, budget0 / 2 - used));
      nStand = extra.length;
      if (extra.length) {
        prxCells.push(...extra);
        const r = replaced;
        for (const { d } of seen) if (d.b.group >= 0 && r[d.b.group]) d.k0 = d.k1 = 0;
        unDs = unDs.filter(({ d }) => !(d.b.group >= 0 && r[d.b.group]));
      }
    }
    const unfinished = unDs.map(({ d }) => d.b.box);
    const drawPrx = prxCells.length > 0 && (!slice || fill?.t === Infinity);
    const prxRuns = (seg: boolean): [number, number][] => {
      const runs: [number, number][] = [];
      for (const k of [...prxCells].sort((a, b) => a - b)) for (const q of p!.cells[k]!.parts) {
        const a = seg ? q.s0 : q.i0, n = seg ? q.ns : q.ni, lr = runs[runs.length - 1];
        if (!n) continue;
        if (lr && lr[0] + lr[1] === a) lr[1] += n;
        else runs.push([a, n]);
      }
      return runs;
    };
    // ---- encode ----
    const enc = device.createCommandEncoder();
    const gpuCull = perfRenderer.gpuCull && scene.nInst > 0;
    if (gpuCull) {
      const ranges = new Uint32Array(2 * scene.nInstBatches);
      for (const d of scene.batches) if (d.ri >= 0) ranges[2 * d.ri] = d.k0, ranges[2 * d.ri + 1] = d.k1;
      cull.run(enc, scene, planes, e, v.ortho, cutT, moving && cutT > 0, ranges);
    }
    const clear = !slice || fill?.t === Infinity;
    const pass = enc.beginRenderPass({
      colorAttachments: [{ view: color!.createView(), loadOp: clear ? "clear" : "load", storeOp: "store",
        clearValue: [BACKGROUND[0] / 255, BACKGROUND[1] / 255, BACKGROUND[2] / 255, 1] }],
      depthStencilAttachment: { view: depth!.createView(), depthLoadOp: clear ? "clear" : "load", depthClearValue: 1, depthStoreOp: "store" },
    });
    const bg = bindGroup();
    let tris = 0, draws = 0, segs = 0, prxTris = 0;
    let curPipe: GPURenderPipeline | null = null, curSlot = -1;
    const use = (k: PipeKey, slot: number) => {
      const pl = pipes.get(k);
      if (pl !== curPipe) pass.setPipeline(pl), curPipe = pl;
      if (slot !== curSlot) pass.setBindGroup(0, bg, [SLOT * slot]), curSlot = slot;
    };
    const baseFront: GPUFrontFace = box && v.mirror ? "cw" : "ccw";
    const flip = (f: GPUFrontFace): GPUFrontFace => (f === "ccw" ? "cw" : "ccw");
    const merged = (link: boolean, k: PipeKey) => {
      let a = -1, z = -1, bound = false;
      const flush = () => {
        if (z <= a) return;
        if (!bound) { use({ ...k, kind: "merged" }, SLOT_MAIN); pass.setVertexBuffer(0, scene.pos); pass.setVertexBuffer(1, scene.vel); pass.setIndexBuffer(scene.faceIdx, "uint32"); bound = true; }
        pass.drawIndexed(z - a, 1, a, 0, 0), draws++, tris += (z - a) / 3;
      };
      for (const d of scene.batches) {
        if (d.g >= 0 || d.link !== link || d.k1 <= d.k0) continue;
        const s = d.f0 + 3 * d.b.cum[d.k0]!, t = d.f0 + 3 * d.b.cum[d.k1]!;
        if (s === z) z = t;
        else flush(), a = s, z = t;
      }
      flush();
    };
    const instFaces = (link: boolean, k: PipeKey) => {
      scene.groups.forEach((g, gi) => {
        if (g.link !== link || !g.ni) return;
        const front = g.mirrored ? flip(k.front ?? "ccw") : k.front ?? "ccw";
        let any = 0;
        for (const d of g.batches) any += d.k1 - d.k0;
        if (!any) return;
        const bindInst = (slot: number) => { use({ ...k, kind: "inst", front }, slot); pass.setVertexBuffer(0, scene.pos); pass.setIndexBuffer(scene.shapeIdx, "uint32"); };
        if (gpuCull) {
          bindInst(SLOT_GROUP0 + gi);
          pass.drawIndexedIndirect(scene.args, 20 * gi), draws++, tris += (g.ni / 3) * any; // at most: the GPU may keep fewer
          return;
        }
        bindInst(SLOT_MAIN);
        let a = -1, z = -1;
        const flush = () => { if (z > a) pass.drawIndexed(g.ni, z - a, g.i0, 0, a), draws++, tris += (g.ni / 3) * (z - a); };
        for (const d of g.batches) {
          if (d.k1 <= d.k0) continue;
          const s = g.first + d.b.first + d.k0, t = g.first + d.b.first + d.k1;
          if (s === z) z = t;
          else flush(), a = s, z = t;
        }
        flush();
      });
    };
    const prxFaces = (k: PipeKey) => {
      if (!drawPrx || !scene.prxPos) return;
      use({ ...k, kind: "proxy" }, SLOT_PRX);
      pass.setVertexBuffer(0, scene.prxPos), pass.setVertexBuffer(1, scene.prxCol!), pass.setIndexBuffer(scene.prxIdx!, "uint32");
      for (const [a, n] of prxRuns(false)) pass.drawIndexed(n, 1, a, 0, 0), draws++, tris += n / 3, prxTris += n / 3;
    };
    const faces = (push: number, cullMode: GPUCullMode) => {
      const main: PipeKey = { kind: "merged", cull: cullMode, front: baseFront, slope: 1.5 + push, bias: 2 + 2.4 * push };
      const lk: PipeKey = { kind: "merged", cull: cullMode, front: baseFront, slope: 2.5 + push, bias: 16 + 2.4 * push };
      merged(false, main), instFaces(false, main), prxFaces(main);
      merged(true, lk), instFaces(true, lk);
    };
    if (!box) faces(0, "none");
    else faces(0, "back"), faces(v.plan ? -3 : 2.5, "front");
    // edges
    const edgesOn = v.edges && !v.noEdges;
    if (kind === "edges") for (const { d } of seen) d.k0 = 0, d.k1 = d.b.n;
    if (edgesOn) {
      const mergedEdges = (link: boolean) => {
        let a = -1, z = -1, bound = false;
        const flush = () => {
          if (z <= a) return;
          if (!bound) { use({ kind: "edge" }, link ? SLOT_LINK : SLOT_MAIN); pass.setVertexBuffer(0, scene.edgePos); pass.setVertexBuffer(1, scene.edgeNrm); pass.setIndexBuffer(scene.edgeIdx, "uint32"); bound = true; }
          pass.drawIndexed(z - a, 1, a, 0, 0), draws++, segs += (z - a) / 2;
        };
        for (const d of scene.batches) {
          if (d.g >= 0 || d.link !== link || d.k1 <= d.k0) continue;
          const s = d.e0 + 2 * d.b.ecum[d.k0]!, t = d.e0 + 2 * d.b.ecum[d.k1]!;
          if (s === z) z = t;
          else flush(), a = s, z = t;
        }
        flush();
      };
      const instEdges = (link: boolean) => {
        scene.groups.forEach((g, gi) => {
          if (g.link !== link || !g.ns) return;
          let any = 0;
          for (const d of g.batches) any += d.k1 - d.k0;
          if (!any) return;
          if (gpuCull && kind !== "edges" && cull.edgeArgs) {
            use({ kind: "edgeInst" }, SLOT_GROUP0 + gi);
            pass.setVertexBuffer(0, scene.edgePos), pass.setVertexBuffer(1, scene.edgeNrm);
            pass.drawIndirect(cull.edgeArgs, 16 * gi), draws++, segs += g.ns * any;
            return;
          }
          use({ kind: "edgeInst" }, link ? SLOT_LINK : SLOT_MAIN);
          pass.setVertexBuffer(0, scene.edgePos), pass.setVertexBuffer(1, scene.edgeNrm);
          let a = -1, z = -1;
          const flush = () => { if (z > a) pass.draw(2 * g.ns, z - a, 2 * g.s0, a), draws++, segs += g.ns * (z - a); };
          for (const d of g.batches) {
            if (d.k1 <= d.k0) continue;
            const s = g.first + d.b.first + d.k0, t = g.first + d.b.first + d.k1;
            if (s === z) z = t;
            else flush(), a = s, z = t;
          }
          flush();
        });
      };
      mergedEdges(false), instEdges(false);
      if (prxCells.length && (drawPrx || kind === "edges") && scene.prxEdge) {
        use({ kind: "edge" }, SLOT_PRX);
        pass.setVertexBuffer(0, scene.prxEdge), pass.setVertexBuffer(1, scene.prxNrm!);
        for (const [a, n] of prxRuns(true)) pass.draw(2 * n, 1, 2 * a, 0), draws++, segs += n;
      }
      mergedEdges(true), instEdges(true);
    }
    if (unfinished.length) { // the outlines of what a moving frame could not draw
      if (unfinished.length > outlineCap) outlineBuf.destroy(), outlineCap = 2 * unfinished.length, outlineBuf = scene.buf(48 * outlineCap, BU.VERTEX);
      const mat = new Float32Array(12 * unfinished.length);
      unfinished.forEach((b, i) => mat.set([b[3]! - b[0]!, 0, 0, 0, b[4]! - b[1]!, 0, 0, 0, b[5]! - b[2]!, b[0]!, b[1]!, b[2]!], 12 * i));
      device.queue.writeBuffer(outlineBuf, 0, mat);
      use({ kind: "box" }, SLOT_OUTLINE);
      pass.setVertexBuffer(0, cubeBuf), pass.setVertexBuffer(1, outlineBuf);
      pass.draw(24, unfinished.length), draws++;
    }
    if (box && !v.plan) { // the section box's 12 edges, over the model (turned by its yaw)
      const c = Math.cos(box.yaw ?? 0), s = Math.sin(box.yaw ?? 0);
      const corner = (i: number) => { const x = (i & 1 ? box.max[0] : box.min[0]) ?? 0, y = (i & 2 ? box.max[1] : box.min[1]) ?? 0, z = (i & 4 ? box.max[2] : box.min[2]) ?? 0; return [c * x - s * y, s * x + c * y, z]; };
      const vb: number[] = [];
      for (let i = 0; i < 8; i++) for (let k = 0; k < 3; k++) if (!((i >> k) & 1)) vb.push(...corner(i), ...corner(i | (1 << k)));
      device.queue.writeBuffer(boxBuf, 0, new Float32Array(vb));
      use({ kind: "box", depthTest: false }, SLOT_BOX);
      pass.setVertexBuffer(0, boxBuf), pass.setVertexBuffer(1, identity);
      pass.draw(24, 1), draws++;
    }
    pass.end();
    enc.copyTextureToTexture({ texture: color! }, { texture: ctx.getCurrentTexture() }, [color!.width, color!.height]);
    device.queue.submit([enc.finish()]);
    scene.flushOld();
    // ---- the counters (glviewport.ts's, read by frames.mjs, ux.mjs and the performance panel) ----
    stats.frameTris = tris, stats.frameDraws = draws, stats.gpuBytes = scene.bytes, stats.clip = box;
    stats.moving = { moving, slice, budget, t: cutT, batches: scene.batches.length, culled, outlines: unfinished.length, tris, proxied: prxCells.length };
    if (stats.proxies) stats.proxies.drawnCells = drawPrx ? prxCells.length : 0, stats.proxies.drawnTris = prxTris, stats.proxies.standIns = nStand, stats.proxies.on = prxOn;
    stats.q2 = { edges: edgesOn, ortho: v.ortho && !v.plan, edgeSegs: edgesOn ? segs : 0 };
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
  const complete = () => { if (stats.fullFrameMs === null) stats.fullFrameMs = last.t0; };

  const r: GpuRenderer = {
    last,
    frame(v: GpuView) {
      gen++;
      const m = v.mesh;
      const resized = targets(Math.max(1, v.w), Math.max(1, v.h));
      if (!m || v.shown.length !== m.ids.length) { // no mesh (or its lists not made yet): the background
        const enc = device.createCommandEncoder();
        enc.beginRenderPass({ colorAttachments: [{ view: color!.createView(), loadOp: "clear", storeOp: "store", clearValue: [BACKGROUND[0] / 255, BACKGROUND[1] / 255, BACKGROUND[2] / 255, 1] }] }).end();
        enc.copyTextureToTexture({ texture: color! }, { texture: ctx.getCurrentTexture() }, [color!.width, color!.height]);
        device.queue.submit([enc.finish()]);
        last.moving = false, last.t0 = performance.now();
        return;
      }
      if (m !== scene.mesh) {
        scene.setMesh(m, v.boxes);
        let t = 0;
        for (let e = 0; e < m.ids.length; e++) t += m.ni[e]! / 3;
        firstOfMesh = t >= FIRST_BUDGET_TRIS, lastColour = null;
      }
      if (v.prx !== scene.prx) scene.setProxies(v.prx && v.prx.nElements <= m.ids.length ? v.prx : null);
      const lk = scene.listKey;
      if (lk[0] !== m || lk[1] !== v.shown || lk[2] !== (scene.prx?.leafOf ?? null)) scene.setLists(m, v.boxes, v.shown, scene.prx);
      if (v.colour !== lastColour) scene.setColours(v.colour), lastColour = v.colour;
      const first = firstOfMesh;
      firstOfMesh = false;
      const moving = (first || (lastCamKey !== null && v.camKey !== lastCamKey)) && v.fps > 0 && !resized;
      lastCamKey = v.camKey;
      const res = draw(v, moving ? "moving" : "whole", null);
      if (moving) {
        if (res.t > 0 || res.outlines > 0 || res.proxied > 0) fillAfter(v, first ? performance.now() : performance.now() + STILL_MS, gen);
        else complete();
      } else complete();
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
    destroy(keepDevice = false) {
      gen++;
      canvas.remove();
      scene.destroy(), cull.destroy();
      for (const b of [frameU, cubeBuf, identity, boxBuf, outlineBuf]) b.destroy();
      color?.destroy(), depth?.destroy();
      ctx.unconfigure();
      if (!keepDevice) device.destroy(); // GPU2: kept for the next switch
      delete window.__bbGpu;
    },
  };
  window.__bbGpu = { done: r.done, pixels: () => r.pixels(), adapter: d.adapter, fallback: d.fallback, scene: () => ({ batches: scene.batches.length, groups: scene.groups.length, copies: scene.nInst, bytes: scene.bytes }) };
  return r;
}

declare global {
  interface Window {
    /** GPU1: the WebGPU renderer while it draws (tests, frames.mjs --renderer webgpu). */
    __bbGpu?: { done: () => Promise<void>; pixels: () => Promise<{ w: number; h: number; rgba: Uint8Array }>; adapter: unknown; fallback: boolean;
      scene: () => { batches: number; groups: number; copies: number; bytes: number } };
  }
}
