# ADR 0070 — Relations as model objects

- Status: Accepted, 25 Nov 2026
- Decided by: Pierre delegated the decision, as for ADR 0068 ("I won't be there to validate the
  next ADR, so accept everything and advance without me! let's follow fable5.2 recommendation").
  Recommendation from Fable 5.2: `docs/research/core-architecture.md` §8 and its row 13 ("open
  classes, relations, types in the model now: small now, costly later"); ticket X1 in
  `docs/research/next-round.md`.
- Reversal cost: low now (no file holds a relation yet), high once files exist.
- Extends: ADR 0068 rule 10 (full IFC compatibility), ADR 0003 (IFC is the model, `.bimble` a
  superset), ADR 0042 (binary `.bimble`, every element).

## Context

Full IFC compatibility (ADR 0068 rule 10) needs assemblies (`IfcElementAssembly` with
`IfcRelAggregates`) and typed connections (`IfcRelConnectsElements`,
`IfcRelConnectsWithRealizingElements`). Today the model holds elements only: an imported
assembly loses its structure, and a connection cannot be stored at all. The Rust oracle has no
relations (`crates/bimble-core/src/model.rs`), so this is new behaviour of the C core. A new
section of the file is cheap before files use it and costly after.

## Decision

1. **One relation table** in the model, `cc_model.relations`, sorted by GlobalId:
   `{id, kind, relating, related[], realizing[], properties{}}`. Each relation has its own
   GlobalId (stable, never regenerated, as every id).
2. **`kind` is the IFC class name as text** (`"IfcRelAggregates"`), checked against a table in
   `ccore/src/relation.c`: `IfcRelAggregates`, `IfcRelConnectsElements`,
   `IfcRelConnectsWithRealizingElements`, `IfcRelDefinesByType`. A new kind is one table row.
   Text in the file (not a number) keeps the file readable and the vocabulary IFC's.
3. **Ids name elements.** `relating`, `related` and `realizing` are element ids of the model.
   `IfcRelDefinesByType` relates a type object; the model holds no type objects yet, so the kind
   is reserved and every such relation is refused ("needs a type object") until types exist.
4. **Rules** (on load and on every Command, one function): the kind is in the table; every id
   exists; the relation's id is used by no other object (project, storey, material, element,
   view, sheet); no relation to itself; `related` and `realizing` sorted, without duplicates,
   1 to 10,000 ids each; the two connection kinds relate exactly one element; only
   `IfcRelConnectsWithRealizingElements` has realizing elements, at least one, none of them
   connected; property keys not empty, at most 1,000. Across relations, `IfcRelAggregates` gives
   each part one whole and has no cycle (each part has one parent, so a walk up the parents is a
   path: linear in the number of parts).
5. **Openings stay embedded** in their element (`IfcRelVoidsElement` is not in the table), as
   model.rs stores them. The file keeps reading that form.
6. **Commands** `add_relation`, `remove_relation`, `set_relation`, each with its inverse, one
   undo step per history apply. `remove_element` of an element named by a relation is refused:
   "`<id>` is still used by the relation `<relation id>` (`<kind>`)". What the `delete` verb does
   (update or remove the relations in the same step) is ticket X2's decision.
7. **File.** Key `relations` in the model, a map GlobalId → relation like `elements`, after
   `schedules`, **left out when empty**; `realizing` and `properties` left out when empty. Same
   shape in v1 (JSON) and v2 (CBOR). Every existing file keeps its bytes and its hash
   (`ccore/test/relation_test.c` checks `golden/frame*.json` and `golden/bin/frame*.bimble`).
8. **`IfcElementAssembly` is an element class** with no generator of its own: its shape is its
   parts. The element's `generator` key is left out (the only element without one), it has no
   openings, and `set_generator` on it is refused. In memory its generator kind is
   `CC_GEN_NONE`, a value past `CC_GEN_COUNT`, so no table of generator kinds (file tags, verbs)
   knows it.
9. **Open classes only as far as needed.** The class stays an enum (`cc_ifc_class`) with one
   more value. A full class-name store (core-architecture §8.1) would change: `cc_element.cls`
   becomes text plus a table of what each class can do (generator or none, openings, edit by
   dimensions); `ifc_entity` and the proxy rule of ADR 0042 merge into it; every `switch` on the
   class becomes a table lookup; the file keeps `"class"` as text, so files would not change.
   Not done now: one class does not pay for it.

## Consequences

- A `.bimble` with relations, or with an `IfcElementAssembly`, is the C core's own: Rust ignores
  the `relations` key (no `deny_unknown_fields` on `Model`) and drops it on save, and refuses
  the class `IfcElementAssembly` ("unknown variant"). `ccore/golden/DEVIATIONS.md` lists this.
- Until X2: the scene draws no mesh for an assembly and warns once ("could not mesh"); the IFC
  export refuses a model with an assembly (an error, never a silent drop, ADR 0003); no verb makes
  a relation. X2 adds the verbs (`assemble`, `connect`…), the query, export and import.
- Disassembly data (fasteners, reversibility) goes in the connection's properties.
- Measured: none needed (no performance claim). Validation is linear in relations and ids, plus
  a sort of the aggregate parts.

## Alternatives considered

- **Relations inside elements** (a `parts` list on the assembly, `connections` on each
  element): no GlobalId for the relationship, two places to keep in step, and IFC's relationship
  objects (with their own ids and properties) cannot round trip.
- **`kind` as an enum number in the file**: smaller, but unreadable and not IFC's vocabulary.
- **Let `IfcRelDefinesByType` relate an element standing for the type**: works today, but the
  files would need a migration when type objects arrive. Refusing it costs nothing now.
- **A generator `none` in the file** (`"generator": {"kind": "none"}`): every reader of
  generators would see a kind that has no shape. Leaving the key out keeps generators as they
  are.
