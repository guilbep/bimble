/* Read-only model queries: bimble_api::dispatch::query and bimble_api::query, byte for byte
 * (golden/query.txt). Answers are compact JSON with sorted keys, as serde_json::to_value then
 * to_string prints them. Argument errors carry serde's texts after "bad arguments: ". */
#ifndef CCORE_QUERY_H
#define CCORE_QUERY_H
#include <stddef.h>
#include "buf.h"
#include "model.h"

#define CC_QUERY_DEFAULT_LIMIT 1000 /* elements returned when the filter sets no limit */
#define CC_QUERY_MAX_ELEMENTS 10000 /* hard cap, whatever the filter asks */
#define CC_QUERY_MAX_IDS 10000      /* dispatch::MAX_IDS: a longer "ids" array is refused first */

/* Runs query `name` (NUL-terminated) with the arguments `args_json[0..n)` (JSON text; empty or
 * only whitespace means null, which reads as {}). Appends the answer to `out` and returns 200,
 * or appends the error text and returns 400 (unknown query, bad arguments, unknown element,
 * JSON that does not parse). 500 with "out of memory" if an allocation failed.
 * Queries: storeys, materials, elements ({class, storey, limit}), element ({id}), project,
 * views, sheets, schedules (the stored ones, as query.rs lists them). C additions from the verb
 * table (verb_table.h), the viewer's calls of the same names: verbs (dispatch::list) and describe
 * ({name}: dispatch::describe, the schema as compact JSON; 400 "no verb with this name").
 * C additions of ADR 0056: grids ([{axes, id, name}]), and storeys gains is_storey and height_mm
 * in a model with a level that is not a storey or with a grid (else Rust's bytes).
 * C additions of ADR 0045 (georef.h): north ({crs, has_map, map_rotation_deg, plan_turn_project,
 * plan_turn_true, rotation_deg}: project north for the viewport and the view cube) and
 * coordinates ({at: [x, y, z] world mm, system: "survey" | "wgs84" | "utm" | "epsg:<n>"} ->
 * {survey, text, wgs84, world_mm}: the status bar's text of a point; 400 for a system the C core
 * does not convert to). */
int cc_query(const cc_model *m, const char *name, const char *args_json, size_t n, Buf *out);
#endif
