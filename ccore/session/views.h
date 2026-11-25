/* The views a session shows and its tabs (ticket C1, the first step of the dynamic 2D view).
 *
 * Ported from the Rust desktop (crates/bimble-viewer, crates/bimble-core/src/view.rs):
 *   view.rs `default_views` and `effective`: the 3D view, then a floor plan per storey (by
 *     elevation), then a ceiling plan per storey; a stored view replaces the default with its id,
 *     the other stored views follow in id order.
 *   view.rs `resolve` and `ResolvedRange::visible`: a plan's range planes in world mm.
 *   viewing.rs `refresh_views` (the active view falls back to the first), `activate_view` (a plan
 *     sets the placing storey; the temporary hide goes), `auto_name`.
 *   ribbon.rs `sync_tabs`, `tab_verb`, `after_tab_clicks`: the open tabs, in order.
 *   views.rs `view_clip`: the box a plan shows (scene coordinates), unlimited sides at the bounds.
 * Deviations:
 *   - a read-only scene (an IFC opened as it is, a .bimbleview) lists the 3D view only (Rust reads
 *     its storeys from the scene; the C read-only scenes keep none).
 *   - "in view" hides stay session-wide (visibility.h): a view's stored visibility is not applied.
 *   - Home: closing the last tab leaves no current tab (`tab_open` 0) until a view is opened.
 *   - only view tabs open: project, materials, schedule and sheet tabs are never open.
 *   - the camera is the page's (glviewport.ts): no camera remembered per view in C. */
#ifndef CCORE_SESSION_VIEWS_H
#define CCORE_SESSION_VIEWS_H
#include <stddef.h>
#include "arena.h"
#include "json_w.h"
#include "model.h"

struct cc_session;

/* The views shown (view.rs effective), built in `a` (stored views are shallow copies of the
 * model's). 0 ok, -1 out of memory. Nothing loaded: none. */
int cc_sv_list(struct cc_session *s, Arena *a, cc_view **out, size_t *n);
/* A model was loaded: its tabs are the 3D view's, current. */
void cc_sv_loaded(struct cc_session *s);
/* refresh_views + sync_tabs: tabs of views gone are dropped, the active view falls back to the
 * first view, the current one has its tab. 0 ok, -1 out of memory. */
int cc_sv_sync(struct cc_session *s);
/* The active view, a copy built in `a`: 1 found, 0 none, -1 out of memory. */
int cc_sv_active(struct cc_session *s, Arena *a, cc_view *out);
/* activate_view (viewing.rs:354) and the tab current: `id` must be a view of cc_sv_list. */
void cc_sv_go(struct cc_session *s, const cc_gid *id);
/* after_tab_clicks with Close::This(view `id`): the tab goes; the current one: the last tab left
 * is shown; none left: Home. `is_view` 0: a tab that is never open (nothing but Home). */
void cc_sv_close(struct cc_session *s, int is_view, const cc_gid *id);
/* The tab is open (or current). */
int cc_sv_tab_open(const struct cc_session *s, const cc_gid *id);

/* view.rs resolve: the plan `v`'s planes, world mm (top, cut, bottom, depth as CC_RANGE_*;
 * +-HUGE_VAL where unlimited), on the model's storeys. 0 ok, -1 not a plan or not valid. */
int cc_sv_resolve(const cc_model *m, const cc_view *v, double out[4]);

/* The ViewSummary of `v` plus auto_name (calls.rs:368-385), keys sorted. */
void cc_sv_summary_json(JsonW *w, const cc_model *m, const cc_view *v);
/* {"id","type"} of a view tab (ribbon.rs:406), keys sorted. */
void cc_sv_tab_json(JsonW *w, const cc_gid *id);
#endif
