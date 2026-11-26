/* Storey plans at scale (L2), byte for byte as bimble-drawing's plan.rs `plan()` and the plan
 * template of template.rs. A plan is a horizontal cut over a storey's level, seen from above:
 * what the plane cuts (filled, hatched, outlined with the cut pen), what is seen below it down to
 * the level (its tops filled lighter, low to high, with its edges), and the storey's edges above
 * the cut (dashed). The drawing goes on paper at 1:N with its title.
 *
 * Ported (D2): the checks, the storey setup,
 * the model geometry, the paper (auto paper and "suggest a scale"), the items back to front
 * (projection, seen tops and edges, slab opening crosses and levels, lines above, cut fills then
 * outlines), the title, the legend entries. D3 (draw_annot.c): grid axes and bubbles, dimension
 * chains, opening dimensions, element labels, section marks, the scale bar, level note, legend
 * box, frame and title block of a whole sheet. D4 (draw_view.c): a view's own setup (turned,
 * reflected, laid down), crop and annotations; a storey plan of a turned building in project north.
 *
 * Template strings are UTF-8 cc_str; a template's arrays live in the arena it was made in. */
#ifndef CCORE_DRAW_PLAN_H
#define CCORE_DRAW_PLAN_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "buf.h"
#include "draw_style.h"
#include "model.h"
#include "paper.h"
#include "scene.h"

/* ---- template.rs ---- */

typedef enum { CC_TPL_MM, CC_TPL_CM, CC_TPL_M } cc_tpl_unit;            /* template::DimUnit */
typedef enum { CC_AXES_OFF, CC_AXES_MODEL, CC_AXES_MODEL_OR_COLUMNS } cc_axes; /* template::Axes */

typedef struct {
    cc_str label, value;
    int has_color;
    cc_rgb color;
    double size; /* relative to the text size; 1 by default */
} cc_tb_entry;

typedef struct {
    int has_width;
    double width_mm;
    cc_tb_entry *entries;
    size_t nentries;
} cc_tb_column;

typedef struct {
    cc_str title, level_note;
    cc_tpl_unit dimension_unit;
    cc_str *hidden_classes;
    size_t nhidden;
    int seen_fills;
    cc_axes axes;
    int dimensions, slab_levels, opening_crosses, opening_dimensions, element_labels, section_markers;
    struct {
        int show;
        cc_str title, cut, seen, above;
        cc_kv *families; /* English name -> name; sorted by key bytes */
        size_t nfamilies;
    } legend;
    struct {
        double height_mm;
        cc_tb_column *columns;
        size_t ncolumns;
    } title_block;
    cc_kv *fields; /* sorted by key bytes */
    size_t nfields;
} cc_plan_template;

/* PlanTemplate::default(). 0, or -1 out of memory. */
int cc_plan_template_default(cc_plan_template *t, Arena *a);
/* PlanTemplate::to_json() (pretty, no newline at the end). 0, or -1. */
int cc_plan_template_json(const cc_plan_template *t, Buf *b);
/* PlanTemplate::check: 0, or 1 with the error text appended to `err`. */
int cc_plan_template_check(const cc_plan_template *t, Buf *err);
/* PlanTemplate::hides: the class is among hidden_classes (ASCII case-insensitive). */
int cc_plan_template_hides(const cc_plan_template *t, const char *cls, size_t n);
/* PlanTemplate::with_project: the project's non-blank values among the fields (they win).
 * 0, or -1 out of memory. */
int cc_plan_template_with_project(cc_plan_template *t, const cc_kv *project, size_t n, Arena *a);
/* template::fill: `{key}` replaced from `values` (pairs key, value; first match), then `fields`;
 * a known key without value gives nothing, an unknown one stays as it is. */
void cc_template_fill(Buf *out, const char *text, size_t n, const cc_str (*values)[2], size_t nvalues,
                      const cc_kv *fields, size_t nfields);

/* ---- plan.rs ---- */

/* viewdraw::Turn (the angle is radians; draw_view.h). */
typedef enum { CC_TURN_PLAN, CC_TURN_REFLECT, CC_TURN_SECTION } cc_turn_kind;
typedef struct {
    cc_turn_kind kind;
    double a;     /* Plan, Reflect */
    double d[2];  /* Section */
} cc_turn;

/* viewdraw::ViewFrame: paper = (ox + (x - x0) k, oy + (y1 - y) k), x, y in the turned scene. */
typedef struct {
    cc_turn turn;
    double origin[3], x0, y1, k, ox, oy, cut;
} cc_view_frame;
/* ViewFrame::to_paper of a world point. */
void cc_view_frame_to_paper(const cc_view_frame *f, const double world[3], double out[2]);

/* plan::Swatch of a legend entry. */
typedef enum { CC_SWATCH_CUT, CC_SWATCH_SEEN, CC_SWATCH_ABOVE } cc_swatch_kind;
typedef struct {
    cc_str label;
    cc_swatch_kind kind;
    cc_category cat; /* Cut, Seen */
} cc_legend_entry;

/* plan::PlanInfo. */
typedef struct {
    cc_paper_size paper;
    int portrait;
    size_t cut_elements, seen_elements, projection_lines, above_lines, axes;
    int model_axes;
    size_t hidden_elements;
    cc_legend_entry *legend;
    size_t nlegend;
    cc_view_frame frame;
} cc_plan_info;

/* plan::SectionMark (draw_annot.h). */
typedef struct cc_section_mark cc_section_mark;

/* plan::PlanOptions. `style`, `tpl`, `view` and `section_marks` are borrowed. */
typedef struct {
    cc_str storey;
    double cut_above_mm; /* 1200 */
    uint32_t scale;      /* 100 */
    int has_paper;       /* 0: the smallest paper it fits on */
    cc_paper_size paper;
    const cc_style *style;
    cc_str project, date;
    const cc_plan_template *tpl;
    int bare;
    /* Draw this view (a plan with its view range, a ceiling plan, a section; draw_view.h) instead
     * of `storey`: its setup, crop, own dimensions and texts, and its name in the title. */
    const cc_view *view;
    /* Sections marked on plans (#24): where they cut and which way they look. */
    const cc_section_mark *section_marks;
    size_t nsection_marks;
    /* env!("CARGO_PKG_VERSION") of Rust's plan: the `{bimble}` placeholder and the frame's
     * "Bimble <version>" note. cc_plan_options_init sets the C core's own (CC_IFC_VERSION). */
    cc_str version;
} cc_plan_options;

/* PlanOptions::default() but `style` and `tpl`, which the caller sets. */
void cc_plan_options_init(cc_plan_options *o);

/* plan::STANDARD_SCALES. */
#define CC_NSTANDARD_SCALES 18
extern const uint32_t CC_STANDARD_SCALES[CC_NSTANDARD_SCALES];

/* plan(): the sheet of `o.storey` of the model `m` drawn from its scene `s`. Everything the sheet
 * and the info hold lives in `a`. 0 ok; 1 with Rust's error text appended to `err` (bad scale,
 * unknown storey, nothing to draw, does not fit); -1 out of memory. */
int cc_plan(const cc_scene *s, const cc_model *m, const cc_plan_options *o, Arena *a, cc_paper *sheet,
            cc_plan_info *info, Buf *err);
#endif
