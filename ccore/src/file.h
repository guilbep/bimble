/* The .bimble JSON text form, version 1 (bimble_core::file::to_string / from_str). Load
 * validates like Rust and makes the model canonical; save writes Rust's exact bytes. */
#ifndef CCORE_FILE_H
#define CCORE_FILE_H
#include "buf.h"
#include "model.h"

#define CC_FORMAT_VERSION 1
#define CC_MAX_FILE_BYTES ((size_t)256 << 20) /* Rust allows 8 GiB; the JSON DOM caps lower */

/* Parses, checks format and version, validates (Model::validate), makes canonical (-0.0 to 0.0).
 * Always initializes `m` (free it with cc_model_free either way). 0 ok, -1 with `err` filled:
 * "parse: …" (offset set), "not a bimble file …", "unsupported bimble format version …",
 * "invalid model: …", "unsupported by the C core: …". */
int cc_model_load(cc_model *m, const char *text, size_t n, cc_error *err);

/* Appends file::to_string(m): pretty JSON, 2 spaces, no final newline. 0 ok, -1 on a
 * non-finite value or out of memory (a validated model has neither). */
int cc_model_save(const cc_model *m, Buf *b);
#endif
