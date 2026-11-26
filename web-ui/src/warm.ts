// FIRST1 (ADR 0074): the GPU's first use of each kind of 2D drawing, paid at start instead of at
// the user's first plan.
//
// The viewport's overlay (glviewport.ts: grids, annotations, setting out, measure) is a 2D canvas
// the browser draws on the GPU. The first time a session draws a kind of stroke, dash, arc, fill or
// text there, the GPU process compiles its programs: under SwiftShader (headless Chromium, a machine
// without a usable GPU) 400 to 550 ms in which no frame of the page is shown, so the first plan
// shown answered in 470 to 550 ms (FIRST1, VIEWSHEET: "the first plan shown costs about 500 ms
// more, once"); with the 2D canvas on the CPU (--disable-accelerated-2d-canvas) 63 to 84 ms.
// `warm2d` draws each of those kinds once, at start, on a canvas of its own that is never shown:
// the programs are compiled before data-bb-ready (deeplink.ts waits for the frames to come at the
// display's pace again), and the overlay's first plan finds them.
// The list follows the overlay's calls (glviewport.ts drawOverlay, gl/*.ts): line widths 1 to 2.5,
// solid and dashed, round and miter joins, arcs stroked and filled, rectangles, round rectangles,
// polygons, translucent fills, and text in the overlay's fonts, straight and turned.

/** Draws the overlay's kinds of 2D drawing once on a canvas never shown, and hands the picture to
 *  the GPU (createImageBitmap). Resolves when the browser took it; never rejects. */
export async function warm2d(): Promise<void> {
  try {
    const cv = document.createElement("canvas");
    cv.width = 512; // large enough for the browser to draw it on the GPU, as the overlay is
    cv.height = 512;
    const ctx = cv.getContext("2d");
    if (!ctx) return;
    const widths = [1, 1.25, 1.5, 2, 2.5];
    const dashes = [[], [8, 4], [4, 3], [10, 4, 2, 4], [12, 4, 3, 4]];
    let y = 8;
    for (const join of ["miter", "round"] as const) {
      for (const w of widths) {
        for (const d of dashes) {
          ctx.lineJoin = join;
          ctx.lineWidth = w;
          ctx.setLineDash(d);
          ctx.strokeStyle = "#141414";
          ctx.beginPath();
          ctx.moveTo(4, y); ctx.lineTo(120, y); ctx.lineTo(160, y + 6); ctx.lineTo(160, y + 2);
          ctx.moveTo(200, y); ctx.lineTo(260, y + 3);
          ctx.stroke();
          y += 5;
        }
      }
    }
    ctx.setLineDash([]);
    for (const r of [3, 4, 5, 9, 14]) { // grid bubbles, markers, the pivot
      ctx.beginPath(); ctx.arc(300 + 4 * r, 40, r, 0, 2 * Math.PI); ctx.fillStyle = "rgba(255,255,255,0.9)"; ctx.fill();
      ctx.lineWidth = 1.5; ctx.strokeStyle = "#c33"; ctx.stroke();
      ctx.beginPath(); ctx.moveTo(300, 80 + r); ctx.arc(300, 80, r, 0, 2 * Math.PI); ctx.stroke();
    }
    ctx.fillStyle = "rgba(20,20,20,0.15)"; ctx.fillRect(300, 120, 80, 30);
    ctx.strokeRect(300, 160, 80, 30);
    ctx.beginPath(); ctx.rect(300, 200, 40, 20); ctx.stroke();
    if (typeof ctx.roundRect === "function") { ctx.beginPath(); ctx.roundRect(300, 230, 80, 24, 6); ctx.fill(); ctx.stroke(); }
    ctx.beginPath(); ctx.moveTo(400, 300); ctx.lineTo(440, 320); ctx.lineTo(410, 350); ctx.closePath(); ctx.fillStyle = "#2a6"; ctx.fill(); ctx.stroke();
    const fonts = ["600 11px 'DM Mono', monospace", "11px 'DM Mono', monospace", "12px 'Source Sans 3', sans-serif", "600 12px 'Source Sans 3', sans-serif", "14px 'Source Sans 3', sans-serif", "10px sans-serif"];
    ctx.fillStyle = "#141414";
    fonts.forEach((f, i) => {
      ctx.font = f;
      for (const [align, base] of [["left", "alphabetic"], ["center", "middle"], ["left", "bottom"]] as const) {
        ctx.textAlign = align; ctx.textBaseline = base;
        ctx.fillText("A1 2 780 mm 0123456789 Niveau +3.000", 20, 300 + 14 * i);
      }
      ctx.save(); ctx.translate(200, 420); ctx.rotate(0.7 + i / 10); ctx.fillText("12 345", 0, 0); ctx.restore();
    });
    ctx.clearRect(0, 0, 16, 16);
    const bitmap = await createImageBitmap(cv); // the drawing goes to the GPU now
    bitmap.close();
  } catch (e) {
    console.warn("warm2d:", e);
  }
}
