// SIZE3 (B, ADR 0078 amendment 5): BMSH version 4 read back in a Web Worker (gl/bmsh4.ts, the same code):
// the page's own thread only receives the version 3 bytes (transferred, not copied), so a link's decode
// never runs in its frames. One message in: {id, ab, max}; one out: {id, out} or {id, error}.
import { bmsh4Rows, bmsh4ToBmsh3 } from "./bmsh4";

const ctx = self as unknown as { onmessage: ((e: MessageEvent) => void) | null; postMessage: (m: unknown, t?: Transferable[]) => void };
ctx.onmessage = (e: MessageEvent) => {
  const { id, ab, max, rows } = e.data as { id: number; ab: ArrayBuffer; max: number; rows?: [number, number] };
  if (rows) { // SIZE3: rows and boxes only (a link dropped at open)
    try {
      const r = bmsh4Rows(ab, rows[0], rows[1]);
      const t = [r.cats, r.v0, r.nv, r.i0, r.ni, r.s0, r.ns, r.shape, r.place, r.boxes].map((a) => a.buffer as ArrayBuffer);
      ctx.postMessage({ id, rows: r }, t);
    } catch (err) {
      ctx.postMessage({ id, error: err instanceof Error ? err.message : String(err) });
    }
    return;
  }
  bmsh4ToBmsh3(ab, max).then(
    (out) => ctx.postMessage({ id, out }, [out]),
    (err: unknown) => ctx.postMessage({ id, error: err instanceof Error ? err.message : String(err) }),
  );
};
