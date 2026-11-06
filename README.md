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
bimble plan model.ifc plan.pdf --storey "Niveau 1" --scale 100      # storey plan (.pdf, .svg, .png)
bimble plan model.ifc plan.pdf --storey N01 --template docs/templates/plan-fr.json \
       --field phase="Projet d'ouvrage"                              # office title block and words
bimble plan-template                           # the default plan template (JSON) to start from

bimble history init frame.bimble               # history in frame.bimble-history/
bimble history commit frame.bimble -m "Deeper beams"
bimble history log frame.bimble
bimble history branch frame.bimble variant && bimble history switch frame.bimble variant
bimble history merge frame.bimble variant      # by GlobalId, field by field; --prefer ours|theirs
```

`.bimble` models can also be edited in the viewer (Edit…, History…, Ctrl+Z / Ctrl+Y / Ctrl+S).

## In a browser

The viewer also runs in a web page (WebGPU, or WebGL2 where there is no WebGPU). Files you open
stay on your computer: nothing is uploaded. Saving and plan exports are downloads; the model
history is desktop-only for now.

```bash
rustup target add wasm32-unknown-unknown
cargo install wasm-bindgen-cli --version <the wasm-bindgen version in Cargo.lock>
scripts/build-web.sh                     # → web/dist/ (static files)
python3 -m http.server -d web/dist 8000  # then open http://localhost:8000
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

See [ROADMAP.md](ROADMAP.md), [docs/adr](docs/adr), [docs/format](docs/format) and, to publish a version, [docs/RELEASING.md](docs/RELEASING.md).
