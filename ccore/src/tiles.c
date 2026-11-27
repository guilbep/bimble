/* The tile pyramid (tiles.h; ticket TILE1, ADR 0078 amendment 6). */
#include <stdlib.h>
#include <string.h>

#include "hlod_grid.h"
#include "math.h"
#include "meshcodec.h"
#include "scene_build.h"
#include "tiles.h"

void cc_tiles_default(cc_tiles_opts *o) {
    o->leaf_tris = CC_TILES_LEAF_TRIS;
    o->proxy_tris = CC_TILES_PROXY_TRIS;
    o->root_tris = CC_TILES_ROOT_TRIS;
    o->res = CC_TILES_RES;
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
} Node;

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
    uint8_t *shape_flag;      /* per shape: closed and outward (bit 0), 0x80 unknown */
    double origin[3], g0;
    uint32_t *order, norder, at; /* post-order */
    Clusters *cl;             /* per node until its parent used them */
    Buf blobs;
    /* the leaf in progress */
    int open;
    Grid g;
    uint32_t j;               /* its next element */
    Arena dec;                /* decoded shapes of the leaf */
    Arena tmp;                /* an own mesh, per element */
    cc_mesh *shapes;          /* per shape: decoded for this leaf (nv = UINT32_MAX: not yet) */
    uint8_t *shape_seen;
    uint32_t *touched, ntouched;
    float (*wpos)[3];
    Key *keys;
    uint8_t *has;
    size_t cwpos;
    Occ occ[CC_TILES_OCC];
    uint32_t nocc;
    uint64_t leaf_verts;
    int chain_done;
};

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
    cc_hg_grid_free(&w->g);
    free(w->cl), free(w->nodes), free(w->els), free(w->box), free(w->tris), free(w->order), free(w->eflag), free(w->shape_flag);
    free(w->shapes), free(w->shape_seen), free(w->touched), free(w->wpos), free(w->keys), free(w->has);
    buf_free(&w->blobs);
    arena_free(&w->arena), arena_free(&w->dec), arena_free(&w->tmp);
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
    w->src = *src, w->o = *o;
    if (!w->o.leaf_tris) w->o.leaf_tris = 1;
    if (!w->o.proxy_tris) w->o.proxy_tris = 1;
    if (!w->o.res) w->o.res = 1;
    arena_init(&w->arena, 0), arena_init(&w->dec, 0), arena_init(&w->tmp, 0);
    buf_init(&w->blobs);
    size_t ne = src_n(src), ns = src_nshapes(src);
    if (ne >= CC_TILES_MAX_TILES) return fail(err, "too many elements for tiles");
    w->box = malloc((ne + 1) * sizeof *w->box);
    w->tris = malloc((ne + 1) * sizeof *w->tris);
    w->els = malloc((ne + 1) * sizeof *w->els);
    w->shapes = malloc((ns + 1) * sizeof *w->shapes);
    w->eflag = calloc(ne + 1, 1);
    w->shape_flag = malloc(ns + 1);
    if (w->shape_flag) memset(w->shape_flag, 0x80, ns + 1);
    w->shape_seen = calloc(ns + 1, 1);
    w->touched = malloc((ns + 1) * sizeof *w->touched);
    uint32_t *tmp = malloc((ne + 1) * sizeof *tmp);
    uint8_t *oct = malloc(ne + 1);
    MK *mk = malloc((ne + 1) * sizeof *mk);
    Pend *stack = malloc((8 * (size_t)CC_TILES_MAX_DEPTH + 16) * sizeof *stack);
    int rc = -1;
    if (!w->box || !w->tris || !w->els || !w->shapes || !w->eflag || !w->shape_flag || !w->shape_seen || !w->touched || !tmp || !oct || !mk || !stack) {
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
    for (size_t i = 0; i < ns; i++) w->shapes[i].nv = UINT32_MAX;
    rc = 0;
done:
    free(tmp), free(oct), free(mk), free(stack);
    return rc;
}

/* ---- per leaf: its elements' triangles clustered (for its parent), its counts, its occluders ---- */

