/* The views a model shows (L1): view.rs `default_views` and `effective`, shared by the session
 * (session/views.c: the view list and tabs) and the sheet drawing (draw_sheet.c: the placed
 * views). In the viewer build too (no drawing code here).
 *   default_views: the 3D view, then a floor plan per storey (by elevation), then a ceiling plan
 *     per storey; ids GlobalId::derived("bimble view 3d"), derived("bimble view plan down|up
 *     <storey id>").
 *   effective: a stored view replaces the default with its id, the other stored views follow in
 *     id order. */
#ifndef CCORE_VIEW_EFFECTIVE_H
#define CCORE_VIEW_EFFECTIVE_H
#include <stddef.h>
#include "arena.h"
#include "model.h"

/* GlobalId::derived of `pre` followed by the storey's id (NULL: none). */
cc_gid cc_view_derived_id(const char *pre, const cc_gid *storey);
/* default_views of `m` (NULL: no storeys) into `out`, which holds 1 + 2 * nstoreys views; names
 * in `a`. 0, or -1 out of memory. */
int cc_views_default(const cc_model *m, Arena *a, cc_view *out);
/* effective: the views of `m` (NULL: the 3D view only), built in `a` (stored views are shallow
 * copies of the model's). 0, or -1 out of memory. */
int cc_views_effective(const cc_model *m, Arena *a, cc_view **out, size_t *n);
#endif
