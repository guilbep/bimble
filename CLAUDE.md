# Bimble — agent instructions

Open-source structural BIM, fast on old machines. Concrete first; the drawing is the product.
A C core (`ccore/`) behind a Vue lab (`web-ui/`); no Rust on `main` (ADR 0072).

- Read `ROADMAP.md`, `docs/adr/README.md` (index of decisions), the latest `docs/journal/` entry and
  `DEV.md` (commands) first. New decision → new ADR. End a working session with a journal entry
  (what happened, who decided, numbers).
- The gate: `ccore/ci/run.sh fast` must pass before claiming something works (`full` before a merge
  to `main`: CI runs it). One suite: `ccore/test/run.sh <name>`; a node or browser test:
  `CCORE_PORT_BASE=auto node ccore/test/<name>.mjs`. Paste the raw last lines, never a summary.
- IFC is the data model and vocabulary: use IFC class names in code and UI. `.bimble` is a superset of IFC.
- GlobalIds are stable: never regenerate them on edit.
- Model mutations go through verbs and `Command`s (undo, history, agent access): a verb's handler
  returns commands, never edits the model. Validate first, then `canonical()`.
- Serialization must stay canonical (sorted maps, exact float round trip, no `-0.0`). Hashes and diffs depend on it.
- Units: millimetres, f64 in the model; f32 only in GPU meshes, relative to the scene origin.
- Rendering: flat shading, edges, ~12-colour palette, hatch patterns per material. No textures.
- Files are untrusted input: bounded recursion, budgets, no crash, no unbounded allocation.
  Parameters are data, never executed. The sanitizer suites (ASan, UBSan) must stay clean.
- Layers (ADR 0068, `ccore/README.md`): `ccore/src` is pure (no files, clock, threads or globals);
  platform code only in the carriers (`ccore/host`, `wasm_api.c`); a file includes only its layer
  and lower ones.
- The oracle (ADR 0067): Rust v0.19.0 is frozen. The references it wrote are committed under
  `ccore/golden/` and the C suites check them byte for byte without Rust. A change of a reference
  is regenerated on the branch `oracle/rust` (never by hand) and committed with the reason; an
  intended difference from Rust gets its section in `ccore/golden/DEVIATIONS.md`. Comments citing
  a Rust file and line (`ribbon.rs:612`) are the port's provenance: keep them.
- What to port next follows ADR 0069's order (IFC in and out, editing, 2D views, paper) and the
  open rows of `ccore/results/parity.md`; the tickets are in `docs/research/round-4.md`.
- The page never guesses what the core serves: actions carry `enabled` (`query actions`), pages,
  panels, data sets and exports a capability (`query capabilities`, `ccore/session/capabilities.c`).
  Nothing fake: what is not served shows "not available yet".
- The lab shell is a reducer: one transition table (`web-ui/src/shell/machine.ts`), components move
  only through `send(event)`; a control is shown only where its event has a row
  (`ccore/test/shell_graph.mjs`).
- Every network port of a test comes from `ccore/test/ports.mjs`, one row each, a unique offset.
- Every C object belongs to a feature in `ccore/tools/weight.mjs` (`ccore/results/weight.md`);
  `ccore/tools/perf.mjs` and `ccore/tools/parity.mjs` write `perf.md` and `parity.md`. Never edit
  a generated report by hand.
- Never commit client/project files, or their names, to this public repo. Fixtures are synthetic.
- IfcOpenShell is the reference beyond Rust: `ccore/bench/corpus.sh ios`, `scripts/check_ifc.py`,
  `scripts/validate_ifc.py`. A regression test should fail on the old code, not only pass on the new.
- No performance claim without a benchmark.
