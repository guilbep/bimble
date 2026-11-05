First preview of Bimble: a fast, open-source structural BIM tool. This release **views** IFC files and Bimble's own `.bimble` models; editing comes next.

## Download

| System | File |
|---|---|
| macOS, Apple Silicon (M1–M4) | `bimble-*-aarch64-apple-darwin.tar.gz` |
| macOS, Intel | `bimble-*-x86_64-apple-darwin.tar.gz` |
| Windows 10/11 | `bimble-*-x86_64-pc-windows-msvc.zip` |
| Linux x86_64 | `bimble-*-x86_64-unknown-linux-gnu.tar.gz` |

`SHA256SUMS` lists the checksums.

## Run

Unpack, then run `bimble` to open the viewer, or `bimble view your-model.ifc`. You can also drop a file on the window.

- **macOS:** the binary isn't signed yet. If macOS blocks it, run `xattr -d com.apple.quarantine bimble` in the unpacked folder once.
- **Windows:** SmartScreen may warn about an unknown publisher; choose *More info → Run anyway*.

## What's in {{VERSION}}

- **Viewer:** drawing-style 3D (flat shading, black edges, hatch pattern per material family). Orbit, pan, zoom, click to select, filters by material, IFC class and storey. Redraws only when something changes.
- **IFC reading:** IFC2x3, IFC4 and IFC4x3 files. Extrusions (including arcs and voids), face sets, B-reps and mapped items. On a 776-element Revit 2025 structural export it matches IfcOpenShell element by element (volumes within 0.07 %, positions within 0.002 mm).
- **Command line:** `bimble render` (PNG, no window), `bimble info`, `bimble demo`, `bimble diff`, `bimble export-ifc`.

## Known limits

- Openings are not cut out yet (walls and slabs show without their holes).
- Boolean clippings are drawn without the cut.
- No editing in the viewer yet.
- Performance on old laptops still has to be measured. Please report what you see (model size, machine, how it feels).
