/* Reference plans: what a PDF or DXF file holds as lines (crates/bimble-drawing/src/reference.rs,
 * ADR 0021), shared by the PDF reader (pdf_in.h, ticket L1) and the DXF reader (ticket L2).
 * L2 (io): no files, no clock; the bytes come from the caller.
 *
 * A read is polylines in mm (x, y pairs; reference.rs `Polyline`) and notes for the user, or an
 * error text. Files are untrusted: the readers bound their sizes, nesting and points
 * (CC_REF_MAX_POINTS, view.rs MAX_REFERENCE_POINTS), and never abort; out of memory is an error. */
#ifndef CCORE_REF_READ_H
#define CCORE_REF_READ_H
#include <stddef.h>
#include "buf.h"

#define CC_REF_MAX_FILE_BYTES ((size_t)256 * 1024 * 1024) /* reference.rs:18 */
#define CC_REF_MAX_DEPTH 8                                  /* reference.rs:19 */
#define CC_REF_CIRCLE_SEGMENTS 64.0                         /* reference.rs:21 */
#define CC_REF_MAX_POINTS ((size_t)2000000)                 /* view.rs MAX_REFERENCE_POINTS */

typedef struct {
    double (*xy)[2];       /* every point, polyline after polyline */
    size_t npts, cappts;
    size_t *start;         /* polyline k is xy[start[k] .. start[k + 1]) (start[nlines] = npts) */
    size_t nlines, caplines;
    int truncated;         /* reference.rs Out::truncated: a polyline passed the point cap */
    Buf notes;             /* the notes, each ended by '\n' */
    int oom;
} cc_refread;

void cc_refread_init(cc_refread *r);
void cc_refread_free(cc_refread *r);
/* reference.rs Out::push: a polyline of n points (fewer than 2 or a non-finite one: dropped; past
 * the point cap: dropped, `truncated` set). 0, or -1 out of memory (oom set). */
int cc_refread_push(cc_refread *r, const double (*p)[2], size_t n);
void cc_refread_note(cc_refread *r, const char *text);

/* reference.rs `read`: by the name's extension (any case) .pdf (pdf_in.h), .dxf (dxf_in.h, ticket
 * L2), .dwg (refused, with how to convert it), else refused. `pdf_scale`: the plan's 1:N.
 * 0 ok (`out` filled; free it with cc_refread_free), -1 with the error text in `err`. */
int cc_ref_read(const char *name, size_t name_len, const unsigned char *bytes, size_t n, double pdf_scale,
                cc_refread *out, Buf *err);
#endif
