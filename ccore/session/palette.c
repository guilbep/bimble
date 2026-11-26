/* The command palette's filter (ticket Q2): see palette.h. */
#include "palette.h"
#include <stdint.h>
#include <string.h>
#include "arena.h"
#include "calls.h"
#include "json_r.h"

/* ---- UTF-8 and lower case: copies of src/quantity.c's (Q1), which keeps them static ---- */

/* Next code point of s[*i..n); a byte that starts no valid sequence stands for itself. */
static uint32_t utf8_next(const char *s, size_t n, size_t *i, size_t *len) {
    const unsigned char *u = (const unsigned char *)s + *i;
    size_t left = n - *i;
    uint32_t c = u[0];
    size_t k = 1;
    if (c >= 0xC0 && c < 0xE0 && left >= 2 && (u[1] & 0xC0) == 0x80) c = (c & 0x1F) << 6 | (u[1] & 0x3F), k = 2;
    else if (c >= 0xE0 && c < 0xF0 && left >= 3 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80)
        c = (c & 0x0F) << 12 | (uint32_t)(u[1] & 0x3F) << 6 | (u[2] & 0x3F), k = 3;
    else if (c >= 0xF0 && c < 0xF8 && left >= 4 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 &&
             (u[3] & 0xC0) == 0x80)
        c = (c & 0x07) << 18 | (uint32_t)(u[1] & 0x3F) << 12 | (uint32_t)(u[2] & 0x3F) << 6 | (u[3] & 0x3F), k = 4;
    *i += k;
    if (len) *len = k;
    return c;
}

static void utf8_put(Buf *b, uint32_t c) {
    if (c < 0x80) buf_putc(b, (char)c);
    else if (c < 0x800) buf_putc(b, (char)(0xC0 | c >> 6)), buf_putc(b, (char)(0x80 | (c & 0x3F)));
    else if (c < 0x10000)
        buf_putc(b, (char)(0xE0 | c >> 12)), buf_putc(b, (char)(0x80 | (c >> 6 & 0x3F))),
            buf_putc(b, (char)(0x80 | (c & 0x3F)));
    else
        buf_putc(b, (char)(0xF0 | c >> 18)), buf_putc(b, (char)(0x80 | (c >> 12 & 0x3F))),
            buf_putc(b, (char)(0x80 | (c >> 6 & 0x3F))), buf_putc(b, (char)(0x80 | (c & 0x3F)));
}

/* char::to_lowercase for the Latin, Greek and Cyrillic blocks; U+0130 lowers to two code points. */
static uint32_t lower1(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0xC0) return c;
    if (c <= 0xD6 || (c >= 0xD8 && c <= 0xDE)) return c + 32;
    if ((c >= 0x100 && c <= 0x12F) || (c >= 0x132 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c & 1 ? c : c + 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c & 1 ? c + 1 : c;
    if (c == 0x178) return 0xFF;
    if (c == 0x386) return 0x3AC;
    if (c >= 0x388 && c <= 0x38A) return c + 37;
    if (c == 0x38C) return 0x3CC;
    if (c == 0x38E || c == 0x38F) return c + 63;
    if ((c >= 0x391 && c <= 0x3A1) || (c >= 0x3A3 && c <= 0x3AB)) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return c & 1 ? c : c + 1;
    if (c == 0x4C0) return 0x4CF;
    if (c >= 0x4C1 && c <= 0x4CE) return c & 1 ? c + 1 : c;
    if ((c >= 0x1E00 && c <= 0x1E95) || (c >= 0x1EA0 && c <= 0x1EFF)) return c & 1 ? c : c + 1;
    if (c == 0x1E9E) return 0xDF;
    if (c >= 0xFF21 && c <= 0xFF3A) return c + 32;
    return c;
}

/* ribbon.rs:469 fold, on one code point (after to_lowercase). */
static uint32_t accent(uint32_t c) {
    if (c >= 0xE0 && c <= 0xE5) return 'a';
    if (c == 0xE7) return 'c';
    if (c >= 0xE8 && c <= 0xEB) return 'e';
    if (c >= 0xEC && c <= 0xEF) return 'i';
    if (c == 0xF1) return 'n';
    if (c >= 0xF2 && c <= 0xF6) return 'o';
    if (c >= 0xF9 && c <= 0xFC) return 'u';
    if (c == 0xFD || c == 0xFF) return 'y';
    return c;
}

/* fold(s), appended. */
static void fold(Buf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        uint32_t c = utf8_next(s, n, &i, NULL);
        if (c == 0x130) { /* 'İ'.to_lowercase() is "i\u{307}" */
            buf_putc(b, 'i');
            utf8_put(b, 0x307);
            continue;
        }
        utf8_put(b, accent(lower1(c)));
    }
}

