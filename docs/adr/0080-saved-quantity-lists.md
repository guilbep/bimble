# ADR 0080 — Saved quantity lists: New quantities, Columns, By material

- Status: Accepted, 26 Nov 2026. Delegated by Pierre (owner): "don't wait for me to validate the ADR,
  just do your best, I'll review once it's implemented". To be reviewed on the implementation
  (ticket QTY1, round 5 wave 3).
- Concerns: `ccore/src/view.c` (the schedule's file keys), `ccore/src/quantity.c` (the table),
  `ccore/src/verbs_schedules.c` (three verbs), `ccore/session/schedules.c` (queries), `ccore/session/calls.c`
  (three actions), the lab's Quantities tab, page and two dialogs.
- Reversal cost: medium (file keys; a model without lists keeps its bytes).

## Context

The Quantities tab had six controls bound to nothing. Mission 3 on bimble.ch: "Quantities per element
and material, with lists". Rust v0.19.0 had schedules (ADR 0029): `ScheduleDef` {name, classes,
family, fields, group_by_level, totals}, 10 built-in ones, the command `SetSchedules`, the verb
`set_schedule` (grouping, family, columns), and in its viewer a panel to create, rename, duplicate,
delete a schedule and tick and order its 13 columns, sorting by a column in the view only. Rust had
no filter on a value, no sort kept in the list, no grouping by material, no count, no type, no gross
volume and no property columns. The C core reads and writes Rust's schedules byte for byte
(`golden/schedule/`, `verbs_all.txt`).

## Decisions (each with its alternative)

1. **A list is Rust's `ScheduleDef` with five keys more, each left out when empty**: `properties`,
   `group_by_material`, `summary`, `filter` {column, value}, `sort` {column, descending}; a model
   without lists keeps its bytes, a Rust list keeps its text. *Alternative:* a new model object
   (`quantity_lists`) beside `schedules`: two ways to say one thing, and the built-in lists would not
   be editable as lists.
2. **Three new fields after Rust's 13** (`count`, `type` = the element's PredefinedType, `gross_volume`
   = the net mesh volume plus its openings' prisms, a sleeve its exact cylinder); `volume` stays Rust's
   and is the net volume (the mesh is cut; the page calls it "Net volume"). Rust's error texts still
   name Rust's 13 only. *Alternative:* rename `volume` to `net_volume`: Rust's files and goldens would
   change.
3. **Property columns come after the fields, in their own order** (keys as the model stores them,
   `Pset_WallCommon/IsExternal`). *Alternative:* one mixed list of column ids (fields and keys): the
   `fields` array would no longer be Rust's.
4. **Three C verbs** (`schedule_add`, `schedule_edit`, `schedule_remove`), each one `set_schedules`
   command; `set_schedule` stays Rust's (its schema and texts are goldens). A new list without a name
   is "Schedule N" with Rust's new_schedule columns. *Alternative:* extend `set_schedule`: its
   "unknown field" text and schema are Rust goldens.
5. **New quantities is a dialog** (name, IFC classes by family or typed, grouping, subtotals only);
   its columns are chosen next with Columns. *Alternative:* one large dialog with the columns too:
   slower for the common case, and Columns must exist anyway for the built-in lists.
6. **Openings are not rows of a list**: an `IfcOpeningElement` is not a scene element; the page's
   Openings table lists them (exact volumes) and a host's `gross_volume` adds them back.
   *Alternative:* a list kind for openings: a second table builder for one table that exists.
7. **By material is a saved list preset** (`schedule_add {preset: "by_material"}`): material, level,
   count, area, volume, mass, grouped by material then level, subtotals only, named "By material";
   one click creates it once (one undo step) and opens it. *Alternative:* a separate summary query:
   a second code path, not editable, not saved.
8. **Grouping by material puts the material's line first** (its sums), then its level groups; materials
   by name (bytes), no material last. *Alternative:* the material's sums after its levels, as Rust
   writes a level's subtotal after its rows: the page shows group lines above their rows, so the
   material line above its levels reads the same way.
9. **Filter is one equality on a column's text** (ASCII case ignored), on any field or property,
   shown or not. *Alternative:* a filter language (several conditions, ranges): not needed for the
   first lists; the key is an object so it can grow.
10. **Sort is kept in the list** (one undo step per header click), stable after the default order.
    *Alternative:* Rust's view-only sort: lost on reopen, and the CSV would not follow the table.
11. **The CSV's header line is the table's**: the page sends the headers it shows
    (`schedule_csv {headers}`), so a French table exports French headers; without them the core writes
    Rust's titles (the golden). *Alternative:* English titles in the CSV always: the audit's
    Level / Storey mismatch.
12. **Paging, not virtual scrolling**: 200 rows a page (`schedule_rows {offset, limit}`), the table
    built once per model revision and kept. *Alternative:* virtual scrolling: one long scroll is
    nicer, but measuring row heights and keeping group lines in place is more code for the same
    numbers.
13. **Bar schedule, Assign CFC and CFC catalogue stay "not available yet"** through
    `capabilities.c` (`rebar`, a new `cfc`): tickets RBR4 and QTY2.
14. **One number formatter in the page** (`web-ui/src/format.ts`): the integer part grouped, the
    decimals untouched (an edit field keeps its exact value), and Rust's `format_mm` for read-only
    lengths. *Alternative:* round every shown length to whole mm: an edit field would then write the
    rounded value back on blur.

## Limits

- `gross_volume` assumes the host mesh is cut (the boolean kernel, or the prism special cases); an
  element left uncut counts its openings twice.
- A file with these keys does not open in Rust v0.19.0 (`deny_unknown_fields`), as grids and the
  survey point.
- Response times on the 20,026-element model: see the ticket's report and `ccore/results/ux.md`.
