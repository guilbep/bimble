/* A sheet of paper with vector content (L2), byte for byte as bimble-drawing's sheet.rs: mm, y
 * pointing down (like SVG). Items are areas (even-odd loops, fill, hatches, outline), open
 * polylines and texts; `cc_paper_svg` writes `Sheet::to_svg()`. Paper numbers are written with
 * `{:.3}`, the paper size with `{}` (fmt_f64_fixed). Texts are UTF-8. */
#ifndef CCORE_PAPER_H
#define CCORE_PAPER_H
#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "draw_cut.h" /* cc_ring */

/* sheet::Paper: ISO 216 sizes, landscape. */
typedef enum { CC_PAPER_A4, CC_PAPER_A3, CC_PAPER_A2, CC_PAPER_A1, CC_PAPER_A0 } cc_paper_size;
void cc_paper_size_mm(cc_paper_size p, double *w, double *h);
const char *cc_paper_size_name(cc_paper_size p);
/* Paper::parse: "a3", " A3 " ...; -1 when unknown. */
int cc_paper_size_parse(const char *s, size_t n);

typedef struct {
    uint8_t c[3];
} cc_rgb;

/* sheet::Pen. ndash 0: continuous. */
typedef struct {
    double width_mm;
    cc_rgb color;
    const double *dash;
    size_t ndash;
} cc_pen;

/* sheet::Hatch: lines at angle_deg (counter-clockwise on paper), spacing_mm apart, shifted. */
typedef struct {
    double angle_deg, spacing_mm, offset_mm;
    cc_pen pen;
} cc_hatch;

/* DXF1 (dxf_out.h, ADR 0082): what an item draws, so a writer that sorts by layer can name it.
 * The SVG and PDF writers never read it (their bytes do not change). A zeroed tag is the sheet's
 * own drawing (frame, title block, legend, scale bar, a drawing's title). */
typedef enum {
    CC_TAG_PAPER,     /* the sheet's: frame, title block, legend, scale bar, title */
    CC_TAG_CUT,       /* an element's outline cut by the plane */
    CC_TAG_VIEW,      /* an element seen beyond the cut (edges, the top it shows) */
    CC_TAG_ABOVE,     /* an element above the cut, dashed */
    CC_TAG_HATCH,     /* the fill and hatch of a cut element */
    CC_TAG_GRID,      /* grid axes and their bubbles */
    CC_TAG_DIMENSION, /* dimension lines and their values */
    CC_TAG_TEXT,      /* labels and notes */
    CC_TAG_SYMBOL     /* level marks, section marks, opening crosses */
} cc_tag_role;

typedef struct {
    const char *cls; /* the element's IFC class ("IfcWall"), not NUL-terminated; NULL: none */
    size_t cls_n;
    uint32_t elem1; /* the element's model_index + 1; 0: none */
    uint8_t role;   /* cc_tag_role */
    int8_t cat;     /* its material family (draw_style.h cc_category) + 1; 0: none */
} cc_item_tag;

typedef enum { CC_ITEM_AREA, CC_ITEM_LINES, CC_ITEM_TEXT } cc_item_kind;
typedef enum { CC_ANCHOR_START, CC_ANCHOR_MIDDLE, CC_ANCHOR_END } cc_text_anchor;
typedef enum { CC_FONT_ROBOTO, CC_FONT_HELVETICA } cc_sheet_font;

typedef struct {
    cc_item_kind kind;
    /* Area: loops; Lines: the polylines. */
    const cc_ring *rings;
    size_t nrings;
    int has_fill; /* Area */
    cc_rgb fill;
    const cc_hatch *hatches; /* Area */
    size_t nhatches;
    int has_pen; /* Area: outline (optional); Lines: always */
    cc_pen pen;
    /* Text */
    double pos[2], size_mm, angle_deg;
    const char *text;
    size_t text_len;
    cc_rgb color;
    cc_text_anchor anchor;
    int bold;
    /* DXF1: what it draws; Lines may tag each ring (the projection and above lines gather many
     * elements in one item), else NULL. */
    cc_item_tag tag;
    const cc_item_tag *ring_tags;
} cc_item;

typedef struct {
    double width_mm, height_mm;
    const char *title;
    size_t title_len;
    const cc_item *items;
    size_t nitems;
    cc_sheet_font font;
} cc_paper;

/* sheet::text_width_mm: Roboto's advances (summed in f32, in order), Helvetica's AFM widths
 * (bold 6 % wider). Roboto knows U+0020-U+024F and some punctuation (roboto_widths.inc); any
 * other character counts as Roboto's missing glyph. */
double cc_text_width_mm(const char *text, size_t n, double size_mm, int bold, cc_sheet_font font);

/* Roboto's glyph id and advance (font units, 2048 per em) of a code point; 0 if not in the table
 * (the missing glyph). */
int cc_roboto_glyph(uint32_t cp, int bold, uint16_t *gid, uint16_t *units);

/* sheet::hatch_lines: the segments covering the loops' bounding box (to be clipped by them).
 * *out is malloc'd (free it); 0 ok, -1 out of memory. */
int cc_hatch_lines(const cc_ring *loops, size_t nloops, const cc_hatch *h, double (**out)[2][2], size_t *n);

/* Sheet::to_svg, appended to `b`. 0 ok, -1 out of memory or a non-finite number. */
int cc_paper_svg(const cc_paper *p, Buf *b);
#endif
