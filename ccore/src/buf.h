/* Growable byte buffer. Appends never fail loudly: on out of memory `oom` is set and later
 * appends do nothing, so a writer checks once at the end. No printf. */
#ifndef CCORE_BUF_H
#define CCORE_BUF_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *p;
    size_t len, cap;
    int oom;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_clear(Buf *b); /* len = 0, keeps the memory */
int buf_reserve(Buf *b, size_t extra); /* 0 ok, -1 out of memory */
void buf_append(Buf *b, const void *src, size_t n);
void buf_putc(Buf *b, char c);
void buf_puts(Buf *b, const char *s); /* NUL-terminated */
void buf_fill(Buf *b, char c, size_t n);
void buf_u64(Buf *b, uint64_t v); /* decimal */
void buf_i64(Buf *b, int64_t v);
const char *buf_cstr(Buf *b); /* adds a NUL after len (not counted); "" on out of memory */
#endif
