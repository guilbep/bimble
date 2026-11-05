# Bimble — agent instructions

Open-source structural BIM, fast on old machines. Concrete first; the drawing is the product.

- Read `ROADMAP.md`, `docs/adr/README.md` (index of decisions) and the latest `docs/journal/` entry first.
  New decision → new ADR. End a working session with a journal entry (what happened, who decided, numbers).
- Rust workspace in `crates/`. `cargo fmt --all`, `cargo clippy --workspace --all-targets -- -D warnings` and
  `cargo test --workspace` must pass before claiming something works. CI uses the latest stable Rust:
  run `rustup update stable` first so new lints show up locally.
- IFC is the data model and vocabulary: use IFC class names in code and UI. `.bimble` is a superset of IFC.
- GlobalIds are stable: never regenerate them on edit.
- Model mutations go through `Command`s (undo, history, agent access). Validate first, then `canonical()`.
- Serialization must stay canonical (sorted maps, exact float round trip, no `-0.0`). Hashes and diffs depend on it.
- Units: millimetres, f64 in the model; f32 only in GPU meshes, relative to the scene origin.
- Rendering: flat shading, edges, ~12-colour palette, hatch patterns per material. No textures.
- Files are untrusted input: bounded recursion, budgets, no panics. Parameters are data, never executed.
- Never commit client/project files, or their names, to this public repo. Fixtures are synthetic.
- IfcOpenShell is the reference: compare with `scripts/check_ifc.py` and `scripts/compare_ifcopenshell.py`.
  A regression test should fail on the old code, not only pass on the new.
- No performance claim without a benchmark.
