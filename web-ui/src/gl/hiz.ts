// A depth pyramid of the last picture (ticket VIS1, ADR 0078 amendment 7): "what is behind something else is
// skipped". After a frame is presented, its depth (gl/target.ts: the faces drawn, pushed back by their polygon
// offset) is reduced on the GPU to the farthest depth of each BLOCK x BLOCK pixels, where a pixel of a proxy (the
// element attachment's 0xffffff: a proxy is not a safe occluder) or of the background counts as the farthest; that
// small level is read back without waiting (a pixel-pack buffer and a fence, polled each loop) and the coarser
// levels are made on the CPU. A box whose nearest depth is farther than the farthest depth over the blocks its
// projection covers is hidden behind what that picture drew: as long as the camera, what is shown, the section
// box and the links' placements are those of that picture (its key), and every element that drew it is drawn
// again (an element that drew a pixel passes the test: its box's nearest depth is at most its own), leaving the
// hidden ones out changes no pixel. Nothing here waits for the GPU.

export const BLOCK = 4;

const VS = `#version 300 es
void main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }`;
const FS = `#version 300 es
precision highp float;
uniform highp sampler2D u_dep;
uniform highp sampler2D u_id;
uniform int u_block;
out vec4 o;
void main() {
  ivec2 base = ivec2(gl_FragCoord.xy) * u_block, hi = textureSize(u_dep, 0) - 1;
  float m = 0.0;
  for (int y = 0; y < 16; y++) {
    if (y >= u_block) break;
    for (int x = 0; x < 16; x++) {
      if (x >= u_block) break;
      ivec2 q = min(base + ivec2(x, y), hi);
      vec4 id = texelFetch(u_id, q, 0);
      float d = texelFetch(u_dep, q, 0).r;
      if (id.a == 0.0 || all(greaterThan(id.rgb, vec3(0.999)))) d = 1.0; // the background, a proxy: no occluder
      m = max(m, d);
    }
  }
  // rounded up (a pyramid never nearer than the picture), at most 2^24 - 1: 16777215.5 is 2^24 in a float, whose low 24
  // bits are 0 (the first version read the background as the nearest depth)
  uint k = min(uint(ceil(clamp(m, 0.0, 1.0) * 16777215.0)), 16777215u);
  o = vec4(float(k & 255u), float((k >> 8u) & 255u), float((k >> 16u) & 255u), 255.0) / 255.0;
}`;

export interface Pyramid { key: string; serial: number; complete: boolean; w: number; h: number; cw: number; ch: number; mvp: Float64Array; levels: Float32Array[]; lw: number[]; lh: number[] }

