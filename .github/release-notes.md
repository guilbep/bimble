Bimble {{VERSION}}: a preview of a fast, open-source structural BIM tool. It **views** IFC files in 3D, plans, ceiling plans and sections, overlays **another office's DXF or PDF plan** to check it, draws **storey plans at scale**, and **edits and versions** Bimble's own `.bimble` models. It also runs **in a browser**.

User manual: https://guilbep.github.io/bimble-docs/

## Download

| System | File |
|---|---|
| macOS, Apple Silicon (M1–M4) | `bimble-*-aarch64-apple-darwin.tar.gz` |
| macOS, Intel | `bimble-*-x86_64-apple-darwin.tar.gz` |
| Windows 10/11 | `bimble-*-x86_64-pc-windows-msvc.zip` |
| Linux x86_64 | `bimble-*-x86_64-unknown-linux-gnu.tar.gz` |
| Any browser (static page, to host yourself) | `bimble-*-web.zip` |

`SHA256SUMS` lists the checksums. The browser version is also online: https://guilbep.github.io/bimble/ (files you open there stay on your computer).

## Run

Unpack, then run `bimble` to open the viewer, or `bimble view your-model.ifc`. You can also drop a file on the window.

- **macOS:** the binary isn't signed yet. If macOS blocks it, run `xattr -d com.apple.quarantine bimble` in the unpacked folder once.
- **Windows:** SmartScreen may warn about an unknown publisher; choose *More info → Run anyway*.

## Try

- `bimble view model.ifc`: the viewer. The left panel lists the model's **views** (3D, plans, ceiling plans looking up, sections) with their **view range** (top, cut plane, bottom, view depth); select, edit and measure in any of them. In a plan, **Add reference plan…** lays a DXF or vector PDF over the model (DWG: convert it to DXF first); move it into place and measure on it. Also: section box (B), measuring (M), view cube, linked models, **Plan…** with a live preview, **Style…** for every colour and hatch.
- `bimble plan model.ifc plan.pdf --storey "Level 1" --scale 100`: a storey plan at scale on A4–A0 (PDF, SVG or PNG); `--template` for your office's title block and words.
- `bimble demo frame.bimble`, then edit it in the viewer (Edit…, History…) or with `bimble history …`.
- `bimble bench --json > bench.json`: how fast it is on your machine. Please send the file, especially from old laptops.

## Known limits

- Editing works on `.bimble` models, not on IFC files (IFC is read for viewing and plans). An IFC's views are saved next to it in `model.ifc.bimble-views.json`.
- Layouts: sheets with several views aren't there yet; the paper plan (**Plan…**) is a storey plan cut at one height.
- Reference plans: DXF model space and the first page of a vector PDF; no text, hatches or scanned PDFs.
- In the browser: no model history yet, and an IFC's views file can't be read back; loading uses one processor core.
- Plans: hidden lines are removed for flat tops only; no dimensioned openings, labels or section markers yet (see the issues).
- Curved geometry is drawn as fine facets (0.5 mm tolerance), not exact surfaces.
- Performance on old laptops still has to be measured. Please report what you see (model size, machine, how it feels).

## Changes
