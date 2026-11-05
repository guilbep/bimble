# Architecture decision records

Short records of decisions: the problem, what we chose, what we rejected, evidence, and when to revisit.
Humans and agents read these before changing anything structural (see `CLAUDE.md`). New decision → new file;
a decision that changes an old one says which one it supersedes.

| # | Decision | Status |
|---|---|---|
| [0001](0001-iteration-1-stack.md) | Rust core, own wgpu renderer, IFC-typed native format | Accepted |
| [0002](0002-pure-rust-ifc-reader-for-display.md) | Pure-Rust IFC reader for display; IfcOpenShell as reference | Accepted |
| [0003](0003-scope-and-data-model.md) | Structural scope; IFC as the model; `.bimble` as a superset; `.b3d` cache | Accepted |
| [0004](0004-versioning-model.md) | Stable ids, canonical form, hashes, semantic diff ("GitHub for building models") | Accepted, store not built |
| [0005](0005-commands-undo-transactions.md) | All changes are Commands; undo via inverses; atomic groups | Accepted |
| [0006](0006-rendering.md) | Drawing-style rendering, offscreen target, redraw on change | Accepted |
| [0007](0007-units-precision-tolerances.md) | Units, precision, tolerances | Accepted |
| [0008](0008-untrusted-input-and-client-data.md) | Untrusted-input limits; client data stays out of the repo | Accepted |
| [0009](0009-testing-ci-releases.md) | IfcOpenShell oracle, CI with software GPU, release workflow | Accepted |
| [0010](0010-geometry-kernel.md) | Geometry kernel: manifold3d for mesh booleans; exact B-rep later | Accepted |
| [0011](0011-performance-regressions.md) | Performance: geometry budgets fail the build, timing trend warns | Accepted |
| [0012](0012-parallel-loading.md) | Parallel loading: chunked parse, per-element geometry on all cores, mimalloc | Accepted |
| [0013](0013-graphic-style.md) | Graphic style: every colour, hatch and light value is data (JSON, `--style`, live editor) | Accepted |
| [0014](0014-section-box.md) | Section box: GPU clipping, back faces as cut surfaces, back edges hidden | Accepted |

Session journals (what happened, in order, with measurements) are in [`docs/journal`](../journal).

## Template

```markdown
# ADR NNNN — Title

- Status: Proposed | Accepted | Superseded by NNNN (date)
- Decided by: who, and on what basis
- Reversal cost: low | medium | high

## Context / Problem
## Decision
## Alternatives considered
## Evidence
## Revisit when
```
