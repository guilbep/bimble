/* JSON writer. Keys come out in the order the caller gives them (see the spec, "Canonical
 * JSON"). Commas, colons and the pretty layout of serde_json::to_string_pretty (2 spaces,
 * empty containers as {} and []) are handled here. Misuse sets an error that sticks. */
#ifndef CCORE_JSON_W_H
#define CCORE_JSON_W_H
#include <stdint.h>
#include "buf.h"

#define JW_MAX_DEPTH 128

enum {
    JW_OK = 0,
    JW_ERR_NONFINITE = 1, /* NaN or infinity given to jw_f64 */
    JW_ERR_DEPTH = 2,     /* more than JW_MAX_DEPTH nested containers */
    JW_ERR_MISUSE = 3,    /* key outside an object, value without key, unbalanced end */
    JW_ERR_OOM = 4,
};

typedef struct {
    Buf *b;
    int pretty;
    int depth;
    int after_key;
    int err;
    uint32_t count[JW_MAX_DEPTH + 1]; /* items written at each level */
    char kind[JW_MAX_DEPTH + 1];      /* '{', '[' or 0 at the top */
} JsonW;

void jw_init(JsonW *w, Buf *b, int pretty);
void jw_obj_begin(JsonW *w);
void jw_obj_end(JsonW *w);
void jw_arr_begin(JsonW *w);
void jw_arr_end(JsonW *w);
void jw_key(JsonW *w, const char *k); /* NUL-terminated */
void jw_key_n(JsonW *w, const char *k, size_t n);
void jw_str(JsonW *w, const char *s);
void jw_str_n(JsonW *w, const char *s, size_t n); /* UTF-8 bytes, passed through */
void jw_f64(JsonW *w, double x);
void jw_i64(JsonW *w, int64_t v);
void jw_u64(JsonW *w, uint64_t v);
void jw_bool(JsonW *w, int v);
void jw_null(JsonW *w);
int jw_finish(JsonW *w); /* JW_OK if every container was closed and nothing failed */

/* Appends s as a quoted JSON string, escaped exactly as serde_json. */
void json_escape(Buf *b, const char *s, size_t n);
#endif
