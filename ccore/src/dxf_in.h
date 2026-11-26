/* DXF reference plans (ticket L2, ADR 0021): the lines of another office's DXF plan, in mm, as
 * polylines, the port of crates/bimble-drawing/src/reference.rs `read_dxf` / `dxf_entity` /
 * `bulged` / `arc` over the `dxf` crate 0.6.2's text reader (Drawing::load: code pairs, 999
 * comments, sections, $INSUNITS, BLOCKS, ENTITIES with POLYLINE/VERTEX/SEQEND and
 * INSERT/ATTRIB/SEQEND gathered as it does, unknown entity types swallowed, errors inside an
 * entity ending the entity list as its EntityIter does).
 *
 * Drawn, as Rust: LINE (3DLINE), LWPOLYLINE and POLYLINE with bulges, ARC, CIRCLE, ELLIPSE,
 * SPLINE (fit points when 2 or more, else control points), SOLID, INSERT (scale, rotation,
 * rows x columns, the block's base point; nested to depth 8). Model space only. Units from
 * $INSUNITS to mm (none: mm). Every other known entity is counted in `skipped`.
 *
 * C additions (golden/DEVIATIONS.md 14), not in Rust's output: each polyline's layer, the layer
 * table (on / frozen), and the anchors of TEXT and MTEXT (placed as their polylines are).
 *
 * Untrusted input (L2 layer: no files, no globals): the file at most 256 MB (Rust's check), at
 * most CC_DXF_MAX_POINTS points (then a note, as Rust), block nesting at most 8 (Rust's), and a
 * work budget on block copies (Rust has none: a hostile file can make it loop for ever). Binary
 * DXF and DXB are refused with a text (Rust reads them: a deviation). Never aborts; out of
 * memory is an error. */
#ifndef CCORE_DXF_IN_H
#define CCORE_DXF_IN_H
#include <stddef.h>
#include <stdint.h>
#include "buf.h"

#define CC_DXF_MAX_BYTES ((size_t)256 * 1024 * 1024)
#define CC_DXF_MAX_POINTS ((size_t)2000000) /* bimble_core::view::MAX_REFERENCE_POINTS */
#define CC_DXF_MAX_DEPTH 8
#define CC_DXF_MAX_LAYERS 4096
#define CC_DXF_MAX_TEXTS 100000
#define CC_DXF_NO_LAYER 0xffffffffu

typedef struct {
    char *name; /* bytes as in the file (not NUL-terminated inside; a NUL is added) */
    size_t n;
    int on, frozen; /* the LAYER table's: colour >= 0, flag 1; a layer not in the table: on */
    size_t polylines;
} cc_dxf_layer;

typedef struct {
    double at[2];     /* placed, mm */
    double height;    /* mm (scaled by the unit, not by blocks) */
    double angle_deg; /* TEXT's 50 (0 for MTEXT) */
    uint32_t layer;
    char *text; /* the 1 (and 3) values, raw bytes, NUL-terminated */
    size_t n;
} cc_dxf_text;

typedef struct {
    double *xy;      /* all points, x y x y ... */
    size_t npts;     /* points */
    size_t *start;   /* polyline k: points start[k] .. start[k+1] (start[npolylines] == npts) */
    uint32_t *layer; /* per polyline: an index of `layers`, or CC_DXF_NO_LAYER */
    size_t npolylines;
    cc_dxf_layer *layers;
    size_t nlayers;
    cc_dxf_text *texts;
    size_t ntexts;
    const char *unit; /* Rust's unit text: "mm", "m", "none (read as mm)"... */
    double unit_mm;
    size_t skipped;  /* entities not drawn (Rust's count) */
    int truncated;   /* stopped at CC_DXF_MAX_POINTS (Rust's note) */
    int over_budget; /* stopped expanding blocks (a C budget; its own note) */
    size_t caps[4];  /* private: capacities */
} cc_dxf;

/* Reads a DXF file's bytes. 0 ok (`out` filled; free it with cc_dxf_free), -1 with the error in
 * `err` (Rust's texts: "not a readable DXF: <the dxf crate's error>", "out of memory"); `out` is
 * then empty. */
int cc_dxf_read(const uint8_t *bytes, size_t n, cc_dxf *out, Buf *err);
void cc_dxf_free(cc_dxf *d);

/* Rust's notes, each followed by '\n': "DXF units: <unit>", "<k> entities not drawn (text,
 * hatches, 3D, nesting)" when k > 0, "stopped at 2000000 points" when truncated; plus the C
 * budget's "stopped expanding blocks (too many copies)". Returns the number of notes. */
size_t cc_dxf_notes(const cc_dxf *d, Buf *out);

/* reference.rs `read` for a DXF or DWG by its name: a .dwg is refused with Rust's text (0 is
 * never returned for it). 1: the name is a DXF (read it), 0: neither, -1: DWG (text in err). */
int cc_dxf_name_kind(const char *name, size_t n, Buf *err);
#endif
