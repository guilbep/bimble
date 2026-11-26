/* The box tree: see bvh.h. A port of crates/bimble-geom/src/bvh.rs (Rust v0.19.0). */
#include "bvh.h"
#include <string.h>

#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */

#define INF (1.0f / 0.0f)

/* f32::min, f32::max: a NaN gives the other operand. */
static float minf(float a, float b) { return a != a ? b : b != b ? a : b < a ? b : a; }
static float maxf(float a, float b) { return a != a ? b : b != b ? a : b > a ? b : a; }

static int is_fin(float x) { return x == x && x != INF && x != -INF; }

cc_aabb cc_aabb_empty(void) {
    cc_aabb b = {{INF, INF, INF}, {-INF, -INF, -INF}};
    return b;
}

cc_aabb cc_aabb_union(const cc_aabb *a, const cc_aabb *o) { /* bvh.rs:24 */
    cc_aabb r;
    for (int k = 0; k < 3; k++) r.min[k] = minf(a->min[k], o->min[k]), r.max[k] = maxf(a->max[k], o->max[k]);
    return r;
}

cc_aabb cc_aabb_of_points(const float (*p)[3], size_t n) { /* bvh.rs:19 */
    cc_aabb b = cc_aabb_empty();
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) b.min[k] = minf(b.min[k], p[i][k]), b.max[k] = maxf(b.max[k], p[i][k]);
    return b;
}

int cc_aabb_is_empty(const cc_aabb *b) { /* bvh.rs:31 */
    return b->min[0] > b->max[0] || b->min[1] > b->max[1] || b->min[2] > b->max[2];
}

int cc_aabb_is_valid(const cc_aabb *b) {
    for (int k = 0; k < 3; k++)
        if (!is_fin(b->min[k]) || !is_fin(b->max[k]) || !(b->min[k] <= b->max[k])) return 0;
    return 1;
}

int cc_aabb_overlaps(const cc_aabb *a, const cc_aabb *o) { /* bvh.rs:35 */
    for (int k = 0; k < 3; k++)
        if (!(a->min[k] <= o->max[k] && o->min[k] <= a->max[k])) return 0;
    return 1;
}

static float centre(const cc_aabb *b, int k) { return (b->min[k] + b->max[k]) * 0.5f; } /* bvh.rs:39 */

int cc_aabb_ray_entry(const cc_aabb *b, const float o[3], const float inv_d[3], float t_max, float *t) { /* bvh.rs:44 */
    float t0 = 0.0f, t1 = t_max;
    for (int k = 0; k < 3; k++) {
        float a = (b->min[k] - o[k]) * inv_d[k], c = (b->max[k] - o[k]) * inv_d[k];
        /* NaN (0 x inf, a ray along a face) keeps the other bound. */
        t0 = maxf(t0, minf(a, c));
        t1 = minf(t1, maxf(a, c));
    }
    if (!(t0 <= t1)) return 0;
    *t = t0;
    return 1;
}

/* ---- the bounds of a scene's elements ---- */

cc_aabb cc_scene_element_bounds(const cc_scene *s, size_t k) { /* scene.rs:150 SceneElement::bounds */
    const cc_scene_element *e = &s->elements[k];
    if (e->shape < 0) return cc_aabb_of_points((const float(*)[3])e->mesh.pos, e->mesh.nv);
    if ((size_t)e->shape >= s->nshapes) return cc_aabb_empty();
    const cc_mesh *sh = &s->shapes[e->shape];
    cc_aabb b = cc_aabb_of_points((const float(*)[3])sh->pos, sh->nv);
    if (cc_aabb_is_empty(&b)) return b;
    /* The 8 corners placed (Placement32::apply, scene.rs:47), then their box. */
    const cc_placement32 *p = &e->to_scene;
    cc_aabb r = cc_aabb_empty();
    for (int i = 0; i < 8; i++) {
        float c[3], q[3];
        for (int j = 0; j < 3; j++) c[j] = ((i >> j) & 1) ? b.max[j] : b.min[j];
        for (int j = 0; j < 3; j++) q[j] = p->x[j] * c[0] + p->y[j] * c[1] + p->z[j] * c[2] + p->t[j];
        for (int j = 0; j < 3; j++) r.min[j] = minf(r.min[j], q[j]), r.max[j] = maxf(r.max[j], q[j]);
    }
    return r;
}

