/* CBOR (RFC 8949) for the binary .bimble (version 2, ADR 0042): the subset Rust's ciborium
 * writes for the serde model. Read: CBOR -> the JSON DOM of json_r.h, so one decoder of the
 * model serves both forms. Untrusted input: depth limit, lengths checked against the data
 * before any allocation, UTF-8 checked, never aborts. Write: ciborium's exact bytes (shortest
 * heads, definite lengths, each float in the shortest of f16/f32/f64 that keeps it exactly). */
#ifndef CCORE_CBOR_H
#define CCORE_CBOR_H
#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "json_r.h"

/* Parses one CBOR item filling `n` bytes. Items map to the DOM as: unsigned and negative
 * integers -> JR_NUM with num_int (s = NULL: no token), floats -> JR_NUM (s = NULL), text ->
 * JR_STR, array -> JR_ARR, map with text keys -> JR_OBJ, false/true -> JR_BOOL, null -> JR_NULL.
 * Byte strings, tags, other simple values, indefinite lengths, non-text keys and trailing bytes
 * are errors (ciborium never writes them for a model). Root, or NULL with `err` filled (offset
 * = byte offset in the CBOR). */
JrVal *cbor_parse(Arena *a, const unsigned char *p, size_t n, JrError *err);

/* Encoders, appending to `b` (check b->oom once at the end). */
enum { CB_UINT = 0, CB_NEG = 1, CB_BYTES = 2, CB_TEXT = 3, CB_ARRAY = 4, CB_MAP = 5 };
void cb_head(Buf *b, int major, uint64_t v);
void cb_text(Buf *b, const char *s, size_t n);
void cb_f64(Buf *b, double x);
#endif
