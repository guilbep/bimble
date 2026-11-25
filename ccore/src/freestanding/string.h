/* Freestanding wasm32 shim (build.sh wasm only); implemented in ../wasm_stub.c. */
#ifndef CCORE_FS_STRING_H
#define CCORE_FS_STRING_H
#include <stddef.h>
void *memcpy(void *restrict d, const void *restrict s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#endif
