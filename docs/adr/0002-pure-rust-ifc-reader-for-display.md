# ADR 0002 — Pure-Rust IFC reader for display; IfcOpenShell stays the reference

- Status: Accepted (5 Nov 2026)
- Reversal cost: low. The reader produces a `Scene`; another backend can produce the same type.

## Problem
The viewer had to open IFC files for the first release. The roadmap's main option for IFC import
was IfcOpenShell (C++) called from Rust. Building IfcOpenShell and OpenCASCADE for macOS (two
architectures), Windows and Linux in CI is a large project of its own. It would have delayed the
first binary by weeks and made every build heavy.

## Decision
- For **display**, Bimble reads IFC with its own Rust code: a STEP parser (`bimble-ifc::step`)
  and a geometry reader (`bimble-ifc::read`) for the representations structural models actually
  use: extrusions (polylines, indexed poly curves with arcs, parametric profiles, voids), polygonal
  and triangulated face sets, faceted B-reps, mapped items.
- Anything unsupported is skipped and **reported** as a warning. Boolean results are drawn as their
  first operand. Openings are not cut.
- **IfcOpenShell remains the reference.** `scripts/compare_ifcopenshell.py` compares elements,
  classes, volumes and bounding boxes element by element. CI runs it on our own exports.

## Evidence
On a Revit 2025 structural export (776 elements, IFC4, metres):
- same elements and IFC classes as IfcOpenShell 0.9;
- volumes within 0.065 %, bounding boxes within 0.002 mm;
- loads in ~70–100 ms in our reader. IfcOpenShell took 3.4 s to tessellate the same file, but it
  also does more work (it cuts openings), so this is not a like-for-like speed comparison.

## Revisit when
- We need boolean operations (opening cuts, clipping) or exact B-rep geometry. That is also the
  open "geometry kernel" decision, and IfcOpenShell/OCCT is a candidate there.
- Full IFC import for **editing** (I3) needs relationships and property sets we don't read yet.
  Decide then whether to extend this reader or bind IfcOpenShell.
