# ADR 0035 — While the view moves, leave out what is smallest; a frame-rate target decides how much

- Status: Accepted (9 Nov 2026)
- Decided by: Pierre ("simplify display during view navigation… like video games"; "guarantee
  frame rate could be an option/setting"); implementation by Claude
- Reversal cost: low (one draw option and one setting)

## Context / Problem

After ADR 0034 the largest models fit and draw whole: the 415 MB model's 35.8 M triangles at
30 fps, the 758 MB one's 21.0 M at 56 fps (1080p, MSAA 4, M4 Pro). Old machines and browsers are
several times slower; orbiting must stay smooth there, and stills must stay complete. Revit's
"simplify display during view navigation" and Navisworks' "guarantee frame rate" drop detail
while moving and draw everything when the view stops.

## Decision

1. **Size order.** The upload puts merged geometry largest element first (bounding radius) and
   keeps the breakpoints; a shared shape's draw (ADR 0034) knows its largest copy. "Everything at
   least r big" is then a prefix of each buffer and a filter on shape draws — no extra draw calls
   and no per-frame work over elements. `DrawOptions::min_radius`.
2. **Screen size.** The threshold is in pixels at the orbit target (`Camera::mm_per_pixel`).
3. **A frame-rate target, not a fixed threshold.** Settings → "While the view moves": draw
   everything / 15 / 30 (default) / 60 fps. The threshold starts at 0 and rises by half while
   moving frames are 15 % slower than the target, falls by a third while 30 % faster, back to 0
   (`next_moving_px`). A fast machine never leaves anything out; a gap over 0.25 s is a pause,
   not a slow frame.
4. **Stills are whole.** 150 ms after the camera stops, the view is drawn again with everything.
   Animated turns (view cube) count as moving.

## Alternatives considered

- **A fixed threshold** (always 3 px): simple, but fast machines lose detail for nothing.
- **Edges off while moving:** an obvious saving, but the picture changes character at every
  drag; the size filter already drops the dense edges of small things.
- **Lower resolution while moving:** needs a second target and an upscale; kept for later if the
  size filter is not enough on fill-bound GPUs.
- **Occlusion culling / LOD clusters (Nanite-like):** far larger; the box tree (ADR 0034) and
  this ordering are steps towards per-cluster culling if needed.

## Evidence

`bimble bench` (1080p, MSAA 4, 60 frames orbit, M4 Pro), full frame → frame drawn as while
moving with 3 px:

| model | triangles drawn | full | moving |
|---|---|---|---|
| 200 MB electrical | 8.8 M | 7.73 ms | 0.62 ms |
| 872 MB plumbing | 9.1 M | 7.48 ms | 1.56 ms |
| 758 MB sanitary | 21.0 M | 17.84 ms | 1.60 ms |
| 415 MB sprinklers | 35.8 M | 33.86 ms | 3.17 ms |

Tests: `small_elements_are_a_suffix_of_the_merged_buffers`, `the_moving_threshold_follows_the_frame_rate`.
No measurement on a weak GPU yet (`bench` on a software adapter in CI is the plan).

## Revisit when

- Measured on an old integrated GPU or WebGL2: whether the size filter alone holds the target.
- Per-cluster culling (frustum, occlusion) through the box tree.
