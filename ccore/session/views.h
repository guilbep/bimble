/* The views a session shows and its tabs (ticket C1, the first step of the dynamic 2D view).
 *
 * Ported from the Rust desktop (crates/bimble-viewer, crates/bimble-core/src/view.rs):
 *   view.rs `default_views` and `effective` (src/view_effective.c, shared with the sheet
 *     drawing): the 3D view, then a floor plan per storey (by elevation), then a ceiling plan per
 *     storey; a stored view replaces the default with its id, the other stored views follow in id
 *     order.
 *   view.rs `resolve` and `ResolvedRange::visible`: a plan's range planes in world mm.
 *   viewing.rs `refresh_views` (the active view falls back to the first), `activate_view` (a plan
 *     sets the placing storey; the temporary hide goes), `auto_name`.
 *   ribbon.rs `sync_tabs`, `tab_verb`, `after_tab_clicks`: the open tabs, in order.
 *   views.rs `view_clip`: the box a plan shows (scene coordinates), unlimited sides at the bounds.
 *   viewing.rs `levels` (ticket S1): the views are made on the levels of what is open: the
 *     model's storeys, or a read-only scene's (an IFC opened as it is, a .bimbleview) as read
 *     (scene.rs levels: key the file's GlobalId text, the name when there is none). A read-only
 *     scene's views are the defaults only (derived, never stored: no side views file in C).
 * Deviations:
 *   - (SEC1) "in view" hides are the active view's, as Rust (viewing.rs apply_visibility,
 *     save_visibility): cc_sv_vis_load reads them on a view change and after every edit, undo and
 *     redo; hide_in_view, isolate_in_view, unhide and show_all_in_view store them in the view
 *     (verb view_edit, one undo step). A read-only scene keeps them session-wide (no side views
 *     file in C).
 *   - Home: closing the last tab leaves no current tab (`tab_open` 0) until a view is opened.
 *   - only view tabs open: project, materials, schedule and sheet tabs are never open.
 *   - the camera is the page's (glviewport.ts): no camera remembered per view in C. */
#ifndef CCORE_SESSION_VIEWS_H
#define CCORE_SESSION_VIEWS_H
#include <stddef.h>
#include "arena.h"
#include "json_w.h"
#include "model.h"
#include "view_effective.h"

struct cc_session;

/* The levels of what is open (viewing.rs levels; view_effective.h), in `a`: the model's storeys
 * or the read-only scene's; none when nothing is loaded. 0 ok, -1 out of memory. */
int cc_sv_levels(const struct cc_session *s, Arena *a, cc_level **out, size_t *n);
/* scene.rs levels of a read-only scene: `kind` CC_VIEW_IFC (`scene` a cc_ifc_scene) or
 * CC_VIEW_FILE (a cc_viewfile); others: none. Keys and names borrow from the scene. */
int cc_sv_scene_levels(int kind, const void *scene, Arena *a, cc_level **out, size_t *n);
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
/* The same on the session's levels (an editable model or a read-only scene, ticket S1). */
int cc_sv_plan_range(const struct cc_session *s, const cc_view *v, double out[4]);
/* The elevation (world mm) of the level `key` as resolve finds it: 1 found, 0 none. */
int cc_sv_level(const struct cc_session *s, cc_str key, double *elevation);

/* The ViewSummary of `v` plus auto_name (calls.rs:368-385) on the levels `lv`, keys sorted. */
void cc_sv_summary_json(JsonW *w, const cc_level *lv, size_t nl, const cc_view *v);
/* S1, the query grid_axes (a C addition; G2 draws the axes): null when nothing is loaded, else
 *   {"axes": [{"end_mm":[x,y],"start_mm":[x,y],"tag"}], "levels": [{"elevation_mm","key","name"}],
 *    "links": [{"axes","levels","name","visible"}]}
 * the grid axes (plan, world mm) and levels of what is open, then of each linked model in link
 * order: a read-only scene's as read (file order; levels lowest first, key as cc_sv_levels), a
 * model's from its grids (id order, axes in order) and storeys (id order). */
void cc_sv_grid_axes_json(struct cc_session *s, JsonW *w);
/* ---- SEC1 ---- viewing.rs apply_visibility: the session's "in view" sets (visibility.h hidden,
 * isolated) become the active view's hidden_elements and isolated_elements, by_view the elements
 * its class, family, storey and material lists hide. An editable model only. 0, -1 out of memory. */
int cc_sv_vis_load(struct cc_session *s);
/* ---- end SEC1 ---- */
/* {"id","type"} of a view tab (ribbon.rs:406), keys sorted. */
void cc_sv_tab_json(JsonW *w, const cc_gid *id);
#endif
