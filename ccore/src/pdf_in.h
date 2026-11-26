/* The paths of a vector PDF's first page as reference-plan polylines (ticket L1; Rust:
 * crates/bimble-drawing/src/reference.rs `read_pdf`, over lopdf 0.47). L2 (io): bytes in, no files.
 *
 * Read: the header, the cross-reference data (tables and streams, /Prev chains, PNG predictors),
 * or, when that fails, a table rebuilt by scanning for "N G obj" (as lopdf does); every object,
 * object streams included; FlateDecode streams (zlib over inflate.c). Then the page tree's first
 * page, its content streams joined, its resources (inherited), and the operators
 * m l c v y h re cm q Q Do (form XObjects, nested at most CC_REF_MAX_DEPTH) with the painting
 * operators (S s f F f* B B* b b*) ending a path and n dropping it. Curves are 12 segments, as in
 * Rust. Numbers are read as lopdf reads them: integers as i64, reals as f32 (so a real's
 * coordinates are those of the nearest f32).
 *
 * Untrusted input: the file is at most CC_REF_MAX_FILE_BYTES, each decoded stream too, objects
 * nest at most 100 deep, at most 1,000,000 objects, 64 cross-reference sections, the q stack 64
 * deep, CC_REF_MAX_POINTS points; every loop is bounded; nothing aborts.
 *
 * Deviations from lopdf (ccore/golden/DEVIATIONS.md, section 13): an encrypted PDF is refused
 * with a text (lopdf decrypts one whose user password is empty); filters other than FlateDecode
 * are not decoded (lopdf also reads LZW, ASCII85, ASCIIHex, RunLength); inline image data ends
 * at the first white space + "EI" (lopdf counts the bytes from the image size); error texts of
 * damaged files are lopdf's most common one ("couldn't parse input"), not each of its texts. */
#ifndef CCORE_PDF_IN_H
#define CCORE_PDF_IN_H
#include <stddef.h>
#include "buf.h"
#include "ref_read.h"

/* reference.rs read_pdf: 0 ok, -1 with the error text in `err` ("not a readable PDF: …", "the
 * PDF has no page", the scale's text). `out` is initialised here either way. */
int cc_pdf_read(const unsigned char *bytes, size_t n, double scale, cc_refread *out, Buf *err);
#endif
