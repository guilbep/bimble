/* Levels and grids in the model (ADR 0056, ticket G1). A level is a cc_storey; one that is not a
 * storey (cc_storey.not_storey, file key "is_storey": false) is a datum: no element belongs to
 * it. A grid is a model object with its own GlobalId, a name and straight axes in plan (world mm),
 * each axis with its own GlobalId and tag. Layer L1 (model), with the decoder and writer the file
 * and the commands share (as relation.h does for relations). No Rust oracle: the rules and texts
 * are this file's own (ccore/golden/DEVIATIONS.md). */
#ifndef CCORE_GRID_H
#define CCORE_GRID_H
#include <stddef.h>
#include "json_r.h"
#include "json_w.h"
#include "model.h"
#include "view_io.h"

#define CC_GRID_MAX_AXES 1000   /* axes of one grid */
#define CC_GRID_MAX_NAME 200    /* characters of a grid's name */
#define CC_GRID_MAX_TAG 32      /* characters of an axis tag */
#define CC_GRID_MAX_MM 1.0e7    /* |coordinate| of an axis end, mm (as the verbs' MAX_MM) */
#define CC_GRID_MIN_LENGTH 1.0  /* mm: an axis is at least this long */

/* Lookup by id (binary search, grids sorted by id); NULL when absent. */
const cc_grid *cc_model_grid(const cc_model *m, const cc_gid *id);
/* The axis `axis` of grid g (linear), NULL when absent. */
const cc_grid_axis *cc_grid_axis_find(const cc_grid *g, const cc_gid *axis);
/* 1 when `id` is a grid's or an axis's GlobalId in m (other objects must not take it). */
int cc_grid_uses_id(const cc_model *m, const cc_gid *id);

/* Validation of a whole grid table against the model: each grid on its own (name, 1..1000
 * axes, tags unique in the grid, ends finite and within 1e7 mm, length >= 1 mm, axes in at least
 * two directions, which IfcGrid's UAxes and VAxes need), then every grid and axis id unique and
 * not used by another object. `keys`: the map keys the file gave (key == id), or NULL. 0 ok, -1
 * with the text appended to `why`. */
int cc_grids_validate(const cc_model *m, const cc_grid *g, size_t n, const cc_gid *keys, Buf *why);
/* The U axes of the IFC export: 1 when axis k of g runs parallel to axis 0 (the V axes are the
 * others). */
int cc_grid_axis_is_u(const cc_grid *g, size_t k);

/* Deep copy into `a` (strings, axes), every -0.0 made 0.0. 0 ok, -1 out of memory. */
int cc_grid_dup(Arena *a, cc_grid *g);
/* In place (a loaded model): -0.0 becomes 0.0. */
void cc_grid_canonical(cc_grid *g);
/* Field equality (for tests and no-op detection). */
int cc_grid_eq(const cc_grid *a, const cc_grid *b);

/* serde-like decoding of {id, name, axes: [{id, tag, start_mm, end_mm}]} from the JSON or CBOR
 * DOM. `skip`: one member ignored (a command's "op"), or NULL. Unknown members are ignored, a
 * repeated one is an error, more than CC_GRID_MAX_AXES axes are refused before any copy. */
int cc_vd_grid(cc_vd *d, const JrVal *v, const char *skip, cc_grid *out);

/* Writes one grid (JSON when e->j, else CBOR into e->c). `op` (JSON only): written first as
 * "op": op, the command's tag. */
void cc_em_grid(cc_em *e, const cc_grid *g, const char *op);
#endif
