# Bimble — agent instructions

Open-source structural BIM, fast on old machines. Concrete first; the drawing is the product.

- Read `ROADMAP.md` and `docs/adr/` first. New decisions → new ADR.
- Rust workspace in `crates/`. `cargo test --workspace` and `cargo clippy --workspace` must pass before claiming something works.
- IFC is the data model and vocabulary: use IFC class names in code and UI. `.bimble` is a superset of IFC.
- GlobalIds are stable: never regenerate them on edit.
- Model mutations go through `Command`s (they give undo, history and agent access). CADCraft is the reference design.
- Serialization must stay canonical (sorted maps, deterministic floats). Hashes and diffs depend on it.
- Units: millimetres, f64 in the model; f32 only in GPU meshes, relative to the element placement.
- Rendering: flat shading, edges, ~12-colour palette, hatch patterns per material. No textures.
- Files are untrusted input. Parameters are data, never executed.
- No performance claim without a benchmark.