export function hizBuilder(gl: WebGL2RenderingContext) {
  const sh = (t: number, src: string) => { const s = gl.createShader(t)!; gl.shaderSource(s, src); gl.compileShader(s); if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s) ?? "shader"); return s; };
  const prog = gl.createProgram()!;
  gl.attachShader(prog, sh(gl.VERTEX_SHADER, VS)), gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, FS));
  gl.linkProgram(prog);
  if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog) ?? "link");
  const uDep = gl.getUniformLocation(prog, "u_dep"), uId = gl.getUniformLocation(prog, "u_id"), uBlock = gl.getUniformLocation(prog, "u_block");
  const vao = gl.createVertexArray();
  let fb: WebGLFramebuffer | null = null, tex: WebGLTexture | null = null, pbo: WebGLBuffer | null = null, w = 0, h = 0;
  let pending: { s: WebGLSync; key: string; w: number; h: number; cw: number; ch: number; mvp: Float64Array; at: number; complete: boolean } | null = null;
  let ready: Pyramid | null = null, serial = 0;
  const UNIT = 12;
  return {
    get ready() { return ready; },
    get pending() { return pending !== null; },
    /** Asks the pyramid of the picture just drawn into the target (its depth and element textures), for `key`. */
    request(dep: WebGLTexture, ids: WebGLTexture, cw: number, ch: number, key: string, mvp: ArrayLike<number>, complete = true): void {
      if (pending) return; // one at a time: the next picture asks again
      const nw = Math.ceil(cw / BLOCK), nh = Math.ceil(ch / BLOCK);
      if (nw !== w || nh !== h || !fb) {
        w = nw, h = nh;
        if (tex) gl.deleteTexture(tex);
        if (fb) gl.deleteFramebuffer(fb);
        if (pbo) gl.deleteBuffer(pbo);
        tex = gl.createTexture(), fb = gl.createFramebuffer(), pbo = gl.createBuffer();
        gl.activeTexture(gl.TEXTURE0 + UNIT);
        gl.bindTexture(gl.TEXTURE_2D, tex);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST), gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
        gl.bindTexture(gl.TEXTURE_2D, null);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
        gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
      }
      gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
      gl.viewport(0, 0, w, h);
      gl.useProgram(prog);
      gl.activeTexture(gl.TEXTURE0 + UNIT), gl.bindTexture(gl.TEXTURE_2D, dep), gl.uniform1i(uDep, UNIT);
      gl.activeTexture(gl.TEXTURE0 + UNIT + 1), gl.bindTexture(gl.TEXTURE_2D, ids), gl.uniform1i(uId, UNIT + 1);
      gl.uniform1i(uBlock, BLOCK);
      gl.disable(gl.DEPTH_TEST), gl.depthMask(false), gl.disable(gl.BLEND);
      gl.bindVertexArray(vao);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
      gl.bindVertexArray(null);
      gl.bindBuffer(gl.PIXEL_PACK_BUFFER, pbo);
      // (its storage given here, with no fence between it and the read: Chromium warns when a read-back buffer is
      // written, a fence made (the viewport's own each frame), then written again before it is read)
      gl.bufferData(gl.PIXEL_PACK_BUFFER, 4 * w * h, gl.STREAM_READ);
      gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, 0);
      gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      for (const k of [0, 1]) { gl.activeTexture(gl.TEXTURE0 + UNIT + k); gl.bindTexture(gl.TEXTURE_2D, null); }
      gl.activeTexture(gl.TEXTURE0);
      gl.enable(gl.DEPTH_TEST), gl.depthMask(true);
      pending = { s: gl.fenceSync(gl.SYNC_GPU_COMMANDS_COMPLETE, 0)!, key, w, h, cw, ch, mvp: Float64Array.from(mvp), at: performance.now(), complete };
    },
    /** Each loop: the read-back level when the GPU is done; true when a new pyramid is ready. */
    poll(): boolean {
      const p = pending;
      if (!p || gl.getSyncParameter(p.s, gl.SYNC_STATUS) !== gl.SIGNALED) return false;
      gl.deleteSync(p.s);
      pending = null;
      const raw = new Uint8Array(4 * p.w * p.h);
      gl.bindBuffer(gl.PIXEL_PACK_BUFFER, pbo);
      gl.getBufferSubData(gl.PIXEL_PACK_BUFFER, 0, raw);
      gl.bindBuffer(gl.PIXEL_PACK_BUFFER, null);
      const l0 = new Float32Array(p.w * p.h);
      for (let i = 0; i < l0.length; i++) l0[i] = (raw[4 * i]! | (raw[4 * i + 1]! << 8) | (raw[4 * i + 2]! << 16)) / 16777215;
      const levels = [l0], lw = [p.w], lh = [p.h];
      while (lw[lw.length - 1]! > 1 || lh[lh.length - 1]! > 1) {
        const pw = lw[lw.length - 1]!, ph = lh[lh.length - 1]!, src = levels[levels.length - 1]!;
        const nw = Math.ceil(pw / 2), nh = Math.ceil(ph / 2), dst = new Float32Array(nw * nh);
        for (let y = 0; y < nh; y++) for (let x = 0; x < nw; x++) {
          let m = 0;
          for (let dy = 0; dy < 2; dy++) for (let dx = 0; dx < 2; dx++) { const sx = Math.min(pw - 1, 2 * x + dx), sy = Math.min(ph - 1, 2 * y + dy); m = Math.max(m, src[sy * pw + sx]!); }
          dst[y * nw + x] = m;
        }
        levels.push(dst), lw.push(nw), lh.push(nh);
      }
      ready = { key: p.key, serial: ++serial, complete: p.complete, w: p.w, h: p.h, cw: p.cw, ch: p.ch, mvp: p.mvp, levels, lw, lh };
      return true;
    },
    /** Forgets the pyramid (a mesh, a resize). */
    reset(): void { ready = null; },
  };
}

