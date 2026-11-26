/* Freestanding wasm32 shim (build.sh wasm only); implemented in ../wasm_stub.c. */
#ifndef CCORE_FS_STDLIB_H
#define CCORE_FS_STDLIB_H
#include <stddef.h>
void *malloc(size_t n);
void *realloc(void *p, size_t n);
void free(void *p);
void *calloc(size_t k, size_t n);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)); /* ../wasm_api.c */
#endif
