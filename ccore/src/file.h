/* The .bimble file (bimble_core::file). Two forms, as in Rust (ADR 0042):
 * - version 1, the JSON text (to_string / from_str): save writes Rust's exact bytes;
 * - version 2, binary (to_bytes, what Rust's `save` writes): the magic "bimble\0", then gzip of
 *   the model as CBOR. The CBOR is Rust's exact bytes; the gzip stream is not (another deflate),
 *   any gzip reader inflates it.
 * Load reads both (the magic tells them apart), validates like Rust, makes the model canonical. */
#ifndef CCORE_FILE_H
#define CCORE_FILE_H
#include "buf.h"
#include "model.h"

#define CC_FORMAT_VERSION 1
#define CC_BINARY_VERSION 2
#define CC_MAGIC "bimble\0"
#define CC_MAGIC_LEN 7
#define CC_MAX_FILE_BYTES ((size_t)256 << 20) /* Rust allows 8 GiB; the JSON DOM caps lower */
/* Binary: the file, and what it inflates to (a small file can unpack huge: a gzip bomb stops
 * here with an error). The CBOR is read into the same DOM as the JSON, so the same cap. */
#define CC_MAX_BINARY_FILE_BYTES CC_MAX_FILE_BYTES
#define CC_MAX_INFLATED_BYTES CC_MAX_FILE_BYTES

/* Parses, checks format and version, validates (Model::validate), makes canonical (-0.0 to 0.0).
 * Always initializes `m` (free it with cc_model_free either way). 0 ok, -1 with `err` filled:
 * "parse: …" (offset set), "not a bimble file …", "unsupported bimble format version …",
 * "invalid model: …", "unsupported by the C core: …". */
int cc_model_load(cc_model *m, const char *text, size_t n, cc_error *err);
/* (binary: also "parse: gzip: …" for damaged or too large compressed data, "parse: …" for
 * broken CBOR, offsets then in the inflated CBOR.) */

/* Appends file::to_string(m): pretty JSON, 2 spaces, no final newline. 0 ok, -1 on a
 * non-finite value or out of memory (a validated model has neither). */
int cc_model_save(const cc_model *m, Buf *b);

/* Appends file::to_bytes(m): the magic, then gzip of the CBOR. 0 ok, -1 out of memory. */
int cc_model_save_v2(const cc_model *m, Buf *b);

/* The same form with the gzip in stored blocks (no compression; ticket J1, the session's model
 * side file): any reader, Rust's too, loads it; ~6x larger, written and read with a memcpy. */
int cc_model_save_v2_stored(const cc_model *m, Buf *b);

/* Appends the CBOR body of the binary form (what Rust's to_bytes compresses), byte for byte as
 * ciborium writes it. 0 ok, -1 out of memory. */
int cc_model_to_cbor(const cc_model *m, Buf *b);
/* ---- H2a: the history store's objects (repo_write.c, repo.rs write_tree) ----
 * Appends one object's compact JSON, byte for byte serde_json::to_vec of it: storey, material,
 * element, view, sheet, relation, grid number `i` of the model, or the model's project map,
 * schedules list or georef (`i` unused). `refs` (elements only; NULL: arrays in full): each mesh
 * array (positions, then indices) is written compactly and handed to mesh_ref, which stores it and
 * gives its 64-hex hash, written in its place (ADR 0042, file::shared::Refs). 0 ok, -1 (an index
 * out of range, a non-finite value, out of memory, mesh_ref failed). */
enum { CC_OBJ_STOREY, CC_OBJ_MATERIAL, CC_OBJ_ELEMENT, CC_OBJ_VIEW, CC_OBJ_SHEET, CC_OBJ_RELATION, CC_OBJ_GRID,
       CC_OBJ_PROJECT, CC_OBJ_SCHEDULES, CC_OBJ_GEOREF };
typedef struct {
    int (*mesh_ref)(void *ctx, const char *json, size_t n, char hash[65]); /* 0 ok, -1 */
    void *ctx;
} cc_mesh_refs;
int cc_model_object_json(const cc_model *m, int kind, size_t i, const cc_mesh_refs *refs, Buf *out);
/* ---- end H2a ---- */
#endif
