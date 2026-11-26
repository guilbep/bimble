/* The place tool (ADR 0068 rule 2: interactive tools live in C): what a pointer over the 3D view
 * does while an element kind is being placed. The page forwards pointer moves and clicks (op
 * "tool", calls.h); this file snaps the point, keeps the clicked points and makes the ghost (the
 * element the next click would add, meshed by the scene code). A completed gesture runs one
 * `verb place` through the session's verb path (calls.c), one history step.
 *
 * Ported from the Rust desktop (crates/bimble-viewer):
 *   snap.rs `targets` and `snap`: axis crossings, column centres, element corners (points), then
 *     axes and element edges (lines), within a tolerance; else the point rounded to 10 mm.
 *     Axes: the model's grid axes and those of the visible links (SLAB1), else through the
 *     columns of the storey.
 *   lib.rs `placing_point`: the tolerance is the plan distance of 12 screen pixels; `free`
 *     (Shift in Rust): no snap, rounded to 10 mm.
 *   lib.rs `storey_point`: the pointer ray meets the plane of the storey's elevation.
 *   edit.rs `Editor::click`: a column takes 1 click, a beam, a wall and a rectangle slab 2; the
 *     points are taken when the element completes (also when it fails); a slab gets
 *     top_offset_mm on its z.
 * A C addition: `ortho` (the second point of a 2-point kind along x or y from the first).
 * SLAB1: polygon slabs (edit.rs click / close_polygon) in calls.c, through cc_verb_new_slab. */
#ifndef CCORE_SESSION_TOOL_H
#define CCORE_SESSION_TOOL_H
#include <stddef.h>
#include <stdint.h>
#include "command.h"
#include "json_w.h"
#include "model.h"
#include "scene.h"

/* snap.rs SnapTo, strongest first; CC_SNAP_NONE: rounded to 10 mm. */
enum { CC_SNAP_NONE = -1, CC_SNAP_AXIS_CROSSING, CC_SNAP_COLUMN_CENTRE, CC_SNAP_CORNER, CC_SNAP_AXIS, CC_SNAP_EDGE };

typedef struct {
    double p[2];
    int kind;
} cc_snap_point;
typedef struct {
    double a[2], b[2];
    int kind;
} cc_snap_line;
typedef struct { /* snap.rs Targets: plan, world mm */
    cc_snap_point *points;
    size_t npoints, cap_points;
    cc_snap_line *lines;
    size_t nlines, cap_lines;
} cc_snap_targets;

/* snap.rs `targets`: the snap targets of the storey at `level` (world mm) up to `next`, from the
 * scene `sc` (its elements in that height band). Frees what `t` held first. 0 ok, -1 out of
 * memory (t is then empty). */
int cc_snap_targets_build(cc_snap_targets *t, const cc_scene *sc, double level, double next);
/* SLAB1: the same with the model's grid axes `grid` (n x {x0, y0, x1, y1}, world mm; snap.rs
 * `scene.grids`): those longer than 1 mm are the axes (their crossings, then the lines); none: the
 * axes through the columns, as above. */
int cc_snap_targets_build_grid(cc_snap_targets *t, const cc_scene *sc, double level, double next, const double (*grid)[4],
                               size_t ngrid);
void cc_snap_targets_free(cc_snap_targets *t);
/* snap.rs `snap`: `p` snapped within `tol` mm into `out`; returns the kind (CC_SNAP_NONE: p
 * rounded to 10 mm). */
int cc_snap(const cc_snap_targets *t, const double p[2], double tol, double out[2]);
/* SnapTo::label: "axis crossing", "column centre", "corner", "grid axis", "edge"; NULL for none. */
const char *cc_snap_label(int kind);
/* (v / 10).round() * 10, f64::round (half away from zero), no -0.0. */
double cc_round10(double v);
/* The ortho point: `p` moved onto the x or y line through `a` (the nearer one). */
void cc_ortho(const double a[2], const double p[2], double out[2]);
/* lib.rs storey_point: where the ray (origin xyz, direction xyz; scene coordinates) meets the
 * plane z = `z` (scene coordinates). 0 with the hit's x, y in `out`; -1: parallel or behind. */
int cc_ray_plane(const double ray[6], double z, double out[2]);

#define CC_TOOL_MAX_POINTS 64

/* The tool state of a session (cc_session.tool): the clicked points and the pointer. */
typedef struct cc_tool {
    double points[CC_TOOL_MAX_POINTS][2]; /* world plan mm, clicked so far */
    size_t npoints;
    int has_hover;
    double hover[2];  /* where the next click goes (snapped; ortho applied) */
    int hover_kind;   /* CC_SNAP_* */
    /* Targets cache, per (scene rev, level). */
    cc_snap_targets targets;
    int has_targets;
    uint64_t targets_rev;
    uint64_t targets_links; /* SLAB1: the session's links_gen the targets were built with */
    double targets_level;
    /* The annotate tool (ticket C4, calls.h "annotate"): 0 off, 1 dimension, 2 text. The anchors
     * of the clicked points and of the hover: an element's feature (annot.h) or none. */
    int annot;
    char *annot_text; /* malloc'd, the text tool's text */
    double annot_size;
    struct {
        int has;
        cc_gid el;
        char feat[24];
    } annot_ft[3]; /* points 0, 1; 2: the hover */
} cc_tool;

void cc_tool_free(cc_tool *t);   /* the targets; the state is zeroed */
void cc_tool_clear(cc_tool *t);  /* points and pointer dropped, the annotate tool off (Esc, a new kind, a new model) */

/* The ghost: `cmds` (a dry run of `verb place` on `m`) applied to a scratch model holding `m`'s
 * storeys and materials, meshed by cc_scene_build. Writes the JSON object
 *   {"edges":[x0,y0,z0,x1,y1,z1,…],"idx":[…],"origin":[x,y,z],"pos":[x,y,z,…]}
 * with positions relative to `origin` (the shown scene's origin, world mm) as the BMSH mesh.
 * 0 ok; -1 with the reason in `err` (the commands do not apply, out of memory). */
int cc_tool_ghost(JsonW *w, const cc_model *m, const cc_command *cmds, size_t n, const double origin[3], Buf *err);
#endif
