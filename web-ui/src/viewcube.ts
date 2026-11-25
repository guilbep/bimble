// View cube and compass of the lab's WebGL viewport (ticket VC1), a port of the Rust viewer's
// crates/bimble-viewer/src/viewcube.rs and its navigation bar (lib.rs `navigation`):
// - a cube in the top-right corner of the 3D view (120 px square, 8 px from the edges), drawn with
//   the camera's orientation; 26 targets: 6 faces, 12 edges, 8 corners (bands of 0.35 of the half
//   size). A click turns the camera to look from that direction (Top, Front, an edge at 45°, an
//   isometric corner) in 0.3 s (smoothstep, lib.rs TURN_SECONDS and `animate`); a drag on the cube
//   orbits (0.008 rad/px), a drag beside it (the compass ring) turns about the vertical (0.01 rad/px);
//   the hovered target is filled with the control accent (ink: pink is selection only, ADR 0049);
// - a compass ring under the cube, N in red, the far half behind the cube, the near half in front;
// - Home (the default view, yaw -55°, pitch 28°, turned the same way, then fit all) and Fit all under
//   the cube.
// The cube and the compass are the building's (ADR 0045): they turn with project north and FRONT is
// the building's south. 2D views (plans) are seen square on: no cube, no Home (ADR 0021); Fit all stays.
// The cube is its own canvas over the viewport: a click on it never reaches the 3D canvas, so it
// never picks or selects an element. Camera state stays in the page (view state, not model state).
// Geometry is plain functions on the camera angles (yaw, pitch as bimble-render's Camera).

/** A view direction on the cube: each component -1, 0 or 1 (not all 0). Faces have one non-zero
 *  component, edges two, corners three. It points from the model towards the camera. */
export type Dir = [number, number, number];
type V3 = [number, number, number];
type P2 = [number, number];

/** bimble-render Camera::default: the first 3D view and Home (plus project north). */
export const HOME_YAW = (-55 * Math.PI) / 180;
export const HOME_PITCH = (28 * Math.PI) / 180;
/** The camera's pitch limit (bimble-render Camera::orbit). */
export const PITCH_LIMIT = 1.57;
/** Duration of a view-cube camera turn, ms (lib.rs TURN_SECONDS). */
export const TURN_MS = 300;

/**
 * PROJECT NORTH HOOK (ADR 0045): the angle (radians) the building's plan is turned by, as Rust's
 * viewing.rs `project_north` = `georef.plan_turn(false)` = the georeference's rotation_deg in
 * radians. The C model has no georeference yet and no query gives it: 0 until the georeference
 * ticket returns it here (from its query). Everything else (cube, compass, Home, the first view)
 * already goes through this function.
 */
export function projectNorth(): number {
  return 0;
}

const dot = (a: V3, b: V3) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const cross = (a: V3, b: V3): V3 => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];

/** Camera yaw and pitch (radians) looking at the model from `d` (viewcube.rs `orientation`). */
export function orientation(d: Dir): { yaw: number; pitch: number } {
  const l = Math.hypot(d[0], d[1], d[2]) || 1;
  const v: V3 = [d[0] / l, d[1] / l, d[2] / l];
  // Straight above or below: keep north up (yaw -90°: looking towards +Y).
  if (Math.hypot(v[0], v[1]) < 1e-6) return { yaw: -Math.PI / 2, pitch: Math.sign(v[2]) * PITCH_LIMIT };
  return { yaw: Math.atan2(v[1], v[0]), pitch: Math.asin(v[2]) };
}

export function label(d: Dir): string | null {
  const k = d.join(",");
  return ({ "0,0,1": "TOP", "0,0,-1": "BOTTOM", "0,-1,0": "FRONT", "0,1,0": "BACK", "1,0,0": "RIGHT", "-1,0,0": "LEFT" } as Record<string, string>)[k] ?? null;
}

