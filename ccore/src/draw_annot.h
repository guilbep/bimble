/* A plan's annotations on paper (L2), byte for byte as bimble-drawing's plan.rs `plan()` (ticket
 * D3): grid axes and their bubbles, dimension chains, opening sizes and positions, section labels
 * of columns and beams, section marks, a view's own dimensions and texts, and on whole sheets the
 * scale bar, the level note, the legend box, the frame and the title block.
 *
 * draw_plan.c calls these at its sites, in Rust's order, each behind its template switch. Points
 * in "scene mm" are relative to the scene origin (plan.rs's scene, already turned by its view);
 * paper is mm, y down. Everything an item holds lives in the context's arena. Functions returning
 * int give 0, or -1 out of memory. */
#ifndef CCORE_DRAW_ANNOT_H
#define CCORE_DRAW_ANNOT_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "buf.h"
#include "draw_plan.h"
#include "draw_style.h"
#include "model.h"
#include "paper.h"
#include "scene.h"

/* The items of a sheet being drawn (malloc'd; the caller copies them to the arena). */
typedef struct {
    cc_item *p;
    size_t n, cap;
} cc_item_list;
/* A zeroed item at the end, or NULL out of memory. */
cc_item *cc_item_list_push(cc_item_list *v);

/* What the annotations draw with: the sheet's items, the arena, the style, and the plan's
 * to_paper (paper = (ox + (x - lo[0]) k, oy + (hi[1] - y) k)). */
typedef struct {
    Arena *a;
    cc_item_list *items;
    const cc_style *style;
    const cc_plan_template *tpl;
    uint32_t scale;
    double ox, oy, k, lo[2], hi[2];
} cc_annot_ctx;

void cc_annot_to_paper(const cc_annot_ctx *c, const double p[2], double out[2]);

/* ---- small texts (plan.rs) ---- */

/* letter_tag: A..Z, AA, AB, ... */
void cc_letter_tag(Buf *b, size_t i);
/* cm: whole cm, or one decimal. */
void cc_cm(Buf *b, double mm);
/* section_text: "30/30", or for timber with a material name "GL24h 24x46". */
void cc_section_text(Buf *b, cc_category cat, cc_str material, double w, double h);
/* clusters: positions shared by at least two of vals (within tol), averaged, sorted. `vals` is
 * sorted in place; `out` holds at least n/2 values. Returns how many. */
size_t cc_clusters(double *vals, size_t n, double tol, double *out);

/* ---- grid axes ---- */

/* A grid axis in scene mm. */
typedef struct {
    cc_str tag;
    double a[2], b[2];
} cc_plan_axis;

/* The axes of a plan (plan.rs "grid axes"): the model's grids (as a Rust scene read from their
 * IFC has them: grids by id, each grid's U axes then its V axes, a repeated tag and position
 * once), else with CC_AXES_MODEL_OR_COLUMNS an axis through each line of at least two column
 * centres (`centres`, scene mm, the columns cut), tagged A, B... across x and 1, 2... across y,
 * from blo to bhi. *model_axes: they came from the model. */
int cc_plan_axes(const cc_model *m, const double origin[3], cc_axes mode, const double (*centres)[2], size_t ncentres,
                 const double blo[2], const double bhi[2], Arena *a, cc_plan_axis **out, size_t *n, int *model_axes);
/* Running up the sheet (its bubble on top). */
int cc_plan_axis_vertical(const cc_plan_axis *ax);

/* The axes drawn: their bubbles' centres and tags, and where each crosses the drawing's top or
 * left edge, for the chains. */
typedef struct {
    double (*bubble)[2];
    cc_str *tag;
    size_t nbubbles;
    double *at_top, *at_left;
    size_t ntop, nleft;
} cc_axes_drawn;

/* The axis lines (dash-dot, the axis pen), from the bubble's edge to 3 mm past the far end.
 * chains_v, chains_h: the dimension chains on top and at the left (0, 1 or 2). */
int cc_annot_axis_lines(cc_annot_ctx *c, const cc_plan_axis *axes, size_t n, double chains_v, double chains_h,
                        cc_axes_drawn *out);
/* The bubbles, over everything. */
int cc_annot_bubbles(cc_annot_ctx *c, const cc_axes_drawn *d);

/* ---- dimensions ---- */

/* The dimension chains along the axes (with the outer faces), then overall, top and left; blo,
 * bhi: the drawing's bounds (scene mm). */
