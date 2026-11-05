# ADR 0013 — Graphic style: every drawing parameter is data

- Status: Accepted (5 Nov 2026)
- Decided by: Pierre ("Il faut que tous les paramètres graphiques soient paramétriques. Si demain je veux
  des lignes rouges et pas des noirs je veux changer."); design by Claude
- Reversal cost: low (one module; the default style reproduces the previous palette)

## Problem
Colours were Rust constants and the six hatch patterns were hard-coded cases in the shader. Changing
anything (red lines, the office's hatch conventions, a print style) meant changing code.

## Decision
- **`GraphicStyle`** (`bimble-render::style`) holds everything: background, edges, selection, hatch
  strength and darkening, ambient light, and per material family a `Fill` (colour, ink, up to two
  line families with angle/spacing/width/dashes, stipple).
- **JSON file with only the differences.** It is deep-merged onto the default, then strictly parsed
  (unknown fields refused) and range-checked; size-capped (untrusted input). `bimble style` prints the
  complete default; the viewer's "Save…" writes only the differences.
- **One generic shader.** The six hard-coded patterns are now data in a uniform table (16 slots ×
  96 bytes: 8 families, selection, spares). Vertices carry only the slot index, so restyling never
  re-uploads the model: the viewer edits live.
- **`--style`** on `view` and `render`; a "Style…" window in the viewer (colour pickers, sliders,
  Load/Save/Reset).

## Alternatives considered
- **TOML or a CSS-like syntax:** JSON is already the `.bimble` format and needs no new parser.
- **Bake colours into vertices (as before):** every change would re-upload the model.
- **Per-element colour overrides now:** not asked yet; the table has spare slots and the format can
  grow (`overrides` keyed by material or IFC class).

## Evidence
- The default style reproduces the old drawing: demo frame pixel-identical; three real files differ
  on 0.10–0.34 % of pixels, and 2 365 of those 2 367 pixels take a colour present within 2 px in the
  old image (stripe edges moved by one pixel: `(x+y)/9` is now `x·a + y·b`).
- `graphic_style_drives_every_colour` renders one box per family with distinct fills, red edges and
  a coloured background, and checks each colour on the GPU output; it fails when the shader's edge
  colour is hard-coded again (checked).
- Style parsing tests: round trip, partial override, typos, ranges, size cap, colour syntax.

### Cost
Frame time on the software GPU (llvmpipe, 1280×720, `bimble bench`, 3 alternating runs):
+5–7 % against the hard-coded shader; no difference at 160×90, so it is per-pixel work. Already
done: colours converted to linear on the CPU, constant per-triangle values passed `flat`, pattern
code skipped for families without hatch and only the patterns a family has evaluated (first version
was +10 %). Passing all pattern data as flat varyings instead of indexing the table gave nothing.
Not measured on a hardware GPU yet.

## Revisit when
- Office drawing conventions arrive (I4): they become a style file, and may need per-material
  overrides and line weights (edges are 1 px; wide lines need a quad-based line renderer).
- A title block / sheet style is designed: it should live next to this format, not inside it.
- The old-laptop benchmark shows the 5–7 % matters: specialise the pipeline per style (override
  constants or generated WGSL, rebuilt when the style changes).
