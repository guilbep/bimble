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
/* arena_reset, and the newest block dropped too when it holds more than `keep` bytes: a worker's
 * scratch arena does not keep its largest job's memory for the rest of the run. */
void arena_trim(Arena *a, size_t keep);
void arena_free(Arena *a);
/* Moves every block of `src` into `dst` (freed with it; `dst` keeps its current block), and
 * empties `src`. For per-thread arenas whose results outlive the job. */
void arena_adopt(Arena *dst, Arena *src);
#endif