void cc_element_boxes(const cc_scene *s, cc_aabb *out) { /* bvh.rs:80 */
    for (size_t k = 0; k < s->nelements; k++) out[k] = cc_scene_element_bounds(s, k);
}

/* ---- the build ---- */

/* f32::total_cmp as an unsigned order: -NaN < -inf < ... < -0 < +0 < ... < inf < NaN. */
static uint32_t total_key(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u >> 31 ? ~u : u | 0x80000000u;
}

/* Item a before item b along axis k: by centre (total_cmp), then by index (a total order, so the
 * halves do not depend on how the selection goes). */
static int before(const cc_aabb *bx, int k, uint32_t a, uint32_t b) {
    uint32_t ka = total_key(centre(&bx[a], k)), kb = total_key(centre(&bx[b], k));
    return ka != kb ? ka < kb : a < b;
}

static void swap(uint32_t *s, size_t i, size_t j) {
    uint32_t x = s[i];
    s[i] = s[j], s[j] = x;
}

static void sift(const cc_aabb *bx, int k, uint32_t *s, size_t root, size_t n) {
    for (;;) {
        size_t c = 2 * root + 1;
        if (c >= n) return;
        if (c + 1 < n && before(bx, k, s[c], s[c + 1])) c++;
        if (!before(bx, k, s[root], s[c])) return;
        swap(s, root, c);
        root = c;
    }
}

static void heap_sort(const cc_aabb *bx, int k, uint32_t *s, size_t n) {
    for (size_t i = n / 2; i-- > 0;) sift(bx, k, s, i, n);
    for (size_t e = n; e-- > 1;) swap(s, 0, e), sift(bx, k, s, 0, e);
}

/* select_nth_unstable_by (bvh.rs:120): s[nth] in place, the smaller before it, the larger after.
 * Quickselect on a median of three; after 2 log2(n) + 4 rounds (an adversarial order), a heap
 * sort of what is left: O(n log n) at worst, no recursion. */
static void select_nth(const cc_aabb *bx, int k, uint32_t *s, size_t n, size_t nth) {
    size_t lo = 0, hi = n - 1;
    unsigned budget = 4;
    for (size_t m = n; m > 1; m >>= 1) budget += 2;
    while (lo < hi) {
        if (!budget--) {
            heap_sort(bx, k, s + lo, hi - lo + 1);
            return;
        }
        size_t m = lo + (hi - lo) / 2;
        if (before(bx, k, s[m], s[lo])) swap(s, m, lo);
        if (before(bx, k, s[hi], s[lo])) swap(s, hi, lo);
        if (before(bx, k, s[hi], s[m])) swap(s, hi, m);
        swap(s, m, hi); /* the median is the pivot, at hi */
        size_t st = lo;
        for (size_t i = lo; i < hi; i++)
            if (before(bx, k, s[i], s[hi])) swap(s, i, st++);
        swap(s, st, hi);
        if (st == nth) return;
        if (nth < st) hi = st - 1;
        else lo = st + 1;
    }
}

static cc_aabb bounds_of(const cc_aabb *bx, const uint32_t *s, size_t n) {
    cc_aabb b = cc_aabb_empty();
    for (size_t i = 0; i < n; i++) b = cc_aabb_union(&b, &bx[s[i]]);
    return b;
}

