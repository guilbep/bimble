# ADR 0006 — Rendering: drawing style, offscreen target, redraw on change

- Status: Accepted (5 Nov 2026)
- Decided by: Pierre (speed on old machines is the main pain point; "black and white with maybe 12 colours,
  no textures, patterns are enough to distinguish materials"; maybe a custom engine / ray tracing long term).
  Implementation choices below by Claude.
- Reversal cost: low (one crate, `bimble-render`)

## Decision
- **Own renderer on wgpu 30**, no game engine. Backends: Vulkan / Metal / DX12, OpenGL fallback.
- **Look:** flat shading (two-sided Lambert, high ambient), black crease edges, one muted colour per material
  family, **screen-space hatch pattern per family** in the fragment shader, no textures. Selection in orange.
- **Edges:** crease edges (boundary, or faces meeting at > 20°), so arc segments on curved surfaces are not
  outlined. Faces are depth-biased back so 1-pixel lines win the depth test. MSAA ×4.
- **Offscreen first:** the renderer draws into its own target. The viewer shows it as an egui texture;
  `bimble render` reads it back to PNG. One code path, testable headless (Mesa lavapipe in CI).
- **Redraw only on change** (camera, visibility, selection, size). Idle costs nothing: quiet old laptops.
- **Colour:** target is plain `Rgba8Unorm` holding gamma-encoded values; the shader shades in linear space and
  encodes. egui samples textures as gamma, PNG stores gamma, and no sRGB view formats are needed (some old
  OpenGL drivers lack them). An sRGB target was tried first and was displayed twice-darkened in the viewer.
- **Clip planes** come from the scene's bounding sphere measured along the view axis (`d ± r`), not from the
  orbit target; otherwise pan + orbit clipped the model (review finding, regression-tested).
- **Picking** on the CPU (ray vs triangles). Fine up to ~1M triangles; a BVH comes when needed.

## Known limits / next
- Screen-space hatching "swims" when orbiting. Object-space or triplanar hatching is the alternative.
- No instancing yet (mapped items are flattened), no culling, no LOD.
- Status bar time is CPU submit time; GPU timing needs timestamp queries.
- Ray tracing: for still images only, later.
- **Not yet measured on an old laptop.** The 20k-elements-at-60-fps target is a proposal.