/** Screen axes of the camera: right, up, towards the viewer (unit vectors, world space). */
export function cameraAxes(yaw: number, pitch: number): [V3, V3, V3] {
  const cp = Math.cos(pitch), sp = Math.sin(pitch);
  const back: V3 = [cp * Math.cos(yaw), cp * Math.sin(yaw), sp];
  // Same "up" choice as the viewport's camera near the poles.
  const worldUp: V3 = Math.abs(pitch) > 1.55 ? [-Math.cos(yaw), -Math.sin(yaw), 0] : [0, 0, 1];
  let right = cross(worldUp, back);
  const n = Math.hypot(...right);
  right = n > 1e-9 ? [right[0] / n, right[1] / n, right[2] / n] : [1, 0, 0];
  return [right, cross(back, right), back];
}

/** One clickable cell of a cube face: its screen polygon, its direction and its depth. */
export interface Cell {
  poly: [P2, P2, P2, P2];
  dir: Dir;
  /** Larger is nearer the viewer. */
  depth: number;
}

/** Width of the edge and corner bands, as a fraction of the half cube size. */
const BAND = 0.35;

/** Visible cells of the cube, back to front, for a cube of half-size `r` px centred on (cx, cy). */
export function cells(yaw: number, pitch: number, cx: number, cy: number, r: number): Cell[] {
  const [right, up, back] = cameraAxes(yaw, pitch);
  const toScreen = (p: V3): P2 => [cx + dot(p, right) * r, cy - dot(p, up) * r];
  const bands: P2[] = [[-1, -1 + BAND], [-1 + BAND, 1 - BAND], [1 - BAND, 1]];
  const out: Cell[] = [];
  for (let axis = 0; axis < 3; axis++) {
    for (const side of [-1, 1]) {
      const n: V3 = [0, 0, 0];
      n[axis] = side;
      if (dot(n, back) <= 1e-4) continue;
      const ua = (axis + 1) % 3, wa = (axis + 2) % 3;
      bands.forEach(([u0, u1], bi) => {
        bands.forEach(([w0, w1], bj) => {
          const corner = (u: number, w: number): P2 => {
            const p: V3 = [...n];
            p[ua] = u;
            p[wa] = w;
            return toScreen(p);
          };
          const dir: Dir = [0, 0, 0];
          dir[axis] = side;
          dir[ua] = bi - 1;
          dir[wa] = bj - 1;
          const centre: V3 = [...n];
          centre[ua] = (u0 + u1) / 2;
          centre[wa] = (w0 + w1) / 2;
          out.push({ poly: [corner(u0, w0), corner(u1, w0), corner(u1, w1), corner(u0, w1)], dir, depth: dot(centre, back) });
        });
      });
    }
  }
  return out.sort((a, b) => a.depth - b.depth);
}

function inside(poly: [P2, P2, P2, P2], x: number, y: number): boolean {
  // Convex quad, either winding: the point is on the same side of every edge.
  let sign = 0;
  for (let k = 0; k < 4; k++) {
    const a = poly[k]!, b = poly[(k + 1) % 4]!;
    const c = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
    if (Math.abs(c) < 1e-6) continue;
    if (sign === 0) sign = Math.sign(c);
    else if (Math.sign(c) !== sign) return false;
  }
  return true;
}

/** The cube direction under a point, if any (nearest cell wins). */
export function hit(cs: Cell[], x: number, y: number): Dir | null {
  for (let i = cs.length - 1; i >= 0; i--) if (inside(cs[i]!.poly, x, y)) return cs[i]!.dir;
  return null;
}

/** A camera turn in progress (lib.rs `turn_to`, `animate`). */
export interface Turn {
  from: [number, number];
  to: [number, number];
  start: number;
}
/** A turn from (yaw, pitch) to (toYaw, toPitch), the shortest way round. */
export function startTurn(yaw: number, pitch: number, toYaw: number, toPitch: number, now: number): Turn {
  const tau = 2 * Math.PI;
  let dy = (toYaw - yaw) % tau;
  if (dy > Math.PI) dy -= tau;
  else if (dy < -Math.PI) dy += tau;
  return { from: [yaw, pitch], to: [yaw + dy, toPitch], start: now };
}
/** The turn's yaw and pitch at `now`; done once it has arrived. */
export function turnAt(t: Turn, now: number): { yaw: number; pitch: number; done: boolean } {
  const k = Math.min(1, Math.max(0, (now - t.start) / TURN_MS));
  const e = k * k * (3 - 2 * k); // smoothstep
  return { yaw: t.from[0] + (t.to[0] - t.from[0]) * e, pitch: t.from[1] + (t.to[1] - t.from[1]) * e, done: k >= 1 };
}

