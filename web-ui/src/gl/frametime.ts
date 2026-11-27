// Where a frame's time goes (ticket VIS1, ADR 0078 amendment 5): a log of the frames the WebGL2 viewport drew,
// for ccore/tools/drive.mjs and the lab tests. Per frame: its start, kind (m moving, s a slice of the fill, w a
// whole picture, i the ground-truth pass), the script's ms (draw start to its last call), draw calls, triangles
// submitted, batches culled, proxies and outlines drawn, and the GPU's ms: EXT_disjoint_timer_query_webgl2 when
// the context exposes it (one query at a time: a frame drawn while one is pending has none), and the fence's
// (draw start to the GPU's end, polled each loop: to a loop's resolution, the script's part included).
// Nothing here waits for the GPU.

export interface FrameRec { at: number; kind: string; js: number; draws: number; tris: number; culled: number; batches: number;
  proxied: number; outlines: number; linkPrxTris: number; budget: number; gpu: number | null; fence: number | null }

const RING = 2000;
export function frameTimer(gl: WebGL2RenderingContext) {
  const ext = gl.getExtension("EXT_disjoint_timer_query_webgl2") as { TIME_ELAPSED_EXT: number; GPU_DISJOINT_EXT: number } | null;
  const log: FrameRec[] = [];
  let q: { q: WebGLQuery; rec: FrameRec } | null = null, open: { q: WebGLQuery; rec: FrameRec } | null = null;
  return {
    timer: ext !== null,
    log,
    /** At a frame's start: a timer query begun when none is pending. */
    begin(): void {
      if (!ext || q || open) return;
      const qq = gl.createQuery();
      if (!qq) return;
      gl.beginQuery(ext.TIME_ELAPSED_EXT, qq);
      open = { q: qq, rec: null as unknown as FrameRec };
    },
    /** At its end: its record (kept in the ring), the query ended. */
    end(rec: FrameRec): void {
      if (open && ext) { gl.endQuery(ext.TIME_ELAPSED_EXT); open.rec = rec; q = open; open = null; }
      log.push(rec);
      if (log.length > RING) log.splice(0, log.length - RING);
    },
    /** Each loop: a pending query's answer, when there is one. */
    poll(): void {
      if (!q || !ext) return;
      if (!gl.getQueryParameter(q.q, gl.QUERY_RESULT_AVAILABLE)) return;
      const disjoint = gl.getParameter(ext.GPU_DISJOINT_EXT) as boolean;
      if (!disjoint) q.rec.gpu = (gl.getQueryParameter(q.q, gl.QUERY_RESULT) as number) / 1e6;
      gl.deleteQuery(q.q);
      q = null;
    },
  };
}
