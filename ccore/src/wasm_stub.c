/* Freestanding wasm32 support for build.sh wasm: the few libc functions src/ uses, a simple
 * allocator on memory.grow, and one exported function that proves the library links. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ccore.h"

__attribute__((no_builtin)) void *memcpy(void *restrict d, const void *restrict s, size_t n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
__attribute__((no_builtin)) void *memmove(void *d, const void *s, size_t n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else while (n--) p[n] = q[n];
    return d;
}
__attribute__((no_builtin)) void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p < *q ? -1 : 1;
    return 0;
}
size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Bump allocator with a size header; free() only returns the last block. Enough for a lab. */
extern unsigned char __heap_base;
static uintptr_t heap_top, heap_end;

void *malloc(size_t n) {
    if (!heap_top) heap_top = heap_end = ((uintptr_t)&__heap_base + 15) & ~(uintptr_t)15;
    if (n > 0x7fff0000u) return NULL;
    size_t need = (n + 16 + 15) & ~(size_t)15;
    while (heap_end - heap_top < need) {
        if (__builtin_wasm_memory_grow(0, (need >> 16) + 1) == (size_t)-1) return NULL;
        heap_end = __builtin_wasm_memory_size(0) << 16;
    }
    size_t *h = (size_t *)heap_top;
    *h = n;
    heap_top += need;
    return (unsigned char *)h + 16;
}
void free(void *p) {
    if (!p) return;
    size_t *h = (size_t *)((unsigned char *)p - 16);
    if ((uintptr_t)h + ((*h + 16 + 15) & ~(size_t)15) == heap_top) heap_top = (uintptr_t)h;
}
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    size_t old = *(size_t *)((unsigned char *)p - 16);
    if (n <= old) return p;
    void *q = malloc(n);
    if (q) {
        memcpy(q, p, old);
        free(p);
    }
    return q;
}

/* Dummy export: formats x as serde_json and as {:?} into a static buffer, returns its length. */
static char out[64];
__attribute__((export_name("ccore_fmt_f64"))) int ccore_fmt_f64(double x, int debug) {
    Buf b;
    buf_init(&b);
    int r = debug ? fmt_f64_debug(&b, x) : fmt_f64_json(&b, x);
    int n = r ? -1 : (int)(b.len < sizeof out ? b.len : sizeof out);
    if (n > 0) memcpy(out, b.p, (size_t)n);
    buf_free(&b);
    return n;
}
__attribute__((export_name("ccore_out"))) char *ccore_out(void) { return out; }
