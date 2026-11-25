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
