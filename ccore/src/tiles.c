/* The tile pyramid (tiles.h; ticket TILE1, ADR 0078 amendment 6). */
#ifdef __clang__
#pragma clang fp contract(off) /* the same bytes from every build (native, wasm): no fused multiply-add */
#endif
#ifdef TILES_DEBUG
#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "hlod_grid.h"
#include "jobs.h"
#include "math.h"
#include "meshcodec.h"
#include "scene_build.h"
#include "tiles.h"

void cc_tiles_default(cc_tiles_opts *o) {
    o->leaf_tris = CC_TILES_LEAF_TRIS;
    o->proxy_tris = CC_TILES_PROXY_TRIS;
    o->root_tris = CC_TILES_ROOT_TRIS;
    o->res = CC_TILES_RES;
    o->occ = CC_TILES_OCC;
    o->shell_views = CC_TILES_SHELL_VIEWS, o->shell_res = CC_TILES_SHELL_RES, o->shell_tol = CC_TILES_SHELL_TOL, o->shell_cross = 1;
}

void cc_tiles_opts_text(const cc_tiles_opts *o, char *out, size_t n) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "leaf="), buf_u64(&b, o->leaf_tris), buf_puts(&b, " proxy="), buf_u64(&b, o->proxy_tris);
    buf_puts(&b, " root="), buf_u64(&b, o->root_tris), buf_puts(&b, " res="), buf_u64(&b, o->res), buf_puts(&b, " occ="), buf_u64(&b, o->occ);
    buf_puts(&b, " shell="), buf_u64(&b, o->shell_views); /* SHELL1 */
    if (o->shell_views) buf_putc(&b, 'x'), buf_u64(&b, o->shell_res), buf_puts(&b, " shell_d="), buf_u64(&b, o->shell_tol);
    size_t k = b.oom || !b.p ? 0 : (b.len < n ? b.len : n - 1);
    if (n) memcpy(out, b.p ? b.p : "", k), out[k] = 0;
    buf_free(&b);
}

void cc_tiles_opts_parse(cc_tiles_opts *o, const char *t) {
    cc_tiles_default(o);
    static const struct { const char *k; uint32_t lo, hi; } K[] = {{"leaf", 1000, 10000000}, {"proxy", 64, 1000000}, {"root", 16, 1000000}, {"res", 4, 1024}, {"occ", 0, CC_TILES_OCC_MAX}, {"shell", 0, 0}, {"shell_d", 0, 100000}, {"cross", 0, 1}, {"cross_d", 0, 100000}, {"cross_touch", 0, 1}};
    for (const char *p = t; p && *p;) {
        while (*p == ' ') p++;
        for (size_t i = 0; i < sizeof K / sizeof *K; i++) {
            size_t kl = strlen(K[i].k);
            if (strncmp(p, K[i].k, kl) || p[kl] != '=') continue;
            uint64_t v = 0;
            const char *q = p + kl + 1;
            int d = 0;
            while (*q >= '0' && *q <= '9' && d < 10) v = v * 10 + (uint64_t)(*q++ - '0'), d++;
            if (i == 7 && d && v <= CC_SHELL_MAX_VIEWS_T && *q == 'x') { /* SHELL1: cross=<views>x<res> */
                uint64_t r = 0;
                int dr = 0;
                q++;
                while (*q >= '0' && *q <= '9' && dr < 10) r = r * 10 + (uint64_t)(*q++ - '0'), dr++;
                if (v && dr && r >= 16 && r <= CC_SHELL_MAX_RES_T) o->shell_cross = 1, o->cross_views = (uint32_t)v, o->cross_res = (uint32_t)r;
                continue;
            }
            if (i == 5 && d && v <= CC_SHELL_MAX_VIEWS_T) { /* SHELL1: shell=<views>x<res> (0: none) */
                uint64_t r = 0;
                int dr = 0;
                if (*q == 'x') {
                    q++;
                    while (*q >= '0' && *q <= '9' && dr < 10) r = r * 10 + (uint64_t)(*q++ - '0'), dr++;
                }
                if (!v) o->shell_views = 0;
                else if (dr && r >= 16 && r <= CC_SHELL_MAX_RES_T) o->shell_views = (uint32_t)v, o->shell_res = (uint32_t)r;
                else if (!dr) o->shell_views = (uint32_t)v;
                continue;
            }
            if (d && v >= K[i].lo && v <= K[i].hi) {
                uint32_t x = (uint32_t)v;
                if (i == 0) o->leaf_tris = x; else if (i == 1) o->proxy_tris = x; else if (i == 2) o->root_tris = x; else if (i == 3) o->res = x; else if (i == 4) o->occ = x; else if (i == 5) {} else if (i == 6) o->shell_tol = x; else if (i == 7) o->shell_cross = x; else if (i == 8) o->cross_d = x; else o->cross_touch = x;
            }
        }
        while (*p && *p != ' ') p++;
    }
}

static int fail(cc_error *err, const char *msg) {
    if (!err) return -1;
    size_t n = strlen(msg);
    err->offset = CC_NO_OFFSET;
    if (n >= sizeof err->msg) n = sizeof err->msg - 1;
    memcpy(err->msg, msg, n);
    err->msg[n] = 0;
    return -1;
}

static int finite3(const float *p) {
    for (int k = 0; k < 3; k++)
        if (!(p[k] - p[k] == 0.0f)) return 0;
    return 1;
}

/* ---- the source: a scene or a view file ---- */

static size_t src_n(const cc_tiles_src *s) { return s->scene ? s->scene->nelements : s->vf->nelements; }
static int src_shape(const cc_tiles_src *s, size_t k) { return s->scene ? s->scene->elements[k].shape : s->vf->elements[k].shape; }
static size_t src_nshapes(const cc_tiles_src *s) { return s->scene ? s->scene->nshapes : s->vf->nshapes; }
static const cc_placement32 *src_pl(const cc_tiles_src *s, size_t k) {
    return s->scene ? &s->scene->elements[k].to_scene : &s->vf->elements[k].to_scene;
}
static cc_category src_cat(const cc_tiles_src *s, size_t k) {
    cc_category c = s->scene ? s->scene->elements[k].category : s->vf->elements[k].category;
    return (unsigned)c < CC_CAT_COUNT ? c : CC_CAT_OTHER;
}
static cc_str src_class(const cc_tiles_src *s, size_t k) {
    return s->scene ? s->scene->elements[k].class_name : s->vf->elements[k].class_name;
}
/* The mesh's counts (arrays may be NULL in a lazy view file). */
static const cc_mesh *src_counts(const cc_tiles_src *s, size_t k) {
    int sh = src_shape(s, k);
    if (s->scene) return sh >= 0 ? ((size_t)sh < s->scene->nshapes ? &s->scene->shapes[sh] : NULL) : &s->scene->elements[k].mesh;
    return sh >= 0 ? ((size_t)sh < s->vf->nshapes ? &s->vf->shapes[sh] : NULL) : &s->vf->elements[k].mesh;
}
static int lazy(const cc_tiles_src *s) { return !s->scene && s->vf->lazy; }
/* Block b's mesh (b < nshapes: shape b; else element b - nshapes's own), decoded into `a` when lazy. */
static int src_block(const cc_tiles_src *s, size_t b, Arena *a, cc_mesh *m) {
    size_t ns = src_nshapes(s);
    if (!lazy(s)) {
        const cc_mesh *x = b < ns ? (s->scene ? &s->scene->shapes[b] : &s->vf->shapes[b])
                                  : (s->scene ? &s->scene->elements[b - ns].mesh : &s->vf->elements[b - ns].mesh);
        if (x->ni && (!x->pos || !x->idx)) return -1;
        *m = *x;
        return 0;
    }
    const uint8_t *p;
    size_t n;
    const char *why;
    if (cc_viewfile_block(s->vf, b, &p, &n) || cc_meshcodec_decode(p, n, a, m, &why) != CC_MC_OK) return -1;
    const cc_mesh *c = b < ns ? &s->vf->shapes[b] : &s->vf->elements[b - ns].mesh;
    return m->nv == c->nv && m->ni == c->ni ? 0 : -1;
}
static size_t el_block(const cc_tiles_src *s, size_t k) {
    int sh = src_shape(s, k);
    return sh >= 0 ? (size_t)sh : src_nshapes(s) + k;
}

/* x p + t (f32, the scene's order: hlod.c placed, scene.c world mesh) */
static void place(const cc_placement32 *pl, const float q[3], float o[3]) {
    for (int j = 0; j < 3; j++) o[j] = pl->x[j] * q[0] + pl->y[j] * q[1] + pl->z[j] * q[2] + pl->t[j];
}
static int mirrors(const cc_placement32 *p) {
    float c[3] = {p->y[1] * p->z[2] - p->y[2] * p->z[1], p->y[2] * p->z[0] - p->y[0] * p->z[2], p->y[0] * p->z[1] - p->y[1] * p->z[0]};
    return p->x[0] * c[0] + p->x[1] * c[1] + p->x[2] * c[2] < 0.0f;
}

/* Element k's box: a lazy view file's from its block's box (a placed shape: the 8 corners placed); otherwise from
 * its placed vertices. 0 ok (b: lo, hi; empty when no finite point). */
static int src_box(const cc_tiles_src *s, size_t k, float b[6], Arena *scratch) {
    for (int j = 0; j < 3; j++) b[j] = 1e30f, b[3 + j] = -1e30f;
    int sh = src_shape(s, k);
    if (lazy(s)) {
        const cc_viewfile_ref *r = sh >= 0 ? ((size_t)sh < s->vf->nshapes ? &s->vf->shape_refs[sh] : NULL) : &s->vf->elements[k].ref;
        if (!r || !(r->lo[0] <= r->hi[0])) return 0;
        for (int c = 0; c < 8; c++) {
            float q[3] = {c & 1 ? r->hi[0] : r->lo[0], c & 2 ? r->hi[1] : r->lo[1], c & 4 ? r->hi[2] : r->lo[2]}, w[3];
            if (sh >= 0) place(src_pl(s, k), q, w);
            else memcpy(w, q, sizeof w);
            for (int j = 0; j < 3; j++) {
                if (w[j] < b[j]) b[j] = w[j];
                if (w[j] > b[3 + j]) b[3 + j] = w[j];
            }
        }
        return 0;
    }
    cc_mesh m;
    if (src_block(s, el_block(s, k), scratch, &m)) return -1;
    for (uint32_t i = 0; i < m.nv; i++) {
        float w[3];
        if (sh >= 0) place(src_pl(s, k), m.pos[i], w);
        else memcpy(w, m.pos[i], sizeof w);
        if (!finite3(w)) continue;
        for (int j = 0; j < 3; j++) {
            if (w[j] < b[j]) b[j] = w[j];
            if (w[j] > b[3 + j]) b[3 + j] = w[j];
        }
    }
    return 0;
}

/* ---- the source for other bakers (shell.c) ---- */
size_t cc_tiles_src_count(const cc_tiles_src *s) { return src_n(s); }
size_t cc_tiles_src_nblocks(const cc_tiles_src *s) { return src_nshapes(s) + src_n(s); }
size_t cc_tiles_src_el_block(const cc_tiles_src *s, size_t k) {
    int sh = src_shape(s, k);
    if (sh >= 0 && (size_t)sh >= src_nshapes(s)) return SIZE_MAX; /* a damaged shape index */
    return el_block(s, k);
}
int cc_tiles_src_block(const cc_tiles_src *s, size_t b, Arena *a, cc_mesh *m) {
    return b < cc_tiles_src_nblocks(s) ? src_block(s, b, a, m) : -1;
}
const cc_placement32 *cc_tiles_src_place(const cc_tiles_src *s, size_t k) { return src_shape(s, k) >= 0 ? src_pl(s, k) : NULL; }
int cc_tiles_src_box(const cc_tiles_src *s, size_t k, float b[6], Arena *scratch) { return src_box(s, k, b, scratch); }

static int opaque(const cc_tiles_src *s, size_t k) {
    if (src_cat(s, k) == CC_CAT_GLASS) return 0;
    cc_str c = src_class(s, k);
    static const char *const OK[] = {"IfcWall", "IfcSlab", "IfcRoof", "IfcFooting", "IfcColumn", "IfcBeam", "IfcCovering"};
    for (size_t i = 0; i < sizeof OK / sizeof *OK; i++) {
        size_t n = strlen(OK[i]);
        if (c.p && c.n >= n && !memcmp(c.p, OK[i], n)) return 1;
    }
    return 0;
}

/* ---- the build ---- */

typedef struct {
    float t[9];
    double area;
    uint32_t el, tri;
} Occ;

typedef struct {
    uint32_t parent;          /* node index, NONE */
    uint32_t *kids, nkids;    /* node indices (own leaves first, then octants) */
    uint32_t cdepth;          /* octree depth of its cell (grid e = MAXD - cdepth) */
    uint32_t flags;           /* LEAF, OWN */
    uint32_t el0, nel;        /* into w->els */
    float box[6];
    uint64_t detail;          /* placed triangles under it */
    uint32_t tris, verts;     /* its content's */
    float error;
    uint64_t off;
    uint32_t len;
    Occ *occ;
    uint32_t nocc;
    uint32_t id, level;
    int occ_heap;             /* its occluders were malloc'd by a worker (a leaf), not in the arena */
    double (*smp)[4];         /* points of its content (vertices and triangle centres, strided) and the size of what they are
                               * on (an element's middle extent: its omission costs no more; a proxy's: unbounded), until
                               * its parent used them */
    uint32_t nsmp;
} Node;

typedef struct { /* a worker's leaf state (leaves are read in parallel: cc_jobs) */
    Grid g;
    Arena dec;                /* decoded shapes of the leaf */
    Arena tmp;                /* an own mesh, per element */
    cc_mesh *shapes;          /* per shape: decoded for this leaf (nv = UINT32_MAX: not yet) */
    uint8_t *shape_flag;      /* per shape: closed and outward (bit 0), 0x80 not known yet */
    uint32_t *touched, ntouched;
    float (*wpos)[3];
    Key *keys;
    uint8_t *has;
    size_t cwpos;
    Occ occ[CC_TILES_OCC_MAX];
    uint32_t nocc, maxocc;
    uint64_t leaf_verts;
    double (*lsmp)[4];        /* the leaf's samples */
    uint32_t nlsmp, clsmp;
    uint64_t lstride, lcount;
    int bad;
} LW;

struct cc_tiles_work {
    cc_tiles_src src;
    cc_tiles_opts o;
    Arena arena;              /* nodes' kids, occluders */
    Node *nodes;
    uint32_t nnodes, cnodes, root; /* root: the octree's root (the chain goes above it) */
    uint32_t *els;            /* the element list (DFS: each node's subtree a range) */
    uint32_t nels;
    float (*box)[6];          /* per scene element */
    uint64_t *tris;           /* per scene element, placed */
    uint8_t *eflag;           /* per scene element: CC_TILES_EL_* */
    double origin[3], g0;
    uint32_t *order, norder, at; /* post-order */
    Clusters *cl;             /* per node until its parent used them */
    Buf blobs;
    LW *lw;                   /* per worker (cc_jobs): the leaves being read */
    size_t nlw;
    const uint8_t *mask;      /* SHELL1: per scene element, 0: left out of the proxies (a shell build) */
    int force_chain;          /* SHELL1: the chain's length to build (-1: until the root is small) */
};

#define CC_TILES_SAMPLES 4096u /* points kept per tile to measure its parent's error */

static double lattice_size(const cc_tiles_work *w, uint32_t e) {
    double g = w->g0;
    for (uint32_t i = 0; i < e; i++) g *= 2.0;
    return g;
}

static int lattice_key(const cc_tiles_work *w, const float p[3], uint32_t e, Key *k) {
    double g = lattice_size(w, e);
    for (int i = 0; i < 3; i++) {
        if (!(p[i] - p[i] == 0.0f)) return 0;
        double q = ((double)p[i] - w->origin[i]) / g;
        if (q >= 9.2e18) k->k[i] = INT64_MAX;
        else if (q <= -9.2e18) k->k[i] = INT64_MIN;
        else {
            int64_t t = (int64_t)q;
            k->k[i] = (double)t > q ? t - 1 : t;
        }
    }
    return 1;
}

static void work_free(cc_tiles_work *w) {
    if (!w) return;
    if (w->cl)
        for (uint32_t i = 0; i < w->nnodes; i++) cc_hg_clusters_free(&w->cl[i]);
    for (uint32_t i = 0; w->nodes && i < w->nnodes; i++) {
        free(w->nodes[i].smp);
        if (w->nodes[i].occ_heap) free(w->nodes[i].occ);
    }
    for (size_t i = 0; w->lw && i < w->nlw; i++) {
        LW *L = &w->lw[i];
        free(L->lsmp), cc_hg_grid_free(&L->g), free(L->shapes), free(L->shape_flag), free(L->touched), free(L->wpos), free(L->keys), free(L->has);
        arena_free(&L->dec), arena_free(&L->tmp);
    }
    free(w->lw);
    free(w->cl), free(w->nodes), free(w->els), free(w->box), free(w->tris), free(w->order), free(w->eflag);
    buf_free(&w->blobs);
    arena_free(&w->arena);
    free(w);
}

