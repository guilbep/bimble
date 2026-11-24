# Viewport spike T4: is the streamed 3D view good enough to work in?

**Method.** `node web-ui/e2e/native-viewport.mjs` starts the headless twin (dev build), Vite and headless
Chromium (1920×1080, DPR 1), opens the lab with `?native=1`, and drives the canvas over CDP. It does 50 short
orbit drags and reads `window.__bbViewport` latency (pointer event → after `putImageData`), then one long
drag (240 moves, 8 ms apart) for frames per second. Models: `bimble demo` (72 elements) and a 30×30×7 demo
(20,715 elements). All numbers: `results.txt`.

**Verdicts** (thresholds fixed before measuring: WORKABLE p95 ≤ 50 ms and ≥ 20 fps; KEEP-WASM p95 > 100 ms):
- small: p95 57 ms, 26 fps → **MARGINAL**;
- large: p95 86 ms, 19 fps → **MARGINAL**.

Most of the time is the transfer of 5.4 MB raw RGBA per frame (fetch 29–48 ms), not the render (3–18 ms).

**Limits.** Headless Chromium with SwiftShader: a real GPU browser may differ. One machine (Apple M1 Pro on
Asahi Linux). The clock stops after `putImageData`: no compositor present, no vsync wait.

## T5: half resolution while dragging

While a pointer is down, or a wheel event came in the last 150 ms, the lab requests the frame at half width and
half height (812×414: 1.34 MB instead of 5.37 MB) and scales it up in the canvas. When the drag ends, the next
frame is full resolution. The measurement script is unchanged.

| model | T4 p50 / p95 / max (ms) | T4 fps | T4 verdict | T5 p50 / p95 / max (ms) | T5 fps | T5 verdict |
|---|---|---|---|---|---|---|
| small (72) | 45.6 / 57.3 / 62.8 | 25.8 | MARGINAL | 27.0 / 41.4 / 43.4 | 43.2 | **WORKABLE** |
| large (20,715) | 64.1 / 86.0 / 91.7 | 19.0 | MARGINAL | 39.3 / 56.2 / 58.4 | 30.2 | **MARGINAL** |

Mean fetch fell from 29 to 13.6 ms (small) and from 48 to 25.7 ms (large). The large model now fails only on
p95 (56 ms against 50 ms); its render (13.5 ms) is about half of the fetch.

**What a half-resolution frame looks like.** The screenshots (`/tmp/viewport-lab-*.png`) are taken after the
drags, so they show the full-resolution frame with crisp edges. No half-resolution frame was captured. It is
the same image scaled up 2× with smoothing: edges are soft and about 2 px wide while the user drags.
