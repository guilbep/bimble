/* Schedules in the session (ticket Q2): the viewer's calls `schedule_list`, `schedule_rows`
 * {name} and `schedule_csv` {name} (crates/bimble-viewer/src/calls.rs:399-490) over the tables
 * of src/quantity.c (Q1).
 *   schedule_list  `json(v.schedule_defs())`: effective(stored) as Vec<ScheduleDef> serializes,
 *                  struct field order (name, classes, family only when set, fields,
 *                  group_by_level, totals). Stored: the open model's schedules; a read-only
 *                  scene stores none (Rust reads a side file next to an IFC: the C core has
 *                  none, so its built-in ones), and without anything open the built-in ones.
 *   schedule_rows  table_json(schedule(scene, def), scene, 50): "no schedule with this name"
 *                  (exact name, as `d.name == a.name`), then "no model open" without a scene.
 *   schedule_csv   {"csv": to_csv(table)}, every row.
 * The table of the last (rev, name) asked is kept: a page that asks rows then CSV, or asks again
 * on a change that is not a rebuild, computes it once. Goldens: golden/schedule/ (Q1, the
 * tables) and golden/schedule/session/ (the list, a changed definition, the CSV object).
 * The viewer-only wasm (CC_VIEW_ONLY) has no schedules: list [] and the other two refused. */
#ifndef CCORE_SESSION_SCHEDULES_H
#define CCORE_SESSION_SCHEDULES_H
#include <stddef.h>
#include "buf.h"
struct cc_session;

/* One of the three queries: CC_OK or CC_ERR (the text in `out`); -1: `name` is not one of them. */
int cc_schedules_query(struct cc_session *s, const char *name, const char *body, size_t n, Buf *out);
/* Frees the kept table (cc_session_free). */
void cc_schedules_cache_free(struct cc_session *s);
#endif