/** Whether the box (min xyz, max xyz at b[o..o+6], scene frame) is hidden behind the pyramid's picture: its
 *  nearest depth farther than the farthest depth over the blocks its projection covers, or wholly outside the
 *  picture (an element of a batch in view may lie out of it: behind the eye, beside, past the far plane). A box
 *  across the plane of the eye is not hidden. */
export function boxHidden(p: Pyramid, b: ArrayLike<number>, o: number, beyond = false): boolean {
  const m = p.mvp;
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity, z0 = Infinity, z1 = -Infinity, behind = 0;
  for (let k = 0; k < 8; k++) {
    const x = k & 1 ? b[o + 3]! : b[o]!, y = k & 2 ? b[o + 4]! : b[o + 1]!, z = k & 4 ? b[o + 5]! : b[o + 2]!;
    const cw = m[3]! * x + m[7]! * y + m[11]! * z + m[15]!;
    if (cw <= 1e-6) { behind++; continue; } // behind the eye
    const cx = (m[0]! * x + m[4]! * y + m[8]! * z + m[12]!) / cw, cy = (m[1]! * x + m[5]! * y + m[9]! * z + m[13]!) / cw, cz = (m[2]! * x + m[6]! * y + m[10]! * z + m[14]!) / cw;
    if (cx < x0) x0 = cx;
    if (cx > x1) x1 = cx;
    if (cy < y0) y0 = cy;
    if (cy > y1) y1 = cy;
    if (cz < z0) z0 = cz;
    if (cz > z1) z1 = cz;
  }
  if (behind === 8) return true; // wholly behind the eye
  if (behind > 0 || z0 < -1) return false; // across the eye's plane or the near plane: not decided here
  if (z0 > 1) return true; // past the far plane
  // VIS2 (`beyond`: the pyramid of a picture of the same camera at another canvas size): what lies beyond that picture may be
  // in this one: never hidden; what lies within it is tested against it (the same eye and direction: the same rays)
  if (beyond && (x0 < -1 || x1 > 1 || y0 < -1 || y1 > 1)) return false;
  if (x1 < -1 || x0 > 1 || y1 < -1 || y0 > 1) return true; // wholly beside the picture
  const zn = (z0 + 1) / 2; // the window depth of the nearest corner (the box's nearest depth is at least this)
  // pixels covered (clamped to the picture), then blocks of the finest level, then the level where they are few
  const px0 = Math.max(0, Math.floor(((x0 + 1) / 2) * p.cw)), px1 = Math.min(p.cw - 1, Math.floor(((x1 + 1) / 2) * p.cw));
  const py0 = Math.max(0, Math.floor(((y0 + 1) / 2) * p.ch)), py1 = Math.min(p.ch - 1, Math.floor(((y1 + 1) / 2) * p.ch));
  let bx0 = Math.floor(px0 / BLOCK), bx1 = Math.floor(px1 / BLOCK), by0 = Math.floor(py0 / BLOCK), by1 = Math.floor(py1 / BLOCK), l = 0;
  while ((bx1 - bx0 + 1) * (by1 - by0 + 1) > 16 && l < p.levels.length - 1) bx0 >>= 1, bx1 >>= 1, by0 >>= 1, by1 >>= 1, l++;
  const lv = p.levels[l]!, lw = p.lw[l]!;
  for (let y = by0; y <= by1; y++) for (let x = bx0; x <= bx1; x++) if (lv[y * lw + x]! + 2e-6 >= zn) return false; // (a margin of about 30 depth steps of 24 bits: a CPU rasteriser's depth and a float32 transform differ by a few)
  return true;
}

