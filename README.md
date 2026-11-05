# bimble

Open-source structural BIM (Building Information Modeling), built to stay fast on old machines.
IFC is the data model; `.bimble` is our own format, a superset of IFC with history and branches.
Concrete first: model → IFC → formwork and rebar drawings.

## Try it

```bash
cargo build --release
B=target/release/bimble

$B demo frame.bimble                     # 2 storeys, 3×2 bays concrete frame
$B info frame.bimble                     # counts, model hash, meshing time
$B ls frame.bimble --class IfcBeam       # GlobalId, class, name, parameters
$B set frame.bimble <GlobalId> height=600 -o frame2.bimble
$B diff frame.bimble frame2.bimble       # element-level diff
$B export-ifc frame.bimble frame.ifc     # IFC4

# Check the IFC with IfcOpenShell (pip install ifcopenshell)
python3 scripts/check_ifc.py frame.bimble frame.ifc
```

## Layout

| Crate | Role |
|---|---|
| `bimble-core` | IFC-typed model, GlobalIds, generators, commands + undo, hashing, diff, `.bimble` I/O |
| `bimble-geom` | Element meshes (flat-shaded, with feature edges) |
| `bimble-ifc` | IFC4 export |
| `bimble-cli` | `bimble` command-line tool |

See [ROADMAP.md](ROADMAP.md), [docs/adr](docs/adr) and [docs/format](docs/format).
