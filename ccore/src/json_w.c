#include "json_w.h"
#include <string.h>
#include "fmt.h"

void jw_init(JsonW *w, Buf *b, int pretty) {
    memset(w, 0, sizeof *w);
    w->b = b;
    w->pretty = pretty;
}

static void fail(JsonW *w, int e) {
    if (!w->err) w->err = e;
}

static void newline_indent(JsonW *w, int depth) {
    buf_putc(w->b, '\n');
    buf_fill(w->b, ' ', (size_t)depth * 2);
}

/* Separator before an item (a key in an object, a value in an array or at the top). */
static int item(JsonW *w) {
    if (w->err) return 0;
    char k = w->kind[w->depth];
    if (k == '{' ) {
        fail(w, JW_ERR_MISUSE);
        return 0;
    }
    if (k == 0 && w->count[0] > 0) { /* one value at the top */
        fail(w, JW_ERR_MISUSE);
        return 0;
    }
    if (k == '[') {
        if (w->count[w->depth] > 0) buf_putc(w->b, ',');
        if (w->pretty) newline_indent(w, w->depth);
    }
    w->count[w->depth]++;
    return 1;
}

/* Called before every value: after a key, or as an item. */
static int value(JsonW *w) {
    if (w->err) return 0;
    if (w->after_key) {
        w->after_key = 0;
        return 1;
    }
    return item(w);
}

void jw_key_n(JsonW *w, const char *k, size_t n) {
    if (w->err) return;
    if (w->kind[w->depth] != '{' || w->after_key) {
        fail(w, JW_ERR_MISUSE);
        return;
    }
    if (w->count[w->depth] > 0) buf_putc(w->b, ',');
    if (w->pretty) newline_indent(w, w->depth);
    w->count[w->depth]++;
    json_escape(w->b, k, n);
    if (w->pretty) buf_append(w->b, ": ", 2);
    else buf_putc(w->b, ':');
    w->after_key = 1;
}

void jw_key(JsonW *w, const char *k) { jw_key_n(w, k, strlen(k)); }

static void begin(JsonW *w, char k) {
    if (!value(w)) return;
    if (w->depth >= JW_MAX_DEPTH) {
        fail(w, JW_ERR_DEPTH);
        return;
    }
    buf_putc(w->b, k);
    w->depth++;
    w->kind[w->depth] = k;
    w->count[w->depth] = 0;
}

static void end(JsonW *w, char open, char close) {
    if (w->err) return;
    if (w->depth == 0 || w->kind[w->depth] != open || w->after_key) {
        fail(w, JW_ERR_MISUSE);
        return;
    }
    int had = w->count[w->depth] > 0;
    w->depth--;
    if (w->pretty && had) newline_indent(w, w->depth);
    buf_putc(w->b, close);
}

void jw_obj_begin(JsonW *w) { begin(w, '{'); }
void jw_obj_end(JsonW *w) { end(w, '{', '}'); }
void jw_arr_begin(JsonW *w) { begin(w, '['); }
void jw_arr_end(JsonW *w) { end(w, '[', ']'); }

void jw_str_n(JsonW *w, const char *s, size_t n) {
    if (value(w)) json_escape(w->b, s, n);
}
void jw_str(JsonW *w, const char *s) { jw_str_n(w, s, strlen(s)); }

void jw_f64(JsonW *w, double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    if (((u >> 52) & 0x7ff) == 0x7ff) { /* check before writing a separator */
        fail(w, JW_ERR_NONFINITE);
        return;
    }
    if (value(w)) fmt_f64_json(w->b, x);
}
void jw_i64(JsonW *w, int64_t v) {
    if (value(w)) buf_i64(w->b, v);
}
void jw_u64(JsonW *w, uint64_t v) {
    if (value(w)) buf_u64(w->b, v);
}
void jw_bool(JsonW *w, int v) {
    if (value(w)) buf_puts(w->b, v ? "true" : "false");
}
void jw_null(JsonW *w) {
    if (value(w)) buf_append(w->b, "null", 4);
}

int jw_finish(JsonW *w) {
    if (!w->err && (w->depth != 0 || w->after_key || w->count[0] != 1)) fail(w, JW_ERR_MISUSE);
    if (!w->err && w->b->oom) fail(w, JW_ERR_OOM);
    return w->err;
}

void json_escape(Buf *b, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    buf_putc(b, '"');
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char esc = 0;
        if (c == '"') esc = '"';
        else if (c == '\\') esc = '\\';
        else if (c >= 0x20) continue;
        else if (c == '\b') esc = 'b';
        else if (c == '\f') esc = 'f';
        else if (c == '\n') esc = 'n';
        else if (c == '\r') esc = 'r';
        else if (c == '\t') esc = 't';
        buf_append(b, s + start, i - start);
        start = i + 1;
        if (esc) {
            char t[2] = {'\\', esc};
            buf_append(b, t, 2);
        } else {
            char t[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            buf_append(b, t, 6);
        }
    }
    buf_append(b, s + start, n - start);
    buf_putc(b, '"');
}
