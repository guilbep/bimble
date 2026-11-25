/* IFC export, the C twin of bimble_ifc::export_as (crates/bimble-ifc/src/lib.rs, ifc2x3.rs):
 * the same bytes as Rust for the same model, file name and timestamp, except the version in the
 * header (and in IFC2x3's IfcApplication), which is the C core's own (CC_IFC_VERSION).
 *
 * Entity ids follow model order; relationship GlobalIds are derived from the project id
 * (GlobalId::derived("<project id>/<seed>")), so two exports of one model give the same text.
 * The C model has no openings and no georeference: Rust writes neither for such a model. */
#ifndef CCORE_IFC_OUT_H
#define CCORE_IFC_OUT_H
#include <stdint.h>
#include "buf.h"
#include "model.h"

/* The C core's version in FILE_NAME's originating system ('bimble <version>'), the twin of
 * Rust's CARGO_PKG_VERSION there. Same text as the host's `state.version`. */
#define CC_IFC_VERSION "0.1.0-ccore"

typedef enum {
    CC_IFC4,   /* IFC4 ADD2 TC1: everything kept */
    CC_IFC2X3, /* IFC2x3 TC1 (Coordination View 2.0): no material values, owner history added */
} cc_ifc_schema;

/* Appends the STEP text to `out`. `file_name` and `timestamp` (ISO 8601, e.g.
 * "2026-11-05T10:00:00") go in FILE_NAME as given (escaped). 0 ok; -1 when a value is not
 * finite, an element points to a missing storey or material (a validated model has neither),
 * or out of memory; `out` then holds a partial text. */
int cc_ifc_export(const cc_model *m, const char *file_name, const char *timestamp, cc_ifc_schema schema,
                  Buf *out);

/* bimble_core::date::iso_utc: "YYYY-MM-DDTHH:MM:SS" of seconds since 1970-01-01 UTC.
 * `out` needs 32 bytes; NUL-terminated. */
void cc_iso_utc(int64_t secs, char *out);

/* STEP string literal as Rust's `string`: quotes doubled, backslash doubled, non-ASCII and
 * ASCII control characters as \X2\<UTF-16 hex>\X0\. Invalid UTF-8 bytes count as U+FFFD. */
void cc_step_string(Buf *b, const char *s, size_t n);
/* STEP REAL as Rust's `real`: {:?} text, always a '.', exponent as 'E'. -1 if not finite. */
int cc_step_real(Buf *b, double x);
#endif