void cc_tiles_free(cc_tiles *t) {
    work_free(t->work);
    buf_free(&t->out);
    memset(t, 0, sizeof *t);
    buf_init(&t->out);
}

static uint32_t new_node(cc_tiles_work *w, uint32_t parent) {
    if (w->nnodes >= CC_TILES_MAX_TILES - 64) return CC_TILES_NONE;
    if (w->nnodes == w->cnodes) {
        size_t n = w->cnodes ? 2 * (size_t)w->cnodes : 256;
        Node *v = realloc(w->nodes, n * sizeof *v);
        if (!v) return CC_TILES_NONE;
        w->nodes = v, w->cnodes = (uint32_t)n;
    }
    Node *x = &w->nodes[w->nnodes];
    memset(x, 0, sizeof *x);
    x->parent = parent;
    return w->nnodes++;
}

static void node_box(cc_tiles_work *w, Node *x) {
    float b[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    x->detail = 0;
    for (uint32_t i = x->el0; i < x->el0 + x->nel; i++) {
        uint32_t e = w->els[i];
        x->detail += w->tris[e];
        for (int j = 0; j < 3; j++) {
            if (w->box[e][j] < b[j]) b[j] = w->box[e][j];
            if (w->box[e][3 + j] > b[3 + j]) b[3 + j] = w->box[e][3 + j];
        }
    }
    memcpy(x->box, b, sizeof b);
}

/* Morton key of a point in the root cube (10 bits an axis) */
static uint32_t morton(const cc_tiles_work *w, const float *bx, double side) {
    uint32_t m = 0;
    for (int k = 0; k < 3; k++) {
        double c = (((double)bx[k] + (double)bx[3 + k]) / 2.0 - w->origin[k]) / side * 1024.0;
        uint32_t q = c < 0 ? 0 : c >= 1023 ? 1023 : (uint32_t)c;
        for (int b = 0; b < 10; b++) m |= ((q >> b) & 1u) << (3 * b + k);
    }
    return m;
}

typedef struct {
    uint32_t m, e;
} MK;
static int mk_cmp(const void *a, const void *b) {
    const MK *x = a, *y = b;
    return x->m != y->m ? (x->m < y->m ? -1 : 1) : (x->e < y->e ? -1 : x->e > y->e);
}

typedef struct {
    uint32_t node, depth;
    uint64_t corner[3];
} Pend;

int cc_tiles_start(cc_tiles *t, const cc_tiles_src *src, const cc_tiles_opts *o, cc_error *err) {
    memset(t, 0, sizeof *t);
    buf_init(&t->out);
    cc_tiles_work *w = calloc(1, sizeof *w);
    if (!w) return fail(err, "out of memory");
    t->work = w;
    w->src = *src, w->o = *o, w->force_chain = -1;
    if (!w->o.leaf_tris) w->o.leaf_tris = 1;
    if (!w->o.proxy_tris) w->o.proxy_tris = 1;
    if (!w->o.res) w->o.res = 1;
    if (w->o.occ > CC_TILES_OCC_MAX) w->o.occ = CC_TILES_OCC_MAX;
    arena_init(&w->arena, 0);
    buf_init(&w->blobs);
    size_t ne = src_n(src), ns = src_nshapes(src);
    if (ne >= CC_TILES_MAX_TILES) return fail(err, "too many elements for tiles");
    w->box = malloc((ne + 1) * sizeof *w->box);
    w->tris = malloc((ne + 1) * sizeof *w->tris);
    w->els = malloc((ne + 1) * sizeof *w->els);
    w->eflag = calloc(ne + 1, 1);
    w->nlw = cc_jobs_threads();
    w->lw = calloc(w->nlw, sizeof *w->lw);
    for (size_t i = 0; w->lw && i < w->nlw; i++) {
        LW *L = &w->lw[i];
        arena_init(&L->dec, 0), arena_init(&L->tmp, 0);
        L->shapes = malloc((ns + 1) * sizeof *L->shapes);
        L->shape_flag = malloc(ns + 1);
        L->touched = malloc((ns + 1) * sizeof *L->touched);
        if (!L->shapes || !L->shape_flag || !L->touched) w->nlw = 0;
        else {
            memset(L->shape_flag, 0x80, ns + 1);
            for (size_t k = 0; k < ns; k++) L->shapes[k].nv = UINT32_MAX;
        }
    }
    uint32_t *tmp = malloc((ne + 1) * sizeof *tmp);
    uint8_t *oct = malloc(ne + 1);
    MK *mk = malloc((ne + 1) * sizeof *mk);
    Pend *stack = malloc((8 * (size_t)CC_TILES_MAX_DEPTH + 16) * sizeof *stack);
    int rc = -1;
    if (!w->box || !w->tris || !w->els || !w->eflag || !w->lw || !w->nlw || !tmp || !oct || !mk || !stack) {
        fail(err, "out of memory");
        goto done;
    }
    float all[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    Arena scratch;
    arena_init(&scratch, 0);
    for (size_t k = 0; k < ne; k++) {
        const cc_mesh *c = src_counts(src, k);
        arena_trim(&scratch, (size_t)1 << 22);
        if (!c || src_box(src, k, w->box[k], &scratch)) {
            arena_free(&scratch);
            fail(err, "a mesh does not decode");
            goto done;
        }
        w->tris[k] = c->ni / 3;
        const float *b = w->box[k];
        if (w->tris[k] && b[0] <= b[3] && b[1] <= b[4] && b[2] <= b[5] && finite3(b) && finite3(b + 3)) {
            w->els[w->nels++] = (uint32_t)k;
            t->total += w->tris[k];
            for (int j = 0; j < 3; j++) {
                if (b[j] < all[j]) all[j] = b[j];
                if (b[3 + j] > all[3 + j]) all[3 + j] = b[3 + j];
            }
        }
    }
    arena_free(&scratch);
    if (!w->nels) { /* nothing drawn: no tile */
        rc = 0;
        goto done;
    }
    double side = 1.0;
    for (int j = 0; j < 3; j++) {
        double d = (double)all[3 + j] - (double)all[j];
        if (d > side) side = d;
        w->origin[j] = all[j];
    }
    w->g0 = side / (double)w->o.res / (double)(1u << CC_TILES_MAX_DEPTH);
    /* the octree, top-down: a node's elements are its range of els; own (too large for a child) first */
    uint32_t r = new_node(w, CC_TILES_NONE);
    if (r == CC_TILES_NONE) { fail(err, "out of memory"); goto done; }
    w->root = r;
    w->nodes[r].el0 = 0, w->nodes[r].nel = w->nels;
    size_t sp = 0;
    stack[sp++] = (Pend){r, 0, {0, 0, 0}};
    while (sp) {
        Pend c = stack[--sp];
        Node *x = &w->nodes[c.node];
        node_box(w, x);
        x->cdepth = c.depth;
        for (;;) {
            if (x->detail <= w->o.leaf_tris || c.depth >= CC_TILES_MAX_DEPTH || x->nel <= 1) {
                x->flags |= CC_TILE_LEAF;
                break;
            }
            double h = side / (double)(1ull << (c.depth + 1)); /* a child's side */
            uint32_t cnt[9] = {0}, at[9];
            for (uint32_t i = 0; i < x->nel; i++) {
                const float *b = w->box[w->els[x->el0 + i]];
                unsigned oc = 0;
                double ext = 0;
                for (int k = 0; k < 3; k++) {
                    double ctr = ((double)b[k] + (double)b[3 + k]) / 2.0 - w->origin[k];
                    double mid = (double)(2 * c.corner[k] + 1) * h;
                    oc |= (unsigned)(ctr >= mid) << k;
                    if ((double)b[3 + k] - (double)b[k] > ext) ext = (double)b[3 + k] - (double)b[k];
                }
                if (ext > h) oc = 8; /* own: larger than a child (loose cube: twice the child) */
                oct[i] = (uint8_t)oc;
                cnt[oc]++;
            }
            int octs = 0;
            for (int k = 0; k < 8; k++) octs += cnt[k] > 0;
            if (!cnt[8] && octs == 1) { /* one octant only: the same node, one level down */
                int k = 0;
                while (!cnt[k]) k++;
                for (int q = 0; q < 3; q++) c.corner[q] = 2 * c.corner[q] + ((k >> q) & 1);
                c.depth++;
                x->cdepth = c.depth;
                continue;
            }
            /* own first (Morton order of their centres), then the octants */
            at[8] = 0;
            for (uint32_t k = 0, a = cnt[8]; k < 8; k++) at[k] = a, a += cnt[k];
            for (uint32_t i = 0; i < x->nel; i++) tmp[at[oct[i]]++] = w->els[x->el0 + i];
            memcpy(w->els + x->el0, tmp, x->nel * sizeof *tmp);
            for (uint32_t i = 0; i < cnt[8]; i++) mk[i].e = w->els[x->el0 + i], mk[i].m = morton(w, w->box[mk[i].e], side);
            qsort(mk, cnt[8], sizeof *mk, mk_cmp);
            for (uint32_t i = 0; i < cnt[8]; i++) w->els[x->el0 + i] = mk[i].e;
            if (!octs) { /* nothing fits a child: own leaves alone */
                uint64_t acc = 0;
                uint32_t chunks = 0;
                for (uint32_t i = 0; i < cnt[8]; i++) {
                    uint64_t tt = w->tris[w->els[x->el0 + i]];
                    if (i && acc + tt > w->o.leaf_tris) chunks++, acc = 0;
                    acc += tt;
                }
                if (!chunks) { x->flags |= CC_TILE_LEAF; break; }
            }
            /* children */
            uint32_t nk = 0;
            uint32_t maxk = cnt[8] + 8;
            uint32_t *kids = arena_alloc_n(&w->arena, maxk, sizeof *kids, 4);
            if (!kids) { fail(err, "out of memory"); goto done; }
            uint32_t me = (uint32_t)(x - w->nodes);
            /* own chunks */
            for (uint32_t i = 0; i < cnt[8];) {
                uint32_t j = i;
                uint64_t acc = 0;
                while (j < cnt[8] && (j == i || acc + w->tris[w->els[x->el0 + j]] <= w->o.leaf_tris)) acc += w->tris[w->els[x->el0 + j]], j++;
                uint32_t q = new_node(w, me);
                if (q == CC_TILES_NONE) { fail(err, "too many tiles"); goto done; }
                x = &w->nodes[me];
                Node *y = &w->nodes[q];
                y->el0 = x->el0 + i, y->nel = j - i, y->flags = CC_TILE_LEAF | CC_TILE_OWN, y->cdepth = c.depth + 1;
                node_box(w, y);
                kids[nk++] = q;
                i = j;
            }
            /* many own leaves: grouped 8 at a time under tiles of their own (their order kept), so that no tile has more
             * than 16 children (a parent's proxy and error read its children's clusters and points at once) */
            while (nk > 8) {
                uint32_t ng = (nk + 7) / 8;
                for (uint32_t g = 0; g < ng; g++) {
                    uint32_t q = new_node(w, me);
                    if (q == CC_TILES_NONE) { fail(err, "too many tiles"); goto done; }
                    x = &w->nodes[me];
                    uint32_t k0 = 8 * g, k1 = k0 + 8 < nk ? k0 + 8 : nk;
                    uint32_t *gk = arena_alloc_n(&w->arena, k1 - k0, sizeof *gk, 4);
                    if (!gk) { fail(err, "out of memory"); goto done; }
                    Node *y = &w->nodes[q];
                    y->el0 = w->nodes[kids[k0]].el0, y->nel = 0, y->cdepth = c.depth + 1;
                    for (uint32_t k = k0; k < k1; k++) gk[k - k0] = kids[k], w->nodes[kids[k]].parent = q, y->nel += w->nodes[kids[k]].nel;
                    y->kids = gk, y->nkids = k1 - k0;
                    node_box(w, y);
                    kids[g] = q;
                }
                nk = ng;
            }
            for (uint32_t k = 0, a = x->el0 + cnt[8]; k < 8; k++) {
                if (cnt[k]) {
                    uint32_t q = new_node(w, me);
                    if (q == CC_TILES_NONE) { fail(err, "too many tiles"); goto done; }
                    x = &w->nodes[me];
                    w->nodes[q].el0 = a, w->nodes[q].nel = cnt[k];
                    kids[nk++] = q;
                    Pend ch = {q, c.depth + 1, {0, 0, 0}};
                    for (int z = 0; z < 3; z++) ch.corner[z] = 2 * c.corner[z] + ((k >> z) & 1);
                    stack[sp++] = ch;
                }
                a += cnt[k];
            }
            x->kids = kids, x->nkids = nk;
            break;
        }
    }
    /* post-order (children before parent), the children in their order */
    w->order = malloc((size_t)w->nnodes * sizeof *w->order);
    w->cl = calloc((size_t)w->nnodes + 64, sizeof *w->cl);
    uint32_t *st = malloc(((size_t)w->nnodes + 1) * sizeof *st), *ix = calloc((size_t)w->nnodes + 1, sizeof *ix);
    if (!w->order || !w->cl || !st || !ix) {
        free(st), free(ix);
        fail(err, "out of memory");
        goto done;
    }
    size_t s2 = 0;
    st[s2++] = w->root;
    while (s2) {
        uint32_t n = st[s2 - 1];
        Node *x = &w->nodes[n];
        if (ix[n] < x->nkids) st[s2++] = x->kids[ix[n]++];
        else w->order[w->norder++] = n, s2--;
    }
    free(st), free(ix);
    rc = 0;
done:
    free(tmp), free(oct), free(mk), free(stack);
    return rc;
}

/* ---- per leaf: its elements' triangles clustered (for its parent), its counts, its occluders ---- */

static void occ_keep(Occ *set, uint32_t *n, uint32_t max, const Occ *c) {
    if (!max) return;
    if (*n < max) {
        set[(*n)++] = *c;
        return;
    }
    uint32_t lo = 0; /* the smallest kept (area, then the later element and triangle) */
    for (uint32_t i = 1; i < *n; i++) {
        const Occ *a = &set[i], *b = &set[lo];
        if (a->area < b->area || (a->area == b->area && (a->el > b->el || (a->el == b->el && a->tri > b->tri)))) lo = i;
    }
    const Occ *b = &set[lo];
    if (c->area > b->area || (c->area == b->area && (c->el < b->el || (c->el == b->el && c->tri < b->tri)))) set[lo] = *c;
}

static int occ_cmp(const void *x, const void *y) {
    const Occ *a = x, *b = y;
    if (a->area != b->area) return a->area > b->area ? -1 : 1;
    if (a->el != b->el) return a->el < b->el ? -1 : 1;
    return a->tri < b->tri ? -1 : a->tri > b->tri;
}

static int occ_store(cc_tiles_work *w, Node *x, Occ *set, uint32_t n) {
    qsort(set, n, sizeof *set, occ_cmp);
    x->occ = n ? arena_alloc_n(&w->arena, n, sizeof *set, 8) : NULL;
    if (n && !x->occ) return -1;
    if (n) memcpy(x->occ, set, n * sizeof *set);
    x->nocc = n;
    return 0;
}

static double extent(const float *b) {
    double e = 0;
    for (int k = 0; k < 3; k++)
        if ((double)b[3 + k] - (double)b[k] > e) e = (double)b[3 + k] - (double)b[k];
    return e;
}

static int lsmp_add(LW *L, const float *p, double size);

static int u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
typedef struct {
    float p[3];
    uint32_t i;
} WeldV;
static int weld_cmp(const void *a, const void *b) {
    const WeldV *x = a, *y = b;
    int c = memcmp(x->p, y->p, 12);
    return c ? c : (x->i < y->i ? -1 : x->i > y->i);
}
/* Whether a mesh is closed and outward (in its own frame): vertices welded by position, every directed edge
 * met once by its reverse and never twice, the signed volume positive. */
static int closed_outward(const cc_mesh *m, Arena *a) {
    uint32_t nt = m->ni / 3;
    if (!nt || !m->nv) return 0;
    WeldV *ord = arena_alloc_n(a, m->nv, sizeof *ord, 4);
    uint32_t *wid = arena_alloc_n(a, m->nv, sizeof *wid, 4);
    uint64_t *e = arena_alloc_n(a, 3 * (size_t)nt, sizeof *e, 8);
    if (!ord || !wid || !e) return 0;
    for (uint32_t i = 0; i < m->nv; i++) memcpy(ord[i].p, m->pos[i], 12), ord[i].i = i;
    qsort(ord, m->nv, sizeof *ord, weld_cmp);
    for (uint32_t i = 0, id = 0; i < m->nv; i++) {
        if (i && memcmp(ord[i].p, ord[i - 1].p, 12)) id++;
        wid[ord[i].i] = id;
    }
    size_t ne = 0;
    double vol = 0;
    for (uint32_t t = 0; t < nt; t++) {
        uint32_t v[3];
        for (int k = 0; k < 3; k++) {
            if (m->idx[3 * t + k] >= m->nv) return 0;
            v[k] = wid[m->idx[3 * t + k]];
        }
        if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;
        for (int k = 0; k < 3; k++) e[ne++] = (uint64_t)v[k] << 32 | v[(k + 1) % 3];
        const float *p = m->pos[m->idx[3 * t]], *q = m->pos[m->idx[3 * t + 1]], *r = m->pos[m->idx[3 * t + 2]];
        vol += (double)p[0] * ((double)q[1] * r[2] - (double)q[2] * r[1]) - (double)p[1] * ((double)q[0] * r[2] - (double)q[2] * r[0]) +
               (double)p[2] * ((double)q[0] * r[1] - (double)q[1] * r[0]);
    }
    if (!ne || !(vol > 0)) return 0;
    qsort(e, ne, sizeof *e, u64_cmp);
    for (size_t i = 0; i < ne; i++) {
        if (i && e[i] == e[i - 1]) return 0;
        uint64_t r = (e[i] & 0xFFFFFFFFull) << 32 | e[i] >> 32;
        size_t lo = 0, hi = ne;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (e[mid] < r) lo = mid + 1;
            else hi = mid;
        }
        if (lo == ne || e[lo] != r) return 0;
    }
    return 1;
}

/* element el's triangles into the leaf's grid; returns -1 on a decode or memory failure */
/* An opaque element's occluder candidates: its triangles grouped by plane; a plane's triangles that fill their rectangle
 * exactly (a face cut in a grid, not a face with a window) stand as that rectangle (two triangles), the others as
 * themselves; each kept when its area reaches `amin`. Positions are L->wpos (placed). */
typedef struct {
    int64_t k[4]; /* the plane: normal x 4096, offset (mm) */
    uint32_t t;
    double area;
} PlaneT;
static int planet_cmp(const void *a, const void *b) {
    const PlaneT *x = a, *y = b;
    for (int i = 0; i < 4; i++)
        if (x->k[i] != y->k[i]) return x->k[i] < y->k[i] ? -1 : 1;
    return x->t < y->t ? -1 : x->t > y->t;
}

static int occ_element(LW *L, const cc_mesh *m, int mir, uint32_t el, double amin) {
    uint32_t nt = m->ni / 3;
    PlaneT *pt = arena_alloc_n(&L->tmp, (size_t)nt + 1, sizeof *pt, 8);
    if (!pt) return -1;
    uint32_t n = 0;
    for (uint32_t t = 0; t < nt; t++) {
        uint32_t a = m->idx[3 * t], b = m->idx[3 * t + (mir ? 2 : 1)], c = m->idx[3 * t + (mir ? 1 : 2)];
        if (a >= m->nv || b >= m->nv || c >= m->nv || !finite3(L->wpos[a]) || !finite3(L->wpos[b]) || !finite3(L->wpos[c])) continue;
        double u[3], v[3], q[3];
        for (int k = 0; k < 3; k++) u[k] = (double)L->wpos[b][k] - L->wpos[a][k], v[k] = (double)L->wpos[c][k] - L->wpos[a][k];
        q[0] = u[1] * v[2] - u[2] * v[1], q[1] = u[2] * v[0] - u[0] * v[2], q[2] = u[0] * v[1] - u[1] * v[0];
        double l = cc_sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        if (!(l > 0)) continue;
        PlaneT *x = &pt[n++];
        for (int k = 0; k < 3; k++) x->k[k] = (int64_t)(q[k] / l * 4096.0 + (q[k] >= 0 ? 0.5 : -0.5));
        double d = (q[0] * L->wpos[a][0] + q[1] * L->wpos[a][1] + q[2] * L->wpos[a][2]) / l;
        x->k[3] = (int64_t)(d + (d >= 0 ? 0.5 : -0.5));
        x->t = t, x->area = l / 2;
    }
    qsort(pt, n, sizeof *pt, planet_cmp);
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i;
        double sum = 0;
        while (j < n && !memcmp(pt[j].k, pt[i].k, sizeof pt[i].k)) sum += pt[j].area, j++;
        int rect = 0;
        if (j - i > 1) { /* the plane's frame: the first triangle's first edge and the normal */
            uint32_t t0 = pt[i].t, a = m->idx[3 * t0], b = m->idx[3 * t0 + (mir ? 2 : 1)];
            double nn[3] = {pt[i].k[0] / 4096.0, pt[i].k[1] / 4096.0, pt[i].k[2] / 4096.0}, u[3], v[3];
            for (int k = 0; k < 3; k++) u[k] = (double)L->wpos[b][k] - L->wpos[a][k];
            double ul = cc_sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            for (int k = 0; k < 3; k++) u[k] /= ul;
            v[0] = nn[1] * u[2] - nn[2] * u[1], v[1] = nn[2] * u[0] - nn[0] * u[2], v[2] = nn[0] * u[1] - nn[1] * u[0];
            double vl = cc_sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (ul > 0 && vl > 0) {
                for (int k = 0; k < 3; k++) v[k] /= vl;
                double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300}, dd = 0;
                const float *o = L->wpos[a];
                for (uint32_t q = i; q < j; q++)
                    for (int c = 0; c < 3; c++) {
                        const float *p = L->wpos[m->idx[3 * pt[q].t + c]];
                        double r[3] = {(double)p[0] - o[0], (double)p[1] - o[1], (double)p[2] - o[2]};
                        double x = r[0] * u[0] + r[1] * u[1] + r[2] * u[2], y = r[0] * v[0] + r[1] * v[1] + r[2] * v[2];
                        double z = r[0] * nn[0] + r[1] * nn[1] + r[2] * nn[2];
                        lo[0] = x < lo[0] ? x : lo[0], hi[0] = x > hi[0] ? x : hi[0], lo[1] = y < lo[1] ? y : lo[1], hi[1] = y > hi[1] ? y : hi[1];
                        dd = (z < 0 ? -z : z) > dd ? (z < 0 ? -z : z) : dd;
                    }
                double ra = (hi[0] - lo[0]) * (hi[1] - lo[1]);
                if (ra > 0 && sum >= 0.9995 * ra && sum <= 1.0005 * ra && dd < 1.0 && ra / 2 >= amin) { /* it fills its rectangle */
                    rect = 1;
                    double cs[4][3];
                    for (int c = 0; c < 4; c++)
                        for (int k = 0; k < 3; k++) cs[c][k] = o[k] + (c & 1 ? hi[0] : lo[0]) * u[k] + (c & 2 ? hi[1] : lo[1]) * v[k];
                    static const int TR[2][3] = {{0, 1, 3}, {0, 3, 2}};
                    for (int h = 0; h < 2; h++) {
                        Occ oc;
                        for (int c = 0; c < 3; c++)
                            for (int k = 0; k < 3; k++) oc.t[3 * c + k] = (float)cs[TR[h][c]][k];
                        oc.area = ra / 2, oc.el = el, oc.tri = pt[i].t * 2 + (uint32_t)h;
                        occ_keep(L->occ, &L->nocc, L->maxocc, &oc);
                    }
                }
            }
        }
        for (uint32_t q = i; !rect && q < j; q++) { /* the triangles themselves */
            if (pt[q].area < amin) continue;
            uint32_t t = pt[q].t, a = m->idx[3 * t], b = m->idx[3 * t + (mir ? 2 : 1)], c = m->idx[3 * t + (mir ? 1 : 2)];
            Occ oc;
            memcpy(oc.t, L->wpos[a], 12), memcpy(oc.t + 3, L->wpos[b], 12), memcpy(oc.t + 6, L->wpos[c], 12);
            oc.area = pt[q].area, oc.el = el, oc.tri = t * 2 + 1;
            occ_keep(L->occ, &L->nocc, L->maxocc, &oc);
        }
        i = j;
    }
    return 0;
}

