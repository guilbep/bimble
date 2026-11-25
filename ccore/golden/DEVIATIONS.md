# Deviations from the Rust oracle

The Rust crates are the oracle: the C core matches them byte for byte (README). A few things are
deliberate departures, decided in an ADR. Each one is listed here: what differs, why, what a Rust
reader sees, and where the C code is. A change to Rust's behaviour that is not in this list is a
bug.

## 1. Dimension anchors name a feature (ticket C4, ADR 0068 rule 9)

**What.** Rust's `Anchor` (crates/bimble-core/src/view.rs) is `{at, element?}`: a world point
and the element it was taken on. The C core adds an optional third key, `feature`:

    {"at": [-150.0, -150.0, 0.0], "element": "2P97ZHH3v3XutfaphHA_Tu", "feature": "corner:0"}

`feature` is `"origin"`, `"end"` or `"corner:N"` of the element's generator (`ccore/src/annot.h`
lists them per generator). When the element and the feature exist, the end of the dimension is
computed from them each time it is asked (`query view_annotations`, the page's overlay), so a
dimension follows its element when it moves or changes size, and undo brings it back. Without the
element (deleted) or the feature, `at` is used. `at` keeps the point at creation; it is never
written back.

**Why.** Pierre's priority 3 (ADR 0069): a 2D view where dimensions stay right when the model
changes. Rust stores only `at` and `element` and does not move `at` when the element moves.

**What Rust sees.** A file without feature anchors is unchanged: same keys, same bytes. A file
with one is refused by Rust's reader (`deny_unknown_fields` on `Anchor`: "unknown field
`feature`"), in v1 (JSON) and v2 (CBOR). Rust needs the same optional field to read them.

**Commands.** The C verbs `add_dimension`, `add_text`, `move_annotation`, `delete_annotation`
(C additions, `ccore/src/verb_table.c`) each make one `set_view` command (`add_view` the first
time a default plan gets an annotation), one history step, as Rust's desktop does with
`update_view`. ADR 0068 rule 9 asks for own Commands (`add_annotation`, `set_annotation`,
`remove_annotation`); they are not done yet, so the history stays readable by Rust. The cost: one
edit records the whole view.

**Where.** `ccore/src/annot.{h,c}` (features, the drawing geometry, the query), `ccore/src/view.c`
(the `feature` key: read, write, copy, validation), `ccore/src/verbs.c` (the verbs),
`ccore/session/calls.c` (`query view_annotations`, the annotate tool), tests
`ccore/test/annotation_test.c`, `annotations_test.mjs`, `lab_dims.mjs`.

## 2. Relations and assemblies (ticket X1, ADR 0070)

**What.** The C model has a relation table that Rust's `Model` does not have: key `relations`
in the model, after `schedules`, a map GlobalId → `{id, kind, relating, related, realizing?,
properties?}`, in v1 (JSON) and v2 (CBOR). `kind` is an IFC class name from a table
(`IfcRelAggregates`, `IfcRelConnectsElements`, `IfcRelConnectsWithRealizingElements`;
`IfcRelDefinesByType` is reserved and refused until the model holds type objects). The element
class `IfcElementAssembly` is new too: such an element has no `generator` key (its shape is its
parts). Commands `add_relation`, `remove_relation`, `set_relation` are the C core's own, and
`remove_element` of an element that a relation names is refused ("`<id>` is still used by the
relation `<relation id>` (`<kind>`)"); Rust has no such case. The serde error texts that list the
classes and the command tags name the new ones too.

**Why.** ADR 0068 rule 10 (full IFC compatibility): assemblies and typed connections as objects
with GlobalIds, cheap before files hold them.

**What Rust sees.** A model without relations and without assemblies is unchanged: same keys,
same bytes, same hash (`relation_test` reloads `golden/frame*.json` and `golden/bin/frame*.bimble`).
A model with relations loads in Rust without them (`Model` ignores unknown keys) and Rust's save
drops them. A model with an `IfcElementAssembly` is refused by Rust ("unknown variant
`IfcElementAssembly`", and "missing field `generator`").

**Goldens.** `golden/relations/assembly.json` and `assembly.bimble`: `golden/frame2.json` plus one
assembly of three parts and one connection with a realizing element and properties, built by
`ccore/test/relation_test.c` through Commands (`CCORE_WRITE_GOLDEN=1` writes them again).
`ccore_golden --check` skips `relations/` and this file (`C_OWN`): Rust does not produce them.

**Until X2.** No verb makes a relation, the scene draws nothing for an assembly and warns once
("could not mesh"), and the IFC export refuses a model with an assembly (an error, never a silent
drop).

**Where.** `ccore/src/relation.{h,c}` (table, rules, read, write), `ccore/src/model.{h,c}`,
`ccore/src/command.{h,c}`, `ccore/src/file.c`, test `ccore/test/relation_test.c`.
