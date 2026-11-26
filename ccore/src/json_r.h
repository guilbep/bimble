/* JSON reader (RFC 8259): text -> DOM in an arena. Untrusted input: depth and size limits,
 * UTF-8 checked, never aborts. Object members keep the document's order and duplicates (the
 * consumer decides: serde rejects a duplicate struct field, a map keeps the last one). */
#ifndef CCORE_JSON_R_H
#define CCORE_JSON_R_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"

#define JR_MAX_DEPTH 64
#define JR_MAX_BYTES ((size_t)256 << 20)
/* The DOM budget, one for both forms of a .bimble (this JSON reader and cbor.c): values (each
 * JrVal, each object member) and bytes (those nodes and the decoded strings), charged as they are
 * decoded and before the memory is taken. The text cap above holds ~10 M values of a typical model
 * (27 bytes of text a value: bench/large_model.py 20000 is 15 MB, 552 k values, a 40 MB DOM);
 * CBOR spends one byte a value, so without this a 19 KB gzip bomb asked for 1.2 GB. */
#define JR_MAX_NODES ((size_t)1 << 24)
#define JR_MAX_DOM_BYTES ((size_t)1 << 30)
#define JR_ERR_NODES "too many values (the limit is 16777216)"
#define JR_ERR_BYTES "decoded data too large (the limit is 1073741824 bytes)"

enum { JR_NULL, JR_BOOL, JR_NUM, JR_STR, JR_ARR, JR_OBJ };

typedef struct JrVal JrVal;
typedef struct JrMember JrMember;

struct JrVal {
    int type;
    int b;           /* JR_BOOL */
    int num_int;     /* JR_NUM: token has no fraction and no exponent */
    int num_neg;     /* JR_NUM: token starts with '-' */
    size_t off;      /* byte offset of the value in the input */
    double num;      /* JR_NUM: correctly rounded (strtod); +-inf when out of range */
    const char *s;   /* JR_STR: decoded UTF-8 (arena, NUL after n); JR_NUM: the token (input) */
    size_t n;        /* JR_STR: byte length; JR_NUM: token length; JR_ARR/JR_OBJ: count */
    JrVal *items;    /* JR_ARR */
    JrMember *members; /* JR_OBJ */
};

struct JrMember {
    const char *key; /* decoded UTF-8, NUL after key_n */
    size_t key_n;
    JrVal val;
};

typedef struct {
    size_t offset;  /* byte offset of the error in the input */
    char msg[128];
} JrError;

typedef struct {
    size_t nodes, bytes; /* taken so far */
} JrBudget;
/* Takes `nodes` values of `each` bytes plus `extra` bytes. 0, or -1 (nothing taken) with *why
 * set to JR_ERR_NODES or JR_ERR_BYTES when that passes a limit. */
int jr_budget_take(JrBudget *b, size_t nodes, size_t each, size_t extra, const char **why);

/* Parses `n` bytes (need not be NUL-terminated). Returns the root, or NULL with `err` filled. */
JrVal *jr_parse(Arena *a, const char *text, size_t n, JrError *err);

/* Whole buffer is UTF-8 as Rust's str::from_utf8 accepts it. Returns -1 if valid, else the
 * offset of the first bad byte. */
long long jr_utf8_check(const char *s, size_t n);

/* Member lookup by key (first match), NULL if absent. */
const JrVal *jr_get(const JrVal *obj, const char *key);
int jr_key_is(const JrMember *m, const char *key);
#endif