/** What the cube asks of the viewport's camera. */
export interface CubeHost {
  /** Turn (animated) to look from this yaw and pitch, world radians (north already added). */
  look(yaw: number, pitch: number): void;
  /** Orbit by (dx, dy) radians, as bimble-render Camera::orbit (yaw -= dx, pitch += dy). */
  orbit(dx: number, dy: number): void;
  /** Home: the default view turned by project north, then fit all. */
  home(): void;
  fitAll(): void;
  /** A wheel event over the cube: the viewport zooms as if it were over it. */
  wheel(e: WheelEvent): void;
}

export interface CubeStats {
  /** The cube is shown (a 3D view with a model). */
  shown: boolean;
  /** The target under the pointer. */
  hover: Dir | null;
  /** Clicks on a target, the last one's direction; drags on the cube and on the ring. */
  looks: number;
  lastLook: Dir | null;
  orbits: number;
  turns: number;
}

export interface ViewCube {
  /** The camera now (world yaw, pitch) and whether the cube is shown (3D) or not (a plan, no model). */
  update(yaw: number, pitch: number, show: boolean, flat: boolean): void;
  stats: CubeStats;
  /** Test hooks: where a target's cell (its centroid) or a compass letter is, client px, or null. */
  cellAt(d: Dir): [number, number] | null;
  letterAt(t: "N" | "E" | "S" | "W"): [number, number] | null;
  /** The target under client (x, y), or null. */
  hitAt(x: number, y: number): Dir | null;
}

const SIZE = 120; // CSS px, lib.rs `navigation`
const MARGIN = 8;
const CLICK_PX = 4;
const css = (name: string, fallback: string) => {
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  return v || fallback;
};

