/* A sheet of paper as a one-page vector PDF (L2), the writer of bimble-drawing's
 * `Sheet::to_pdf()` (sheet.rs, pdf-writer 0.15): the same paper items as the SVG writer
 * (paper.h), so a plan, a view or a sheet gets the same drawing in both formats. Not to be
 * confused with the PDF reader of links (pdf_in.h).
 *
 * Byte for byte with Rust: the page's content stream always (cc_paper_pdf_content); with
 * Helvetica the whole file (no stream is compressed). With Roboto, Rust packs each font file with
 * flate2 (zlib, level 6): the C core packs it with its own deflate (inflate.h, fixed Huffman), so
 * that stream, its /Length and the offsets after it differ; everything else is the same bytes
 * (golden/DEVIATIONS.md section 12).
 *
 * Numbers are Rust's f32 operands as pdf-writer prints them: integers without a point, else the
 * ryu crate's shortest digits, "NaN" / "inf" and plain digits outside 1e-6..1e12. Texts are
 * UTF-8; invalid bytes count as U+FFFD, as in cc_text_width_mm. */
#ifndef CCORE_PDF_OUT_H
#define CCORE_PDF_OUT_H
#include <stddef.h>
#include "buf.h"
#include "paper.h"

#define CC_PDF_ERR_OOM (-1)
/* Roboto text without the shipped font file of that weight (or another file in its place). */
#define CC_PDF_ERR_FONT (-2)
/* A pen of negative or NaN width: pdf-writer's set_line_width asserts (Rust panics). */
#define CC_PDF_ERR_PEN (-3)

typedef struct {
    /* "Bimble <version>" in the information dictionary (env!("CARGO_PKG_VERSION") in Rust). */
    const char *version;
    size_t version_len;
    /* Roboto-Regular.ttf and Roboto-Bold.ttf (Roboto 2.138, as crates/bimble-drawing/fonts/),
     * borrowed. Needed only for a Roboto sheet whose texts use that weight: the files are 350 KB
     * each, so they stay out of the wasm and the carrier hands them over. */
    const unsigned char *roboto[2];
    size_t roboto_len[2];
} cc_pdf_options;

/* The page's content stream (object 4 of Rust's file), appended to `out`. 0, or a CC_PDF_ERR_. */
int cc_paper_pdf_content(const cc_paper *p, Buf *out);

/* Sheet::to_pdf(), appended to `out`. 0, or a CC_PDF_ERR_ (then `out` holds a partial file). */
int cc_paper_pdf(const cc_paper *p, const cc_pdf_options *o, Buf *out);

/* 1 if `data` is the shipped Roboto file of that weight (its size and the checkSumAdjustment of
 * its `head` table), the file whose glyphs and metrics roboto_widths.inc holds. */
int cc_pdf_roboto_is_shipped(const unsigned char *data, size_t n, int bold);
#endif