int cc_annot_chains(cc_annot_ctx *c, const cc_axes_drawn *d, const double blo[2], const double bhi[2], int any_v,
                    int any_h);
/* An opening of a slab (scene mm, its bounding box): its size inside, its position from the
 * nearest axes. */
int cc_annot_opening(cc_annot_ctx *c, const double a[2], const double b[2], const cc_plan_axis *axes, size_t naxes);

/* ---- section labels ---- */

/* The label of one cut section (cut_label): a rectangle "b/h", a circle "Ø d"; 0 none, 1 with the
 * text appended. */
int cc_cut_label(cc_str cls, cc_category cat, cc_str material, const cc_ring *loops, size_t nloops, Buf *out);
/* The cut label of an element beside what is cut, if it has one. */
int cc_annot_cut_label(cc_annot_ctx *c, cc_str cls, cc_category cat, cc_str material, const cc_ring *loops,
                       size_t nloops);

/* A beam above the cut: its label, the ends of its axis and its half width (scene mm). */
typedef struct {
    cc_str text;
    double a[2], b[2], half;
} cc_beam_label;
/* beam_label of a beam's mesh (scene mm): 1 with *out (text in `a`), 0 none, -1 out of memory. */
int cc_beam_label_of(const cc_mesh *m, cc_category cat, cc_str material, Arena *a, cc_beam_label *out);
/* Its label along it. */
int cc_annot_beam_label(cc_annot_ctx *c, const cc_beam_label *l);

/* ---- section marks ---- */

/* plan::SectionMark: a section's mark on a plan (typedef in draw_plan.h). */
struct cc_section_mark {
    cc_str tag;
    double origin[2]; /* world mm */
    cc_facing facing;
    int true_north;
};
/* plan::section_marks: the marks of the section views among `views`, tagged by the last word of
 * their name when it is short, else A, B, C... in their order. *out in `a`. */
int cc_section_marks(const cc_view *views, size_t n, Arena *a, cc_section_mark **out, size_t *nout);
/* The marks across the drawing (plans only: the caller checks the turn). blo, bhi: the drawing's
 * bounds (scene mm); `so`: the scene origin; plan_turn(true_north) of the model's georeference. */
int cc_annot_section_marks(cc_annot_ctx *c, const cc_section_mark *marks, size_t n, const cc_turn *turn,
                           const double so[3], const cc_georef *geo, const double blo[2], const double bhi[2]);

/* ---- a view's own dimensions and texts ---- */

/* viewdraw::Turn::apply of a scene point (mm, relative to the scene origin). */
void cc_turn_apply_point(const cc_turn *t, const double p[3], double out[3]);
/* viewdraw::dimension_on_paper: the line of a dimension from pa to pb `off` mm to the left (on
 * paper), along `axis`; 0 ok, 1 none (degenerate). */
int cc_dimension_on_paper(const double pa[2], const double pb[2], double off, cc_dim_axis axis, double u[2],
                          double qa[2], double qb[2], double *len);
/* pens::DimType::format. */
void cc_dim_type_format(Buf *b, const cc_dim_type *d, double mm);
/* The view's dimensions (an anchor on a feature resolved to its point first, annot.h) and texts.
 * `m` may be NULL (anchors at their `at`). */
int cc_annot_view(cc_annot_ctx *c, const cc_model *m, const cc_view *v, const cc_turn *turn, const double so[3]);

/* ---- whole sheets ---- */

/* The values of the template's placeholders (template::fill's arguments). */
typedef struct {
    const cc_str (*values)[2];
    size_t nvalues;
    const cc_kv *fields;
    size_t nfields;
} cc_fill_values;

/* The graphic scale bar at the right of the title line (w: the sheet's width). */
int cc_annot_scale_bar(cc_annot_ctx *c, double w, double title_y);
/* The level note above the title block, at the right (nothing when it fills to nothing). */
int cc_annot_level_note(cc_annot_ctx *c, double w, double h, const cc_fill_values *fv);
/* plan::draw_legend: the legend box, its bottom on `bottom`, its left at x0 (draws on c->items
 * with c->a; its swatches as at 1:100). */
int cc_draw_legend(cc_annot_ctx *c, double x0, double bottom, const cc_legend_entry *legend, size_t n);
/* plan::frame_and_title_block for a w x h mm sheet; `version` is the "Bimble <version>" note's. */
int cc_frame_and_title_block(cc_annot_ctx *c, double w, double h, const cc_fill_values *fv, cc_str version);
#endif
