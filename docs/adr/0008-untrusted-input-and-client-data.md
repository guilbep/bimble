# ADR 0008 — Files are untrusted; client data stays out of the repo

- Status: Accepted (5 Nov 2026)
- Decided by: the PRD's security section; limits chosen by Claude after the independent review
- Reversal cost: low

## Decision: limits on untrusted input
| Where | Limit |
|---|---|
| `.bimble` file size | 512 MB; format and version header checked; full `Model::validate` on load |
| IFC file size | 2 GB |
| STEP list nesting | 64 levels; malformed entities skipped and counted, never a panic |
| Placement chains | 64 levels, cycles detected |
| Representation nesting | 32 levels; a representation map that contains itself is skipped |
| Geometry budget per IFC file | 10 M triangles and 5 M item visits (`read::Limits`), then stop with a warning |
| Parameters | data only, never executed (generator parameters are plain JSON) |
| `bimble render` size | ≤ the GPU's maximum texture size |

Why the budget: the review built a **1 KB IFC** whose mapped items nest and repeat; it expanded
exponentially and exhausted memory. Depth limits alone don't stop fan-out.

## Decision: client data
- Real project files (e.g. the Revit structural export used for validation) **never go into the repository**,
  and their names or client codes don't go into public docs, commit messages or PR text.
  The repo is public. A file name with a client code was in `ROADMAP.md` on `main` (removed in PR #2; still
  in history unless rewritten).
- Test fixtures are synthetic: our own exports, or small hand-written IFC files in
  `crates/bimble-ifc/tests/fixtures/`.
- Checks on real files are run locally with `scripts/compare_ifcopenshell.py`.
