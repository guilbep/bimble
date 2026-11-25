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

/* Parses `n` bytes (need not be NUL-terminated). Returns the root, or NULL with `err` filled. */
JrVal *jr_parse(Arena *a, const char *text, size_t n, JrError *err);

/* Whole buffer is UTF-8 as Rust's str::from_utf8 accepts it. Returns -1 if valid, else the
 * offset of the first bad byte. */
long long jr_utf8_check(const char *s, size_t n);

/* Member lookup by key (first match), NULL if absent. */
const JrVal *jr_get(const JrVal *obj, const char *key);
int jr_key_is(const JrMember *m, const char *key);
#endif
