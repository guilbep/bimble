/* Sheets on paper (L2), byte for byte as bimble-drawing's sheetdraw.rs `draw` (ticket D5): the
 * frame and the template's title block once, each placed view drawn bare at its own scale (ADR
 * 0046) centred where it is placed, the note of a view that cannot be drawn, the sheet's own
 * texts, and one legend for all its views.
 *
 * The placed views are looked up among the model's effective views (view_effective.h), so a sheet
 * placing a default view (the 3D view, a storey's plan) finds it; a 3D view has no paper drawing
 * and shows as a note ("3D: view 3D: a 3D view has no paper drawing"). */
#ifndef CCORE_DRAW_SHEET_H
#define CCORE_DRAW_SHEET_H
#include <stddef.h>
#include "arena.h"
#include "buf.h"
#include "draw_plan.h"
#include "draw_style.h"
#include "model.h"
#include "paper.h"
#include "scene.h"

/* sheetdraw::SheetOptions. `style` and `tpl` are borrowed. `version`: env!("CARGO_PKG_VERSION")
 * of Rust's sheetdraw, the `{bimble}` value and the frame's "Bimble <version>" note (as
 * cc_plan_options.version). */
typedef struct {
    const cc_style *style;
    const cc_plan_template *tpl;
    cc_str project, date, version;
} cc_sheet_options;

/* sheetdraw::PlacedInfo: a placed view on paper. */
typedef struct {
    cc_gid id, view;      /* the Placed entry and its view */
    double rect[4];       /* its drawing's box on the paper, mm: x0, y0, x1, y1 */
    cc_view_frame frame;  /* its frame, shifted to where it is placed */
    double (*segments)[2][2]; /* its lines on paper, for snapping */
    size_t nsegments;
} cc_placed_info;

/* sheetdraw::SheetInfo. */
typedef struct {
    size_t views_drawn;
    cc_str *problems; /* views that could not be drawn, with why (a note on the sheet) */
    size_t nproblems;
    cc_placed_info *placed;
    size_t nplaced;
} cc_sheet_info;

/* sheetdraw::draw of sheet `def` of model `m` (its scene `s`). Everything the paper and the info
 * hold lives in `a`. 0 ok; 1 with Rust's error text appended to `err` (Sheet::validate, "unknown
 * paper"); -1 out of memory. */
int cc_sheet_draw(const cc_scene *s, const cc_model *m, const cc_sheet *def, const cc_sheet_options *o, Arena *a,
                  cc_paper *out, cc_sheet_info *info, Buf *err);

/* sheetdraw::scales: the scales of the views placed on `def` (among `views`), each once,
 * ascending, into `out` (room for def->nplaced). Returns how many. */
size_t cc_sheet_scales(const cc_sheet *def, const cc_view *views, size_t nviews, uint32_t *out);
#endif
