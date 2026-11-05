# ADR 0005 — All model changes are Commands; undo via inverses; atomic groups

- Status: Accepted (5 Nov 2026)
- Decided by: both source documents ("the UI never writes the model directly"), implemented by Claude.
  CADCraft (Rust CAD, github.com/storytold/cadcraft) is the reference design to read before I2.
- Reversal cost: high once the UI and agents depend on it

## Decision
- Every mutation is a `Command` (`AddElement`, `SetGenerator`, `SetPlacement`, `Rename`, …).
- `Command::apply(model)` validates, applies, and **returns its inverse**. Undo/redo stacks hold inverses.
- `History::apply(group)` is all-or-nothing: on the first error, already-applied commands are rolled back.
- Order inside `apply`: **validate first, then canonicalise** (`version::canonical`), because
  canonicalisation goes through JSON, which cannot hold NaN or infinity. (A review regression showed
  canonicalising first turned an invalid NaN axis into a panic.)
- The same commands serve the viewer, the CLI (`bimble set`), scripts, and later an MCP server for agents.
- Commands are serializable (`op` tag): the history can become the operation log for versioning (ADR 0004).

## Validation enforced at the command boundary
- Generators: positive finite dimensions; slab outlines counter-clockwise with non-zero area.
- Placements: finite, unit axes, `x_axis ⟂ z_axis` (tolerance 1e-6).
- References: storey and material must exist; storeys/materials in use cannot be removed.
