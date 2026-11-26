/* DXF export of a drawing (L2, ticket DXF1, ADR 0082): the items the paper pipeline draws for SVG
 * and PDF (paper.h, draw_plan.c, draw_sheet.c) written as ASCII DXF, version AC1015 (AutoCAD
 * 2000). A C addition: Rust v0.19.0 read DXF (reference.rs) but never wrote one.
 *
 * Two kinds of file:
 *   - a view (sheet = 0): model space in real millimetres at 1:1, the view's own coordinates
 *     (a plan: world X, Y; a section: along the section, the height above the project zero),
 *     taken back from paper through the drawing's frame (draw_plan.h cc_view_frame) and rounded
 *     to 0.001 mm (the meshes are f32: the digits beyond are noise). Texts, dimension offsets,
 *     hatch spacings are the paper's times the scale; line types in paper mm with $LTSCALE = N.
 *     The sheet's own items (tag CC_TAG_PAPER: frame, title block, legend, the title) are left out.
 *   - a sheet (sheet = 1): everything as on paper, in paper mm, y up.
 *
 * File: HEADER ($ACADVER, $DWGCODEPAGE ANSI_1252, $INSBASE, $EXTMIN, $EXTMAX, $LTSCALE,
 * $INSUNITS 4 = mm, $MEASUREMENT 1 = metric, $HANDSEED), an empty CLASSES, TABLES (VPORT
 * *ACTIVE, LTYPE ByBlock / ByLayer / Continuous and the dashed types used, LAYER "0" and one per
 * class and role, STYLE Standard on arial.ttf, VIEW, UCS, APPID ACAD, DIMSTYLE Standard,
 * BLOCK_RECORD *Model_Space / *Paper_Space), BLOCKS (the two spaces), ENTITIES (model space),
 * OBJECTS (the root dictionary, ACAD_GROUP, ACAD_LAYOUT with Model and Layout1, ACAD_PLOTSTYLENAME
 * with Normal). Handles count from 1 in that order, then the entities.
 *
 * Layers (paper.h cc_item_tag): "<IfcClass>-cut", "-view", "-above"; "Hatch-<family>" (cut
 * fills and hatches), "Hatch-<family>-view" (the fill of a top seen below the cut); "IfcGrid";
 * "Annotation-dimension", "Annotation-text", "Annotation-symbol"; "Sheet" (a sheet's own). A
 * linked model's element: its link's name and "-" first ("Structure.ifc-IfcWall-cut"). Names
 * keep letters, digits and "-_.$ "; the characters AutoCAD refuses become "_". Sorted by bytes.
 * Each layer takes the colour (ACI nearest to the RGB, and the RGB as true colour, group 420),
 * the lineweight (group 370, the nearest standard weight to the pen's paper width) and the line
 * type of the first entity drawn on it; an entity that differs writes its own.
 *
 * Entities: a two-point line LINE; a polyline LWPOLYLINE (closed: flag 1); a ring of 16 points or
 * more on one circle (grid bubbles) CIRCLE; an area's fill a solid HATCH (the paper's white is
 * left out), each of its hatches a user-defined HATCH (angle, spacing, shift), its outline closed
 * LWPOLYLINEs; a text TEXT (left, centre or right aligned, its angle). Dimensions are the drawn
 * lines and value text (no DIMENSION entity). Texts: non-ASCII as \U+XXXX, control characters
 * dropped. Numbers: the shortest text that reads back the same double (fmt.h), never "-0".
 *
 * Streamed: the bytes go to `sink` in chunks of at most CC_DXF_CHUNK; the writer keeps only its
 * layers and line types (two passes over the items: one to name the layers and count, one to
 * write). Deterministic: same items, same bytes. */
#ifndef CCORE_DXF_OUT_H
#define CCORE_DXF_OUT_H
#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "draw_plan.h" /* cc_view_frame */
#include "paper.h"

#define CC_DXF_CHUNK 65536

enum { CC_DXF_OK = 0, CC_DXF_OOM = -1, CC_DXF_SINK = -2, CC_DXF_NONFINITE = -3 };

/* 0 ok, else the write failed (the writer stops). */
typedef int (*cc_dxf_sink)(void *ctx, const char *p, size_t n);

typedef struct {
    int sheet;            /* 0: a view (model space at 1:1 through `frame`), 1: a sheet in paper mm */
    cc_view_frame frame;  /* a view: its drawing's frame (cc_plan_info.frame) */
    uint32_t scale;       /* a view's 1:N ($LTSCALE); a sheet: ignored (1) */
    /* A linked model's name for an element (model_index), or NULL (the host's): the layer prefix. */
    const char *(*link_name)(const void *ctx, uint32_t model_index, size_t *n);
    const void *link_ctx;
    cc_dxf_sink sink;
    void *sink_ctx;
} cc_dxf_options;

/* Writes the drawing `p` as DXF to o->sink. CC_DXF_OK, or CC_DXF_OOM, CC_DXF_SINK (the sink
 * failed), CC_DXF_NONFINITE (a number is NaN or infinite). */
int cc_dxf_write(const cc_paper *p, const cc_dxf_options *o);
/* The same into `out` (o->sink and o->sink_ctx are ignored). */
int cc_dxf_write_buf(const cc_paper *p, const cc_dxf_options *o, Buf *out);
#endif
