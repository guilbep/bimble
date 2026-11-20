# ADR 0063 — Carbon balance (LCA): KBOB factors on materials, amounts from quantities

- Status: Proposed
- Decided by: not yet: proposed from the wireframes v01, for Pierre
- Reversal cost: low (a factor table and a report, the model gains one link per material)
- Extends: ADR 0029 (schedules), ADR 0031 (material library), ADR 0051 (the LCA trade tab)

## Context / Problem

Swiss clients and competitions ask for the embodied carbon of the structure (SIA 2032, SIA 390/1).
The structure is most of a building's embodied carbon, and Bimble already has the quantities:
volumes per material, mass from the grade's density (ADR 0031). The wireframe D7 shows an LCA tab
with a carbon balance: total, per m² of floor, a variant comparison, a table of quantities and
factors.

## Decision

- **Where the factors come from.** The emission factors are the KBOB list ("Ökobilanzdaten im
  Baubereich", KBOB 2022/1 in the wireframe), the Swiss public reference. Bimble ships the version
  it was tested with as data, cites it by name and version on every report, and lets an office
  load a newer list. Bimble does not invent or average factors.
- Each material of the model links to one KBOB entry (Data → Assign). The link is guessed from
  the grade ("Concrete C30/37" → the matching KBOB concrete) and the user confirms. Elements whose
  material has no link are listed as "Unassigned: 12 elements".
- Modules A1–A3 (product stage) first. Other modules later.
- Amounts come from Quantities (ADR 0029): m³ for concrete and timber, kg for steel and rebar.
  Carbon = amount × factor. Nothing is stored: the balance is computed.
- Per m² uses the energy reference area (ERA, SIA 380) from project information (ADR 0030).
- Compare variants: the balance of the main model against a variant (wireframe "poutres-650":
  +23 t). Variants depend on the variants ADR (open in ADR 0038's list). Until then, compare
  against another saved version.
- **The sample numbers in the wireframe are synthetic.** The 265 t, the per-material values and
  the factors (431, 1.73, 2.93, 689 kg CO₂-eq per unit) illustrate the layout only. They are not
  KBOB values and are not used in tests. Test fixtures use synthetic factors marked as such.
- Report: Export report writes a PDF and a CSV with the KBOB version, modules and the table.

## Alternatives considered

- Ecoinvent or EPD databases: licensed, not free. KBOB is public and Swiss. Others later as an
  extra source.
- Factors typed by hand per material: no reference, no check. Kept only as an override, shown as
  such in the report.
- A separate LCA program fed by IFC export: works today, but loses the live link to the model.

## Evidence

- `docs/design/wireframes-v01/en/LCA.html`: "Data KBOB 2022/1, Modules A1–A3, Unassigned 12
  elements", "Amounts come from Quantities; each material in the model is linked to a KBOB entry",
  "Total, main variant 265 t", "Per m² of floor 186 kg CO₂-eq / m² ERA", "Sample data".
- `docs/adr/0031-material-library.md:9-25`: grades with density, `lookup(name)`, a Mass (kg)
  column in schedules.
- `crates/bimble-core/src/command.rs:48`: `SetMaterial` exists to store the link.

## API needed

- Core: `Material` gains `lca: Option<LcaLink { source: "KBOB", version, entry_id }>`. Set with the
  existing `SetMaterial` Command. A factor table loaded from a file, with budgets (files are
  untrusted).
- Verbs: `lca_assign` `{"material":"<GlobalId>","entry":"<KBOB id>"}`, `lca_load`
  `{"path":"kbob-2024.csv"}`.
- Queries: `lca_balance` `{"modules":"A1-A3","compare":"<version or variant>"}` → `{total_t,
  per_m2_kg, rows:[{material, amount, unit, factor, t}], unassigned:[GlobalId]}`, `lca_entries`
  `{"search":"C30/37"}`.
- IFC: `IfcEnvironmentalImpactValue` existed in IFC2x3 and is gone in IFC4. IFC4 has
  `Pset_EnvironmentalImpactIndicators` (ClimateChangePerUnit, FunctionalUnitReference) on the
  material or type: Bimble writes the factor there on export. The KBOB link itself is
  Bimble's superset (ADR 0003).

## Revisit when

KBOB publishes a new list, an office needs modules beyond A1–A3, or variants exist.