/** The cube, its compass and the Home and Fit all buttons, over `canvas` (in its positioned parent). */
export function viewCube(canvas: HTMLCanvasElement, host: CubeHost): ViewCube {
  const stats: CubeStats = { shown: false, hover: null, looks: 0, lastLook: null, orbits: 0, turns: 0 };
  const box = document.createElement("div");
  box.dataset.bb = "view-nav";
  box.style.cssText = `position:absolute;top:${MARGIN}px;right:${MARGIN}px;width:${SIZE}px;display:flex;flex-direction:column;` +
    "align-items:center;gap:4px;z-index:1;pointer-events:none";
  const cv = document.createElement("canvas");
  cv.dataset.bb = "viewcube";
  cv.setAttribute("role", "img");
  cv.setAttribute("aria-label", "View cube");
  cv.style.cssText = `width:${SIZE}px;height:${SIZE}px;display:block;pointer-events:auto;touch-action:none`;
  const button = (key: string, text: string, tip: string) => {
    const b = document.createElement("button");
    b.type = "button";
    b.dataset.bb = key;
    b.textContent = text;
    b.title = tip;
    b.style.cssText = "pointer-events:auto;width:64px;height:22px;padding:0;border:1px solid var(--bb-stone, #5C574D);" +
      "border-radius:var(--bb-radius-sm, 8px);background:var(--bb-sheet, #fff);color:var(--bb-ink, #141414);" +
      "font:13px var(--bb-font-text, sans-serif);cursor:pointer";
    box.append(b);
    return b;
  };
  box.append(cv);
  // Labels as Rust's navigation bar (lib.rs `navigation`, English there too).
  const homeBtn = button("view-home", "Home", "Default 3D view of everything visible");
  const fitBtn = button("view-fit", "Fit all", "Zoom to everything visible");
  homeBtn.addEventListener("click", () => host.home());
  fitBtn.addEventListener("click", () => host.fitAll());
  (canvas.parentElement ?? document.body).append(box);
  const ctx = cv.getContext("2d");
  const accent = css("--bb-ink", "#141414"); // CONTROL_ACCENT: ink

  // The view the cube shows: yaw relative to project north.
  let yaw = HOME_YAW, pitch = HOME_PITCH, shown = false, key = "";
  const geom = () => ({ cx: SIZE / 2, cy: SIZE / 2 - SIZE * 0.08, r: SIZE * 0.19 });
  const cellsNow = () => {
    const g = geom();
    return cells(yaw, pitch, g.cx, g.cy, g.r);
  };
  const RING_R = 1.6, RING_Z = -1.15;
  const LETTERS: ["N" | "E" | "S" | "W", V3][] = [["N", [0, 1, 0]], ["E", [1, 0, 0]], ["S", [0, -1, 0]], ["W", [-1, 0, 0]]];

  const draw = () => {
    if (!ctx) return;
    const dpr = window.devicePixelRatio || 1;
    const w = Math.round(SIZE * dpr);
    if (cv.width !== w) { cv.width = w; cv.height = w; }
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, cv.width, cv.height);
    if (!shown) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const { cx, cy, r } = geom();
    const [right, up, back] = cameraAxes(yaw, pitch);
    const toScreen = (p: V3): P2 => [cx + dot(p, right) * r, cy - dot(p, up) * r];
    const ringPoint = (a: number): V3 => [RING_R * Math.cos(a), RING_R * Math.sin(a), RING_Z];
    // Compass ring under the cube, in the horizontal plane: the far half behind the cube, the near
    // half and its letters in front of it.
    const drawRing = (near: boolean) => {
      ctx.strokeStyle = "rgb(90, 90, 90)";
      ctx.lineWidth = 5;
      ctx.lineCap = "round"; // no gaps between the 48 segments
      ctx.beginPath();
      for (let k = 0; k < 48; k++) {
        const a0 = (k / 48) * 2 * Math.PI, a1 = ((k + 1) / 48) * 2 * Math.PI;
        if ((dot(ringPoint((a0 + a1) / 2), back) >= 0) !== near) continue;
        const p = toScreen(ringPoint(a0)), q = toScreen(ringPoint(a1));
        ctx.moveTo(p[0], p[1]);
        ctx.lineTo(q[0], q[1]);
      }
      ctx.stroke();
      ctx.font = `10px ${css("--bb-font-text", "sans-serif")}`;
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      for (const [t, d] of LETTERS) {
        const wp: V3 = [d[0] * RING_R, d[1] * RING_R, RING_Z];
        if ((dot(wp, back) >= 0) !== near) continue;
        const p = toScreen(wp);
        ctx.fillStyle = "rgb(70, 70, 70)";
        ctx.beginPath();
        ctx.arc(p[0], p[1], 7.5, 0, 2 * Math.PI);
        ctx.fill();
        ctx.fillStyle = t === "N" ? "rgb(255, 90, 90)" : "rgb(255, 255, 255)";
        ctx.fillText(t, p[0], p[1]);
      }
    };
    drawRing(false);
    for (const c of cellsNow()) {
      const face = c.dir.filter((x) => x !== 0).length === 1;
      const hov = stats.hover !== null && stats.hover.join() === c.dir.join();
      ctx.fillStyle = hov ? accent : face ? "rgb(214, 214, 214)" : "rgb(196, 196, 196)";
      ctx.strokeStyle = "rgb(150, 150, 150)";
      ctx.lineWidth = 0.6;
      ctx.beginPath();
      c.poly.forEach((p, i) => (i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1])));
      ctx.closePath();
      ctx.fill();
      ctx.stroke();
    }
    drawRing(true);
    // Face labels, centred on the faces turned to the viewer.
    ctx.font = `9px ${css("--bb-font-text", "sans-serif")}`;
    ctx.fillStyle = "rgb(60, 60, 60)";
    for (let axis = 0; axis < 3; axis++)
      for (const side of [-1, 1]) {
        const d: Dir = [0, 0, 0];
        d[axis] = side;
        const t = label(d);
        if (t && dot(d, back) > 0.25) {
          const p = toScreen(d);
          ctx.fillText(t, p[0], p[1]);
        }
      }
  };

  const local = (e: { clientX: number; clientY: number }): P2 => {
    const r = cv.getBoundingClientRect();
    return [e.clientX - r.left, e.clientY - r.top];
  };
  const hitLocal = (p: P2) => (shown ? hit(cellsNow(), p[0], p[1]) : null);
  const setHover = (d: Dir | null) => {
    if ((d?.join() ?? "") === (stats.hover?.join() ?? "")) return;
    stats.hover = d;
    cv.style.cursor = d ? "pointer" : "";
    draw();
  };

  let drag: { x: number; y: number; x0: number; y0: number; moved: boolean } | null = null;
  cv.addEventListener("pointerdown", (e) => {
    if (!shown || e.button !== 0) return;
    e.preventDefault();
    cv.setPointerCapture(e.pointerId);
    drag = { x: e.clientX, y: e.clientY, x0: e.clientX, y0: e.clientY, moved: false };
  });
  cv.addEventListener("pointermove", (e) => {
    const p = local(e);
    if (!drag) { setHover(hitLocal(p)); return; }
    if (Math.hypot(e.clientX - drag.x0, e.clientY - drag.y0) > CLICK_PX) drag.moved = true;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    if (!drag.moved || (dx === 0 && dy === 0)) return;
    drag.x = e.clientX;
    drag.y = e.clientY;
    // On the ring (beside the cube): turn about the vertical; on the cube: orbit (viewcube.rs `show`:
    // the pointer where it is now decides).
    if (hitLocal(p) === null) { stats.turns++; host.orbit(dx * 0.01, 0); }
    else { stats.orbits++; host.orbit(dx * 0.008, dy * 0.008); }
  });
  const release = (e: PointerEvent) => {
    const d = drag;
    drag = null;
    if (cv.hasPointerCapture(e.pointerId)) cv.releasePointerCapture(e.pointerId);
    if (!d || d.moved || e.type !== "pointerup") return;
    const dir = hitLocal(local(e));
    if (!dir) return;
    stats.looks++;
    stats.lastLook = dir;
    const o = orientation(dir), north = projectNorth();
    host.look(o.yaw + north, o.pitch);
  };
  cv.addEventListener("pointerup", release);
  cv.addEventListener("pointercancel", release);
  cv.addEventListener("pointerleave", () => { if (!drag) setHover(null); });
  cv.addEventListener("contextmenu", (e) => e.preventDefault());
  cv.addEventListener("wheel", (e) => { e.preventDefault(); host.wheel(e); }, { passive: false });

  const toClient = (p: P2): [number, number] => {
    const r = cv.getBoundingClientRect();
    return [r.left + p[0], r.top + p[1]];
  };
  return {
    stats,
    update(worldYaw, worldPitch, show, flat) {
      // Rust hides the navigation when the view is smaller than it (lib.rs `navigation`).
      const fits = canvas.clientWidth >= SIZE + 2 * MARGIN && canvas.clientHeight >= SIZE + 2 * MARGIN;
      const cube = show && !flat && fits;
      const k = `${worldYaw}|${worldPitch}|${cube}|${show && fits}|${window.devicePixelRatio}|${projectNorth()}`;
      if (k === key) return;
      key = k;
      yaw = worldYaw - projectNorth();
      pitch = worldPitch;
      shown = cube;
      stats.shown = cube;
      box.style.display = show && fits ? "flex" : "none";
      cv.style.display = cube ? "block" : "none";
      homeBtn.style.display = cube ? "" : "none";
      if (!cube) { drag = null; stats.hover = null; cv.style.cursor = ""; }
      draw();
    },
    cellAt(d) {
      const c = shown ? cellsNow().find((x) => x.dir.join() === d.join()) : undefined;
      if (!c) return null;
      const x = c.poly.reduce((s, p) => s + p[0], 0) / 4, y = c.poly.reduce((s, p) => s + p[1], 0) / 4;
      return toClient([x, y]);
    },
    letterAt(t) {
      if (!shown) return null;
      const { cx, cy, r } = geom();
      const [right, up] = cameraAxes(yaw, pitch);
      const d = LETTERS.find((l) => l[0] === t)![1];
      const wp: V3 = [d[0] * RING_R, d[1] * RING_R, RING_Z];
      return toClient([cx + dot(wp, right) * r, cy - dot(wp, up) * r]);
    },
    hitAt(x, y) {
      const r = cv.getBoundingClientRect();
      return hitLocal([x - r.left, y - r.top]);
    },
  };
}
