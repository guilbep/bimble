#include "arena.h"
#include <stdlib.h>
#include <string.h>

struct ArenaBlock {
    ArenaBlock *next;
    size_t cap, used;
    _Alignas(16) unsigned char data[];
};

void arena_init(Arena *a, size_t block_size) {
    a->head = NULL;
    a->block_size = block_size ? block_size : 64 * 1024;
}

void *arena_alloc(Arena *a, size_t size, size_t align) {
    if (align == 0 || align > 16 || (align & (align - 1))) return NULL;
    ArenaBlock *b = a->head;
    if (b) {
        size_t off = (b->used + align - 1) & ~(align - 1);
        if (off <= b->cap && size <= b->cap - off) {
            b->used = off + size;
            memset(b->data + off, 0, size);
            return b->data + off;
        }
    }
    size_t cap = a->block_size;
    if (size > cap) cap = size;
    if (cap > (size_t)-1 - sizeof(ArenaBlock)) return NULL;
    ArenaBlock *nb = malloc(sizeof(ArenaBlock) + cap);
    if (!nb) return NULL;
    nb->next = b;
    nb->cap = cap;
    nb->used = size;
    a->head = nb;
    memset(nb->data, 0, size);
    return nb->data;
}

void *arena_dup(Arena *a, const void *src, size_t size) {
    void *p = arena_alloc(a, size ? size : 1, 1);
    if (p && size) memcpy(p, src, size);
    return p;
}

void arena_reset(Arena *a) {
    ArenaBlock *b = a->head;
    if (!b) return;
    ArenaBlock *old = b->next;
    while (old) {
        ArenaBlock *n = old->next;
        free(old);
        old = n;
    }
    b->next = NULL;
    b->used = 0;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *n = b->next;
        free(b);
        b = n;
    }
    a->head = NULL;
}

void arena_adopt(Arena *dst, Arena *src) {
    ArenaBlock *first = src->head;
    if (!first) return;
    ArenaBlock *last = first;
    while (last->next) last = last->next;
    if (dst->head) {
        last->next = dst->head->next;
        dst->head->next = first;
    } else dst->head = first;
    src->head = NULL;
}