static int leaf_element(cc_tiles_work *w, LW *L, Node *x, uint32_t el) {
    const cc_tiles_src *s = &w->src;
    int sh = src_shape(s, el);
    cc_mesh m;
    if (sh >= 0) {
        if ((size_t)sh >= src_nshapes(s)) return -1;
        if (L->shapes[sh].nv == UINT32_MAX) {
            if (src_block(s, (size_t)sh, &L->dec, &L->shapes[sh])) return -1;
            L->touched[L->ntouched++] = (uint32_t)sh;
            L->leaf_verts += L->shapes[sh].nv;
        }
        m = L->shapes[sh];
        if (L->shape_flag[sh] & 0x80) {
            arena_trim(&L->tmp, (size_t)1 << 24);
            L->shape_flag[sh] = (uint8_t)closed_outward(&m, &L->tmp);
        }
        w->eflag[el] = L->shape_flag[sh] ? CC_TILES_EL_CLOSED : 0;
    } else {
        arena_trim(&L->tmp, (size_t)1 << 24);
        if (src_block(s, src_nshapes(s) + el, &L->tmp, &m)) return -1;
        L->leaf_verts += m.nv;
        w->eflag[el] = closed_outward(&m, &L->tmp) ? CC_TILES_EL_CLOSED : 0;
    }
    if (opaque(s, el)) w->eflag[el] |= CC_TILES_EL_OPAQUE;
    if (m.nv > L->cwpos) {
        float(*p)[3] = realloc(L->wpos, (size_t)m.nv * sizeof *p);
        if (p) L->wpos = p;
        Key *k = realloc(L->keys, (size_t)m.nv * sizeof *k);
        if (k) L->keys = k;
        uint8_t *h = realloc(L->has, m.nv);
        if (h) L->has = h;
        if (!p || !k || !h) return -1;
        L->cwpos = m.nv;
    }
    const cc_placement32 *pl = src_pl(s, el);
    int mir = sh >= 0 && mirrors(pl);
    for (uint32_t i = 0; i < m.nv; i++) {
        if (sh >= 0) place(pl, m.pos[i], L->wpos[i]);
        else memcpy(L->wpos[i], m.pos[i], 12);
        L->has[i] = (uint8_t)lattice_key(w, L->wpos[i], L->g.e, &L->keys[i]);
    }
    Fam *f = &L->g.f[src_cat(s, el)];
    uint32_t e0 = L->g.e; /* the grid the keys were made on (a fit below may coarsen it: keys moved up by the difference) */
    /* pass 1: each used vertex's quadric (its faces' planes, area weighted); pass 2: each used vertex once (an unused one
     * would only move a cluster's mean); pass 3: the triangles */
    double(*vq)[10] = arena_alloc_n(&L->tmp, (size_t)m.nv + 1, sizeof *vq, 8);
    uint8_t *used = arena_alloc_n(&L->tmp, (size_t)m.nv + 1, 1, 1);
    if (!used || !vq) return -1;
    memset(used, 0, (size_t)m.nv + 1);
    memset(vq, 0, ((size_t)m.nv + 1) * sizeof *vq);
    for (uint32_t t = 0; t / 3 < m.ni / 3; t += 3) {
        uint32_t a = m.idx[t], b = m.idx[t + 1], c = m.idx[t + 2];
        if (a >= m.nv || b >= m.nv || c >= m.nv || !L->has[a] || !L->has[b] || !L->has[c]) continue;

        double u[3], v[3], n[3];
        for (int q = 0; q < 3; q++) u[q] = (double)L->wpos[b][q] - L->wpos[a][q], v[q] = (double)L->wpos[c][q] - L->wpos[a][q];
        n[0] = u[1] * v[2] - u[2] * v[1], n[1] = u[2] * v[0] - u[0] * v[2], n[2] = u[0] * v[1] - u[1] * v[0];
        double l = cc_sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        used[a] = used[b] = used[c] = 1;
#ifdef TILES_NOQ
        continue;
#endif
        if (!(l > 0)) continue;
        double area = l / 2, pa = n[0] / l, pb = n[1] / l, pc = n[2] / l, pd = -(pa * L->wpos[a][0] + pb * L->wpos[a][1] + pc * L->wpos[a][2]);
        double Q[10] = {pa * pa, pa * pb, pa * pc, pa * pd, pb * pb, pb * pc, pb * pd, pc * pc, pc * pd, pd * pd};
        for (int z = 0; z < 3; z++) {
            uint32_t x3 = z == 0 ? a : z == 1 ? b : c;
            for (int k = 0; k < 10; k++) vq[x3][k] += area * Q[k];
        }
    }
    for (uint32_t i = 0; i < m.nv; i++)
        if (used[i]) {
            double sm[3] = {L->wpos[i][0], L->wpos[i][1], L->wpos[i][2]};
            if (cc_hg_fam_vert(f, L->keys[i], sm, 1, NULL, vq[i])) return -1;
        }
    int opq = opaque(s, el);
    double esize; /* its middle extent */
    {
        double e[3];
        for (int k = 0; k < 3; k++) e[k] = (double)w->box[el][3 + k] - (double)w->box[el][k];
        double lo = e[0] < e[1] ? e[0] : e[1], hi = e[0] < e[1] ? e[1] : e[0];
        esize = e[2] < lo ? lo : e[2] > hi ? hi : e[2];
    }
    double amin = extent(x->box) / 64.0;
    amin *= amin;
    for (uint32_t t = 0; t / 3 < m.ni / 3; t += 3) {
        uint32_t a = m.idx[t], b = m.idx[t + (mir ? 2 : 1)], c = m.idx[t + (mir ? 1 : 2)];
        if (a >= m.nv || b >= m.nv || c >= m.nv) continue;
        int ok = L->has[a] && L->has[b] && L->has[c];
        unsigned sh2 = L->g.e - e0;
        if (ok && cc_hg_fam_tri(f, cc_hg_up(L->keys[a], sh2), cc_hg_up(L->keys[b], sh2), cc_hg_up(L->keys[c], sh2))) return -1;
        { /* the leaf's points: its first corner and its centre */
            float ctr[3];
            for (int k = 0; k < 3; k++) ctr[k] = (L->wpos[a][k] + L->wpos[b][k] + L->wpos[c][k]) / 3.0f;
            if ((finite3(L->wpos[a]) && lsmp_add(L, L->wpos[a], esize)) || (finite3(ctr) && lsmp_add(L, ctr, esize))) return -1;
        }
        if ((t / 3) % 4096 == 4095 && cc_hg_grid_fit(&L->g, w->o.proxy_tris, 0)) return -1;
    }
    if (opq && occ_element(L, &m, mir, el, amin)) return -1;
    return cc_hg_grid_fit(&L->g, w->o.proxy_tris, 0);
}

/* ---- a cluster's point: where its quadric is least (Lindstrom 2000), kept in its members' box ---- */

/* the eigen decomposition of a symmetric 3x3 (Jacobi, fixed sweeps: deterministic) */
static void eig3(double a[3][3], double v[3][3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) v[i][j] = i == j;
    for (int sweep = 0; sweep < 12; sweep++)
        for (int p = 0; p < 2; p++)
            for (int q = p + 1; q < 3; q++) {
                if (a[p][q] == 0) continue;
                double th = (a[q][q] - a[p][p]) / (2 * a[p][q]), t = (th >= 0 ? 1 : -1) / ((th >= 0 ? th : -th) + cc_sqrt(th * th + 1));
                double c = 1 / cc_sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; k++) {
                    double x = a[k][p], y = a[k][q];
                    a[k][p] = c * x - s * y, a[k][q] = s * x + c * y;
                }
                for (int k = 0; k < 3; k++) {
                    double x = a[p][k], y = a[q][k];
                    a[p][k] = c * x - s * y, a[q][k] = s * x + c * y;
                }
                for (int k = 0; k < 3; k++) {
                    double x = v[k][p], y = v[k][q];
                    v[k][p] = c * x - s * y, v[k][q] = s * x + c * y;
                }
            }
}

static void rep(const Vert *x, double out[3]) {
    double n = (double)(x->n ? x->n : 1), m[3];
    for (int k = 0; k < 3; k++) m[k] = x->s[k] / n, out[k] = m[k];
    const double *q = x->q;
    double A[3][3] = {{q[0], q[1], q[2]}, {q[1], q[4], q[5]}, {q[2], q[5], q[7]}}, b[3] = {q[3], q[6], q[8]}, V[3][3];
    double r[3]; /* -(A m + b): the step from the mean */
    for (int i = 0; i < 3; i++) r[i] = -(A[i][0] * m[0] + A[i][1] * m[1] + A[i][2] * m[2] + b[i]);
    eig3(A, V);
    double lmax = 0;
    for (int i = 0; i < 3; i++) lmax = A[i][i] > lmax ? A[i][i] : lmax;
    if (!(lmax > 0)) return;
    for (int i = 0; i < 3; i++) { /* the pseudo inverse: directions of a flat or an edge left at the mean */
        if (!(A[i][i] > 1e-3 * lmax)) continue;
        double d = (V[0][i] * r[0] + V[1][i] * r[1] + V[2][i] * r[2]) / A[i][i];
        for (int k = 0; k < 3; k++) out[k] += d * V[k][i];
    }
    for (int k = 0; k < 3; k++) { /* within its members' box */
        if (!(out[k] >= x->lo[k])) out[k] = x->lo[k];
        if (!(out[k] <= x->hi[k])) out[k] = x->hi[k];
        if (!(out[k] - out[k] == 0)) out[k] = m[k];
    }
}

