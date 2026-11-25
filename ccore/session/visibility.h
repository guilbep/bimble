/* Visibility and the section box of a session (L5): what the 3D view hides, isolates and cuts.
 * Session state, not model: no history step, no rev change (ADR 0068 rule 2, "session" calls).
 *
 * Ported from the Rust desktop (crates/bimble-viewer):
 *   visibility.rs `Temporary` (hidden or isolated for now), `hidden_by`, `hide_or_isolate`;
 *   lib.rs `hidden_ids`, `isolated_ids` (hidden or isolated in the view), `reveal_hidden`,
 *     `section_on`, `section_box`, `fit_section_box`, `toggle_section_box`, `visible`;
 *   viewing.rs `show_all_in_view`, `hidden_in_view`;
 *   bimble-render ClipBox (`around`: the visible elements' bounds plus a margin, f32);
 *   section.rs `overlay` (six handles, one per face; MIN_SIZE 10 mm).
 * Deviations (the C core has no views in its model, docs/research/c-core-spec.md §1):
 *   - "in view" hides and isolates are kept in the session, not stored in the view as a Command
 *     (Rust's save_visibility makes a history step on a .bimble model). show_all_in_view clears them.
 *   - the box is axis-aligned (yaw 0), as every box Rust makes today.
 *   - the face drag takes pointer rays (op tool: box_press, box_move, box_release): the face
 *     follows the point of its axis nearest to the pointer ray; Rust projects the screen drag on
 *     the axis as seen on screen. Same result for a pointer that stays on the axis. */
#ifndef CCORE_SESSION_VISIBILITY_H
#define CCORE_SESSION_VISIBILITY_H
#include <stddef.h>
#include "buf.h"
#include "json_w.h"
#include "model.h"
#include "scene.h"

typedef struct { /* a sorted set of GlobalIds (Rust's BTreeSet<String>: byte order) */
    cc_gid *v;
    size_t n, cap;
} cc_gidset;

int cc_gidset_has(const cc_gidset *s, const cc_gid *g);
int cc_gidset_add(cc_gidset *s, const cc_gid *g); /* 0 ok, -1 out of memory */
void cc_gidset_remove(cc_gidset *s, const cc_gid *g);

typedef struct {
    cc_gidset hidden, isolated; /* in the view: hidden_ids, isolated_ids */
    int has_temp, temp_isolate; /* temporary: Some(Temporary { isolate, elements }) */
    cc_gidset temp;
    int reveal; /* reveal_hidden */
    int section_on;
    int has_box;
    float min[3], max[3]; /* scene coordinates (mm relative to the scene origin), f32 as ClipBox */
    /* The face drag (a tool, ADR 0068 rule 2): the face held and the box while it moves. */
    int drag_face; /* -1: none */
    double drag_t0;
    float drag_min[3], drag_max[3]; /* the box when the drag started */
    float prev_min[3], prev_max[3]; /* the box the drag shows now (committed on release) */
    /* ---- SEC1 ---- the active view's other lists (families, classes, storeys, materials, links:
     * views.h cc_sv_vis_load): the elements they hide, and how many entries they have. */
    cc_gidset by_view;
    size_t view_lists;
    /* ---- end SEC1 ---- */
} cc_vis;

void cc_vis_init(cc_vis *v);
void cc_vis_free(cc_vis *v);
/* A model loaded or closed (lib.rs:1657-1676): nothing hidden, no box, no drag. */
void cc_vis_reset(cc_vis *v);

/* visibility.rs hidden_by: hidden in the view, outside the view's isolation, or hidden or outside
 * the isolation for now. */
int cc_vis_hidden(const cc_vis *v, const cc_gid *g);
/* viewing.rs hidden_in_view (the parts the C core has: elements). */
int cc_vis_hidden_in_view(const cc_vis *v);

/* The actions; `sel` is the selection (sorted, unique), `n` > 0. visibility.rs hide_or_isolate. */
enum { CC_VIS_HIDE_NOW, CC_VIS_ISOLATE_NOW, CC_VIS_HIDE_IN_VIEW, CC_VIS_ISOLATE_IN_VIEW, CC_VIS_UNHIDE };
int cc_vis_hide_or_isolate(cc_vis *v, int action, const cc_gid *sel, size_t n);
void cc_vis_show_all_in_view(cc_vis *v);
void cc_vis_clear_temporary(cc_vis *v);

/* lib.rs fit_section_box: the box around the visible elements of `sc`, or around `primary` alone
 * (when not NULL). Unchanged when that is empty. */
void cc_vis_fit(cc_vis *v, const cc_scene *sc, const cc_gid *primary);
/* lib.rs toggle_section_box: on/off; the first time on, fitted. */
void cc_vis_toggle(cc_vis *v, const cc_scene *sc, const cc_gid *primary);

/* `query visibility` (calls.rs:442-460), keys sorted. `origin` NULL: null. */
void cc_vis_json(const cc_vis *v, JsonW *w, const double *origin);
/* {"isolate":…} etc.: a C addition for the page, the whole sets (query visibility caps them at
 * 50): {"by_view":[…] (SEC1: hidden by the view's class, family, storey, material lists),"hidden_in_view":[…],"isolated_in_view":[…],"now":null|{"ids":[…],"isolate":b},"reveal":b}. */
void cc_vis_sets_json(const cc_vis *v, JsonW *w);

/* The box tool (op tool, names box_press, box_move, box_release, box_cancel, box_set; calls.h).
 * Answers the box tool's state into `out` (or the error text): 200 / 400 as calls.h. */
int cc_vis_tool(cc_vis *v, const char *name, const char *body, size_t n, Buf *out);
/* 1 if `name` is one of the box tool's calls. */
int cc_vis_tool_name(const char *name);
#endif
