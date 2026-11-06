# ADR 0031 — Material library and browser

- Status: Accepted (6 Nov 2026)
- Decided by: Pierre (proposed a material library like Revit's material browser); design by
  Claude
- Reversal cost: low (optional properties on materials, one command, one page)

## Context / Problem
Materials had a name and a family only. An engineer needs a grade's density (steel tonnage,
self-weight), modulus and strength, and a place to see and change materials like Revit's
material browser.

## Decision
- **Library** (`bimble_core::library`): standard grades with density, mean modulus,
  characteristic strength and the table they come from: concrete C20/25–C50/60 (EN 1992-1-1
  Table 3.1, 25 kN/m³), steel S235–S460 (EN 1993-1-1, E = 210 GPa), B500B (SIA 262, Es = 205 GPa),
  timber C24 (EN 338), glulam GL24h–GL32h (EN 14080). A starting point, to be checked against the
  standard a project uses.
- **`lookup(name)`** finds the grade a material name refers to ("S235(R)", "Steel S355 J2",
  "GL24h 24x46"), the longest grade name winning, so IFC materials get values without being
  edited.
- **`Material.props`**: optional density, modulus, strength, source (absent when unknown, so older
  models keep their hash). **`SetMaterial`** changes a material (name, family, values): undo,
  history.
- **Schedules** gain a **Mass (kg)** column: volume × the grade's density; in steel schedules and
  All elements by default.
- **The page** (Manage → Materials): the model's materials with a search and their element
  counts, the library below with **Add**; the selected material's identity (name, family, IFC
  category), properties (dragged values are one undo step), "Use the values of <grade>", and its
  graphics from the standards of its family (screen colour, cut and seen hatch types), with
  **Edit in the standards**. An IFC's materials are listed read only, with their grade's values.

## Alternatives considered
- Graphics per material (Revit-style) instead of per family: the drawings, styles and hatch
  types are per family today; per-material overrides can come later on top.
- Reading Pset_MaterialMechanical / Pset_MaterialCommon from the IFC: rarely filled by exporters;
  the grade name is.

## Evidence
Tests: library valid and named once; names find their grade (and "C25/30" isn't C24); bad
values refused; demo columns weigh 0.25 m³ × 2500 kg/m³. On the virtual display: the page, C30/37
with its library values, Add S235.

## Revisit when
An office needs its own library (a file of materials), or graphics per material.