/* char::is_whitespace (the White_Space property). */
static int is_ws(uint32_t c) {
    return (c >= 9 && c <= 13) || c == ' ' || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

/* char::is_alphanumeric, roughly (palette.h). */
static int is_alnum(uint32_t c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (c < 0xC0) /* Latin-1: ª ² ³ µ ¹ º ¼ ½ ¾ are letters or numbers, the rest punctuation */
        return c == 0xAA || c == 0xB2 || c == 0xB3 || c == 0xB5 || c == 0xB9 || c == 0xBA || (c >= 0xBC && c <= 0xBE);
    if (c == 0xD7 || c == 0xF7) return 0; /* × ÷ */
    if (is_ws(c) || (c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x3003)) return 0;
    if (c >= 0x2190 && c <= 0x2BFF) return 0; /* arrows, maths, technical, box drawing, shapes */
    return 1;
}

/* str::trim */
static void trim(const char **s, size_t *n) {
    size_t i = 0, start = *n, end = 0;
    while (i < *n) {
        size_t at = i, len;
        uint32_t c = utf8_next(*s, *n, &i, &len);
        if (!is_ws(c)) {
            if (start == *n) start = at;
            end = at + len;
        }
    }
    if (start < end) *s += start, *n = end - start;
    else *n = 0;
}

static int contains(const char *h, size_t hn, const char *nd, size_t nn) {
    if (nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++)
        if (!memcmp(h + i, nd, nn)) return 1;
    return 0;
}

/* Whether a word of `l` (runs of alphanumerics) starts with q[0..qn). */
static int starts_a_word(const char *l, size_t ln, const char *q, size_t qn) {
    size_t i = 0;
    while (i < ln) {
        size_t at = i, len;
        uint32_t c = utf8_next(l, ln, &i, &len);
        if (!is_alnum(c)) continue;
        size_t end = i; /* the word [at, end) */
        while (end < ln) {
            size_t j = end;
            if (!is_alnum(utf8_next(l, ln, &j, NULL))) break;
            end = j;
        }
        if (end - at >= qn && !memcmp(l + at, q, qn)) return 1;
        i = end;
        (void)len;
    }
    return 0;
}

int cc_palette_matches(const char *label, size_t ln, const char *query, size_t qn) {
    trim(&query, &qn);
    Buf l, q;
    buf_init(&l), buf_init(&q);
    fold(&l, label, ln);
    fold(&q, query, qn);
    int r;
    if (l.oom || q.oom) r = -1;
    else if (!q.len || contains(l.p, l.len, q.p, q.len)) r = 1;
    else { /* every word of the query (split_whitespace) starts a word of the label */
        r = 1;
        size_t i = 0;
        while (r && i < q.len) {
            size_t at = i, len;
            uint32_t c = utf8_next(q.p, q.len, &i, &len);
            if (is_ws(c)) continue;
            size_t end = i;
            while (end < q.len) {
                size_t j = end;
                if (is_ws(utf8_next(q.p, q.len, &j, NULL))) break;
                end = j;
            }
            r = starts_a_word(l.p, l.len, q.p + at, end - at);
            i = end;
            (void)len;
        }
    }
    buf_free(&l), buf_free(&q);
    return r;
}

int cc_palette_match(const char *body, size_t n, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    int rc = CC_ERR;
    const JrVal *v = jr_parse(&a, body, n, &je);
    const JrVal *q = v && v->type == JR_OBJ ? jr_get(v, "q") : NULL;
    const JrVal *items = v && v->type == JR_OBJ ? jr_get(v, "items") : NULL;
    if (!v) buf_puts(out, je.msg);
    else if (v->type != JR_OBJ) buf_puts(out, "invalid type: expected struct Args");
    else if (!q) buf_puts(out, "missing field `q`");
    else if (q->type != JR_STR) buf_puts(out, "q: invalid type: expected a string");
    else if (!items) buf_puts(out, "missing field `items`");
    else if (items->type != JR_ARR) buf_puts(out, "items: invalid type: expected a sequence");
    else {
        rc = CC_OK;
        for (size_t i = 0; i < items->n && rc == CC_OK; i++)
            if (items->items[i].type != JR_STR) rc = CC_ERR, buf_puts(out, "items: invalid type: expected a string");
        if (rc == CC_OK) {
            buf_putc(out, '[');
            int first = 1;
            for (size_t i = 0; i < items->n; i++) {
                int m = cc_palette_matches(items->items[i].s, items->items[i].n, q->s, q->n);
                if (m < 0) {
                    rc = CC_ERR;
                    break;
                }
                if (!m) continue;
                if (!first) buf_putc(out, ',');
                first = 0;
                buf_u64(out, i);
            }
            buf_putc(out, ']');
            if (rc != CC_OK || out->oom) buf_clear(out), buf_puts(out, "out of memory"), rc = CC_ERR;
        }
    }
    arena_free(&a);
    return rc;
}