int cc_bvh_build(cc_bvh *t, const cc_aabb *boxes, size_t n, Arena *a) { /* bvh.rs:86 */
    memset(t, 0, sizeof *t);
    if (n > CC_BVH_MAX_ITEMS) return -1;
    size_t m = 0;
    for (size_t i = 0; i < n; i++) m += cc_aabb_is_valid(&boxes[i]);
    if (!m) return 0;
    /* Every split is of more than LEAF items into halves of 2 or more: m nodes at most. */
    uint32_t *items = arena_alloc_n(a, m, sizeof *items, 4);
    cc_bvh_node *nodes = arena_alloc_n(a, m, sizeof *nodes, 4);
    cc_aabb *bx = arena_alloc_n(a, m, sizeof *bx, 4);
    if (!items || !nodes || !bx) return -1;
    for (size_t i = 0, j = 0; i < n; i++)
        if (cc_aabb_is_valid(&boxes[i])) items[j++] = (uint32_t)i;
    nodes[0].bounds = bounds_of(boxes, items, m), nodes[0].first = 0, nodes[0].count = (uint32_t)m;
    size_t nn = 1;
    unsigned depth = 0;
    /* Explicit stack: no recursion on untrusted sizes (bvh.rs:107). Each level leaves one sibling
     * waiting at most, and halving 2^32 items reaches LEAF in 31 levels. */
    struct {
        uint32_t node;
        unsigned depth;
    } stack[CC_BVH_STACK];
    size_t sp = 0;
    stack[sp].node = 0, stack[sp].depth = 0, sp++;
    while (sp) {
        sp--;
        uint32_t ni = stack[sp].node;
        unsigned dep = stack[sp].depth;
        if (dep > depth) depth = dep;
        size_t first = nodes[ni].first, count = nodes[ni].count;
        if (count <= CC_BVH_LEAF) continue;
        const cc_aabb *b = &nodes[ni].bounds;
        int k = 0; /* the longest axis; the last of equal ones (max_by, bvh.rs:117) */
        for (int j = 1; j < 3; j++)
            if (total_key(b->max[j] - b->min[j]) >= total_key(b->max[k] - b->min[k])) k = j;
        size_t mid = count / 2;
        select_nth(boxes, k, items + first, count, mid);
        if (nn + 2 > m || sp + 2 > CC_BVH_STACK) return memset(t, 0, sizeof *t), -1; /* never: see above */
        size_t left = nn;
        nodes[nn].bounds = bounds_of(boxes, items + first, mid), nodes[nn].first = (uint32_t)first, nodes[nn].count = (uint32_t)mid, nn++;
        nodes[nn].bounds = bounds_of(boxes, items + first + mid, count - mid), nodes[nn].first = (uint32_t)(first + mid),
        nodes[nn].count = (uint32_t)(count - mid), nn++;
        nodes[ni].first = (uint32_t)left, nodes[ni].count = 0;
        stack[sp].node = (uint32_t)left, stack[sp].depth = dep + 1, sp++;
        stack[sp].node = (uint32_t)left + 1, stack[sp].depth = dep + 1, sp++;
    }
    for (size_t i = 0; i < m; i++) bx[i] = boxes[items[i]];
    t->nodes = nodes, t->nnodes = nn, t->items = items, t->boxes = bx, t->nitems = m, t->depth = depth;
    return 0;
}

int cc_bvh_is_empty(const cc_bvh *t) { return t->nitems == 0; } /* bvh.rs:145 */

/* ---- the queries ---- */

int cc_bvh_ray(const cc_bvh *t, const float o[3], const float d[3], cc_bvh_hit_fn hit, void *ctx, float *dist,
               uint64_t *key) { /* bvh.rs:153 */
    if (!t->nnodes) return 0;
    float inv[3] = {1.0f / d[0], 1.0f / d[1], 1.0f / d[2]};
    struct {
        uint32_t node;
        float t;
    } stack[CC_BVH_STACK];
    size_t sp = 0;
    float te;
    if (!cc_aabb_ray_entry(&t->nodes[0].bounds, o, inv, INF, &te)) return 0;
    stack[sp].node = 0, stack[sp].t = te, sp++;
    int found = 0;
    float bd = 0.0f;
    uint64_t bk = 0;
    while (sp) {
        sp--;
        uint32_t n = stack[sp].node;
        float tn = stack[sp].t;
        if (tn > (found ? bd : INF)) continue;
        const cc_bvh_node *node = &t->nodes[n];
        if (node->count > 0) {
            for (uint32_t i = node->first; i < node->first + node->count; i++) {
                float hd;
                uint64_t hk;
                int r = hit(ctx, t->items[i], tn, &hd, &hk);
                if (r < 0) return -1;
                if (r > 0 && (!found || hd < bd || (hd == bd && hk < bk))) found = 1, bd = hd, bk = hk;
            }
            continue;
        }
        uint32_t l = node->first;
        float limit = found ? bd : INF, ta, tb;
        int ea = cc_aabb_ray_entry(&t->nodes[l].bounds, o, inv, limit, &ta);
        int eb = cc_aabb_ray_entry(&t->nodes[l + 1].bounds, o, inv, limit, &tb);
        if (sp + 2 > CC_BVH_STACK) return -1; /* never: the depth is 31 at most */
        /* Push the farther child first, so the nearer one is popped first (bvh.rs:190). */
        if (ea && eb && ta <= tb) {
            stack[sp].node = l + 1, stack[sp].t = tb, sp++;
            stack[sp].node = l, stack[sp].t = ta, sp++;
        } else if (ea && eb) {
            stack[sp].node = l, stack[sp].t = ta, sp++;
            stack[sp].node = l + 1, stack[sp].t = tb, sp++;
        } else if (ea) stack[sp].node = l, stack[sp].t = ta, sp++;
        else if (eb) stack[sp].node = l + 1, stack[sp].t = tb, sp++;
    }
    if (found) *dist = bd, *key = bk;
    return found;
}

