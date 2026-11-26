/* Any 2D view on paper (L2), byte for byte as bimble-drawing's viewdraw.rs (ticket D4): the scene
 * turned so the view looks straight down, then drawn by draw_plan.c like a storey plan.
 *
 *   - a plan looking down is the scene as it is (turned to project north when the building is
 *     turned in its site, ADR 0045);
 *   - a ceiling plan looking up is reflected (z -> -z): seen from below with north up;
 *   - a section looking north, east, south or west is turned so the look direction is down, x
 *     along the section to the right and y up.
 * The heights the plan drawing works with (level, cut, next) are those of the turned scene. What a
 * view hides (its visibility lists) and what lies wholly outside its heights are left out before
 * the scene is copied and turned (ADR 0046).
 *
 * The range of a plan view is view_range.h (shared with the screen); the crop is draw_cut.h. A
 * turned scene owns its meshes (its own arena): free a setup with cc_view_setup_free. */
#ifndef CCORE_DRAW_VIEW_H
#define CCORE_DRAW_VIEW_H
#include <stddef.h>
#include "buf.h"
#include "draw_annot.h" /* cc_plan_axis, cc_turn_apply_point */
#include "draw_cut.h"  /* cc_crop_rect */
#include "draw_plan.h"  /* cc_turn, cc_view_frame */
#include "model.h"
#include "scene.h"

/* viewdraw::ViewSetup: a view (or a storey plan) ready for the plan drawing. */
typedef struct {
    const cc_scene *scene; /* the scene as seen from above: the input, or `own` */
    cc_scene own;          /* a turned copy (mapped = 1) */
    int mapped;
    int grids; /* the copy keeps the grid axes (plans), turned with it */
    /* Lowest height seen, the cut plane, the highest height drawn dashed above the cut; turned
     * scene mm. */
    double level, cut, next;
    /* The storey whose elements are drawn dashed above the cut (plans looking down). */
    int has_storey;
    cc_str storey;
    /* For the texts: storey name and level (world mm). */
    cc_str storey_name;
    double storey_elevation_mm;
    int plan_down; /* plans looking down: slab levels, beam labels */
    int axes;      /* plans (down or up): grid axes */
    cc_turn turn;
} cc_view_setup;

/* viewdraw::Turn::of: how a view turns the scene (3D views: Plan). */
void cc_turn_of(const cc_view *v, const cc_georef *g, cc_turn *out);
/* Turn::invert: the scene point (relative to its origin) that cc_turn_apply_point turns into q. */
void cc_turn_invert(const cc_turn *t, const double q[3], double out[3]);
/* ViewFrame::to_world: the world point (on the cut plane) drawn at `paper`. */
void cc_view_frame_to_world(const cc_view_frame *f, const double paper[2], double out[3]);
/* ViewFrame::shifted: the frame moved by d on paper. */
cc_view_frame cc_view_frame_shifted(cc_view_frame f, const double d[2]);
/* View::facing_dir: the direction (world x, y) a section facing `f` looks in. */
void cc_view_facing_dir(const cc_view *v, cc_facing f, const cc_georef *g, double out[2]);

/* A view's name is blank (str::trim().is_empty()): its drawing keeps the template's title. */
int cc_view_name_blank(cc_str name);

/* viewdraw::within_heights: any of scene element k lies between lo and hi (world mm, infinite
 * for an unlimited side), a millimetre to spare; an empty element counts as within. */
int cc_view_within_heights(const cc_scene *s, size_t k, double lo, double hi);

/* viewdraw::map_scene: the elements of `s` that `keep` keeps, their world meshes turned by `turn`
 * (positions, edges and edge normals; triangles flipped when `reflect`), into su->own. */
typedef int (*cc_view_keep)(const void *ctx, const cc_scene *s, size_t k);
int cc_view_map_scene(const cc_scene *s, cc_view_keep keep, const void *ctx, const cc_turn *turn, int reflect,
                      cc_scene *out);

/* viewdraw::setup: turn `s` (the scene of `m`) for `v`. 0 ok; 1 with Rust's error text appended to
 * `err` (a 3D view, an unknown storey, a range not valid); -1 out of memory. Free *out with
 * cc_view_setup_free whatever the result. */
int cc_view_prepare(const cc_scene *s, const cc_model *m, const cc_view *v, cc_view_setup *out, Buf *err);
void cc_view_setup_free(cc_view_setup *su);
/* plan::storey_setup of a building turned in its site: only what lies between lo and hi (world
 * mm) copied, turned by su->turn (set by the caller), into su->own. 0, or -1 out of memory. */
int cc_view_storey_turned(const cc_scene *s, double lo, double hi, cc_view_setup *su);

/* The grid axes of a mapped setup turned as map_scene turns the scene's grids (scene mm in and out:
 * ((turn of a) + origin) - origin); nothing when the setup keeps the scene or drops the grids. */
void cc_view_turn_axes(const cc_view_setup *su, cc_plan_axis *axes, size_t n);

/* The crop rectangle of `v` in the turned scene (the crop is in the view's plane, world mm: less
 * the turned origin). 1 with *out, 0 the view has no crop. */
int cc_view_crop_rect(const cc_view *v, const cc_turn *turn, const double origin[3], cc_crop_rect *out);
#endif
