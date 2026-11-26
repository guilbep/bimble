/* The views a model shows (L1): view.rs `default_views` and `effective`, shared by the session
 * (session/views.c: the view list and tabs) and the sheet drawing (draw_sheet.c: the placed
 * views). In the viewer build too (no drawing code here).
 *   default_views: the 3D view, then a floor plan per storey (by elevation), then a ceiling plan
 *     per storey; ids GlobalId::derived("bimble view 3d"), derived("bimble view plan down|up
 *     <storey id>").
 *   effective: a stored view replaces the default with its id, the other stored views follow in
 *     id order.
 * Levels (view.rs Level, ticket S1): what the defaults are made from. A model's are its storeys
 * in id order, keyed by GlobalId (model.rs levels); a read-only scene's are its storeys as read,
 * lowest first, keyed by the file's GlobalId text, or by the name when it has none (scene.rs
 * levels). A plan's `storey` is its level's key. */
#ifndef CCORE_VIEW_EFFECTIVE_H
#define CCORE_VIEW_EFFECTIVE_H
#include <stddef.h>
#include "arena.h"
#include "model.h"

typedef struct { /* view.rs Level */
    cc_str key, name;
    double elevation; /* world mm */
} cc_level;

/* GlobalId::derived of `pre` followed by the storey's id (NULL: none). */
cc_gid cc_view_derived_id(const char *pre, const cc_gid *storey);
/* model.rs levels: the model's storeys (id order), key their GlobalId, in `a` (NULL `m`: none).
 * 0, or -1 out of memory. */
int cc_levels_of_model(const cc_model *m, Arena *a, cc_level **out, size_t *n);
/* default_views of the levels `lv` into `out` (1 + 2 * n views); names and keys in `a`. */
int cc_views_default_lv(const cc_level *lv, size_t n, Arena *a, cc_view *out);
/* effective on the levels `lv` with the stored views of `m` (NULL: none). */
int cc_views_effective_lv(const cc_model *m, const cc_level *lv, size_t nl, Arena *a, cc_view **out, size_t *n);
/* default_views of `m` (NULL: no storeys) into `out`, which holds 1 + 2 * nstoreys views; names
 * in `a`. 0, or -1 out of memory. */
int cc_views_default(const cc_model *m, Arena *a, cc_view *out);
/* effective: the views of `m` (NULL: the 3D view only), built in `a` (stored views are shallow
 * copies of the model's). 0, or -1 out of memory. */
int cc_views_effective(const cc_model *m, Arena *a, cc_view **out, size_t *n);
#endif
