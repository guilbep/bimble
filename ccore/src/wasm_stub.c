/* Freestanding wasm32 support for build.sh wasm: the few libc functions src/ uses and an
 * allocator on memory.grow. The exported API is in wasm_api.c.
 *
 * Allocator: segregated free lists of size classes. Block sizes (16-byte header included) are
 * 32, 48, then 4 steps per power of two: 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, …
 * (at most 25 % waste), up to 2^31. malloc pops the class's free list, or carves a new block
 * from the top of the heap (growing memory by whole 64 KiB pages as needed); free pushes the
 * block back on its class's list. Blocks are never split or merged: a workload that repeats
 * (open a model, close it, again) reuses the same blocks and memory stops growing after the
 * first round (test/wasm_run.mjs measures it). Single-threaded, like the wasm module. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- allocator ---- */

#define HDR 16u
#define NCLASS 104
extern unsigned char __heap_base;
static uintptr_t heap_base, heap_top, heap_end;
static size_t class_size[NCLASS]; /* block bytes, header included; 0 past the last class */
static void *free_list[NCLASS];

static void classes_init(void) {
    int c = 0;
    class_size[c++] = 32;
    class_size[c++] = 48;
    for (int k = 4; k <= 28; k++)
        for (int j = 0; j < 4; j++) class_size[c++] = (size_t)(4 + j) << k; /* up to 1.75 GiB */
    class_size[c++] = (size_t)1 << 31;
    heap_base = heap_top = heap_end = ((uintptr_t)&__heap_base + 15) & ~(uintptr_t)15;
}

static int class_of(size_t total) {
    for (int c = 0; c < NCLASS && class_size[c]; c++)
        if (class_size[c] >= total) return c;
    return -1;
}

/* The header holds the class index; the payload follows at +16 (16-aligned). */
void *malloc(size_t n) {
    if (!heap_top) classes_init();
    if (n > 0x7ff00000u) return NULL;
    int c = class_of(n + HDR);
    if (c < 0) return NULL;
    unsigned char *b;
    if (free_list[c]) {
        b = (unsigned char *)free_list[c] - HDR;
        free_list[c] = *(void **)free_list[c];
    } else {
        size_t need = class_size[c];
        if (heap_end - heap_top < need) {
            size_t pages = (need - (heap_end - heap_top) + 65535) >> 16;
            if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) return NULL;
            heap_end = __builtin_wasm_memory_size(0) << 16;
        }
        b = (unsigned char *)heap_top;
        heap_top += need;
    }
    *(uint32_t *)b = (uint32_t)c;
    return b + HDR;
}

void free(void *p) {
    if (!p) return;
    uint32_t c = *(uint32_t *)((unsigned char *)p - HDR);
    *(void **)p = free_list[c];
    free_list[c] = p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    uint32_t c = *(uint32_t *)((unsigned char *)p - HDR);
    size_t cap = class_size[c] - HDR;
    if (n <= cap) return p;
    void *q = malloc(n);
    if (q) {
        memcpy(q, p, cap);
        free(p);
    }
    return q;
}

void *calloc(size_t k, size_t n) {
    if (n && k > (size_t)-1 / n) return NULL;
    void *p = malloc(k * n);
    if (p) memset(p, 0, k * n);
    return p;
}

/* Bytes of the heap carved so far (blocks in use or on a free list); wasm_api.c exports it. */
size_t cc_wasm_heap_used(void) { return heap_top - heap_base; }

#ifdef CC_VIEW_ONLY
/* ---- the viewer-only build (build.sh wasm-view, ticket W1) ----
 * build.sh VIEW_DENY leaves out verbs.c, verb_table.c, ifc_in.c and ifc_out.c. Their symbols that
 * the kept files still reference are defined here: an empty verb table (the `verbs` query answers
 * [], `describe` "no verb with this name") and refusals. The calls that reach them (verb, the place
 * tool, import, export) are refused before, by session/view_only.c and wasm_api.c's VIEW_ONLY. */
#include "ifc_in.h"
#include "ifc_out.h"
#include "verb_table.h"
#include "verbs.h"
#include "view_only.h"

static const cc_verb_def no_verb = {"", "", "{}", NULL, 1};
const cc_verb_def cc_verb_table[] = {no_verb};
const size_t cc_verb_count = 0;
const cc_verb_def cc_verb_c_additions[] = {no_verb};
const size_t cc_verb_c_addition_count = 0;
void cc_verbs_list_json(Buf *out) { buf_puts(out, "[]"); }

int cc_verb_run_ex(const cc_model *m, const char *name, const char *args, size_t n, int dry_run, int64_t if_rev,
                   uint64_t current_rev, cc_cmds *out, Buf *err) {
    (void)m, (void)name, (void)args, (void)n, (void)dry_run, (void)if_rev, (void)current_rev, (void)out;
    buf_puts(err, CC_VIEW_ONLY_TEXT);
    return -1;
}

int cc_ifc_import(cc_model *m, cc_ifc_report *rep, const char *text, size_t n, cc_error *err) {
    (void)text, (void)n;
    cc_model_init(m);
    memset(rep, 0, sizeof *rep);
    memcpy(err->msg, CC_VIEW_ONLY_TEXT, sizeof CC_VIEW_ONLY_TEXT);
    err->offset = CC_NO_OFFSET;
    return -1;
}
void cc_ifc_report_free(cc_ifc_report *rep) { buf_free(&rep->lines); }

int cc_ifc_export(const cc_model *m, const char *file_name, const char *timestamp, cc_ifc_schema schema, Buf *out) {
    (void)m, (void)file_name, (void)timestamp, (void)schema, (void)out;
    return -1;
}
void cc_iso_utc(int64_t secs, char *out) { (void)secs, out[0] = 0; }
#endif
