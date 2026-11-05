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

In the viewer, **Style…** opens the same settings with colour pickers and sliders. Changes show at
once. **Save…** writes only what differs from the default.

## Fields

| Field | Meaning |
|---|---|
| `background`, `edges`, `selection` | Colours, `"#rrggbb"` (sRGB) |
| `hatch_strength` | Overall hatch intensity, 0..1 (the H key turns hatching off and on) |
| `hatch_darken` | For families without `ink`: how much darker hatch lines are than the fill, 0..1 |
| `ambient` | Light that ignores face orientation, 0..1. `1` gives flat colours, as on a printed plan |
| `families.<family>` | One entry per material family: `concrete`, `steel`, `timber`, `masonry`, `insulation`, `earth`, `glass`, `other` |

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
- Line weights: edges are 1 pixel. Thicker lines need a different line renderer.
- Title block (cartouche) and sheet layout: they belong with drawings (I4).
