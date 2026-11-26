# Bonsai, IfcOpenShell and Mission 1

_26 Nov 2026. Research ticket. "Opened" means the page was read. "Inference" means our reading. Earlier survey: `2026-11-20-state-of-the-art.md` (not repeated)._

## Summary

1. Bonsai does not open large IFC files directly for viewing. It converts once into a cache beside the IFC and links that: the same idea as `.ifc.bimbleview`.
2. Since May 2026 IfcOpenShell's own author builds a standalone "Bonsai Viewer" on a `.ifcview` sidecar cache, for 111 models and over a million objects. Inference: the convert-once design is the field's answer, not a Bimble quirk.
3. "Native IFC" is an editing property. Viewing is not blocked by its absence (inference).
4. IfcOpenShell as engine: licence is workable, the weight is not (a wheel is 42-55 MB against our 0.5 MB wasm). It fits as an optional native fallback converter, not as the viewer core.
5. Bonsai's drawings end in SVG, then PDF/DXF through external commands. Bimble's paper and DXF export are already ahead of that.

## 1. How Bonsai loads an IFC

- The importer uses `ifcopenshell.geom.iterator`, with `multiprocessing.cpu_count()` workers only if the user enables it. Kernel defaults to `opencascade`. Mapped shapes are cached by mesh name, so repeats share one Blender mesh. Elements with over 30 openings skip booleans. Source: `src/bonsai/bonsai/bim/import_ifc.py` (v0.8.0, raw.githubusercontent.com/IfcOpenShell/IfcOpenShell/v0.8.0/...; I opened it through a path with `..` that resolved, so the exact repository path is an inference).
- `iterate()` takes `num_threads=1` by default and an `.h5` geometry cache; kernels `cgal`, `cgal-simple`, `opencascade` and hybrids. Source: `src/ifcopenshell-python/ifcopenshell/geom/main.py`.
- Options for big files (Advanced Mode): filter by decomposition, class, type or white/blacklist; element offset and limit; "Native Meshes". No LOD is documented. Source: https://docs-unstable.bonsaibim.org/guides/viewing/dealing_with_large_models.html
- Stated limits: over 750 MB may cause memory problems; over 50,000 loaded elements may lag. Maintainer: about 30k objects is a struggle, 60-80k unusable; "link instead" https://community.osarch.org/discussion/comment/24397/
- A 400-500 MB file: Blender used 20+ GB RAM to open it, 80+ GB during a batch edit https://community.osarch.org/discussion/comment/27624/
- Parsing is the main bottleneck: `ifcopenshell.open()` takes about 20 s above 200 MB; web-ifc parses 3-7 times faster and wins total time on 22 of 26 test models, while IfcOpenShell wins geometry on 13 https://community.osarch.org/discussion/comment/28921/

## 2. Several files at once

- Bonsai's "Link IFC" tessellates in a background headless Blender, saves `<file>.ifc.cache.blend`, and links that collection. A JSON sidecar keeps georeferencing and the query. Properties go to `<file>.ifc.cache.sqlite` via the `ExtractPropertiesToSQLite` ifcpatch recipe. Source: `src/bonsai/bonsai/bim/module/project/operator.py`. The docs name the cache files and say linking is "much more efficient" than opening, with Explore Tool culling and right-click to query an object https://docs.bonsaibim.org/studio/guides/viewing/dealing_with_large_models.html
- Linked models cannot be edited. Whether clipping and section work on linked elements: not verified.
- Comparison (inference): same shape as ADR 0079. Differences: Bonsai's cache is a Blender file, so reading it needs Blender; Bimble's is a documented, bounded format. Bonsai has no LOD proxies and no stated byte budget.

## 3. Native IFC

- Bonsai is called native because "edits change the IFC data directly", with no translation to an internal schema https://wiki.osarch.org/bonsai/
- Gain: no round trip, full fidelity. Cost (from section 1): whole STEP graph and Blender objects in RAM, minutes to hours on large batch edits.
- Inference: Mission 1 is read-only and the cache holds what it needs. ".bimble is a superset of IFC" matters for editing, not viewing.
- In May 2026 Moult proposed `.ifcview`, a geometry-only cache of about 40% of the IFC size, and RDB, a database form of about 50%, and wrote that HDF5 will likely be dropped https://community.osarch.org/discussion/comment/28921/

## 4. Mission 1 line by line in Bonsai

