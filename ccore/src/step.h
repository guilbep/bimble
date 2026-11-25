/* ISO 10303-21 (STEP physical file) parser, the C twin of crates/bimble-ifc/src/step.rs.
 * Input is untrusted: nesting is bounded (CC_STEP_MAX_DEPTH), sizes are capped, nothing is
 * executed, and errors carry a line and a byte offset. Like Rust, a malformed entity instance
 * is skipped and counted (`skipped`), not an error; only a bad header fails the parse. */
#ifndef CCORE_STEP_H
#define CCORE_STEP_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "buf.h"
#include "model.h"

#define CC_STEP_MAX_DEPTH 64                       /* step.rs MAX_DEPTH */
/* Larger texts are refused: read.rs MAX_FILE_BYTES (64 GiB; step.rs itself has no cap). Was
 * 1 GiB, which refused the IFC4 export of a large imported model that Rust reads. */
#define CC_STEP_MAX_BYTES ((uint64_t)64 << 30)
#define CC_STEP_CHUNK_BYTES ((size_t)1 << 20)       /* step.rs CHUNK_BYTES: same statement split */

typedef enum {
    CC_SV_NULL,    /* $ (also: a missing argument) */
    CC_SV_DERIVED, /* * */
    CC_SV_INT,
    CC_SV_REAL,
    CC_SV_STR,     /* decoded UTF-8 ('' and \X\, \X2\, \X4\, \S\, \\ escapes); binary "…" as text */
    CC_SV_ENUM,    /* .T. -> "T" (upper case) */
    CC_SV_REF,     /* #12 */
    CC_SV_LIST,
    CC_SV_TYPED    /* IFCLABEL('x'): name + one inner value (a list when it had several) */
} cc_sv_kind;

typedef struct cc_sv cc_sv;
struct cc_sv {
    cc_sv_kind kind;
    union {
        int64_t i;
        double r;
        uint64_t ref;
        struct { const char *p; size_t n; } s;   /* STR and ENUM (NUL-terminated too) */
        struct { cc_sv *items; size_t n; } list;
        struct { const char *name; cc_sv *inner; } typed;
    } u;
};

typedef struct {
    uint64_t id;
    const char *name; /* upper case, NUL-terminated, e.g. "IFCWALL" */
    cc_sv *args;
    size_t nargs;
} cc_step_entity;

typedef struct {
    Arena arena;
    cc_step_entity *ents; /* sorted by id, unique (a later duplicate id wins, as in Rust) */
    size_t n;
    const char **schemas; /* FILE_SCHEMA names, upper case, in Rust's order */
    size_t nschemas;
    size_t skipped;       /* malformed instances skipped */
} cc_step_file;

/* Parses `text`. Always initializes `f` (free with cc_step_free). 0 ok; -1 with `err`:
 * "line N: msg" (Rust's ParseError text), offset set. */
int cc_step_parse(cc_step_file *f, const char *text, size_t n, cc_error *err);
void cc_step_free(cc_step_file *f);

const cc_step_entity *cc_step_get(const cc_step_file *f, uint64_t id);
/* The i-th argument, or a NULL value when absent (Entity::arg). */
const cc_sv *cc_step_arg(const cc_step_entity *e, size_t i);
/* Entity behind a REF value, or NULL (StepFile::deref). */
const cc_step_entity *cc_step_deref(const cc_step_file *f, const cc_sv *v);
/* Index of an entity in f->ents (for side tables). */
size_t cc_step_index(const cc_step_file *f, const cc_step_entity *e);

/* Value accessors as step.rs Value::as_*: 1 and the value, or 0. TYPED values are looked
 * through for f64, i64 and str (not for enum, ref, list). INT counts as f64. */
int cc_sv_as_ref(const cc_sv *v, uint64_t *out);
int cc_sv_as_f64(const cc_sv *v, double *out);
int cc_sv_as_i64(const cc_sv *v, int64_t *out);
int cc_sv_as_str(const cc_sv *v, const char **p, size_t *n);
int cc_sv_as_enum(const cc_sv *v, const char **p);
int cc_sv_as_list(const cc_sv *v, const cc_sv **items, size_t *n);

/* step::decode_step_string on raw bytes (between the quotes, '' already folded). Appends UTF-8. */
void cc_step_decode_string(const char *raw, size_t n, Buf *out);
/* Parses a real token as Rust's str::parse::<f64> (whole token, exact). 0 ok, -1 invalid. */
int cc_step_parse_real(const char *s, size_t n, double *out);
#endif
