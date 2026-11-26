/* The View, Sheets and Annotate tabs' tools (ticket VIEWSHEET, ADR 0076): what the session keeps
 * for New view…, Section, Place view, Revision and the Text type / Dimension type boxes.
 *
 * Actions (calls.c ACTIONS, after SETOUT's): new_view, sheet_place_view and sheet_revision open the
 * page's dialog (the C core answers ok, as `project` and `settings`: the dialog then runs the
 * verbs view_add, sheet_place, set_sheet); draw_section is the section tool, a switch (session
 * state, no history step): while it is on, two clicks in a plan make a section (the page sends
 * view_add with the cut through the line's middle, looking to its right, snapped to the four
 * facings the core's sections have). Enabled (calls.c action_enabled): new_view with an editable
 * model; draw_section with an editable model and a plan shown; sheet_place_view and sheet_revision
 * with an editable model and a sheet shown.
 *
 * The annotation types (Rust ribbon.rs:1285 annotation_types, layout.rs `text_kind`,
 * `dim_kind`): the graphic standards' text and dimension types (draw_style.h, the default style:
 * the C core keeps no other), the one new texts and dimensions get (null: "Default"), session
 * state. The annotate tool (calls.c annot_call) gives a new text the type's name (`kind`) and
 * size, a new dimension the type's name.
 *
 * query annotation_types:
 *   {"dimension": name | null, "dimension_types": [{"decimals", "name", "text_mm", "unit"}],
 *    "text": name | null, "text_types": [{"font", "name", "size_mm"}]}
 * action annotation_type {"text": name | null} or {"dimension": name | null}: one of the
 * standards' names (an unknown one is refused), null for Default. */
#ifndef CCORE_SESSION_VIEW_SHEET_H
#define CCORE_SESSION_VIEW_SHEET_H
#include <stddef.h>
#include "buf.h"
#include "draw_style.h"
#include "json_w.h"

#define CC_VS_KIND_MAX 200 /* a type name, bytes (set_sheet's revision limit) */

typedef struct {
    int section_draw; /* the section tool is on */
    char text_kind[CC_VS_KIND_MAX + 1], dim_kind[CC_VS_KIND_MAX + 1]; /* "" = Default */
} cc_viewsheet;

/* 0 new_view, 1 draw_section, 2 sheet_place_view, 3 sheet_revision, -1 another name. */
int cc_vs_index(const char *name);
/* The query's answer, for the standards `st`. */
void cc_vs_types_json(const cc_viewsheet *v, const cc_paper_style *st, JsonW *w);
/* action annotation_type: 0, or -1 with the reason in `out`. */
int cc_vs_set_type(cc_viewsheet *v, const cc_paper_style *st, const char *body, size_t n, Buf *out);
/* The text type named `kind` ("" or unknown: NULL). */
const cc_text_type *cc_vs_text_type(const cc_paper_style *st, const char *kind);
#endif
