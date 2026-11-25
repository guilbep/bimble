/* Bump allocator: many small allocations, freed all at once. */
#ifndef CCORE_ARENA_H
#define CCORE_ARENA_H
#include <stddef.h>

typedef struct ArenaBlock ArenaBlock;
typedef struct {
    ArenaBlock *head;  /* current block; older blocks follow ->next */
    size_t block_size; /* default size of a new block */
} Arena;

void arena_init(Arena *a, size_t block_size); /* block_size 0 = 64 KiB */
/* Zeroed memory, aligned to `align` (a power of two, at most 16). NULL when out of memory. */
void *arena_alloc(Arena *a, size_t size, size_t align);
void *arena_dup(Arena *a, const void *src, size_t size);
void arena_reset(Arena *a); /* keeps the newest block, drops the rest */
void arena_free(Arena *a);
#endif
