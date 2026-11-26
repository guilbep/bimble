/* A box tree (ticket 3D2): a port of bimble_geom::bvh (bvh.rs, a BoxTree: Barequet, Chazelle &
 * Guibas 1996; Zachmann). Each node's box holds its two children's. Built over element bounds it
 * answers what touches a ray (picking, snapping), a box (searching, culling) or another tree (the
 * broad phase of clash detection between models) without looking at every element.
 *
 * Memory: the tree lives in the caller's arena; the queries allocate nothing (their stacks are
 * fixed arrays: the median split bounds the depth at 31 levels for 2^32 items). Untrusted boxes:
 * a box that is empty or not finite is left out of the tree (Rust leaves out only the empty ones).
 * f32 throughout, as Rust; the file is compiled without contraction (a*b+c rounds twice). */
#ifndef CCORE_BVH_H
#define CCORE_BVH_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "scene.h"

/* bvh.rs:8 Aabb: an axis-aligned box (scene coordinates, mm). */
typedef struct {
    float min[3], max[3];
} cc_aabb;

#define CC_BVH_LEAF 4                     /* bvh.rs:77 LEAF: items per leaf at most */
#define CC_BVH_MAX_ITEMS 0x7fffffffu      /* boxes a tree takes at most (u32 item and node indices) */
#define CC_BVH_STACK 128                  /* the queries' fixed stacks (>= 2 * 31 + 1) */

/* bvh.rs:14 Aabb::EMPTY: min +inf, max -inf (the identity of union). */
cc_aabb cc_aabb_empty(void);
/* bvh.rs:19 Aabb::of_points: the box of `n` points (NaN coordinates ignored, as f32::min). */
cc_aabb cc_aabb_of_points(const float (*p)[3], size_t n);
/* bvh.rs:24 Aabb::union. */
cc_aabb cc_aabb_union(const cc_aabb *a, const cc_aabb *b);
/* bvh.rs:31 Aabb::is_empty: min > max on some axis. */
int cc_aabb_is_empty(const cc_aabb *b);
/* Not empty and every coordinate finite: what a tree takes. */
int cc_aabb_is_valid(const cc_aabb *b);
/* bvh.rs:35 Aabb::overlaps (touching counts). */
int cc_aabb_overlaps(const cc_aabb *a, const cc_aabb *b);
/* bvh.rs:44 Aabb::ray_entry: where a ray (origin `o`, `inv_d` = 1 / direction) enters the box, if
 * it does before `t_max`: 1 with `*t` (>= 0), else 0. A NaN slab (0 x inf: a ray along a face)
 * keeps the other bound. */
int cc_aabb_ray_entry(const cc_aabb *b, const float o[3], const float inv_d[3], float t_max, float *t);

/* scene.rs:150 SceneElement::bounds: element k's box in scene coordinates. An own mesh: its
 * positions. A placed shape: the shape's box, its 8 corners placed by `to_scene` (f32,
 * x p.x + y p.y + z p.z + t as cc_scene_world_mesh), then their box; nothing is allocated. Empty
 * for an empty mesh (or a shape index out of range). */
cc_aabb cc_scene_element_bounds(const cc_scene *s, size_t k);
/* bvh.rs:80 element_boxes: the bounds of each element, into `out` (s->nelements boxes). */
void cc_element_boxes(const cc_scene *s, cc_aabb *out);

typedef struct { /* bvh.rs:60 Node */
    cc_aabb bounds;
    uint32_t first; /* leaf: first item in `items`; inner: the left child (the right one follows) */
    uint32_t count; /* items in a leaf; 0 for an inner node */
} cc_bvh_node;

typedef struct { /* bvh.rs:70 Bvh. Items are indices into the boxes it was built from. */
    cc_bvh_node *nodes; /* nodes[0] is the root; none when empty */
    size_t nnodes;
    uint32_t *items;
    cc_aabb *boxes; /* the box of each of `items`, in the same order */
    size_t nitems;
    unsigned depth; /* levels below the root */
} cc_bvh;

/* bvh.rs:86 Bvh::build: median split on the longest axis, O(n log n) (a selection that goes bad
 * falls back to a heap sort: never quadratic), depth ~log2(n / 4). Ties between centres go to the
 * smaller index (Rust's select_nth_unstable leaves them unordered). Memory from `a`. Always sets
 * `t` (empty on an error). 0 ok, -1 out of memory or more than CC_BVH_MAX_ITEMS boxes. */
int cc_bvh_build(cc_bvh *t, const cc_aabb *boxes, size_t n, Arena *a);
/* bvh.rs:145 Bvh::is_empty. */
int cc_bvh_is_empty(const cc_bvh *t);

/* The test of one item along a ray (`entry`: where the ray enters its leaf's box): 1 with its hit
 * distance `*dist` and a tie key `*key` (smaller wins between equally near hits: an element
 * index, say), 0 for no hit, -1 to stop the query with an error. */
typedef int (*cc_bvh_hit_fn)(void *ctx, uint32_t item, float entry, float *dist, uint64_t *key);

/* bvh.rs:153 Bvh::ray: the items whose boxes the ray (o, d) meets, nearest box first, through
 * `hit`; once something is hit, boxes entered beyond it are skipped. 1 with the nearest hit
 * (`*dist`, `*key`; between equally near ones the smallest key, whatever the order they were
 * found in), 0 for none, -1 when `hit` returned -1. */
int cc_bvh_ray(const cc_bvh *t, const float o[3], const float d[3], cc_bvh_hit_fn hit, void *ctx, float *dist,
               uint64_t *key);
/* bvh.rs:203 Bvh::overlapping: `f` for each item whose box overlaps `b` (searching, culling). */
void cc_bvh_overlapping(const cc_bvh *t, const cc_aabb *b, void (*f)(void *ctx, uint32_t item), void *ctx);
/* bvh.rs:228 Bvh::pairs: `f` for each pair (item here, item there) whose boxes overlap (the broad
 * phase of clash detection between two models). */
void cc_bvh_pairs(const cc_bvh *t, const cc_bvh *other, void (*f)(void *ctx, uint32_t i, uint32_t j), void *ctx);
/* Not in Rust: the walk under `overlapping` with the caller's test. `f` for each item of every
 * leaf reached through nodes whose box `enter` accepts (1); `f` tests the item itself. For a test
 * that bvh.rs's f32 slabs do not give, e.g. measure.c's ray in f64 with a margin. */
void cc_bvh_visit(const cc_bvh *t, int (*enter)(void *ctx, const cc_aabb *b), void (*f)(void *ctx, uint32_t item),
                  void *ctx);
#endif
