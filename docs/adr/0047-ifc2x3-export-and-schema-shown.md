# ADR 0047 — IFC2x3 export, and the schema shown

- Status: Accepted (11 Nov 2026)
- Decided by: Pierre ("we should support IFC4 and IFC3 and make it clear in the interface"); details
  by Claude
- Reversal cost: low (one module writing the IFC4 output down; two menu entries)

## Context / Problem

Bimble reads IFC2x3 and IFC4 alike (the office's Revit files are IFC2x3) but wrote IFC4 only, and
only from the command line. Many structural tools still read IFC2x3 only. Nothing in the window
said which schema an open file was.

## Decision

1. **File → Export IFC4… / Export IFC2x3…** (and `bimble export-ifc --schema 2x3`): two entries,
   not a dialog, so the schema is chosen where it is seen. For a `.bimble` model (an IFC open as it
   is: send the IFC itself, or convert it first).
2. **IFC2x3 is Bimble's IFC4 output written down** (`bimble_ifc::ifc2x3`): Bimble writes few
   entity kinds, so the differences are a short list. Every rooted object gets an owner history
   (Bimble, no change tracking); columns, beams, walls and openings lose their PredefinedType;
   a proxy's last attribute becomes its CompositionType; a material is its name only; a rectangle
   profile gets its position. Left out, said when saving: material values (IFC4 material property
   sets) and the map conversion (the place and project north stay in the building's placement,
   ADR 0045).
3. **Checked against the schemas**: IfcOpenShell's validator passes both exports with no issue,
   on the demo frame and on an office model (5 965 elements, proxies, meshes, a place and a turn),
   which also read back whole, with their place. CI validates both (`scripts/validate_ifc.py`).
4. **The schema is shown**: next to the file name in the top row ("model.ifc · IFC2X3"), and by
   `bimble info`.

Found while doing it: the IFC4 export wrote a profile without voids as one "with voids" and an
empty list (invalid), and a profile with voids lost them; import made a rectangle with a hole a
solid rectangle. Both fixed, tested.

## Alternatives considered

- A second exporter written for IFC2x3: two to keep in step for the same few entities.
- A schema choice in a dialog: one more step, the schema not visible in the menu.

## Revisit when

- Bimble writes more entity kinds (types, materials by layer): check the list.
- IFC4.3 is asked for.
