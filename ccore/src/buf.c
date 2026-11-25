#include "buf.h"
#include <stdlib.h>
#include <string.h>

void buf_init(Buf *b) {
    b->p = NULL;
    b->len = b->cap = 0;
    b->oom = 0;
}

void buf_free(Buf *b) {
    free(b->p);
    buf_init(b);
}

void buf_clear(Buf *b) { b->len = 0; }

int buf_reserve(Buf *b, size_t extra) {
    if (b->oom) return -1;
    if (extra <= b->cap - b->len) return 0;
    if (extra > (size_t)-1 / 2 - b->len) {
        b->oom = 1;
        return -1;
    }
    size_t cap = b->cap ? b->cap : 64;
    while (cap - b->len < extra) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) {
        b->oom = 1;
        return -1;
    }
    b->p = p;
    b->cap = cap;
    return 0;
}

void buf_append(Buf *b, const void *src, size_t n) {
    if (n == 0 || buf_reserve(b, n)) return;
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

void buf_putc(Buf *b, char c) {
    if (buf_reserve(b, 1)) return;
    b->p[b->len++] = c;
}

void buf_puts(Buf *b, const char *s) { buf_append(b, s, strlen(s)); }

void buf_fill(Buf *b, char c, size_t n) {
    if (n == 0 || buf_reserve(b, n)) return;
    memset(b->p + b->len, c, n);
    b->len += n;
}

void buf_u64(Buf *b, uint64_t v) {
    char t[20];
    int i = 20;
    do {
        t[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    buf_append(b, t + i, (size_t)(20 - i));
}

void buf_i64(Buf *b, int64_t v) {
    if (v < 0) {
        buf_putc(b, '-');
        buf_u64(b, (uint64_t)0 - (uint64_t)v);
    } else {
        buf_u64(b, (uint64_t)v);
    }
}

const char *buf_cstr(Buf *b) {
    if (buf_reserve(b, 1)) return "";
    b->p[b->len] = '\0';
    return b->p;
}