- Section box: module `clip_box` (gizmos, operator) https://github.com/IfcOpenShell/IfcOpenShell/tree/v0.8.0/src/bonsai/bonsai/bim/module/clip_box . Behaviour not read.
- Automatic floor plans: `ifcopenshell.draw` calls the SVG serializer with `setSectionHeightsFromStoreys()` (`auto_floorplan=True`); hidden lines by HLR; cells filled with Shapely or svgfill. Source: `src/ifcopenshell-python/ifcopenshell/draw.py`. In Bonsai the camera is the cut (`setElevationRefGuid`), cut by OpenCascade or mesh bisect. Source: `src/bonsai/bonsai/bim/module/drawing/operator.py`.
- Show/hide and filter: selector language, below.
- Measure: Bonsai's tool not verified. The new Bonsai Viewer has length and area measurement (file list, v0.9.0 `src/ifcviewer`).
- Properties: read from the IFC graph, or from the sqlite sidecar for links (section 2).
- Notes: BCF 2.1/3 topics, comments, viewpoints with camera, selection, visibility, colours and clipping planes restored. Source: `module/bcf/operator.py`. IfcAnnotation drawings: in the drawing module, not read.
- PDF and DXF: user-set commands run on the SVG, for example Inkscape for PDF, Inkscape then pstoedit for DXF. Source: `drawing/operator.py`. The official drawing guide is an empty stub https://docs.bonsaibim.org/studio/guides/drawings/index.html

Selector syntax (`ifcopenshell/util/selector.py`): comma narrows, `+` unions, `!` negates; facets for class, GlobalId, attribute, `Pset.Prop = value`, `type`, `material`, `location`, `group`, `parent`; `= != > < *=`, `/regex/`, `NULL`, `TRUE`.

## 5. IfcOpenShell as an engine for Bimble

- Licence: LGPL-3.0 (`COPYING.LESSER`; PyPI: LGPL-3.0-or-later). Bimble is MIT (`LICENSE`). Dynamic linking to a shared library is allowed, with the user able to replace it. Static linking into the wasm would put the combined work under LGPL terms for relinking (inference, not legal advice). A separate `IfcConvert` process has no linking at all.
- Weight: the 0.9.0.post1 PyPI wheels are 42.0-54.9 MB https://pypi.org/project/ifcopenshell/#files . Per-component sizes (OpenCascade, Boost, CGAL, HDF5): not found. The viewer wasm is 0.5 MB.
- Wasm: only Pyodide test wheels, 0.8.1 to 0.9.0.post1, "Pyodide Test Wheels", feature set and size not stated https://github.com/IfcOpenShell/wasm-wheels .
- web-ifc: MPL-2.0, wasm builds with SIMD and multi-thread https://github.com/ThatOpen/engine_web-ifc . Size and geometry coverage not stated there.
- Speed: web-ifc 3-7x faster to parse; geometry roughly even (section 1). A rival's self-reported benchmark (IFClite) claims 21 of 21 wins https://community.osarch.org/discussion/comment/28995/ . No published same-machine benchmark covers xeokit or Fragments.
- Gain: NURBS, advanced B-rep, every class. Loss: size, startup, a heavy build.

## 6. Other viewers

- xeokit: `convert2xkt` (IFC through web-ifc, "alpha", property sets not supported; glTF from xeoIFC recommended); geometry reuse on by default; AGPL-3.0 https://github.com/xeokit/xeokit-convert , https://github.com/xeokit/xeokit-sdk
- That Open Fragments: FlatBuffers binary, `IfcImporter`, worker, culling and LOD, MIT https://github.com/ThatOpen/engine_fragment , https://docs.thatopen.com/Tutorials/Fragments/Fragments/FragmentsModels/ . Tiles and streaming (IfcStreamer, IfcGeometryTiler): search snippet only, pages not opened.
- Speckle: instancing cut IFC data by over 60% on average (2.2 GB to 299 MB in one case); converter not named https://speckle.systems/updates/optimized-ifc-models-for-faster-loading/
- Bonsai Viewer: first open converts, later opens "instant"; Moult likens it to XKT and Fragments; sidecar sits beside the IFC because moving files makes a separate cache hard to tie to its source; stale caches are deleted by hand https://community.osarch.org/discussion/comment/28995/ , https://community.osarch.org/discussion/comment/29259/ . Streaming and web build still to come.

## What Bimble could take (ranked)

1. The selector language for Mission 1's show/hide and filters (class, pset value, location, regex). Small, data-only, fits "parameters are data".
2. BCF for notes: a topic with a viewpoint of camera, selection, visibility and clipping planes. Exchange with other tools for free.
3. Sidecar lessons: tell the user where the cache is and how to clear it; check staleness (ADR 0079 already hashes the source).
4. Drawing conventions: cut at storey heights, IFC class as the SVG class.
5. `IfcConvert` as an optional native fallback for classes the C reader cannot draw (NURBS, advanced B-rep), run as a separate process into the same `.bimbleview`. LGPL stays out of our binary.

## What would not fit

- IfcOpenShell inside the viewer wasm: size, Pyodide start-up, LGPL relinking.
- Bonsai's cache format (a Blender file): needs Blender.
- Direct opening of 4.18 GB: Bonsai's own limits (750 MB, 50k elements) say it fails.
- Inkscape export chains: Bimble writes DXF itself (ADR 0082).

## Not verified

Bonsai clip box, measure and IfcAnnotation behaviour; clipping on linked models; IfcConvert options (README 404); web-ifc wasm size; component sizes of IfcOpenShell; Fragments 2.0 tiling; the `ifcviewer` README and licence (404; a third-party mirror was unreachable).