void cc_bvh_visit(const cc_bvh *t, int (*enter)(void *ctx, const cc_aabb *b), void (*f)(void *ctx, uint32_t item),
                  void *ctx) {
    if (!t->nnodes) return;
    uint32_t stack[CC_BVH_STACK];
    size_t sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const cc_bvh_node *node = &t->nodes[stack[--sp]];
        if (!enter(ctx, &node->bounds)) continue;
        if (node->count > 0) {
            for (uint32_t k = node->first; k < node->first + node->count; k++) f(ctx, t->items[k]);
        } else if (sp + 2 <= CC_BVH_STACK) /* always: the depth is 31 at most */
            stack[sp++] = node->first, stack[sp++] = node->first + 1;
    }
}

void cc_bvh_overlapping(const cc_bvh *t, const cc_aabb *b, void (*f)(void *ctx, uint32_t item), void *ctx) { /* bvh.rs:203 */
    if (!t->nnodes) return;
    uint32_t stack[CC_BVH_STACK];
    size_t sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const cc_bvh_node *node = &t->nodes[stack[--sp]];
        if (!cc_aabb_overlaps(&node->bounds, b)) continue;
        if (node->count > 0) {
            for (uint32_t k = node->first; k < node->first + node->count; k++)
                if (cc_aabb_overlaps(&t->boxes[k], b)) f(ctx, t->items[k]);
        } else if (sp + 2 <= CC_BVH_STACK) /* always */
            stack[sp++] = node->first, stack[sp++] = node->first + 1;
    }
}

static float volume(const cc_aabb *x) { /* bvh.rs:252 */
    float v = 1.0f;
    for (int k = 0; k < 3; k++) v = v * (x->max[k] - x->min[k]);
    return v;
}

void cc_bvh_pairs(const cc_bvh *t, const cc_bvh *other, void (*f)(void *ctx, uint32_t i, uint32_t j), void *ctx) { /* bvh.rs:228 */
    if (!t->nnodes || !other->nnodes) return;
    /* Each push goes one level down one tree: depth here + depth there + 1 pairs waiting at most. */
    struct {
        uint32_t a, b;
    } stack[CC_BVH_STACK];
    size_t sp = 0;
    stack[sp].a = 0, stack[sp].b = 0, sp++;
    while (sp) {
        sp--;
        uint32_t a = stack[sp].a, b = stack[sp].b;
        const cc_bvh_node *na = &t->nodes[a], *nb = &other->nodes[b];
        if (!cc_aabb_overlaps(&na->bounds, &nb->bounds)) continue;
        if (na->count > 0 && nb->count > 0) {
            for (uint32_t i = na->first; i < na->first + na->count; i++)
                for (uint32_t j = nb->first; j < nb->first + nb->count; j++)
                    if (cc_aabb_overlaps(&t->boxes[i], &other->boxes[j])) f(ctx, t->items[i], other->items[j]);
            continue;
        }
        if (sp + 2 > CC_BVH_STACK) continue; /* never */
        /* Descend the inner node (the larger one when both are). */
        int down_a = nb->count > 0 || (na->count == 0 && volume(&na->bounds) >= volume(&nb->bounds));
        if (down_a) {
            stack[sp].a = na->first, stack[sp].b = b, sp++;
            stack[sp].a = na->first + 1, stack[sp].b = b, sp++;
        } else {
            stack[sp].a = a, stack[sp].b = nb->first, sp++;
            stack[sp].a = a, stack[sp].b = nb->first + 1, sp++;
        }
    }
}