/* ---- the error: a sampled two-sided distance between a proxy and the level below ---- */

static int lsmp_add(LW *L, const float *p, double size) { /* every lstride-th point of the leaf's content */
    if (L->lcount++ % L->lstride) return 0;
    if (L->nlsmp == L->clsmp) {
        uint32_t c = L->clsmp ? 2 * L->clsmp : 1024;
        double(*v)[4] = realloc(L->lsmp, (size_t)c * sizeof *v);
        if (!v) return -1;
        L->lsmp = v, L->clsmp = c;
    }
    for (int k = 0; k < 3; k++) L->lsmp[L->nlsmp][k] = p[k];
    L->lsmp[L->nlsmp][3] = size;
    L->nlsmp++;
    return 0;
}

typedef struct { /* a uniform grid of triangles for nearest queries; per worker a stamp array (an item seen once a query) */
    double lo[3], h;
    int n[3];
    uint32_t *start, *items, nitems;
    uint32_t *stamp, *mark; /* nw x nitems, nw */
    size_t nw;
} UGrid;

static void ug_free(UGrid *g) { free(g->start), free(g->items), free(g->stamp), free(g->mark); }

static void ug_cell(const UGrid *g, const double *p, int c[3]) {
    for (int k = 0; k < 3; k++) {
        double q = (p[k] - g->lo[k]) / g->h;
        c[k] = q < 0 ? 0 : q >= g->n[k] ? g->n[k] - 1 : (int)q;
    }
}

/* items: n boxes box(i, lo, hi) */
static int ug_build(UGrid *g, uint32_t n, void (*box)(const void *, uint32_t, double *, double *), const void *ctx) {
    memset(g, 0, sizeof *g);
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300}, a[3], b[3];
    for (uint32_t i = 0; i < n; i++) {
        box(ctx, i, a, b);
        for (int k = 0; k < 3; k++) lo[k] = a[k] < lo[k] ? a[k] : lo[k], hi[k] = b[k] > hi[k] ? b[k] : hi[k];
    }
    double ext = 0;
    for (int k = 0; k < 3; k++) ext = hi[k] - lo[k] > ext ? hi[k] - lo[k] : ext;
    g->h = ext / 32 > 1 ? ext / 32 : 1;
    for (int k = 0; k < 3; k++) g->lo[k] = lo[k], g->n[k] = n ? 1 + (int)((hi[k] - lo[k]) / g->h) : 1;
    for (int k = 0; k < 3; k++) if (g->n[k] > 33) g->n[k] = 33;
    size_t nc = (size_t)g->n[0] * g->n[1] * g->n[2];
    g->start = calloc(nc + 1, sizeof *g->start);
    g->nitems = n, g->nw = cc_jobs_workers(1u << 30);
    g->stamp = calloc(((size_t)n + 1) * g->nw, sizeof *g->stamp);
    g->mark = calloc(g->nw, sizeof *g->mark);
    if (!g->start || !g->stamp || !g->mark) return -1;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < n; i++) {
            int c0[3], c1[3];
            box(ctx, i, a, b), ug_cell(g, a, c0), ug_cell(g, b, c1);
            for (int z = c0[2]; z <= c1[2]; z++)
                for (int y = c0[1]; y <= c1[1]; y++)
                    for (int x = c0[0]; x <= c1[0]; x++) {
                        size_t c = ((size_t)z * g->n[1] + y) * g->n[0] + x;
                        if (!pass) g->start[c + 1]++;
                        else g->items[g->start[c]++] = i;
                    }
        }
        if (!pass) {
            for (size_t c = 0; c < nc; c++) g->start[c + 1] += g->start[c];
            g->items = malloc(((size_t)g->start[nc] + 1) * sizeof *g->items);
            if (!g->items) return -1;
        } else { /* start[c] moved to the next cell's start: back */
            for (size_t c = nc; c > 0; c--) g->start[c] = g->start[c - 1];
            g->start[0] = 0;
        }
    }
    return 0;
}

static double pt_tri(const double *p, const double *a, const double *b, const double *c) { /* Ericson 5.1.5 */
    double ab[3], ac[3], ap[3], bp[3], cp[3], q[3];
    for (int k = 0; k < 3; k++) ab[k] = b[k] - a[k], ac[k] = c[k] - a[k], ap[k] = p[k] - a[k], bp[k] = p[k] - b[k], cp[k] = p[k] - c[k];
#define D3(u, v) (u[0] * v[0] + u[1] * v[1] + u[2] * v[2])
    double d1 = D3(ab, ap), d2 = D3(ac, ap), d3 = D3(ab, bp), d4 = D3(ac, bp), d5 = D3(ab, cp), d6 = D3(ac, cp);
    double va = d3 * d6 - d5 * d4, vb = d5 * d2 - d1 * d6, vc = d1 * d4 - d3 * d2;
    if (d1 <= 0 && d2 <= 0) memcpy(q, a, 24);
    else if (d3 >= 0 && d4 <= d3) memcpy(q, b, 24);
    else if (d6 >= 0 && d5 <= d6) memcpy(q, c, 24);
    else if (vc <= 0 && d1 >= 0 && d3 <= 0) { double v = d1 / (d1 - d3); for (int k = 0; k < 3; k++) q[k] = a[k] + v * ab[k]; }
    else if (vb <= 0 && d2 >= 0 && d6 <= 0) { double v = d2 / (d2 - d6); for (int k = 0; k < 3; k++) q[k] = a[k] + v * ac[k]; }
    else if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) { double v = (d4 - d3) / ((d4 - d3) + (d5 - d6)); for (int k = 0; k < 3; k++) q[k] = b[k] + v * (c[k] - b[k]); }
    else {
        double den = va + vb + vc, v = den != 0 ? vb / den : 0, u = den != 0 ? vc / den : 0;
        for (int k = 0; k < 3; k++) q[k] = a[k] + v * ab[k] + u * ac[k];
    }
    double d[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
    return D3(d, d);
#undef D3
}

typedef struct { /* a proxy as a mesh in doubles */
    double (*v)[3];
    uint32_t (*t)[3];
    uint32_t nv, nt;
} PM;
static void pm_tbox(const void *ctx, uint32_t i, double *lo, double *hi) {
    const PM *m = ctx;
    for (int k = 0; k < 3; k++) {
        const double *a = m->v[m->t[i][0]], *b = m->v[m->t[i][1]], *c = m->v[m->t[i][2]];
        lo[k] = a[k] < b[k] ? (a[k] < c[k] ? a[k] : c[k]) : (b[k] < c[k] ? b[k] : c[k]);
        hi[k] = a[k] > b[k] ? (a[k] > c[k] ? a[k] : c[k]) : (b[k] > c[k] ? b[k] : c[k]);
    }
}
typedef struct {
    double (*p)[4];
    uint32_t n;
} PS;

/* the squared distance from p to the nearest item (rings of cells until no nearer can be) */
static double ug_near(UGrid *g, size_t worker, const double *p, const PM *m, double cap2) { /* cap2: nothing farther matters */
    int c[3];
    ug_cell(g, p, c);
    double best = 1e300;
    uint32_t *stamp = g->stamp + ((size_t)g->nitems + 1) * worker;
    if (++g->mark[worker] == 0) { /* wrapped: start over */
        memset(stamp, 0, ((size_t)g->nitems + 1) * sizeof *stamp);
        g->mark[worker] = 1;
    }
    uint32_t mark = g->mark[worker];
    int maxr = g->n[0] > g->n[1] ? g->n[0] : g->n[1];
    maxr = maxr > g->n[2] ? maxr : g->n[2];
    for (int r = 0; r <= maxr; r++) {
        for (int z = c[2] - r; z <= c[2] + r; z++)
            for (int y = c[1] - r; y <= c[1] + r; y++)
                for (int x = c[0] - r; x <= c[0] + r; x++) {
                    if (x < 0 || y < 0 || z < 0 || x >= g->n[0] || y >= g->n[1] || z >= g->n[2]) continue;
                    if (r && x != c[0] - r && x != c[0] + r && y != c[1] - r && y != c[1] + r && z != c[2] - r && z != c[2] + r) continue;
                    size_t cell = ((size_t)z * g->n[1] + y) * g->n[0] + x;
                    for (uint32_t q = g->start[cell]; q < g->start[cell + 1]; q++) {
                        uint32_t i = g->items[q];
                        if (stamp[i] == mark) continue;
                        stamp[i] = mark;
                        double d = pt_tri(p, m->v[m->t[i][0]], m->v[m->t[i][1]], m->v[m->t[i][2]]);
                        if (d < best) best = d;
                    }
                }
        double reach = r * g->h;
        if (best <= reach * reach || cap2 <= reach * reach) break;
    }
    return best;
}

typedef struct { /* the largest squared distance of points to a grid's triangles, in chunks (one slot each) */
    UGrid *g;
    const PM *m;
    double (*p)[4];
    uint32_t n;
    int capped; /* a point's 4th value caps its squared distance (a vanished part's size) */
    double *slot;
} QJ;
#define QJ_CHUNK 256u

static void query_job(void *ctx, size_t task, size_t worker) {
    QJ *q = ctx;
    double mx = 0;
    for (uint32_t i = (uint32_t)task * QJ_CHUNK; i < q->n && i < (uint32_t)(task + 1) * QJ_CHUNK; i++) {
        double cap2 = q->capped ? q->p[i][3] * q->p[i][3] : 1e300;
        double d = ug_near(q->g, worker, q->p[i], q->m, cap2);
        if (q->capped && d > q->p[i][3] * q->p[i][3]) d = q->p[i][3] * q->p[i][3]; /* a part that vanished: its omission costs its size */
        if (d > mx) mx = d;
    }
    q->slot[task] = mx;
}

static int query_max(UGrid *g, const PM *m, double (*p)[4], uint32_t n, int capped, double *out) {
    size_t nt = (n + QJ_CHUNK - 1) / QJ_CHUNK;
    double *slot = calloc(nt + 1, sizeof *slot);
    if (!slot) return -1;
    QJ q = {g, m, p, n, capped, slot};
    cc_jobs_run(nt, query_job, &q);
    for (size_t i = 0; i < nt; i++)
        if (slot[i] > *out) *out = slot[i];
    free(slot);
    return 0;
}

/* The proxy `cl` against its children's samples: max over the children's points of the distance to the proxy's
 * triangles, and over the proxy's points of the distance to the nearest child point; its own samples set (x->smp). */
static int pm_from(const Clusters *const *cls, uint32_t ncl, PM *m) { /* clusters as one mesh (vertices at their means) */
    m->nv = m->nt = 0;
    for (uint32_t c = 0; c < ncl; c++)
        for (int i = 0; i < CC_CAT_COUNT; i++) m->nv += (uint32_t)cls[c]->p[i].nv, m->nt += (uint32_t)cls[c]->p[i].nt;
    m->v = malloc(((size_t)m->nv + 1) * sizeof *m->v), m->t = malloc(((size_t)m->nt + 1) * sizeof *m->t);
    if (!m->v || !m->t) return -1;
    m->nv = m->nt = 0;
    for (uint32_t c = 0; c < ncl; c++)
        for (int i = 0; i < CC_CAT_COUNT; i++) {
            const Part *p = &cls[c]->p[i];
            for (size_t v = 0; v < p->nv; v++) rep(&p->v[v], m->v[m->nv + v]);
            for (size_t t = 0; t < p->nt; t++)
                for (int k = 0; k < 3; k++) m->t[m->nt + t][k] = m->nv + p->t[t][k];
            m->nv += (uint32_t)p->nv, m->nt += (uint32_t)p->nt;
        }
    return 0;
}

/* children: their clusters (the level below as meshes: a leaf's at its fine grid, a proxy's its proxy) */
static double proxy_error(cc_tiles_work *w, const Clusters *cl, const Clusters *const *kids, uint32_t nk, uint32_t n, int *bad) {
    Node *x = &w->nodes[n];

    PM m = {0}, km = {0};
    PS ch = {0}, own = {0};
    double err = 0;
    if (pm_from(kids, nk, &km)) goto oom;
    for (int i = 0; i < CC_CAT_COUNT; i++) m.nv += (uint32_t)cl->p[i].nv, m.nt += (uint32_t)cl->p[i].nt;
    for (uint32_t k = 0; k < x->nkids; k++) ch.n += w->nodes[x->kids[k]].nsmp;
    m.v = malloc(((size_t)m.nv + 1) * sizeof *m.v), m.t = malloc(((size_t)m.nt + 1) * sizeof *m.t);
    ch.p = malloc(((size_t)ch.n + 1) * sizeof *ch.p);
    uint64_t total = (uint64_t)m.nv + m.nt, stride = total / CC_TILES_SAMPLES + 1;
    own.p = malloc(((size_t)(total / stride) + 2) * sizeof *own.p);
    if (!m.v || !m.t || !ch.p || !own.p) goto oom;
    m.nv = m.nt = 0;
    for (int i = 0; i < CC_CAT_COUNT; i++) {
        const Part *p = &cl->p[i];
        for (size_t v = 0; v < p->nv; v++) {
            double d = (double)(p->v[v].n ? p->v[v].n : 1);
            for (int k = 0; k < 3; k++) m.v[m.nv + v][k] = p->v[v].s[k] / d;
        }
        for (size_t t = 0; t < p->nt; t++)
            for (int k = 0; k < 3; k++) m.t[m.nt + t][k] = m.nv + p->t[t][k];
        m.nv += (uint32_t)p->nv, m.nt += (uint32_t)p->nt;
    }
    uint32_t all = ch.n, cstride = all / (8 * CC_TILES_SAMPLES) + 1; /* at most 8 children's worth of points */
    ch.n = 0;
    double cerr = 0;
    for (uint32_t k = 0, at = 0; k < x->nkids; k++) {
        Node *c = &w->nodes[x->kids[k]];
        for (uint32_t i = 0; i < c->nsmp; i++, at++)
            if (at % cstride == 0) memcpy(ch.p[ch.n++], c->smp[i], sizeof *ch.p);
        if (!(c->flags & CC_TILE_LEAF) && c->error > cerr) cerr = c->error;
    }
    for (uint64_t i = 0, q = 0; i < total; i++) { /* the proxy's own points: vertices, then triangle centres */
        if (i % stride) continue;
        if (i < m.nv) memcpy(own.p[q], m.v[i], 24);
        else {
            const uint32_t *t = m.t[i - m.nv];
            for (int k = 0; k < 3; k++) own.p[q][k] = (m.v[t[0]][k] + m.v[t[1]][k] + m.v[t[2]][k]) / 3.0;
        }
        own.p[q][3] = 1e150;
        own.n = (uint32_t)++q;
    }
    double d2 = 0;
    if (m.nt && ch.n) {
        UGrid g;
        if (ug_build(&g, m.nt, pm_tbox, &m)) { ug_free(&g); goto oom; }
        if (query_max(&g, &m, ch.p, ch.n, 1, &d2)) { ug_free(&g); goto oom; }
        ug_free(&g);
        if (km.nt) { /* the proxy's points to the level below's triangles */
            if (ug_build(&g, km.nt, pm_tbox, &km)) { ug_free(&g); goto oom; }
            if (query_max(&g, &km, own.p, own.n, 0, &d2)) { ug_free(&g); goto oom; }
            ug_free(&g);
        }
    } else if (ch.n) { /* an empty proxy: everything vanished: the farthest child point from the box's centre */
        double ctr[3] = {(x->box[0] + x->box[3]) / 2.0, (x->box[1] + x->box[4]) / 2.0, (x->box[2] + x->box[5]) / 2.0};
        for (uint32_t i = 0; i < ch.n; i++) {
            double e[3] = {ch.p[i][0] - ctr[0], ch.p[i][1] - ctr[1], ch.p[i][2] - ctr[2]}, d = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
            if (d > d2) d2 = d;
        }
    }
    err = cc_sqrt(d2) + cerr;
    for (uint32_t k = 0; k < x->nkids; k++) free(w->nodes[x->kids[k]].smp), w->nodes[x->kids[k]].smp = NULL, w->nodes[x->kids[k]].nsmp = 0;
    free(x->smp);
    x->smp = own.p, x->nsmp = own.n;
    free(m.v), free(m.t), free(ch.p), free(km.v), free(km.t);
    return err;
oom:
    free(m.v), free(m.t), free(ch.p), free(own.p), free(km.v), free(km.t);
    *bad = 1;
    return 0;
}

/* ---- a proxy: clusters to TPX1 ---- */

