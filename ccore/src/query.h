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
 * ({name}: dispatch::describe, the schema as compact JSON; 400 "no verb with this name"). */
int cc_query(const cc_model *m, const char *name, const char *args_json, size_t n, Buf *out);
#endif
