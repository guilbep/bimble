# bimble

Open-source structural BIM (Building Information Modeling), built to stay fast on old machines.
IFC is the data model; `.bimble` is our own format, a superset of IFC with history and branches.
Concrete first: model → IFC → formwork and rebar drawings.

## Download

Prebuilt binaries for macOS, Windows and Linux are on the
[releases page](https://github.com/guilbep/bimble/releases). Unpack and run `bimble`.

## View a model

```bash
bimble                         # opens the viewer; Open… or drop a file on the window
bimble view model.ifc          # IFC2x3 / IFC4 / IFC4x3, or a .bimble file
```

Drag to orbit, right-drag or Shift-drag to pan, scroll to zoom, click to select, double-click or
`F` to fit, `T` top view, `O` orthographic, `E` edges, `H` hatching. The side panel shows or hides
elements by material family, IFC class and storey.

## Command line

```bash
bimble render model.ifc out.png --hide earth   # PNG without a window (also works on servers)
bimble info model.ifc                          # counts, materials, triangles, load time
bimble demo frame.bimble                       # 2 storeys, 3×2 bays concrete frame
bimble ls frame.bimble --class IfcBeam         # GlobalId, class, name, parameters
bimble set frame.bimble <GlobalId> height=600 -o frame2.bimble
bimble diff frame.bimble frame2.bimble         # element-level diff
bimble export-ifc frame.bimble frame.ifc       # IFC4
```

## Build from source

```bash
cargo build --release          # binary in target/release/bimble
cargo test --workspace
```

Checks against IfcOpenShell (`pip install ifcopenshell`):

```bash
python3 scripts/check_ifc.py frame.bimble frame.ifc            # our export, read by IfcOpenShell
bimble info model.ifc --elements-json > ours.jsonl
python3 scripts/compare_ifcopenshell.py model.ifc ours.jsonl    # our reader vs IfcOpenShell
```

## Layout

| Crate | Role |
|---|---|
| `bimble-core` | IFC-typed model, GlobalIds, generators, commands + undo, hashing, diff, `.bimble` I/O |
| `bimble-geom` | Meshes, scenes (render-ready, rebased to a local origin), crease edges |
| `bimble-ifc` | IFC4 export; STEP parser and IFC reader for display |
| `bimble-render` | Drawing-style wgpu renderer, camera, picking, headless PNG output |
| `bimble-viewer` | The viewer window (eframe) |
| `bimble-cli` | The `bimble` binary |

See [ROADMAP.md](ROADMAP.md), [docs/adr](docs/adr) and [docs/format](docs/format).