static void put32(Buf *b, uint32_t v) {
    unsigned char c[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
    buf_append(b, c, 4);
}
static void put16(Buf *b, uint32_t v) {
    unsigned char c[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
    buf_append(b, c, 2);
}
static void putf(Buf *b, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    put32(b, u);
}

static uint16_t quant16(float p, float lo, float hi) {
    double sp = (double)hi - (double)lo;
    if (!(sp > 0)) return 0;
    double q = ((double)p - (double)lo) / sp * 65535.0 + 0.5;
    return q <= 0 ? 0 : q >= 65535 ? 65535 : (uint16_t)q;
}

/* the TPX1 of clusters `cl` quantised in `box`, appended to w->blobs (4-aligned); x's tris, verts, off, len */
static int write_proxy(cc_tiles_work *w, const Clusters *cl, Node *x) {
    uint64_t nv = 0, nt = 0;
    uint32_t np = 0;
    for (int i = 0; i < CC_CAT_COUNT; i++)
        if (cl->p[i].nt) {
            np++, nt += cl->p[i].nt;
            /* vertices used */
            nv += cl->p[i].nv; /* every finished vertex is a corner of a kept triangle or not: counted below */
        }
    /* count used vertices exactly */
    Buf *b = &w->blobs;
    while (b->len % 4) buf_putc(b, 0);
    x->off = b->len;
    uint32_t *map = NULL;
    size_t cmap = 0;
    uint64_t used_total = 0;
    for (int i = 0; i < CC_CAT_COUNT; i++) {
        const Part *p = &cl->p[i];
        if (!p->nt) continue;
        if (p->nv > cmap) {
            free(map);
            map = malloc(((size_t)p->nv + 1) * sizeof *map), cmap = p->nv;
            if (!map) return -1;
        }
        memset(map, 0xff, (size_t)p->nv * sizeof *map);
        for (size_t t = 0; t < p->nt; t++)
            for (int k = 0; k < 3; k++)
                if (map[p->t[t][k]] == UINT32_MAX) map[p->t[t][k]] = 0, used_total++;
    }
    nv = used_total;
    buf_append(b, "TPX1", 4);
    put32(b, (uint32_t)nv), put32(b, (uint32_t)(3 * nt)), put32(b, np);
    uint32_t i0 = 0;
    for (int i = 0; i < CC_CAT_COUNT; i++)
        if (cl->p[i].nt) put32(b, (uint32_t)i), put32(b, 0), put32(b, i0), put32(b, (uint32_t)(3 * cl->p[i].nt)), i0 += (uint32_t)(3 * cl->p[i].nt);
    /* positions, then indices (per part: its used vertices in first-use order) */
    size_t pos_at = b->len;
    if (buf_reserve(b, b->len + 6 * (size_t)nv + 4 + 4 * 3 * (size_t)nt)) { free(map); return -1; }
    uint32_t v0 = 0;
    double dev2 = 0;
    Buf idx;
    buf_init(&idx);
    int wide = nv > 65536;
    for (int i = 0; i < CC_CAT_COUNT; i++) {
        const Part *p = &cl->p[i];
        if (!p->nt) continue;
        memset(map, 0xff, (size_t)p->nv * sizeof *map);
        for (size_t t = 0; t < p->nt; t++)
            for (int k = 0; k < 3; k++) {
                uint32_t v = p->t[t][k];
                if (map[v] == UINT32_MAX) {
                    map[v] = v0++;
                    double pt[3];
                    rep(&p->v[v], pt);
                    for (int j = 0; j < 3; j++) put16(b, quant16((float)pt[j], x->box[j], x->box[3 + j]));
                }
                if (wide) put32(&idx, map[v]);
                else put16(&idx, map[v]);
            }
    }
    (void)pos_at;
    while (b->len % 4) buf_putc(b, 0);
    buf_append(b, idx.p, idx.len);
    buf_free(&idx);
    free(map);
    while (b->len % 4) buf_putc(b, 0);
    x->len = (uint32_t)(b->len - x->off);
    x->tris = (uint32_t)nt, x->verts = (uint32_t)nv;
    /* the error: the farthest member of any cluster from its proxy point, plus the 16-bit quantum's half diagonal */
    double qd = 0;
    for (int j = 0; j < 3; j++) {
        double q = ((double)x->box[3 + j] - (double)x->box[j]) / 65535.0 / 2.0;
        qd += q * q;
    }
    x->error = (float)cc_sqrt(qd); /* the 16-bit quantum's half diagonal; the caller adds the measured distance */
    (void)dev2;                     /* (the farthest member of a cluster: a bound 3 to 10 times what the picture shows) */
    x->flags |= CC_TILE_PROXY;
    return b->oom ? -1 : 0;
}

/* the inner node x: its children's clusters merged, fit to its cap, its proxy */
static int inner(cc_tiles_work *w, uint32_t n) {
    Node *x = &w->nodes[n];
    Grid g;
    memset(&g, 0, sizeof g);
    g.e = CC_TILES_MAX_DEPTH - (x->cdepth < CC_TILES_MAX_DEPTH ? x->cdepth : CC_TILES_MAX_DEPTH);
    uint64_t below = 0;
    for (uint32_t k = 0; k < x->nkids; k++) {
        const Node *c = &w->nodes[x->kids[k]];
        below += c->tris;
        if (w->cl[x->kids[k]].built && w->cl[x->kids[k]].e > g.e) g.e = w->cl[x->kids[k]].e;
    }
    uint64_t cap = below / 4;
    if (cap > w->o.proxy_tris) cap = w->o.proxy_tris;
    if (cap < 1) cap = 1;
    for (uint32_t k = 0; k < x->nkids; k++) {
        Clusters *ch = &w->cl[x->kids[k]];
        if (!ch->built) continue;
        if (cc_hg_grid_add_clusters(&g, ch) || cc_hg_grid_fit(&g, cap, 0)) return cc_hg_grid_free(&g), -1;
    }
    Clusters cl;
    memset(&cl, 0, sizeof cl);
    if (cc_hg_grid_fit(&g, cap, 1) || cc_hg_grid_finish(&g, &cl)) return cc_hg_grid_free(&g), cc_hg_clusters_free(&cl), -1;
    if (write_proxy(w, &cl, x)) return cc_hg_clusters_free(&cl), -1;
    int bad = 0;
    const Clusters **kc = malloc(((size_t)x->nkids + 1) * sizeof *kc);
    uint32_t nk = 0;
    for (uint32_t k = 0; kc && k < x->nkids; k++)
        if (w->cl[x->kids[k]].built) kc[nk++] = &w->cl[x->kids[k]];
    double pe = kc ? proxy_error(w, &cl, kc, nk, n, &bad) : (bad = 1, 0.0);
    free(kc);
    x = &w->nodes[n];
    for (uint32_t k = 0; k < x->nkids; k++) cc_hg_clusters_free(&w->cl[x->kids[k]]);
    if (bad) return cc_hg_clusters_free(&cl), -1;
    x->error = (float)(x->error + pe);
    w->cl[n] = cl;
    /* occluders: the children's largest */
    Occ set[CC_TILES_OCC_MAX];
    uint32_t no = 0, mo = w->o.occ < CC_TILES_OCC_MAX ? w->o.occ : CC_TILES_OCC_MAX;
    for (uint32_t k = 0; k < x->nkids; k++) {
        const Node *c = &w->nodes[x->kids[k]];
        for (uint32_t q = 0; q < c->nocc; q++) occ_keep(set, &no, mo, &c->occ[q]);
    }
    return occ_store(w, x, set, no);
}

/* the chain above the root: coarser proxies of the whole until one has root_tris or fewer */
static int chain(cc_tiles_work *w) {
    uint32_t top = w->root;
    for (int guard = 0; guard < 24; guard++) {
        Node *x = &w->nodes[top];
        Clusters *cl = &w->cl[top];
        uint64_t cur = (x->flags & CC_TILE_PROXY) ? x->tris : x->detail;
        if (w->force_chain >= 0) { /* SHELL1: the full build's chain, level for level */
            if (guard >= w->force_chain) break;
        } else {
            if (cur <= w->o.root_tris && (x->flags & CC_TILE_PROXY)) break;
            if (x->flags & CC_TILE_LEAF && x->detail <= w->o.root_tris) break; /* a tiny link: the leaf is the root */
        }
        if (!cl->built) break;
        uint64_t cap = cur / 4;
        if (cap < w->o.root_tris && cur > w->o.root_tris) cap = w->o.root_tris;
        if (cap < 1) cap = 1;
        uint32_t q = new_node(w, CC_TILES_NONE);
        if (q == CC_TILES_NONE) return -1;
        x = &w->nodes[top];
        Node *y = &w->nodes[q];
        uint32_t *kid = arena_alloc_n(&w->arena, 1, sizeof *kid, 4);
        if (!kid) return -1;
        kid[0] = top;
        y->kids = kid, y->nkids = 1, y->el0 = x->el0, y->nel = x->nel, y->detail = x->detail, y->cdepth = 0;
        memcpy(y->box, x->box, sizeof y->box);
        x->parent = q;
        Grid g;
        memset(&g, 0, sizeof g);
        g.e = cl->e;
        Clusters out;
        memset(&out, 0, sizeof out);
        if (cc_hg_grid_add_clusters(&g, cl) || cc_hg_grid_fit(&g, cap, 1) || cc_hg_grid_finish(&g, &out))
            return cc_hg_grid_free(&g), cc_hg_clusters_free(&out), -1;
        if (write_proxy(w, &out, y)) return cc_hg_clusters_free(&out), -1;
        int bad = 0;
        const Clusters *kc1[1] = {cl};
        double pe = proxy_error(w, &out, kc1, 1, q, &bad);
        x = &w->nodes[top], y = &w->nodes[q];
        if (bad) return cc_hg_clusters_free(&out), -1;
        y->error = (float)(y->error + pe);
        if (occ_store(w, y, x->occ, x->nocc)) return cc_hg_clusters_free(&out), -1;
        cc_hg_clusters_free(cl);
        w->cl[q] = out;
        top = q;
        if (w->force_chain < 0 && y->tris <= w->o.root_tris) break;
    }
    w->root = top;
    return 0;
}

/* the section: tile ids depth first (preorder) from the top: a subtree is the ids [i, i + skip) */
static int write_section(cc_tiles *t) {
    cc_tiles_work *w = t->work;
    uint32_t *pre = malloc(((size_t)w->nnodes + 1) * sizeof *pre), *st = malloc(((size_t)w->nnodes + 1) * sizeof *st);
    uint32_t *skip = calloc((size_t)w->nnodes + 1, sizeof *skip);
    if (!pre || !st || !skip) return free(pre), free(st), free(skip), -1;
    uint32_t np = 0, sp = 0, levels = 0;
    st[sp++] = w->root;
    w->nodes[w->root].level = 0;
    while (sp) {
        uint32_t n = st[--sp];
        Node *x = &w->nodes[n];
        x->id = np, pre[np++] = n;
        if (x->level + 1 > levels) levels = x->level + 1;
        for (uint32_t k = x->nkids; k-- > 0;) w->nodes[x->kids[k]].level = x->level + 1, st[sp++] = x->kids[k];
    }
    for (uint32_t i = np; i-- > 0;) { /* subtree sizes */
        Node *x = &w->nodes[pre[i]];
        skip[i] = 1;
        for (uint32_t k = 0; k < x->nkids; k++) skip[i] += skip[w->nodes[x->kids[k]].id];
    }
    uint32_t nocc = 0, nleaf = 0, nprox = 0;
    for (uint32_t i = 0; i < np; i++) nocc += w->nodes[pre[i]].nocc;
    Buf *o = &t->out;
    buf_clear(o);
    uint64_t toc = CC_TILES_HEAD + (uint64_t)CC_TILES_REC * np + 4ull * w->nels + ((w->nels + 3ull) & ~3ull);
    uint64_t tocp = (toc + 36ull * nocc + 7) & ~7ull; /* the occluders after the table, then the blobs */
    if (buf_reserve(o, (size_t)(tocp + w->blobs.len))) return free(pre), free(st), free(skip), -1;
    buf_append(o, "TIL1", 4);
    put32(o, CC_TILES_VERSION), put32(o, np), put32(o, (uint32_t)src_n(&w->src)), put32(o, w->nels), put32(o, nocc);
    put32(o, (uint32_t)toc), put32(o, w->o.leaf_tris);
    put32(o, (uint32_t)w->blobs.len), put32(o, (uint32_t)((uint64_t)w->blobs.len >> 32));
    put32(o, levels), put32(o, 0);
    uint32_t occ0 = 0;
    for (uint32_t i = 0; i < np; i++) {
        const Node *x = &w->nodes[pre[i]];
        put32(o, x->parent == CC_TILES_NONE ? CC_TILES_NONE : w->nodes[x->parent].id);
        put32(o, skip[i]);
        uint32_t f = (x->flags & (CC_TILE_LEAF | CC_TILE_PROXY | CC_TILE_OWN)) | (x->nocc ? CC_TILE_OCC : 0);
        put16(o, x->nkids);
        buf_putc(o, (char)x->level), buf_putc(o, (char)f);
        for (int k = 0; k < 6; k++) putf(o, x->box[k]);
        if (x->flags & CC_TILE_LEAF) putf(o, 0.0f);
        else if (x->flags & CC_TILE_PROXY) putf(o, x->error);
        else putf(o, 1.0f / 0.0f);
        put32(o, x->el0), put32(o, x->nel);
        uint64_t off = (x->flags & CC_TILE_PROXY) ? x->off : 0;
        put32(o, (uint32_t)off), put32(o, (uint32_t)(off >> 32));
        put32(o, (x->flags & CC_TILE_PROXY) ? x->len : 0);
        put32(o, x->tris), put32(o, x->verts);
        put32(o, x->nocc ? occ0 : 0), put16(o, x->nocc), put16(o, 0);
        put32(o, x->detail > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)x->detail);
        occ0 += x->nocc;
        if (x->flags & CC_TILE_LEAF) nleaf++, t->leaf_tris += x->tris;
        if (x->flags & CC_TILE_PROXY) nprox++, t->proxy_tris += x->tris;
    }
    for (uint32_t i = 0; i < w->nels; i++) put32(o, w->els[i]);
    for (uint32_t i = 0; i < w->nels; i++) buf_putc(o, (char)w->eflag[w->els[i]]);
    while ((o->len - 0) % 4) buf_putc(o, 0);
    for (uint32_t i = 0; i < np; i++) {
        const Node *x = &w->nodes[pre[i]];
        for (uint32_t q = 0; q < x->nocc; q++)
            for (int k = 0; k < 9; k++) putf(o, x->occ[q].t[k]);
    }
    while (o->len < tocp) buf_putc(o, 0);
    buf_append(o, w->blobs.p, w->blobs.len);
    free(pre), free(st), free(skip);
    t->ntiles = np, t->nleaves = nleaf, t->nproxies = nprox, t->levels = levels, t->nocc = nocc;
    return o->oom ? -1 : 0;
}

/* leaf n read whole by worker state L: its clusters (for its parent), counts, occluders and points */
static int leaf_run(cc_tiles_work *w, LW *L, uint32_t n) {
    Node *x = &w->nodes[n];
    memset(&L->g, 0, sizeof L->g);
    L->g.e = CC_TILES_MAX_DEPTH - (x->cdepth < CC_TILES_MAX_DEPTH ? x->cdepth : CC_TILES_MAX_DEPTH);
    L->nocc = 0, L->leaf_verts = 0, L->ntouched = 0, L->maxocc = w->o.occ < CC_TILES_OCC_MAX ? w->o.occ : CC_TILES_OCC_MAX;
    L->nlsmp = 0, L->lcount = 0, L->lstride = 2 * x->detail / CC_TILES_SAMPLES + 1;
    arena_reset(&L->dec);
    int rc = 0;
    for (uint32_t j = 0; j < x->nel && !rc; j++)
        if (!w->mask || w->mask[w->els[x->el0 + j]]) rc = leaf_element(w, L, x, w->els[x->el0 + j]);
    for (uint32_t i = 0; i < L->ntouched; i++) L->shapes[L->touched[i]].nv = UINT32_MAX;
    L->ntouched = 0;
    arena_reset(&L->dec);
    if (rc) return cc_hg_grid_free(&L->g), -1;
    Clusters cl;
    memset(&cl, 0, sizeof cl);
    if (cc_hg_grid_fit(&L->g, w->o.proxy_tris, 1) || cc_hg_grid_finish(&L->g, &cl)) return cc_hg_clusters_free(&cl), cc_hg_grid_free(&L->g), -1;
    w->cl[n] = cl;
    uint64_t dt = x->detail;
    if (w->mask) { /* SHELL1: a shell build's leaf stands for its exterior elements */
        dt = 0;
        for (uint32_t j = 0; j < x->nel; j++)
            if (w->mask[w->els[x->el0 + j]]) dt += w->tris[w->els[x->el0 + j]];
    }
    x->tris = (uint32_t)(dt > 0xFFFFFFFFull ? 0xFFFFFFFFu : dt);
    x->verts = (uint32_t)(L->leaf_verts > 0xFFFFFFFFull ? 0xFFFFFFFFu : L->leaf_verts);
    /* its occluders, sorted (occ_store, without the shared arena: a worker allocates its own) */
    qsort(L->occ, L->nocc, sizeof *L->occ, occ_cmp);
    x->occ = L->nocc ? malloc(L->nocc * sizeof *L->occ) : NULL;
    if (L->nocc && !x->occ) return -1;
    if (L->nocc) memcpy(x->occ, L->occ, L->nocc * sizeof *L->occ);
    x->nocc = L->nocc, x->occ_heap = 1;
    x->smp = L->lsmp, x->nsmp = L->nlsmp; /* the leaf's points move to it */
    L->lsmp = NULL, L->nlsmp = L->clsmp = 0;
    return 0;
}

typedef struct {
    cc_tiles_work *w;
    const uint32_t *nodes;
} LJ;

static void leaf_job(void *ctx, size_t task, size_t worker) {
    LJ *j = ctx;
    LW *L = &j->w->lw[worker];
    if (leaf_run(j->w, L, j->nodes[task])) L->bad = 1;
}

int cc_tiles_step(cc_tiles *t, uint64_t budget, cc_error *err) {
    cc_tiles_work *w = t->work;
    if (!w) return 1;
    uint64_t used = 0;
    const cc_tiles_src *s = &w->src;
    while (w->at < w->norder) {
        uint32_t n = w->order[w->at];
        Node *x = &w->nodes[n];
        if (x->flags & CC_TILE_LEAF) { /* a run of leaves (next in post-order), read in parallel */
            uint32_t k = w->at;
            uint64_t tris = 0;
            while (k < w->norder && (w->nodes[w->order[k]].flags & CC_TILE_LEAF) && k - w->at < 4 * (uint32_t)w->nlw &&
                   (k == w->at || used + tris + w->nodes[w->order[k]].detail <= budget))
                tris += w->nodes[w->order[k]].detail, k++;
            LJ lj = {w, w->order + w->at};
            for (size_t i = 0; i < w->nlw; i++) w->lw[i].bad = 0;
            cc_jobs_run(k - w->at, leaf_job, &lj);
            for (size_t i = 0; i < w->nlw; i++)
                if (w->lw[i].bad) return fail(err, "a mesh does not decode, or out of memory");
            used += tris, t->done += tris;
            w->at = k;
            if (used >= budget && w->at < w->norder) return 0;
            continue;        } else if (inner(w, n)) return fail(err, "out of memory");
        w->at++;
        if (used >= budget && w->at < w->norder) return 0;
    }
    (void)s;
    if (w->nnodes && chain(w)) return fail(err, "out of memory");
    if (w->nnodes && write_section(t)) return fail(err, "out of memory");
    if (!w->nnodes) { /* no geometry: a section with no tile */
        Buf *o = &t->out;
        buf_append(o, "TIL1", 4);
        put32(o, CC_TILES_VERSION), put32(o, 0), put32(o, (uint32_t)src_n(s)), put32(o, 0), put32(o, 0);
        put32(o, CC_TILES_HEAD), put32(o, w->o.leaf_tris), put32(o, 0), put32(o, 0), put32(o, 0), put32(o, 0);
        if (o->oom) return fail(err, "out of memory");
    }
    work_free(w);
    t->work = NULL;
    return 1;
}

int cc_tiles_build(cc_tiles *t, const cc_tiles_src *src, const cc_tiles_opts *o, cc_error *err) {
    if (cc_tiles_start(t, src, o, err)) return -1;
    return cc_tiles_step(t, UINT64_MAX, err) < 0 ? -1 : 0;
}

/* ---- reading (untrusted) ---- */

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static float rdf(const uint8_t *p) {
    uint32_t u = rd32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void cc_tiles_get(const cc_tiles_toc *toc, uint32_t i, cc_tile *o) {
    const uint8_t *r = toc->p + CC_TILES_HEAD + (size_t)CC_TILES_REC * i;
    o->parent = rd32(r), o->skip = rd32(r + 4), o->nchildren = rd16(r + 8), o->level = r[10], o->flags = r[11];
    for (int k = 0; k < 6; k++) o->box[k] = rdf(r + 12 + 4 * k);
    o->error = rdf(r + 36);
    o->el0 = rd32(r + 40), o->nel = rd32(r + 44);
    o->off = (uint64_t)rd32(r + 48) | (uint64_t)rd32(r + 52) << 32;
    o->len = rd32(r + 56), o->tris = rd32(r + 60), o->verts = rd32(r + 64), o->occ0 = rd32(r + 68), o->nocc = rd16(r + 72);
    o->detail = rd32(r + 76);
}

uint32_t cc_tiles_list(const cc_tiles_toc *toc, uint32_t i) { return rd32(toc->list + 4 * (size_t)i); }
uint32_t cc_tiles_el_flags(const cc_tiles_toc *toc, uint32_t i) { return toc->eflags[i]; }
int cc_tiles_el_exterior(const cc_tiles_toc *toc, uint32_t i) { return toc->xmask ? toc->xmask[i] != 0 : (toc->eflags[i] & CC_TILES_EL_EXTERIOR) != 0; }

int cc_tiles_check(const uint8_t *p, size_t n, int with_blobs, uint32_t nelements, cc_tiles_toc *toc, cc_error *err) {
    memset(toc, 0, sizeof *toc);
    if (n < CC_TILES_HEAD || memcmp(p, "TIL1", 4) || rd32(p + 4) != CC_TILES_VERSION) return fail(err, "tiles: not TIL1 version 1");
    uint64_t nt = rd32(p + 8), ne = rd32(p + 12), nl = rd32(p + 16), no = rd32(p + 20), tb = rd32(p + 24);
    uint64_t blobs = (uint64_t)rd32(p + 32) | (uint64_t)rd32(p + 36) << 32;
    if (nelements != CC_TILES_NONE && ne != nelements) return fail(err, "tiles: of another scene");
    if (nt > CC_TILES_MAX_TILES || nl > ne || no > (uint64_t)CC_TILES_OCC_MAX * nt) return fail(err, "tiles: counts");
    if (tb != CC_TILES_HEAD + (uint64_t)CC_TILES_REC * nt + 4 * nl + ((nl + 3) & ~3ull)) return fail(err, "tiles: table size");
    uint64_t tp = (tb + 36 * no + 7) & ~7ull; /* the blobs' start */
    if (with_blobs ? (tp + blobs != n) : (n < tb)) return fail(err, "tiles: section size");
    if (rd32(p + 40) > CC_TILES_MAX_LEVELS || (rd32(p + 44) & ~(uint32_t)CC_TILES_BAKE_SHELL)) return fail(err, "tiles: levels or bake flags");
    if (!nt && (nl || no || blobs)) return fail(err, "tiles: counts");
    toc->p = p, toc->n = n, toc->ntiles = (uint32_t)nt, toc->nelements = (uint32_t)ne, toc->nlist = (uint32_t)nl;
    toc->nocc = (uint32_t)no, toc->toc = (uint32_t)tb, toc->levels = rd32(p + 40), toc->leaf_target = rd32(p + 28), toc->bake = rd32(p + 44);
    toc->blobs = blobs;
    toc->list = p + CC_TILES_HEAD + (size_t)CC_TILES_REC * nt;
    toc->eflags = toc->list + 4 * nl;
    toc->occ = n >= tb + 36 * no ? p + tb : NULL; /* the occluders: absent from a table of contents alone */
    toc->blob_at = (uint32_t)tp;
    for (uint64_t i = 0; i < nl; i++)
        if (toc->eflags[i] & ~(unsigned)(CC_TILES_EL_CLOSED | CC_TILES_EL_OPAQUE | CC_TILES_EL_EXTERIOR)) return fail(err, "tiles: an element's flags");
    for (uint64_t i = 0; i < nl; i++)
        if (rd32(toc->list + 4 * i) >= ne) return fail(err, "tiles: an element");
    uint8_t *seen = calloc((size_t)(ne / 8 + 1), 1);
    if (!seen) return fail(err, "out of memory");
    int bad = 0;
    for (uint64_t i = 0; i < nl && !bad; i++) {
        uint32_t e = rd32(toc->list + 4 * i);
        if (seen[e / 8] & (1u << (e % 8))) bad = 1;
        seen[e / 8] |= (uint8_t)(1u << (e % 8));
    }
    free(seen);
    if (bad) return fail(err, "tiles: an element twice");
    for (uint64_t i = 0; toc->occ && i < 9 * no; i++)
        if (!(rdf(toc->occ + 4 * i) > -3.0e38f && rdf(toc->occ + 4 * i) < 3.0e38f)) return fail(err, "tiles: an occluder");
    uint64_t leaves_el = 0;
    for (uint32_t i = 0; i < nt; i++) {
        cc_tile t;
        cc_tiles_get(toc, i, &t);
        if (i == 0 ? (t.parent != CC_TILES_NONE || t.level != 0 || t.skip != nt) : (t.parent >= i)) return fail(err, "tiles: a parent");
        if (t.flags & ~31u) return fail(err, "tiles: flags");
        for (int k = 0; k < 6; k++)
            if (!(t.box[k] > -3.0e38f && t.box[k] < 3.0e38f)) return fail(err, "tiles: a box");
        if (!(t.box[0] <= t.box[3] && t.box[1] <= t.box[4] && t.box[2] <= t.box[5])) return fail(err, "tiles: a box");
        if ((uint64_t)t.el0 + t.nel > nl) return fail(err, "tiles: an element range");
        if ((uint64_t)t.occ0 + t.nocc > no || (!t.nocc) != !(t.flags & CC_TILE_OCC)) return fail(err, "tiles: occluders");
        if (t.flags & CC_TILE_LEAF) {
            if (t.nchildren || (t.flags & CC_TILE_PROXY) || t.len || t.error != 0.0f) return fail(err, "tiles: a leaf");
            leaves_el += t.nel;
        } else {
            if (!t.nchildren) return fail(err, "tiles: an inner tile without children");
            if (t.flags & CC_TILE_OWN) return fail(err, "tiles: flags");
            if (t.flags & CC_TILE_PROXY) {
                if (!(t.error >= 0.0f && t.error < 3.0e38f) || t.len < 16 || (t.off & 3) || t.off + t.len > blobs) return fail(err, "tiles: a proxy");
            } else if (!(t.error > 3.0e38f) || t.len) return fail(err, "tiles: an inner tile without proxy");
        }
        if ((uint64_t)i + t.skip > nt || !t.skip) return fail(err, "tiles: a subtree");
        if (t.nchildren) {
            uint64_t at = t.el0, c = i + 1, end = (uint64_t)i + t.skip;
            for (uint32_t q = 0; q < t.nchildren; q++) {
                cc_tile u;
                if (c >= end) return fail(err, "tiles: children");
                cc_tiles_get(toc, (uint32_t)c, &u);
                if (u.parent != i || u.level != t.level + 1) return fail(err, "tiles: a child's parent");
                if (u.el0 != at) return fail(err, "tiles: a child's elements");
                at += u.nel;
                for (int k = 0; k < 3; k++)
                    if (u.box[k] < t.box[k] || u.box[3 + k] > t.box[3 + k]) return fail(err, "tiles: a child outside its parent");
                if (!u.skip || c + u.skip > end) return fail(err, "tiles: a subtree");
                c += u.skip;
            }
            if (c != end) return fail(err, "tiles: a subtree");
            if (at != (uint64_t)t.el0 + t.nel) return fail(err, "tiles: a child's elements");
        } else if (t.skip != 1) return fail(err, "tiles: a subtree");
    }
    if (nt && leaves_el != nl) return fail(err, "tiles: an element in no leaf");
    if (nt) {
        cc_tile r;
        cc_tiles_get(toc, 0, &r);
        if (r.el0 != 0 || r.nel != nl) return fail(err, "tiles: the root's elements");
    }
    if (with_blobs && (toc->bake & CC_TILES_BAKE_SHELL)) { /* SHELL1: the shell table, found by the trailer at the end */
        const uint8_t *tr = p + n - 16;
        uint64_t tl = blobs >= 16 ? rd32(tr + 4) : 0, to = blobs >= 16 ? ((uint64_t)rd32(tr + 8) | (uint64_t)rd32(tr + 12) << 32) : 0;
        if (blobs < 16 || memcmp(tr, "TSHT", 4) || to > blobs - 16 || tl > blobs - 16 - to || (to & 7))
            return fail(err, "tiles: the shell's trailer");
        if (cc_tiles_shell_check(p + tp + to, (size_t)tl, toc, err)) return -1;
    }
    if (with_blobs) {
        const uint8_t *bl = p + tp;
        if (tp > UINT32_MAX) return fail(err, "tiles: section size");
        for (uint32_t i = 0; i < nt; i++) {
            cc_tile t;
            cc_tiles_get(toc, i, &t);
            if ((t.flags & CC_TILE_PROXY) && cc_tiles_proxy_check(bl + t.off, t.len, err)) return -1;
            if (t.flags & CC_TILE_PROXY) {
                uint32_t nv = rd32(bl + t.off + 4), ni = rd32(bl + t.off + 8);
                if (nv != t.verts || ni != 3ull * t.tris) return fail(err, "tiles: a proxy's counts");
            }
        }
    }
    return 0;
}

int cc_tiles_proxy_check(const uint8_t *p, size_t n, cc_error *err) {
    if (n < 16 || memcmp(p, "TPX1", 4)) return fail(err, "tiles: not a TPX1 proxy");
    uint64_t nv = rd32(p + 4), ni = rd32(p + 8), np = rd32(p + 12);
    if (ni % 3 || np > CC_CAT_COUNT) return fail(err, "tiles: a proxy's counts");
    uint64_t at = 16 + 16 * np + 6 * nv;
    at = (at + 3) & ~3ull;
    int wide = nv > 65536;
    uint64_t end = at + (wide ? 4 : 2) * ni;
    if (((end + 3) & ~3ull) != n && end != n) return fail(err, "tiles: a proxy's size");
    uint64_t i0 = 0;
    for (uint64_t k = 0; k < np; k++) {
        const uint8_t *q = p + 16 + 16 * k;
        if (rd32(q) >= CC_CAT_COUNT || rd32(q + 4) & ~1u || rd32(q + 8) != i0 || rd32(q + 12) % 3) return fail(err, "tiles: a proxy's parts");
        i0 += rd32(q + 12);
    }
    if (i0 != ni) return fail(err, "tiles: a proxy's parts");
    for (uint64_t k = 0; k < ni; k++) {
        uint32_t x = wide ? rd32(p + at + 4 * k) : rd16(p + at + 2 * k);
        if (x >= nv) return fail(err, "tiles: a proxy's index");
    }
    return 0;
}

int cc_tiles_proxy_mesh(const uint8_t *p, size_t n, const float box[6], Arena *a, cc_mesh *m, uint8_t **cat) {
    if (cc_tiles_proxy_check(p, n, NULL)) return -1;
    memset(m, 0, sizeof *m);
    uint32_t nv = rd32(p + 4), ni = rd32(p + 8), np = rd32(p + 12);
    m->pos = arena_alloc_n(a, (size_t)nv + 1, sizeof m->pos[0], 4);
    m->idx = arena_alloc_n(a, (size_t)ni + 1, sizeof *m->idx, 4);
    uint8_t *c = arena_alloc_n(a, (size_t)ni / 3 + 1, 1, 1);
    if (!m->pos || !m->idx || !c) return -1;
    const uint8_t *q = p + 16 + 16 * (size_t)np;
    for (uint32_t v = 0; v < nv; v++)
        for (int k = 0; k < 3; k++) {
            double s = (double)box[3 + k] - (double)box[k];
            m->pos[v][k] = (float)((double)box[k] + (double)rd16(q + 6 * (size_t)v + 2 * k) * s / 65535.0);
        }
    size_t at = (16 + 16 * (size_t)np + 6 * (size_t)nv + 3) & ~(size_t)3;
    int wide = nv > 65536;
    for (uint32_t k = 0; k < ni; k++) m->idx[k] = wide ? rd32(p + at + 4 * (size_t)k) : rd16(p + at + 2 * (size_t)k);
    for (uint32_t k = 0; k < np; k++) {
        const uint8_t *r = p + 16 + 16 * (size_t)k;
        for (uint32_t t = rd32(r + 8) / 3; t < (rd32(r + 8) + rd32(r + 12)) / 3; t++) c[t] = (uint8_t)rd32(r);
    }
    m->nv = nv, m->ni = ni;
    if (cat) *cat = c;
    return 0;
}

/* ---- the leaf answer ---- */

int cc_tiles_leaf_answer(const cc_tiles_src *src, const cc_tiles_toc *toc, uint32_t id, Buf *out, cc_error *err) {
    return cc_tiles_leaf_answer_ex(src, toc, id, 0, out, err);
}

int cc_tiles_leaf_answer_ex(const cc_tiles_src *src, const cc_tiles_toc *toc, uint32_t id, unsigned flags, Buf *out, cc_error *err) {
    int only = (flags & CC_TILES_ANS_SHELL) && (toc->bake & CC_TILES_BAKE_SHELL); /* SHELL1: its exterior elements only */
    if (id >= toc->ntiles) return fail(err, "tiles: no such tile");
    cc_tile t;
    cc_tiles_get(toc, id, &t);
    if (!(t.flags & CC_TILE_LEAF)) return fail(err, "tiles: not a leaf");
    size_t ns = src_nshapes(src), ne = src_n(src);
    uint32_t *blk = malloc((ns + t.nel + 1) * sizeof *blk); /* per shape: its block in the answer (+1), or 0 */
    uint32_t *order = malloc(((size_t)t.nel + 1) * sizeof *order); /* the answer's blocks: their source block */
    if (!blk || !order) return free(blk), free(order), fail(err, "out of memory");
    memset(blk, 0, (ns + 1) * sizeof *blk);
    uint32_t nb = 0;
    size_t head = out->len;
    uint32_t nel = 0;
    for (uint32_t i = 0; i < t.nel; i++) nel += !only || cc_tiles_el_exterior(toc, t.el0 + i);
    buf_append(out, "TLF1", 4);
    put32(out, id), put32(out, nel), put32(out, 0);
    static const float ID[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    for (uint32_t i = 0; i < t.nel; i++) {
        if (only && !cc_tiles_el_exterior(toc, t.el0 + i)) continue;
        uint32_t el = cc_tiles_list(toc, t.el0 + i);
        if (el >= ne) return free(blk), free(order), fail(err, "tiles: an element");
        int sh = src_shape(src, el);
        uint32_t b;
        if (sh >= 0 && (size_t)sh < ns) {
            if (!blk[sh]) order[nb] = (uint32_t)sh, blk[sh] = ++nb;
            b = blk[sh] - 1;
        } else order[nb] = (uint32_t)(ns + el), b = nb++;
        put32(out, el), put32(out, b);
        if (sh >= 0) {
            const cc_placement32 *pl = src_pl(src, el);
            for (int k = 0; k < 3; k++) putf(out, pl->x[k]);
            for (int k = 0; k < 3; k++) putf(out, pl->y[k]);
            for (int k = 0; k < 3; k++) putf(out, pl->z[k]);
            for (int k = 0; k < 3; k++) putf(out, pl->t[k]);
        } else
            for (int k = 0; k < 12; k++) putf(out, ID[k]);
    }
    size_t lens = out->len;
    for (uint32_t i = 0; i < nb; i++) put32(out, 0);
    int rc = 0;
    Arena a;
    arena_init(&a, 0);
    for (uint32_t i = 0; i < nb && !rc; i++) {
        size_t at = out->len;
        if (lazy(src)) {
            const uint8_t *q;
            size_t qn;
            if (cc_viewfile_block(src->vf, order[i], &q, &qn)) rc = fail(err, "tiles: a block");
            else buf_append(out, q, qn);
        } else {
            cc_mesh m;
            if (src_block(src, order[i], &a, &m) || cc_meshcodec_encode(&m, out)) rc = fail(err, "out of memory");
        }
        uint32_t ln = (uint32_t)(out->len - at);
        if (!out->oom) memcpy(out->p + lens + 4 * (size_t)i, (unsigned char[4]){(unsigned char)ln, (unsigned char)(ln >> 8), (unsigned char)(ln >> 16), (unsigned char)(ln >> 24)}, 4);
    }
    arena_free(&a);
    if (!out->oom) {
        unsigned char c[4] = {(unsigned char)nb, (unsigned char)(nb >> 8), (unsigned char)(nb >> 16), (unsigned char)(nb >> 24)};
        memcpy(out->p + head + 12, c, 4);
    }
    free(blk), free(order);
    if (out->oom) return fail(err, "out of memory");
    return rc;
}

int cc_tiles_leaf_mesh(const uint8_t *p, size_t n, Arena *a, cc_mesh *m, uint32_t **tri_row) {
    memset(m, 0, sizeof *m);
    if (n < 16 || memcmp(p, "TLF1", 4)) return -1;
    uint64_t nel = rd32(p + 8), nb = rd32(p + 12);
    if (nel > n / 56 || nb > nel || 16 + 56 * nel + 4 * nb > n) return -1;
    const uint8_t *els = p + 16, *lens = els + 56 * nel, *bl = lens + 4 * nb;
    const uint8_t **bp = arena_alloc_n(a, nb + 1, sizeof *bp, 8);
    cc_mesh *bm = arena_alloc_n(a, nb + 1, sizeof *bm, 8);
    if (!bp || !bm) return -1;
    uint64_t at = 0, nv = 0, ni = 0;
    for (uint64_t i = 0; i < nb; i++) {
        uint32_t ln = rd32(lens + 4 * i);
        if (ln > n - (size_t)(bl - p) - at) return -1;
        const char *why;
        if (cc_meshcodec_decode(bl + at, ln, a, &bm[i], &why) != CC_MC_OK) return -1;
        at += ln;
    }
    if ((size_t)(bl - p) + at != n) return -1;
    for (uint64_t i = 0; i < nel; i++) {
        uint32_t b = rd32(els + 56 * i + 4);
        if (b >= nb) return -1;
        nv += bm[b].nv, ni += bm[b].ni;
    }
    if (nv > UINT32_MAX || ni > UINT32_MAX) return -1;
    m->pos = arena_alloc_n(a, nv + 1, sizeof m->pos[0], 4);
    m->idx = arena_alloc_n(a, ni + 1, sizeof *m->idx, 4);
    uint32_t *rows = arena_alloc_n(a, ni / 3 + 1, sizeof *rows, 4);
    if (!m->pos || !m->idx || !rows) return -1;
    for (uint64_t i = 0; i < nel; i++) {
        const uint8_t *e = els + 56 * i;
        const cc_mesh *x = &bm[rd32(e + 4)];
        cc_placement32 pl;
        float f[12];
        for (int k = 0; k < 12; k++) f[k] = rdf(e + 8 + 4 * k);
        memcpy(pl.x, f, 12), memcpy(pl.y, f + 3, 12), memcpy(pl.z, f + 6, 12), memcpy(pl.t, f + 9, 12);
        uint32_t v0 = m->nv;
        for (uint32_t v = 0; v < x->nv; v++) place(&pl, x->pos[v], m->pos[m->nv++]);
        for (uint32_t k = 0; k < x->ni; k++) {
            if (k % 3 == 0) rows[m->ni / 3] = rd32(e);
            m->idx[m->ni++] = v0 + x->idx[k];
        }
    }
    if (tri_row) *tri_row = rows;
    return 0;
}

/* ---- the 21-bit lattice ---- */

static int64_t floor16(double v) {
    double q = v * 16.0;
    if (q >= 4.0e18) return (int64_t)4e18;
    if (q <= -4.0e18) return -(int64_t)4e18;
    int64_t t = (int64_t)q;
    return (double)t > q ? t - 1 : t;
}
static int64_t ashr(int64_t v, int k) { return v >= 0 ? v >> k : ~((~v) >> k); }

void cc_tiles_lattice(const float box[6], int *k, int64_t o[3]) {
    int64_t lo[3], span = 0;
    for (int j = 0; j < 3; j++) {
        lo[j] = floor16(box[j]);
        int64_t hi = -floor16(-(double)box[3 + j]); /* ceil */
        if (hi - lo[j] > span) span = hi - lo[j];
    }
    int s = 0;
    while (s < 40 && ashr(span, s) + 1 >= ((int64_t)1 << 21)) s++;
    *k = s;
    for (int j = 0; j < 3; j++) o[j] = ashr(lo[j], s) * ((int64_t)1 << s);
}

void cc_tiles_pack21(const float p[3], int k, const int64_t o[3], uint32_t q[3]) {
    for (int j = 0; j < 3; j++) {
        double v = (double)p[j] * 16.0;
        int64_t r = (int64_t)(v < 0 ? v - 0.5 : v + 0.5); /* round half away */
        int64_t d = r - o[j];
        if (k) d = ashr(d + ((int64_t)1 << (k - 1)), k);
        q[j] = d < 0 ? 0 : d > 0x1FFFFF ? 0x1FFFFF : (uint32_t)d;
    }
}

/* ---- the reference walker ---- */

typedef struct {
    double pri, dist;
    uint32_t id;
} HeapE;

static int heap_less(const HeapE *a, const HeapE *b) { return a->pri < b->pri || (a->pri == b->pri && a->id > b->id); }
static void heap_push(HeapE *h, uint32_t *n, HeapE e) {
    uint32_t i = (*n)++;
    h[i] = e;
    while (i && heap_less(&h[(i - 1) / 2], &h[i])) {
        HeapE t = h[i];
        h[i] = h[(i - 1) / 2], h[(i - 1) / 2] = t;
        i = (i - 1) / 2;
    }
}
static HeapE heap_pop(HeapE *h, uint32_t *n) {
    HeapE top = h[0];
    h[0] = h[--(*n)];
    for (uint32_t i = 0;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < *n && heap_less(&h[m], &h[l])) m = l;
        if (r < *n && heap_less(&h[m], &h[r])) m = r;
        if (m == i) break;
        HeapE t = h[i];
        h[i] = h[m], h[m] = t;
        i = m;
    }
    return top;
}

static double box_dist(const double eye[3], const float *b) {
    double s = 0;
    for (int k = 0; k < 3; k++) {
        double d = eye[k] < b[k] ? b[k] - eye[k] : eye[k] > b[3 + k] ? eye[k] - b[3 + k] : 0;
        s += d * d;
    }
    return cc_sqrt(s);
}

static int in_view(const cc_tiles_view *v, const float *b) {
    for (int i = 0; i < v->nplanes; i++) {
        const double *p = v->planes[i];
        double x = p[0] >= 0 ? b[3] : b[0], y = p[1] >= 0 ? b[4] : b[1], z = p[2] >= 0 ? b[5] : b[2];
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return 0;
    }
    return 1;
}

static int sel_cmp(const void *x, const void *y) {
    const HeapE *a = x, *b = y;
    if (a->dist != b->dist) return a->dist < b->dist ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id;
}

/* SHELL1: a tile as the outside walk sees it: no exterior element: not in view; a leaf: its exterior triangles; a proxy: the
 * shell table's (its exterior proxy), or none (descend) when the table says so */
static void sel_get(const cc_tiles_toc *toc, uint32_t i, int shell, cc_tile *t) {
    cc_tiles_get(toc, i, t);
    if (!shell) return;
    if (toc->xmask && toc->xtile) { /* the cross-link shell: what a tile holds seen from outside */
        if (toc->xtile[i]) t->flags |= CC_TILE_EXTERIOR;
        else t->flags &= ~(uint32_t)CC_TILE_EXTERIOR;
        if ((t->flags & CC_TILE_LEAF) && toc->xtris) {
            t->tris = toc->xtris[i];
            return;
        }
    }
    if (!toc->shell) return;
    cc_tile_shell r;
    cc_tiles_shell_get(toc, i, &r);
    if (t->flags & CC_TILE_LEAF) t->tris = r.ext;
    else if (r.flags & CC_TILE_SHELL_PROXY) t->tris = r.tris, t->error = r.error;
    else if (t->flags & CC_TILE_PROXY) t->flags &= ~(uint32_t)CC_TILE_PROXY, t->error = 1.0f / 0.0f, t->tris = 0;
}

uint32_t cc_tiles_select(const cc_tiles_toc *toc, const cc_tiles_view *v, double error_px, uint64_t budget, unsigned flags,
                         const uint8_t *descend, uint32_t *out, uint32_t cap, uint64_t *tris) {
    int leaves = (flags & CC_TILES_SEL_LEAVES) != 0; /* levels of detail off */
    int shell = (flags & CC_TILES_SEL_SHELL) && (toc->bake & CC_TILES_BAKE_SHELL); /* SHELL1: the outside walk */
#define IN_VIEW(T) (in_view(v, (T).box) && (!shell || ((T).flags & CC_TILE_EXTERIOR)))
    *tris = 0;
    if (!toc->ntiles) return 0;
    HeapE *h = malloc((size_t)toc->ntiles * sizeof *h), *fin = malloc((size_t)toc->ntiles * sizeof *fin);
    if (!h || !fin) return free(h), free(fin), 0;
    uint32_t nh = 0, nf = 0;
    uint64_t total = 0;
    cc_tile t;
    sel_get(toc, 0, shell, &t);
#define PRI(T, I, D)                                                                                                                   \
    (((T).flags & CC_TILE_LEAF) ? 0.0                                                                                                  \
     : (leaves || !((T).flags & CC_TILE_PROXY) || (descend && descend[I])) ? 1e300                                                              \
     : v->px_per_mm > 0 ? (double)(T).error * v->px_per_mm : (D) > 1.0 ? (double)(T).error * v->k / (D) : 1e299)
    if (IN_VIEW(t)) {
        double d = box_dist(v->eye, t.box);
        heap_push(h, &nh, (HeapE){PRI(t, 0, d), d, 0});
        total = t.tris;
    }
    while (nh) {
        HeapE e = h[0];
        sel_get(toc, e.id, shell, &t);
        int forced = e.pri >= 1e300;
        if ((t.flags & CC_TILE_LEAF) || (!forced && e.pri <= error_px)) break;
        uint64_t add = 0;
        for (uint32_t c = e.id + 1, q = 0; q < t.nchildren; q++) {
            cc_tile u;
            sel_get(toc, c, shell, &u);
            if (IN_VIEW(u)) add += u.tris;
            c += u.skip;
        }
        if (!forced && total - t.tris + add > budget) break;
        heap_pop(h, &nh);
        total = total - t.tris + add;
        for (uint32_t c = e.id + 1, q = 0; q < t.nchildren; q++) {
            cc_tile u;
            sel_get(toc, c, shell, &u);
            uint32_t me = c;
            c += u.skip;
            if (!IN_VIEW(u)) continue;
            double d = box_dist(v->eye, u.box);
            heap_push(h, &nh, (HeapE){PRI(u, me, d), d, me});
        }
    }
#undef PRI
    for (uint32_t i = 0; i < nh; i++) {
        cc_tile u;
        sel_get(toc, h[i].id, shell, &u);
        if (!(u.flags & (CC_TILE_LEAF | CC_TILE_PROXY))) continue; /* cannot stand (never left when refined) */
        fin[nf++] = h[i];
    }
    qsort(fin, nf, sizeof *fin, sel_cmp);
    uint32_t n = 0;
    for (uint32_t i = 0; i < nf; i++) {
        cc_tile u;
        sel_get(toc, fin[i].id, shell, &u);
        *tris += u.tris;
        if (n < cap) out[n++] = fin[i].id;
    }
    free(h), free(fin);
    return n;
#undef IN_VIEW
}

/* ---- the reference occlusion (tiles.h) ---- */

int cc_tiles_occ_init(cc_tiles_occ *z, const double eye[3], const double at[3], double tan_half, double w_px, double h_px, int cells_w) {
    memset(z, 0, sizeof *z);
    double f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]}, up[3] = {0, 0, 1};
    double l = cc_sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (!(l > 0) || !(tan_half > 0) || !(w_px > 0) || !(h_px > 0) || cells_w < 8 || cells_w > 4096) return -1;
    for (int k = 0; k < 3; k++) f[k] /= l, z->eye[k] = eye[k], z->fwd[k] = f[k];
    if (f[2] > 0.999 || f[2] < -0.999) up[0] = 0, up[1] = 1, up[2] = 0;
    double r[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    l = cc_sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (int k = 0; k < 3; k++) z->right[k] = r[k] / l;
    z->up[0] = z->right[1] * f[2] - z->right[2] * f[1], z->up[1] = z->right[2] * f[0] - z->right[0] * f[2], z->up[2] = z->right[0] * f[1] - z->right[1] * f[0];
    z->w = cells_w, z->cell = w_px / cells_w, z->h = (int)(h_px / z->cell + 0.5);
    if (z->h < 1) z->h = 1;
    z->k = h_px / (2 * tan_half) / z->cell; /* in cells */
    z->near_mm = 100;
    size_t ns = (size_t)(z->w + 1) * (z->h + 1); /* samples at the cells' corners */
    z->depth = malloc(ns * sizeof *z->depth);
    if (!z->depth) return -1;
    for (size_t i = 0; i < ns; i++) z->depth[i] = 1.0f / 0.0f;
    return 0;
}

void cc_tiles_occ_free(cc_tiles_occ *z) {
    free(z->depth);
    memset(z, 0, sizeof *z);
}

/* a link-frame point to the camera: x, y in cells (from the buffer's corner), depth (mm along forward) */
static void occ_proj(const cc_tiles_occ *z, const double p[3], double c, double s, const double o[3], double out[3]) {
    double w3[3] = {c * p[0] - s * p[1] + o[0] - z->eye[0], s * p[0] + c * p[1] + o[1] - z->eye[1], p[2] + o[2] - z->eye[2]};
    double d = w3[0] * z->fwd[0] + w3[1] * z->fwd[1] + w3[2] * z->fwd[2];
    double x = w3[0] * z->right[0] + w3[1] * z->right[1] + w3[2] * z->right[2], y = w3[0] * z->up[0] + w3[1] * z->up[1] + w3[2] * z->up[2];
    out[2] = d;
    if (d > 0) out[0] = z->w / 2.0 + z->k * x / d, out[1] = z->h / 2.0 - z->k * y / d;
}

void cc_tiles_occ_draw(cc_tiles_occ *z, const cc_tiles_toc *toc, uint32_t i, double c, double s, const double o[3]) {
    if (!toc->occ || i >= toc->ntiles || !z->depth) return;
    cc_tile t;
    cc_tiles_get(toc, i, &t);
    for (uint32_t q = t.occ0; q < t.occ0 + t.nocc && q < toc->nocc; q++) {
        double P[3][3];
        int ok = 1;
        for (int v = 0; v < 3; v++) {
            double p[3] = {rdf(toc->occ + 36 * (size_t)q + 12 * v), rdf(toc->occ + 36 * (size_t)q + 12 * v + 4), rdf(toc->occ + 36 * (size_t)q + 12 * v + 8)};
            occ_proj(z, p, c, s, o, P[v]);
            ok &= P[v][2] > z->near_mm;
        }
        if (!ok) continue;
        /* 1 / depth is affine in screen space over a plane: its value at the cells' corners from three corners */
        double iz[3] = {1 / P[0][2], 1 / P[1][2], 1 / P[2][2]};
        double ax = P[1][0] - P[0][0], ay = P[1][1] - P[0][1], bx = P[2][0] - P[0][0], by = P[2][1] - P[0][1], den = ax * by - ay * bx;
        if (!(den > 1e-9 || den < -1e-9)) continue;
        double gx = ((iz[1] - iz[0]) * by - (iz[2] - iz[0]) * ay) / den, gy = ((iz[2] - iz[0]) * ax - (iz[1] - iz[0]) * bx) / den;
        double x0 = P[0][0], x1 = P[0][0], y0 = P[0][1], y1 = P[0][1];
        for (int v = 1; v < 3; v++) {
            x0 = P[v][0] < x0 ? P[v][0] : x0, x1 = P[v][0] > x1 ? P[v][0] : x1;
            y0 = P[v][1] < y0 ? P[v][1] : y0, y1 = P[v][1] > y1 ? P[v][1] : y1;
        }
        int px0 = x0 < 0 ? 0 : (int)x0 + 1, px1 = x1 > z->w ? z->w : (int)x1, py0 = y0 < 0 ? 0 : (int)y0 + 1, py1 = y1 > z->h ? z->h : (int)y1;
        double sg = den > 0 ? 1 : -1, eps = 1e-9 * (den > 0 ? den : -den);
        for (int py = py0; py <= py1; py++) /* each corner sample inside it (edges included): the nearest occluder there */
            for (int px = px0; px <= px1; px++) {
                int in = 1;
                for (int e = 0; e < 3 && in; e++) {
                    const double *a = P[e], *b = P[(e + 1) % 3];
                    double cr = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
                    if (cr * sg < -eps) in = 0;
                }
                double v = iz[0] + gx * (px - P[0][0]) + gy * (py - P[0][1]);
                if (!in || !(v > 0)) continue;
                float d = (float)(1 / v) * 1.0001f; /* rounded away from the eye */
                float *cell = &z->depth[(size_t)py * (z->w + 1) + px];
                if (d < *cell) *cell = d;
            }
    }
}

int cc_tiles_occ_hidden(const cc_tiles_occ *z, const float box[6], double c, double s, const double o[3]) {
    if (!z->depth) return 0;
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300, dmin = 1e300;
    for (int k = 0; k < 8; k++) {
        double p[3] = {k & 1 ? box[3] : box[0], k & 2 ? box[4] : box[1], k & 4 ? box[5] : box[2]}, q[3];
        occ_proj(z, p, c, s, o, q);
        if (!(q[2] > z->near_mm)) return 0;
        dmin = q[2] < dmin ? q[2] : dmin;
        x0 = q[0] < x0 ? q[0] : x0, x1 = q[0] > x1 ? q[0] : x1, y0 = q[1] < y0 ? q[1] : y0, y1 = q[1] > y1 ? q[1] : y1;
    }
    if (x1 < 0 || y1 < 0 || x0 >= z->w || y0 >= z->h) return 0; /* off the buffer: the frustum's business */
    int cx0 = x0 < 0 ? 0 : (int)x0, cx1 = x1 >= z->w ? z->w - 1 : (int)x1, cy0 = y0 < 0 ? 0 : (int)y0, cy1 = y1 >= z->h ? z->h - 1 : (int)y1;
    for (int cy = cy0; cy <= cy1; cy++) /* a cell is covered at the farthest of its corners */
        for (int cx = cx0; cx <= cx1; cx++)
            for (int k = 0; k < 4; k++)
                if (!(z->depth[(size_t)(cy + (k >> 1)) * (z->w + 1) + cx + (k & 1)] < dmin)) return 0;
    return 1;
}

/* ---- SHELL1: the exterior shell in the section (tiles.h) ---- */

void cc_tiles_shell_get(const cc_tiles_toc *toc, uint32_t i, cc_tile_shell *o) {
    memset(o, 0, sizeof *o);
    if (!toc->shell || i >= toc->ntiles) return;
    const uint8_t *r = toc->shell + CC_TILES_SHELL_HEAD + (size_t)CC_TILES_SHELL_REC * i;
    o->ext = rd32(r), o->flags = rd32(r + 4), o->off = (uint64_t)rd32(r + 8) | (uint64_t)rd32(r + 12) << 32;
    o->len = rd32(r + 16), o->tris = rd32(r + 20), o->verts = rd32(r + 24), o->error = rdf(r + 28);
}

int cc_tiles_shell_check(const uint8_t *p, size_t n, cc_tiles_toc *toc, cc_error *err) {
    toc->shell = NULL, toc->shell_n = 0;
    if (!(toc->bake & CC_TILES_BAKE_SHELL)) return fail(err, "tiles: no shell baked");
    if (n < CC_TILES_SHELL_HEAD || memcmp(p, "TSH1", 4) || rd32(p + 4) != 1 || rd32(p + 8) != toc->ntiles ||
        n != CC_TILES_SHELL_HEAD + (uint64_t)CC_TILES_SHELL_REC * toc->ntiles)
        return fail(err, "tiles: the shell table");
    int blobs = toc->n >= (uint64_t)toc->blob_at + toc->blobs; /* the proxies are there to check */
    const uint8_t *bl = toc->p + toc->blob_at;
    uint64_t ext_el = 0;
    for (uint32_t i = 0; i < toc->nlist; i++) ext_el += (toc->eflags[i] & CC_TILES_EL_EXTERIOR) != 0;
    if (rd32(p + 24) != ext_el) return fail(err, "tiles: the shell's elements");
    toc->shell = p, toc->shell_n = (uint32_t)n;
    for (uint32_t i = 0; i < toc->ntiles; i++) {
        cc_tile t;
        cc_tile_shell r;
        cc_tiles_get(toc, i, &t);
        cc_tiles_shell_get(toc, i, &r);
        int bad = (r.flags & ~(uint32_t)CC_TILE_SHELL_PROXY) || (r.ext > t.detail) || (!r.ext != !(t.flags & CC_TILE_EXTERIOR));
        if (t.flags & CC_TILE_LEAF) bad |= r.flags != 0 || r.error != 0.0f || r.len || r.tris != r.ext;
        else if (r.flags & CC_TILE_SHELL_PROXY) {
            bad |= !(t.flags & CC_TILE_PROXY) || !r.ext || !(r.error >= 0.0f && r.error < 3.0e38f) || r.len < 16 || (r.off & 3) || r.off + r.len > toc->blobs;
            if (!bad && blobs) {
                if (cc_tiles_proxy_check(bl + r.off, r.len, err)) return toc->shell = NULL, toc->shell_n = 0, -1;
                bad |= rd32(bl + r.off + 4) != r.verts || rd32(bl + r.off + 8) != 3ull * r.tris;
            }
        } else bad |= !(r.error > 3.0e38f) || r.len || r.tris;
        if (bad) return toc->shell = NULL, toc->shell_n = 0, fail(err, "tiles: a tile's shell");
        if (t.flags & CC_TILE_LEAF) { /* a leaf has exterior elements exactly when one is flagged */
            int any = 0;
            for (uint32_t k = 0; k < t.nel && !any; k++) any = (toc->eflags[t.el0 + k] & CC_TILES_EL_EXTERIOR) != 0;
            if (any != !!r.ext) return toc->shell = NULL, toc->shell_n = 0, fail(err, "tiles: a leaf's shell");
        }
    }
    return 0;
}

static void set32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

int cc_tiles_shell_apply(const uint8_t *sec, size_t n, const cc_tiles_src *src, const cc_tiles_opts *o, const uint8_t *ext,
                         const int *cancel, Buf *out, cc_tiles_shell_info *info, cc_error *err) {
    cc_tiles_toc toc;
    if (info) memset(info, 0, sizeof *info);
    if (cc_tiles_check(sec, n, 1, (uint32_t)src_n(src), &toc, err)) return -1;
    if (toc.bake & CC_TILES_BAKE_SHELL) return fail(err, "tiles: the shell is there already");
    uint32_t nt = toc.ntiles, nl = toc.nlist;
    uint64_t *pa = calloc((size_t)nl + 1, sizeof *pa), *pe = calloc((size_t)nl + 1, sizeof *pe); /* prefix sums: all, exterior */
    uint8_t *mask = calloc(src_n(src) + 1, 1);
    if (!pa || !pe || !mask) return free(pa), free(pe), free(mask), fail(err, "out of memory");
    uint32_t next = 0;
    for (uint32_t i = 0; i < nl; i++) {
        uint32_t e = cc_tiles_list(&toc, i);
        const cc_mesh *c = src_counts(src, e);
        uint64_t tr = c ? c->ni / 3 : 0;
        int x = ext[e] != 0;
        mask[e] = (uint8_t)x, next += (uint32_t)x;
        pa[i + 1] = pa[i] + tr, pe[i + 1] = pe[i] + (x ? tr : 0);
    }
    /* the tiles with both kinds of elements get a proxy of their exterior ones: a build of the same tree with the others left out */
    uint32_t chain = 0, mixed = 0;
    for (uint32_t i = 0; i < nt; i++) {
        cc_tile t;
        cc_tiles_get(&toc, i, &t);
        if (i == chain && t.nchildren == 1) chain++;
        uint64_t a = pa[t.el0 + t.nel] - pa[t.el0], x = pe[t.el0 + t.nel] - pe[t.el0];
        if ((t.flags & CC_TILE_PROXY) && x && x < a) mixed++;
    }
    cc_tiles m;
    cc_tiles_toc mt;
    int have_m = 0;
    memset(&m, 0, sizeof m);
    buf_init(&m.out);
    if (mixed) {
        cc_tiles_opts mo = *o;
        mo.occ = 0;
        int rc = cc_tiles_start(&m, src, &mo, err);
        if (!rc && m.work) m.work->mask = mask, m.work->force_chain = (int)chain;
        while (!rc) {
            if (cancel && cc_atomic_load(cancel)) {
                cc_tiles_free(&m), free(pa), free(pe), free(mask);
                return fail(err, "cancelled");
            }
            int st = cc_tiles_step(&m, 1000000, err);
            if (st < 0) rc = -1;
            if (st != 0) break;
        }
        if (rc) return cc_tiles_free(&m), free(pa), free(pe), free(mask), -1;
        have_m = !cc_tiles_check((const uint8_t *)m.out.p, m.out.len, 1, (uint32_t)src_n(src), &mt, NULL) && mt.ntiles == nt;
        for (uint32_t i = 0; have_m && i < nt; i++) { /* the same tree, tile for tile */
            cc_tile a, b;
            cc_tiles_get(&toc, i, &a), cc_tiles_get(&mt, i, &b);
            if (a.parent != b.parent || a.nchildren != b.nchildren || a.el0 != b.el0 || a.nel != b.nel ||
                (a.flags & 7u) != (b.flags & 7u))
                have_m = 0;
        }
        if (info) info->shell_build_ok = have_m;
    }
    /* the new section: the old one patched, the blobs grown by the mixed tiles' proxies, the table and its trailer */
    buf_clear(out);
    if (buf_reserve(out, n + 64 + (size_t)CC_TILES_SHELL_REC * nt + (have_m ? m.out.len : 0))) {
        cc_tiles_free(&m), free(pa), free(pe), free(mask);
        return fail(err, "out of memory");
    }
    buf_append(out, sec, n);
    uint8_t *q = (uint8_t *)out->p;
    set32(q + 44, toc.bake | CC_TILES_BAKE_SHELL);
    for (uint32_t i = 0; i < nl; i++)
        if (mask[cc_tiles_list(&toc, i)]) q[(size_t)(toc.eflags - toc.p) + i] |= CC_TILES_EL_EXTERIOR;
    Buf tab;
    buf_init(&tab);
    buf_append(&tab, "TSH1", 4);
    put32(&tab, 1), put32(&tab, nt), put32(&tab, o->shell_views), put32(&tab, o->shell_res), put32(&tab, o->shell_tol), put32(&tab, next), put32(&tab, 0);
    const uint8_t *mbl = have_m ? (const uint8_t *)m.out.p + mt.blob_at : NULL;
    uint64_t ext_tris = 0, shell_proxy_tris = 0, added = 0;
    for (uint32_t i = 0; i < nt; i++) {
        cc_tile t;
        cc_tiles_get(&toc, i, &t);
        uint64_t a = pa[t.el0 + t.nel] - pa[t.el0], x = pe[t.el0 + t.nel] - pe[t.el0];
        uint32_t x32 = x > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)x;
        if (x32 > t.detail) x32 = t.detail; /* both saturate */
        if (x) q[CC_TILES_HEAD + (size_t)CC_TILES_REC * i + 11] |= CC_TILE_EXTERIOR;
        if (i == 0) ext_tris = x;
        uint32_t fl = 0, len = 0, tr = 0, vs = 0;
        uint64_t off = 0;
        float er = (t.flags & CC_TILE_LEAF) ? 0.0f : 1.0f / 0.0f;
        if ((t.flags & CC_TILE_LEAF)) tr = x32;
        else if ((t.flags & CC_TILE_PROXY) && x && x == a) /* all exterior: its own proxy */
            fl = CC_TILE_SHELL_PROXY, off = t.off, len = t.len, tr = t.tris, vs = t.verts, er = t.error;
        else if ((t.flags & CC_TILE_PROXY) && x && have_m) { /* both kinds: the shell build's */
            cc_tile u;
            cc_tiles_get(&mt, i, &u);
            if (u.flags & CC_TILE_PROXY) {
                while ((out->len - (n - toc.blobs)) % 4) buf_putc(out, 0);
                off = out->len - (n - toc.blobs), len = u.len, tr = u.tris, vs = u.verts, er = u.error, fl = CC_TILE_SHELL_PROXY;
                buf_append(out, mbl + u.off, u.len);
                added += u.len, shell_proxy_tris += u.tris;
                q = (uint8_t *)out->p;
            }
        }
        put32(&tab, x32), put32(&tab, fl), put32(&tab, (uint32_t)off), put32(&tab, (uint32_t)(off >> 32));
        put32(&tab, len), put32(&tab, tr), put32(&tab, vs), putf(&tab, er);
    }
    while ((out->len - (n - toc.blobs)) % 8) buf_putc(out, 0);
    uint64_t toff = out->len - (n - toc.blobs);
    buf_append(out, tab.p, tab.len);
    buf_append(out, "TSHT", 4), put32(out, (uint32_t)tab.len), put32(out, (uint32_t)toff), put32(out, (uint32_t)(toff >> 32));
    uint64_t nb = out->len - (n - toc.blobs);
    if (!out->oom) set32((uint8_t *)out->p + 32, (uint32_t)nb), set32((uint8_t *)out->p + 36, (uint32_t)(nb >> 32));
    int bad = out->oom || tab.oom;
    buf_free(&tab), cc_tiles_free(&m), free(pa), free(pe), free(mask);
    if (bad) return fail(err, "out of memory");
    if (info) info->exterior = next, info->elements = nl, info->ext_tris = ext_tris, info->added = out->len - n, info->shell_proxy_tris = shell_proxy_tris, info->mixed = mixed;
    (void)added;
    cc_tiles_toc chk; /* what we wrote reads */
    if (cc_tiles_check((const uint8_t *)out->p, out->len, 1, (uint32_t)src_n(src), &chk, err)) return -1;
    return 0;
}

int cc_tiles_shell_outside(const cc_tiles_toc *toc, const double eye[3], double margin) {
    if (!(toc->bake & CC_TILES_BAKE_SHELL) || !toc->ntiles) return 0;
    cc_tile r;
    cc_tiles_get(toc, 0, &r);
    for (int k = 0; k < 3; k++)
        if (eye[k] < (double)r.box[k] - margin || eye[k] > (double)r.box[3 + k] + margin) return 1;
    return 0;
}
