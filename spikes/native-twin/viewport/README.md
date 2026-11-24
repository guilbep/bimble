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