static void occ_keep(Occ *set, uint32_t *n, const Occ *c) {
    if (*n < CC_TILES_OCC) {
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

static int u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static const float (*weld_pos)[3];
static int weld_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    int c = memcmp(weld_pos[x], weld_pos[y], 12);
    return c ? c : (x < y ? -1 : x > y);
}
/* Whether a mesh is closed and outward (in its own frame): vertices welded by position, every directed edge
 * met once by its reverse and never twice, the signed volume positive. Not thread-safe (weld_pos): the build
 * runs on one thread. */
static int closed_outward(const cc_mesh *m, Arena *a) {
    uint32_t nt = m->ni / 3;
    if (!nt || !m->nv) return 0;
    uint32_t *ord = arena_alloc_n(a, m->nv, sizeof *ord, 4), *wid = arena_alloc_n(a, m->nv, sizeof *wid, 4);
    uint64_t *e = arena_alloc_n(a, 3 * (size_t)nt, sizeof *e, 8);
    if (!ord || !wid || !e) return 0;
    for (uint32_t i = 0; i < m->nv; i++) ord[i] = i;
    weld_pos = (const float(*)[3])m->pos;
    qsort(ord, m->nv, sizeof *ord, weld_cmp);
    for (uint32_t i = 0, id = 0; i < m->nv; i++) {
        if (i && memcmp(m->pos[ord[i]], m->pos[ord[i - 1]], 12)) id++;
        wid[ord[i]] = id;
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
static int leaf_element(cc_tiles_work *w, Node *x, uint32_t el) {
    const cc_tiles_src *s = &w->src;
    int sh = src_shape(s, el);
    cc_mesh m;
    if (sh >= 0) {
        if ((size_t)sh >= src_nshapes(s)) return -1;
        if (w->shapes[sh].nv == UINT32_MAX) {
            if (src_block(s, (size_t)sh, &w->dec, &w->shapes[sh])) return -1;
            w->touched[w->ntouched++] = (uint32_t)sh;
            w->leaf_verts += w->shapes[sh].nv;
        }
        m = w->shapes[sh];
        if (w->shape_flag[sh] & 0x80) {
            arena_trim(&w->tmp, (size_t)1 << 24);
            w->shape_flag[sh] = (uint8_t)closed_outward(&m, &w->tmp);
        }
        w->eflag[el] = w->shape_flag[sh] ? CC_TILES_EL_CLOSED : 0;
    } else {
        arena_trim(&w->tmp, (size_t)1 << 24);
        if (src_block(s, src_nshapes(s) + el, &w->tmp, &m)) return -1;
        w->leaf_verts += m.nv;
        w->eflag[el] = closed_outward(&m, &w->tmp) ? CC_TILES_EL_CLOSED : 0;
    }
    if (opaque(s, el)) w->eflag[el] |= CC_TILES_EL_OPAQUE;
    if (m.nv > w->cwpos) {
        float(*p)[3] = realloc(w->wpos, (size_t)m.nv * sizeof *p);
        if (p) w->wpos = p;
        Key *k = realloc(w->keys, (size_t)m.nv * sizeof *k);
        if (k) w->keys = k;
        uint8_t *h = realloc(w->has, m.nv);
        if (h) w->has = h;
        if (!p || !k || !h) return -1;
        w->cwpos = m.nv;
    }
    const cc_placement32 *pl = src_pl(s, el);
    int mir = sh >= 0 && mirrors(pl);
    for (uint32_t i = 0; i < m.nv; i++) {
        if (sh >= 0) place(pl, m.pos[i], w->wpos[i]);
        else memcpy(w->wpos[i], m.pos[i], 12);
        w->has[i] = (uint8_t)lattice_key(w, w->wpos[i], w->g.e, &w->keys[i]);
    }
    Fam *f = &w->g.f[src_cat(s, el)];
    /* each used vertex once (an unused one would only move a cluster's mean) */
    uint8_t *used = arena_alloc_n(&w->tmp, (size_t)m.nv + 1, 1, 1);
    if (!used) return -1;
    memset(used, 0, (size_t)m.nv + 1);
    int opq = opaque(s, el);
    double amin = extent(x->box) / 64.0;
    amin *= amin;
    for (uint32_t t = 0; t / 3 < m.ni / 3; t += 3) {
        uint32_t a = m.idx[t], b = m.idx[t + (mir ? 2 : 1)], c = m.idx[t + (mir ? 1 : 2)];
        if (a >= m.nv || b >= m.nv || c >= m.nv) continue;
        uint32_t v3[3] = {a, b, c};
        int ok = 1;
        for (int q = 0; q < 3; q++) {
            uint32_t v = v3[q];
            if (!w->has[v]) { ok = 0; continue; }
            if (!used[v]) {
                used[v] = 1;
                double sm[3] = {w->wpos[v][0], w->wpos[v][1], w->wpos[v][2]};
                if (cc_hg_fam_vert(f, w->keys[v], sm, 1, NULL)) return -1;
            }
        }
        if (ok && cc_hg_fam_tri(f, w->keys[a], w->keys[b], w->keys[c])) return -1;
        if (opq && ok) {
            double u[3], v[3], n[3];
            for (int q = 0; q < 3; q++) u[q] = (double)w->wpos[b][q] - w->wpos[a][q], v[q] = (double)w->wpos[c][q] - w->wpos[a][q];
            n[0] = u[1] * v[2] - u[2] * v[1], n[1] = u[2] * v[0] - u[0] * v[2], n[2] = u[0] * v[1] - u[1] * v[0];
            double ar = cc_sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) / 2.0;
            if (ar >= amin && ar > 0) {
                Occ o;
                memcpy(o.t, w->wpos[a], 12), memcpy(o.t + 3, w->wpos[b], 12), memcpy(o.t + 6, w->wpos[c], 12);
                o.area = ar, o.el = el, o.tri = t / 3;
                occ_keep(w->occ, &w->nocc, &o);
            }
        }
        if ((t / 3) % 4096 == 4095 && cc_hg_grid_fit(&w->g, w->o.proxy_tris, 0)) return -1;
    }
    return cc_hg_grid_fit(&w->g, w->o.proxy_tris, 0);
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
                    double d = (double)(p->v[v].n ? p->v[v].n : 1), r2 = 0;
                    for (int j = 0; j < 3; j++) {
                        double mj = p->v[v].s[j] / d, a = mj - p->v[v].lo[j], c = p->v[v].hi[j] - mj, q = a > c ? a : c;
                        r2 += q * q;
                        put16(b, quant16((float)mj, x->box[j], x->box[3 + j]));
                    }
                    if (r2 > dev2) dev2 = r2;
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
    x->error = (float)(cc_sqrt(dev2) + cc_sqrt(qd));
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
        cc_hg_clusters_free(ch);
    }
    Clusters cl;
    memset(&cl, 0, sizeof cl);
    if (cc_hg_grid_fit(&g, cap, 1) || cc_hg_grid_finish(&g, &cl)) return cc_hg_grid_free(&g), cc_hg_clusters_free(&cl), -1;
    if (write_proxy(w, &cl, x)) return cc_hg_clusters_free(&cl), -1;
    w->cl[n] = cl;
    /* occluders: the children's largest */
    Occ set[CC_TILES_OCC];
    uint32_t no = 0;
    for (uint32_t k = 0; k < x->nkids; k++) {
        const Node *c = &w->nodes[x->kids[k]];
        for (uint32_t q = 0; q < c->nocc; q++) occ_keep(set, &no, &c->occ[q]);
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
        if (cur <= w->o.root_tris && (x->flags & CC_TILE_PROXY)) break;
        if (x->flags & CC_TILE_LEAF && x->detail <= w->o.root_tris) break; /* a tiny link: the leaf is the root */
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
        if (occ_store(w, y, x->occ, x->nocc)) return cc_hg_clusters_free(&out), -1;
        cc_hg_clusters_free(cl);
        w->cl[q] = out;
        top = q;
        if (y->tris <= w->o.root_tris) break;
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
    uint64_t toc = CC_TILES_HEAD + (uint64_t)CC_TILES_REC * np + 4ull * w->nels + ((w->nels + 3ull) & ~3ull) + 36ull * nocc;
    uint64_t tocp = (toc + 7) & ~7ull;
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

int cc_tiles_step(cc_tiles *t, uint64_t budget, cc_error *err) {
    cc_tiles_work *w = t->work;
    if (!w) return 1;
    uint64_t used = 0;
    const cc_tiles_src *s = &w->src;
    while (w->at < w->norder) {
        uint32_t n = w->order[w->at];
        Node *x = &w->nodes[n];
        if (x->flags & CC_TILE_LEAF) {
            if (!w->open) {
                memset(&w->g, 0, sizeof w->g);
                w->g.e = CC_TILES_MAX_DEPTH - (x->cdepth < CC_TILES_MAX_DEPTH ? x->cdepth : CC_TILES_MAX_DEPTH);
                w->open = 1, w->j = 0, w->nocc = 0, w->leaf_verts = 0, w->ntouched = 0;
                arena_reset(&w->dec);
            }
            while (w->j < x->nel) {
                uint32_t el = w->els[x->el0 + w->j];
                if (leaf_element(w, x, el)) return fail(err, "a mesh does not decode, or out of memory");
                x = &w->nodes[n];
                w->j++;
                used += w->tris[el], t->done += w->tris[el];
                if (used >= budget && w->j < x->nel) return 0;
            }
            for (uint32_t i = 0; i < w->ntouched; i++) w->shapes[w->touched[i]].nv = UINT32_MAX;
            w->ntouched = 0;
            Clusters cl;
            memset(&cl, 0, sizeof cl);
            if (cc_hg_grid_fit(&w->g, w->o.proxy_tris, 1) || cc_hg_grid_finish(&w->g, &cl)) {
                cc_hg_clusters_free(&cl);
                return fail(err, "out of memory");
            }
            w->cl[n] = cl;
            x->tris = (uint32_t)(x->detail > 0xFFFFFFFFull ? 0xFFFFFFFFu : x->detail);
            x->verts = (uint32_t)(w->leaf_verts > 0xFFFFFFFFull ? 0xFFFFFFFFu : w->leaf_verts);
            if (occ_store(w, x, w->occ, w->nocc)) return fail(err, "out of memory");
            w->open = 0;
            arena_reset(&w->dec);
        } else if (inner(w, n)) return fail(err, "out of memory");
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

int cc_tiles_check(const uint8_t *p, size_t n, int with_blobs, uint32_t nelements, cc_tiles_toc *toc, cc_error *err) {
    memset(toc, 0, sizeof *toc);
    if (n < CC_TILES_HEAD || memcmp(p, "TIL1", 4) || rd32(p + 4) != CC_TILES_VERSION) return fail(err, "tiles: not TIL1 version 1");
    uint64_t nt = rd32(p + 8), ne = rd32(p + 12), nl = rd32(p + 16), no = rd32(p + 20), tb = rd32(p + 24);
    uint64_t blobs = (uint64_t)rd32(p + 32) | (uint64_t)rd32(p + 36) << 32;
    if (nelements != CC_TILES_NONE && ne != nelements) return fail(err, "tiles: of another scene");
    if (nt > CC_TILES_MAX_TILES || nl > ne || no > (uint64_t)CC_TILES_OCC * nt) return fail(err, "tiles: counts");
    if (tb != CC_TILES_HEAD + (uint64_t)CC_TILES_REC * nt + 4 * nl + ((nl + 3) & ~3ull) + 36 * no) return fail(err, "tiles: table size");
    uint64_t tp = (tb + 7) & ~7ull;
    if (with_blobs ? (tp + blobs != n) : (n < tb)) return fail(err, "tiles: section size");
    if (rd32(p + 40) > CC_TILES_MAX_LEVELS || rd32(p + 44)) return fail(err, "tiles: levels");
    if (!nt && (nl || no || blobs)) return fail(err, "tiles: counts");
    toc->p = p, toc->n = n, toc->ntiles = (uint32_t)nt, toc->nelements = (uint32_t)ne, toc->nlist = (uint32_t)nl;
    toc->nocc = (uint32_t)no, toc->toc = (uint32_t)tb, toc->levels = rd32(p + 40), toc->leaf_target = rd32(p + 28);
    toc->blobs = blobs;
    toc->list = p + CC_TILES_HEAD + (size_t)CC_TILES_REC * nt;
    toc->eflags = toc->list + 4 * nl;
    toc->occ = toc->eflags + ((nl + 3) & ~3ull);
    for (uint64_t i = 0; i < nl; i++)
        if (toc->eflags[i] & ~3u) return fail(err, "tiles: an element's flags");
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
    for (uint64_t i = 0; i < 9 * no; i++)
        if (!(rdf(toc->occ + 4 * i) > -3.0e38f && rdf(toc->occ + 4 * i) < 3.0e38f)) return fail(err, "tiles: an occluder");
    uint64_t leaves_el = 0;
    for (uint32_t i = 0; i < nt; i++) {
        cc_tile t;
        cc_tiles_get(toc, i, &t);
        if (i == 0 ? (t.parent != CC_TILES_NONE || t.level != 0 || t.skip != nt) : (t.parent >= i)) return fail(err, "tiles: a parent");
        if (t.flags & ~15u) return fail(err, "tiles: flags");
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
    if (with_blobs) {
        const uint8_t *bl = p + tp;
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
    buf_append(out, "TLF1", 4);
    put32(out, id), put32(out, t.nel), put32(out, 0);
    static const float ID[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    for (uint32_t i = 0; i < t.nel; i++) {
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

uint32_t cc_tiles_select(const cc_tiles_toc *toc, const cc_tiles_view *v, double error_px, uint64_t budget,
                         const uint8_t *descend, uint32_t *out, uint32_t cap, uint64_t *tris) {
    *tris = 0;
    if (!toc->ntiles) return 0;
    HeapE *h = malloc((size_t)toc->ntiles * sizeof *h), *fin = malloc((size_t)toc->ntiles * sizeof *fin);
    if (!h || !fin) return free(h), free(fin), 0;
    uint32_t nh = 0, nf = 0;
    uint64_t total = 0;
    cc_tile t;
    cc_tiles_get(toc, 0, &t);
#define PRI(T, I, D)                                                                                                                   \
    (((T).flags & CC_TILE_LEAF) ? 0.0                                                                                                  \
     : (!((T).flags & CC_TILE_PROXY) || (descend && descend[I])) ? 1e300                                                              \
     : v->px_per_mm > 0 ? (double)(T).error * v->px_per_mm : (D) > 1.0 ? (double)(T).error * v->k / (D) : 1e299)
    if (in_view(v, t.box)) {
        double d = box_dist(v->eye, t.box);
        heap_push(h, &nh, (HeapE){PRI(t, 0, d), d, 0});
        total = t.tris;
    }
    while (nh) {
        HeapE e = h[0];
        cc_tiles_get(toc, e.id, &t);
        int forced = e.pri >= 1e300;
        if ((t.flags & CC_TILE_LEAF) || (!forced && e.pri <= error_px)) break;
        uint64_t add = 0;
        for (uint32_t c = e.id + 1, q = 0; q < t.nchildren; q++) {
            cc_tile u;
            cc_tiles_get(toc, c, &u);
            if (in_view(v, u.box)) add += u.tris;
            c += u.skip;
        }
        if (!forced && total - t.tris + add > budget) break;
        heap_pop(h, &nh);
        total = total - t.tris + add;
        for (uint32_t c = e.id + 1, q = 0; q < t.nchildren; q++) {
            cc_tile u;
            cc_tiles_get(toc, c, &u);
            uint32_t me = c;
            c += u.skip;
            if (!in_view(v, u.box)) continue;
            double d = box_dist(v->eye, u.box);
            heap_push(h, &nh, (HeapE){PRI(u, me, d), d, me});
        }
    }
#undef PRI
    for (uint32_t i = 0; i < nh; i++) {
        cc_tile u;
        cc_tiles_get(toc, h[i].id, &u);
        if (!(u.flags & (CC_TILE_LEAF | CC_TILE_PROXY))) continue; /* cannot stand (never left when refined) */
        fin[nf++] = h[i];
    }
    qsort(fin, nf, sizeof *fin, sel_cmp);
    uint32_t n = 0;
    for (uint32_t i = 0; i < nf; i++) {
        cc_tile u;
        cc_tiles_get(toc, fin[i].id, &u);
        *tris += u.tris;
        if (n < cap) out[n++] = fin[i].id;
    }
    free(h), free(fin);
    return n;
}
