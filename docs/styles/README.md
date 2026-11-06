# Graphic styles

Every colour, hatch and light value of a Bimble drawing is set by a **style file** (JSON). A style
file lists only what differs from the default:

```json
{ "edges": "#d01010" }
```

```sh
bimble style                      # prints the complete default style: the reference for every field
bimble style my-style.json        # checks a file and prints it in full
bimble view model.ifc --style my-style.json
bimble render model.ifc out.png --style my-style.json
```

In the viewer, **Manage → Graphic standards** opens the same settings in the Standards panel
(ADR 0027). Changes show at once. **Save…** writes only what differs from the default.

## Fields

| Field | Meaning |
|---|---|
| `background`, `edges`, `selection`, `hover`, `dimensions` | Colours, `"#rrggbb"` (sRGB); `hover` colours the edges of the element under the cursor, `dimensions` measurements; `reference`, `reference_edges`, `only_here`, `only_in_reference` are for linked models |
| `cut_shade` | Brightness of surfaces cut by the section box, relative to their fill, 0..1 |
| `hatch_strength` | Overall hatch intensity, 0..1 (the H key turns hatching off and on) |
| `hatch_darken` | For families without `ink`: how much darker hatch lines are than the fill, 0..1 |
| `ambient` | Light that ignores face orientation, 0..1. `1` gives flat colours, as on a printed plan |
| `families.<family>` | One entry per material family: `concrete`, `steel`, `timber`, `masonry`, `insulation`, `earth`, `glass`, `other` |

`paper` sets drawings on paper (plans), in mm on the printed sheet:

| Field | Meaning |
|---|---|
| `cut_pen_mm` | Outline of what the cut plane cuts (default 0.5) |
| `projection_pen_mm` | Visible edges below the cut plane (0.18) |
| `above_pen_mm`, `above_dash_mm` | Edges above the cut plane, dashed: pen, then [dash, gap] (0.13, [2, 1]) |
| `hatch_pen_mm`, `hatch_mm_per_px` | Hatch lines: pen, and paper spacing per screen pixel of a family's `spacing_px` (0.13, 0.35) |
| `frame_pen_mm`, `text_mm` | Sheet frame and title block: pen and text height (0.5, 2.5) |
| `annotation_pen_mm` | Dimension lines (0.13) |
| `axis_pen_mm`, `axis_dash_mm` | Grid axes, dash-dot: pen, then [dash, gap, dot, gap] (0.13, [12, 1.5, 0.5, 1.5]) |
| `seen_lighten` | What is seen below the cut is filled with the family colour mixed with this much white, 0..1 (0.6) |
| `pens` | Line weights (ADR 0026): `scales` (e.g. `[5, 10, 20, 50, 100, 200, 500]`), `model` (16 rows, one width per scale), `perspective` and `annotation` (16 widths). A scale between two columns takes the nearest |
| `line_types` | `[{name, color, dash_mm, pen}]`: dash, gap… in mm (empty: continuous), pen 1–16 |
| `lines` | The line type of each kind of line: `cut`, `seen`, `above`, `hatch`, `axis`, `dimension`, `frame`. They replace the `*_pen_mm` and dash fields above |
| `text_types` | `[{name, font, size_mm, color}]` |
| `dimension_types` | `[{name, unit ("m", "cm", "mm"), decimals, show_unit, color, text_mm, line_type}]` |
| `hatch_types` | `[{name, fill?, lines: [{angle_deg, spacing_mm, offset_mm?, dash_mm?}], color, pen}]`: spacing in mm on paper, the same at every scale |
| `material_hatches` | `[{category, cut?, seen?}]`: the hatch type of a material family where cut and where seen; a family not listed keeps its own pattern |

Each family:

| Field | Meaning |
|---|---|
| `color` | Fill colour |
| `ink` | Hatch colour. Absent: the fill, darkened by `hatch_darken` |
| `lines` | 0, 1 or 2 families of parallel lines: `angle_deg` (counter-clockwise from horizontal), `spacing_px` (between lines, across them), `width` (fraction of the spacing), optional `dash_px` (dash period) and `dash_on` (drawn fraction) |
| `stipple` | Random dots: `cell_px` (grid), `density` (fraction of cells), `strength` |

Hatches are sized in screen pixels, so they read the same at every zoom, like a drawing.

Unknown fields and out-of-range values are errors: a typo is reported instead of being ignored.

## Examples

- [`red-edges.json`](red-edges.json): red edges, everything else the default.
- [`black-and-white.json`](black-and-white.json): white fills, black hatches, flat light (print-style).

## Not yet

- Overrides per material or per IFC class (only per material family today).
- Line weights on screen: edges are 1 pixel (on paper they follow `pens`). Thicker lines need a
  different line renderer.
- Hatch types on screen: the screen uses each family's `lines` and `stipple`; `hatch_types` are for
  paper.
- Title block, legend and the words on a plan are not in the style: they are in the plan template
  (`bimble plan-template`, ADR 0018).
