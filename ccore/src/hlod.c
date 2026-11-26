/* HLOD proxies (hlod.h): a port of bimble-geom hlod.rs (ticket 3D2, ADR 0078). */
#include <stdlib.h>
#include <string.h>

#include "hlod.h"
#include "math.h"
#include "scene_build.h"

void cc_hlod_default(cc_hlod_opts *o) { /* hlod.rs:50 */
    o->leaf_triangles = 65536;
    o->resolution = 64;
    o->max_proxy_triangles = 65536;
    o->max_ratio = 0.25;
}

static int fail(cc_error *err, const char *msg) {
    size_t n = strlen(msg);
    err->offset = CC_NO_OFFSET;
    if (n >= sizeof err->msg) n = sizeof err->msg - 1;
    memcpy(err->msg, msg, n);
    err->msg[n] = 0;
    return -1;
}

/* ---- the clusters (hlod.rs:236 Family, Grid; 377 Clusters) ---- */

typedef struct {
    int64_t k[3];
} Key;
typedef struct {
    Key k;
    double s[3]; /* sum of the original vertices in the grid cell */
    uint64_t n;  /* their count */
} Vert;
typedef struct {
    Key t[3];
} Tri;
typedef struct {
    Vert *v;
    size_t nv, cv;
    Tri *t;
    size_t nt, ct;
} Fam;
typedef struct {
    uint32_t e; /* the grid's cell size is g0 x 2^e */
    Fam f[CC_CAT_COUNT];
    uint64_t tmark, vmark; /* triangles and vertices after the last merge */
} Grid;
typedef struct { /* a finished family: verts sorted by key, triangles as indices into them */
    Vert *v;
    size_t nv;
    uint32_t (*t)[3];
    size_t nt;
} Part;
typedef struct {
    uint32_t e;
    int built;
    Part p[CC_CAT_COUNT];
} Clusters;

static int key_cmp(const Key *a, const Key *b) {
    for (int i = 0; i < 3; i++)
        if (a->k[i] != b->k[i]) return a->k[i] < b->k[i] ? -1 : 1;
    return 0;
}
static int key_eq(const Key *a, const Key *b) { return !key_cmp(a, b); }
static int vert_cmp(const void *a, const void *b) { return key_cmp(&((const Vert *)a)->k, &((const Vert *)b)->k); }
static int tri_cmp(const void *a, const void *b) {
    const Tri *x = a, *y = b;
    for (int i = 0; i < 3; i++) {
        int c = key_cmp(&x->t[i], &y->t[i]);
        if (c) return c;
    }
    return 0;
}

/* `c >> s` on i64 (floor division by 2^s), as Rust's arithmetic shift. */
static int64_t shr(int64_t c, unsigned s) { return c >= 0 ? c >> s : ~((~c) >> s); }
static Key up(Key k, unsigned s) {
    for (int i = 0; i < 3; i++) k.k[i] = shr(k.k[i], s);
    return k;
}

static int fam_vert(Fam *f, Key k, const double s[3], uint64_t n) { /* hlod.rs:247 add_vertex (merged later) */
    if (f->nv == f->cv) {
        size_t c = f->cv ? 2 * f->cv : 64;
        Vert *v = realloc(f->v, c * sizeof *v);
        if (!v) return -1;
        f->v = v, f->cv = c;
    }
    Vert *v = &f->v[f->nv++];
    v->k = k, v->n = n;
    memcpy(v->s, s, sizeof v->s);
    return 0;
}

/* hlod.rs:254 add_triangle: dropped when two corners share a cell; kept once whatever corner it
 * starts from (the smallest first, the turn kept). */
static int fam_tri(Fam *f, Key a, Key b, Key c) {
    if (key_eq(&a, &b) || key_eq(&b, &c) || key_eq(&a, &c)) return 0;
    Key t[3] = {a, b, c};
    int r = 0;
    for (int i = 1; i < 3; i++)
        if (key_cmp(&t[i], &t[r]) < 0) r = i;
    if (f->nt == f->ct) {
        size_t cp = f->ct ? 2 * f->ct : 64;
        Tri *v = realloc(f->t, cp * sizeof *v);
        if (!v) return -1;
        f->t = v, f->ct = cp;
    }
    Tri *o = &f->t[f->nt++];
    o->t[0] = t[r], o->t[1] = t[(r + 1) % 3], o->t[2] = t[(r + 2) % 3];
    return 0;
}

/* The vertices of one grid cell merged (sums added), the triangles made unique. */
static void fam_compact(Fam *f) {
    if (f->nv > 1) {
        qsort(f->v, f->nv, sizeof *f->v, vert_cmp);
        size_t o = 0;
        for (size_t i = 1; i < f->nv; i++) {
            if (key_eq(&f->v[i].k, &f->v[o].k)) {
                for (int c = 0; c < 3; c++) f->v[o].s[c] += f->v[i].s[c];
                f->v[o].n += f->v[i].n;
            } else f->v[++o] = f->v[i];
        }
        f->nv = o + 1;
    }
    if (f->nt > 1) {
        qsort(f->t, f->nt, sizeof *f->t, tri_cmp);
        size_t o = 0;
        for (size_t i = 1; i < f->nt; i++)
            if (tri_cmp(&f->t[i], &f->t[o])) f->t[++o] = f->t[i];
        f->nt = o + 1;
    }
}

static void grid_free(Grid *g) {
    for (int i = 0; i < CC_CAT_COUNT; i++) free(g->f[i].v), free(g->f[i].t);
    memset(g, 0, sizeof *g);
}

static uint64_t grid_triangles(const Grid *g) {
    uint64_t n = 0;
    for (int i = 0; i < CC_CAT_COUNT; i++) n += g->f[i].nt;
    return n;
}

static uint64_t grid_vertices(const Grid *g) {
    uint64_t n = 0;
    for (int i = 0; i < CC_CAT_COUNT; i++) n += g->f[i].nv;
    return n;
}

/* hlod.rs:345 fit: coarsens the grid (each step halves the cells per axis) until the triangles fit.
 * Rust fits after every chunk with hash maps; here the appended vertices and triangles are merged
 * (sorted) only when they have doubled since the last merge and pass the cap, and always at the end
 * (`force`): the unique triangles at a grid only grow as triangles are added and only fall as the
 * grid coarsens, so the final grid is the same coarsest-needed one, in O(n log n). */
static int grid_fit(Grid *g, uint64_t max, int force) {
    uint64_t nt = grid_triangles(g), nv = grid_vertices(g);
    if (!force && !((nt > max && nt >= 2 * g->tmark) || (nv > 4 * max && nv >= 2 * g->vmark))) return 0;
    for (int i = 0; i < CC_CAT_COUNT; i++) fam_compact(&g->f[i]);
    while (grid_triangles(g) > max && g->e < 62) {
        g->e++;
        for (int i = 0; i < CC_CAT_COUNT; i++) {
            Fam *f = &g->f[i];
            for (size_t v = 0; v < f->nv; v++) f->v[v].k = up(f->v[v].k, 1);
            size_t n = f->nt;
            f->nt = 0;
            for (size_t t = 0; t < n; t++) { /* in place: fam_tri writes at most where it reads */
                Tri x = f->t[t];
                if (fam_tri(f, up(x.t[0], 1), up(x.t[1], 1), up(x.t[2], 1))) return -1;
            }
            fam_compact(f);
        }
    }
    g->tmark = grid_triangles(g), g->vmark = grid_vertices(g);
    return 0;
}

/* hlod.rs:330 add_clusters: a child's clusters, on this coarser grid. */
static int grid_add_clusters(Grid *g, const Clusters *cl) {
    unsigned shift = g->e > cl->e ? g->e - cl->e : 0;
    if (shift > 63) shift = 63;
    for (int i = 0; i < CC_CAT_COUNT; i++) {
        const Part *p = &cl->p[i];
        Fam *f = &g->f[i];
        for (size_t v = 0; v < p->nv; v++)
            if (fam_vert(f, up(p->v[v].k, shift), p->v[v].s, p->v[v].n)) return -1;
        for (size_t t = 0; t < p->nt; t++)
            if (fam_tri(f, up(p->v[p->t[t][0]].k, shift), up(p->v[p->t[t][1]].k, shift), up(p->v[p->t[t][2]].k, shift)))
                return -1;
    }
    return 0;
}

static void clusters_free(Clusters *c) {
    for (int i = 0; i < CC_CAT_COUNT; i++) free(c->p[i].v), free(c->p[i].t);
    memset(c, 0, sizeof *c);
}

static size_t find_vert(const Vert *v, size_t n, const Key *k) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = key_cmp(&v[mid].k, k);
        if (!c) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0; /* hlod.rs:372 unwrap_or(0): never, every corner was added as a vertex */
}

/* hlod.rs:360 finish: sorted (deterministic) and compact; the grid is emptied. */
static int grid_finish(Grid *g, Clusters *out) {
    memset(out, 0, sizeof *out);
    out->e = g->e, out->built = 1;
    for (int i = 0; i < CC_CAT_COUNT; i++) {
        Fam *f = &g->f[i];
        fam_compact(f);
        if (!f->nt) continue; /* a family without triangles is dropped */
        Part *p = &out->p[i];
        p->t = malloc(f->nt * sizeof *p->t);
        if (!p->t) return -1;
        for (size_t t = 0; t < f->nt; t++)
            for (int c = 0; c < 3; c++) p->t[t][c] = (uint32_t)find_vert(f->v, f->nv, &f->t[t].t[c]);
        p->nt = f->nt;
        p->v = f->v, p->nv = f->nv; /* the grid's sorted vertices move into the part */
        f->v = NULL, f->nv = f->cv = 0;
    }
    grid_free(g);
    return 0;
}

static uint64_t clusters_triangles(const Clusters *c) {
    uint64_t n = 0;
    for (int i = 0; i < CC_CAT_COUNT; i++) n += c->p[i].nt;
    return n;
}

/* ---- the build's state ---- */

struct cc_hlod_work {
    const cc_scene *s;
    cc_hlod_opts o;
    double origin[3], g0; /* the lattice (hlod.rs:224): grids of g0 x 2^e from the root's corner */
    uint64_t *tris;       /* per scene element, its triangles */
    uint32_t *order;      /* the cells deepest first */
    uint32_t at;          /* the next of `order` */
    Clusters *cl;         /* per cell, until its parent has used them */
    Grid g;               /* the cell in progress (`open`) */
    int open;
    uint32_t j;           /* its element j, from triangle `off` */
    uint64_t off;
    float (*wpos)[3];     /* an element's placed positions */
    size_t cwpos;
    uint32_t *stamp;      /* per vertex: the element (serial) its key was made for */
    Key *keys;
    uint8_t *ke;          /* the grid (e) each key was made on */
    uint8_t *has;
    size_t ckeys;
    uint32_t chunk;
    Arena scratch;
};

static double lattice_size(const struct cc_hlod_work *w, uint32_t e) { /* hlod.rs:229 */
    double g = w->g0;
    for (uint32_t i = 0; i < e; i++) g *= 2.0;
    return g;
}

/* hlod.rs:233 key: none for a non-finite vertex; floor, saturating like Rust's `as`. */
static int lattice_key(const struct cc_hlod_work *w, const float p[3], uint32_t e, Key *k) {
    double g = lattice_size(w, e);
    for (int i = 0; i < 3; i++) {
        if (p[i] != p[i] || p[i] - p[i] != 0.0f) return 0;
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

static void work_free(struct cc_hlod_work *w, uint32_t ncells) {
    if (!w) return;
    if (w->cl)
        for (uint32_t i = 0; i < ncells; i++) clusters_free(&w->cl[i]);
    grid_free(&w->g);
    free(w->cl), free(w->tris), free(w->order), free(w->wpos), free(w->stamp), free(w->keys), free(w->has), free(w->ke);
    arena_free(&w->scratch);
    free(w);
}

void cc_hlod_free(cc_hlod *h) {
    work_free(h->work, h->ncells);
    free(h->cells);
    arena_free(&h->arena);
    memset(h, 0, sizeof *h);
}

/* The mesh element k's triangles come from, and whether its placement mirrors. */
static const cc_mesh *el_mesh(const cc_scene *s, size_t k, int *mirrors) {
    const cc_scene_element *e = &s->elements[k];
    *mirrors = 0;
    if (e->shape < 0) return &e->mesh;
    const cc_placement32 *p = &e->to_scene;
    float c[3] = {p->y[1] * p->z[2] - p->y[2] * p->z[1], p->y[2] * p->z[0] - p->y[0] * p->z[2], p->y[0] * p->z[1] - p->y[1] * p->z[0]};
    *mirrors = p->x[0] * c[0] + p->x[1] * c[1] + p->x[2] * c[2] < 0.0f;
    return &s->shapes[e->shape];
}

/* Element k's vertices in scene coordinates (scene.c cc_scene_world_mesh's sums), into w->wpos. */
static const float (*placed(struct cc_hlod_work *w, size_t k, const cc_mesh *m))[3] {
    const cc_scene_element *e = &w->s->elements[k];
    if (e->shape < 0) return (const float (*)[3])m->pos;
    if (m->nv > w->cwpos) {
        float(*p)[3] = realloc(w->wpos, (size_t)m->nv * sizeof *p);
        if (!p) return NULL;
        w->wpos = p, w->cwpos = m->nv;
    }
    const cc_placement32 *p = &e->to_scene;
    for (uint32_t i = 0; i < m->nv; i++) {
        const float *q = m->pos[i];
        for (int j = 0; j < 3; j++) w->wpos[i][j] = p->x[j] * q[0] + p->y[j] * q[1] + p->z[j] * q[2] + p->t[j];
    }
    return (const float (*)[3])w->wpos;
}

/* hlod.rs:284 add_triangles: triangles [t0, t1) of a placed mesh into the grid's family; each vertex
 * counts once per element in its cluster's mean (Rust: once per chunk of the cap; here per element, so
 * that the steps' chunks do not change the result), its key moved to the grid as it coarsens. */
static int add_triangles(struct cc_hlod_work *w, cc_category cat, const float (*pos)[3], uint32_t nv, const uint32_t *idx,
                         uint64_t t0, uint64_t t1, int mirrors) {
    Fam *f = &w->g.f[(unsigned)cat < CC_CAT_COUNT ? cat : CC_CAT_OTHER];
    for (uint64_t t = t0; t < t1; t++) {
        Key k[3];
        int ok = 1;
        for (int c = 0; c < 3; c++) {
            uint32_t i = idx[3 * t + (uint64_t)(mirrors && c ? 3 - c : c)]; /* a mirror turns the winding */
            if (i >= nv) { ok = 0; continue; }
            if (w->stamp[i] != w->chunk) {
                w->stamp[i] = w->chunk;
                w->has[i] = (uint8_t)lattice_key(w, pos[i], w->g.e, &w->keys[i]);
                w->ke[i] = (uint8_t)w->g.e;
                if (w->has[i]) {
                    double s[3] = {pos[i][0], pos[i][1], pos[i][2]};
                    if (fam_vert(f, w->keys[i], s, 1)) return -1;
                }
            }
            if (!w->has[i]) ok = 0;
            else k[c] = up(w->keys[i], w->g.e - w->ke[i]);
        }
        if (ok && fam_tri(f, k[0], k[1], k[2])) return -1;
    }
    return 0;
}

/* hlod.rs:384 meshes: vertices at their cluster's mean, unused ones dropped, crease edges. */
static int proxy_of(cc_hlod *h, struct cc_hlod_work *w, const Clusters *cl, cc_hlod_cell *c) {
    uint32_t n = 0;
    for (int i = 0; i < CC_CAT_COUNT; i++) n += cl->p[i].nt > 0;
    c->proxy = arena_alloc(&h->arena, ((size_t)n + 1) * sizeof *c->proxy, 8);
    if (!c->proxy) return -1;
    for (int i = 0; i < CC_CAT_COUNT; i++) {
        const Part *p = &cl->p[i];
        if (!p->nt) continue;
        cc_hlod_part *o = &c->proxy[c->nproxy++];
        o->cat = (cc_category)i;
        cc_mesh *m = &o->mesh;
        memset(m, 0, sizeof *m);
        arena_reset(&w->scratch);
        uint32_t *used = arena_alloc_n(&w->scratch, p->nv + 1, sizeof *used, 4);
        m->pos = arena_alloc_n(&h->arena, p->nv + 1, sizeof m->pos[0], 4);
        m->idx = arena_alloc_n(&h->arena, 3 * p->nt + 1, sizeof *m->idx, 4);
        if (!used || !m->pos || !m->idx) return -1;
        memset(used, 0xff, (p->nv + 1) * sizeof *used);
        for (size_t t = 0; t < p->nt; t++)
            for (int k = 0; k < 3; k++) {
                uint32_t v = p->t[t][k];
                if (used[v] == UINT32_MAX) {
                    used[v] = m->nv;
                    double d = (double)(p->v[v].n ? p->v[v].n : 1);
                    for (int j = 0; j < 3; j++) m->pos[m->nv][j] = (float)(p->v[v].s[j] / d);
                    m->nv++;
                }
                m->idx[m->ni++] = used[v];
            }
        if (cc_scene_crease_edges(m, &h->arena, &w->scratch)) return -1;
    }
    return 0;
}

/* ---- the tree, top-down (hlod.rs:116) ---- */

typedef struct {
    uint32_t depth, start, count, parent;
    uint64_t corner[3];
} Pending;

int cc_hlod_start(cc_hlod *h, const cc_scene *s, const cc_hlod_opts *o, cc_error *err) {
    memset(h, 0, sizeof *h);
    arena_init(&h->arena, 0);
    struct cc_hlod_work *w = calloc(1, sizeof *w);
    if (!w) return fail(err, "out of memory");
    h->work = w;
    arena_init(&w->scratch, 0);
    w->s = s, w->o = *o;
    size_t ne = s->nelements;
    if (ne > (CC_HLOD_MAX_CELLS / 2)) return fail(err, "too many elements for proxies");
    float(*box)[6] = malloc((ne + 1) * sizeof *box);
    w->tris = malloc((ne + 1) * sizeof *w->tris);
    h->elements = arena_alloc_n(&h->arena, ne + 1, sizeof *h->elements, 4);
    uint32_t *tmp = malloc((ne + 1) * sizeof *tmp);
    uint8_t *oct = malloc(ne + 1);
    Pending *stack = malloc((8 * (size_t)CC_HLOD_MAX_DEPTH + 9) * sizeof *stack);
    uint32_t(*kids)[8] = NULL;
    uint8_t *nkids = NULL;
    size_t ccells = 0;
    int rc = -1;
    if (!box || !w->tris || !h->elements || !tmp || !oct || !stack) { fail(err, "out of memory"); goto done; }
    /* bvh::element_boxes: each element's placed vertices (non-finite ones skipped) */
    float all[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    for (size_t k = 0; k < ne; k++) {
        int mir;
        const cc_mesh *m = el_mesh(s, k, &mir);
        const float(*p)[3] = placed(w, k, m);
        if (!p) { fail(err, "out of memory"); goto done; }
        float b[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
        for (uint32_t i = 0; i < m->nv; i++) {
            int fin = 1;
            for (int j = 0; j < 3; j++) fin &= p[i][j] == p[i][j] && p[i][j] - p[i][j] == 0.0f;
            if (!fin) continue;
            for (int j = 0; j < 3; j++) {
                if (p[i][j] < b[j]) b[j] = p[i][j];
                if (p[i][j] > b[3 + j]) b[3 + j] = p[i][j];
            }
        }
        memcpy(box[k], b, sizeof b);
        w->tris[k] = m->ni / 3;
        if (b[0] <= b[3] && b[1] <= b[4] && b[2] <= b[5]) {
            h->elements[h->nelements++] = (uint32_t)k;
            for (int j = 0; j < 3; j++) {
                if (b[j] < all[j]) all[j] = b[j];
                if (b[3 + j] > all[3 + j]) all[3 + j] = b[3 + j];
            }
        }
    }
    if (!h->nelements) { rc = 0; goto done; } /* hlod.rs:118: an empty scene has no cells */
    double side = 1.0;
    for (int j = 0; j < 3; j++) {
        double d = (double)all[3 + j] - (double)all[j];
        if (d > side) side = d;
        w->origin[j] = all[j];
    }
    w->g0 = side / (double)(o->resolution ? o->resolution : 1) / (double)(1u << CC_HLOD_MAX_DEPTH);
    size_t sp = 0;
    stack[sp++] = (Pending){0, 0, h->nelements, UINT32_MAX, {0, 0, 0}};
    while (sp) {
        Pending c = stack[--sp];
        if (h->ncells >= CC_HLOD_MAX_CELLS) { fail(err, "too many proxy cells"); goto done; }
        if (h->ncells == ccells) {
            size_t n = ccells ? 2 * ccells : 64;
            cc_hlod_cell *v = realloc(h->cells, n * sizeof *v);
            if (!v) { fail(err, "out of memory"); goto done; }
            h->cells = v;
            uint32_t(*kv)[8] = realloc(kids, n * sizeof *kv);
            if (!kv) { fail(err, "out of memory"); goto done; }
            kids = kv;
            uint8_t *nk = realloc(nkids, n);
            if (!nk) { fail(err, "out of memory"); goto done; }
            nkids = nk;
            ccells = n;
        }
        uint32_t me = h->ncells++;
        if (c.parent != UINT32_MAX) kids[c.parent][nkids[c.parent]++] = me;
        nkids[me] = 0;
        cc_hlod_cell *cell = &h->cells[me];
        memset(cell, 0, sizeof *cell);
        cell->depth = c.depth, cell->el0 = c.start, cell->nel = c.count;
        float b[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
        for (uint32_t i = c.start; i < c.start + c.count; i++) {
            uint32_t e = h->elements[i];
            cell->original_triangles += w->tris[e];
            for (int j = 0; j < 3; j++) {
                if (box[e][j] < b[j]) b[j] = box[e][j];
                if (box[e][3 + j] > b[3 + j]) b[3 + j] = box[e][3 + j];
            }
        }
        memcpy(cell->bounds, b, sizeof b);
        int split = cell->original_triangles > o->leaf_triangles && c.depth < CC_HLOD_MAX_DEPTH && c.count > 1;
        if (!split) continue;
        double half = side / (double)(1ull << (c.depth + 1));
        uint32_t cnt[8] = {0}, at[8];
        for (uint32_t i = c.start; i < c.start + c.count; i++) {
            uint32_t e = h->elements[i];
            unsigned oc = 0;
            for (int k = 0; k < 3; k++) {
                double ctr = ((double)box[e][k] + (double)box[e][3 + k]) / 2.0 - w->origin[k];
                double mid = (double)(2 * c.corner[k] + 1) * half;
                oc |= (unsigned)(ctr >= mid) << k;
            }
            oct[i - c.start] = (uint8_t)oc;
            cnt[oc]++;
        }
        for (uint32_t k = 0, a = 0; k < 8; k++) at[k] = a, a += cnt[k];
        for (uint32_t i = 0; i < c.count; i++) tmp[at[oct[i]]++] = h->elements[c.start + i];
        memcpy(h->elements + c.start, tmp, c.count * sizeof *tmp);
        for (uint32_t k = 0, a = c.start; k < 8; k++) {
            if (cnt[k]) {
                Pending ch = {c.depth + 1, a, cnt[k], me, {0, 0, 0}};
                for (int q = 0; q < 3; q++) ch.corner[q] = 2 * c.corner[q] + ((k >> q) & 1);
                stack[sp++] = ch;
            }
            a += cnt[k];
        }
    }
    /* children lists, and the cells deepest first (hlod.rs:189) */
    w->order = malloc((size_t)h->ncells * sizeof *w->order);
    w->cl = calloc(h->ncells, sizeof *w->cl);
    if (!w->order || !w->cl) { fail(err, "out of memory"); goto done; }
    uint32_t deepest = 0, n = 0;
    for (uint32_t i = 0; i < h->ncells; i++) {
        cc_hlod_cell *c = &h->cells[i];
        if (c->depth > deepest) deepest = c->depth;
        c->nchildren = nkids[i];
        c->children = arena_dup(&h->arena, kids[i], 8 * sizeof(uint32_t));
        if (!c->children) { fail(err, "out of memory"); goto done; }
        if (!c->nchildren) h->total += c->original_triangles;
    }
    for (uint32_t d = deepest + 1; d-- > 0;)
        for (uint32_t i = 0; i < h->ncells; i++)
            if (h->cells[i].depth == d) w->order[n++] = i;
    rc = 0;
done:
    free(box), free(tmp), free(oct), free(stack), free(kids), free(nkids);
    return rc;
}

/* hlod.rs:195-219: cells deepest first; a leaf reads its elements' triangles in chunks of the cap
 * (fitting after each, so a huge element stays bounded too), an inner cell its children's clusters. */
int cc_hlod_step(cc_hlod *h, uint64_t budget, cc_error *err) {
    struct cc_hlod_work *w = h->work;
    if (!w) return 1;
    uint64_t max = w->o.max_proxy_triangles ? w->o.max_proxy_triangles : 1, used = 0;
    while (w->at < h->ncells) {
        uint32_t ci = w->order[w->at];
        cc_hlod_cell *c = &h->cells[ci];
        if (!w->open) {
            memset(&w->g, 0, sizeof w->g);
            w->g.e = CC_HLOD_MAX_DEPTH - c->depth;
            w->open = 1, w->j = 0, w->off = 0;
        }
        if (!c->nchildren) {
            for (; w->j < c->nel; w->j++, w->off = 0) {
                uint32_t el = h->elements[c->el0 + w->j];
                int mir;
                const cc_mesh *m = el_mesh(w->s, el, &mir);
                uint64_t nt = m->ni / 3;
                if (w->off >= nt) continue;
                if (m->nv > w->ckeys) {
                    uint32_t *st = realloc(w->stamp, (size_t)m->nv * sizeof *st);
                    if (st) w->stamp = st;
                    Key *ks = realloc(w->keys, (size_t)m->nv * sizeof *ks);
                    if (ks) w->keys = ks;
                    uint8_t *hs = realloc(w->has, m->nv);
                    if (hs) w->has = hs;
                    uint8_t *es = realloc(w->ke, m->nv);
                    if (es) w->ke = es;
                    if (!st || !ks || !hs || !es) return fail(err, "out of memory");
                    memset(w->stamp, 0, (size_t)m->nv * sizeof *st);
                    w->ckeys = m->nv;
                }
                if (!w->off && ++w->chunk == 0) { /* a new element; the serials wrapped: start over */
                    memset(w->stamp, 0, w->ckeys * sizeof *w->stamp);
                    w->chunk = 1;
                }
                const float(*p)[3] = placed(w, el, m);
                if (!p) return fail(err, "out of memory");
                while (w->off < nt) {
                    /* chunks of the cap (hlod.rs:272), smaller to end the step near its budget */
                    uint64_t rem = budget > used ? budget - used : 0, k = nt - w->off;
                    if (rem < 1024) rem = 1024;
                    if (k > max) k = max;
                    if (k > rem) k = rem;
                    if (add_triangles(w, w->s->elements[el].category, p, m->nv, m->idx, w->off, w->off + k, mir) ||
                        grid_fit(&w->g, max, 0))
                        return fail(err, "out of memory");
                    w->off += k, h->read += k, used += k;
                    if (used >= budget) { /* the next step goes on from here */
                        if (w->off >= nt) w->j++, w->off = 0;
                        return 0;
                    }
                }
            }
        }
        /* A child coarsened to fit its cap: the parent is no finer (hlod.rs:207). */
        for (uint32_t k = 0; k < c->nchildren; k++)
            if (w->cl[c->children[k]].built && w->cl[c->children[k]].e > w->g.e) w->g.e = w->cl[c->children[k]].e;
        for (uint32_t k = 0; k < c->nchildren; k++) {
            Clusters *ch = &w->cl[c->children[k]];
            if (!ch->built) continue;
            used += clusters_triangles(ch);
            if (grid_add_clusters(&w->g, ch) || grid_fit(&w->g, max, 0)) return fail(err, "out of memory");
        }
        Clusters cl;
        if (grid_fit(&w->g, max, 1) || grid_finish(&w->g, &cl)) {
            clusters_free(&cl);
            return fail(err, "out of memory");
        }
        w->open = 0;
        for (uint32_t k = 0; k < c->nchildren; k++) clusters_free(&w->cl[c->children[k]]);
        c->error_mm = (float)(lattice_size(w, cl.e) * cc_sqrt(3.0));
        if ((double)clusters_triangles(&cl) <= w->o.max_ratio * (double)c->original_triangles && proxy_of(h, w, &cl, c)) {
            clusters_free(&cl);
            return fail(err, "out of memory");
        }
        w->cl[ci] = cl;
        w->at++;
        if (used >= budget && w->at < h->ncells) return 0;
    }
    work_free(w, h->ncells);
    h->work = NULL;
    return 1;
}

int cc_hlod_build(cc_hlod *h, const cc_scene *s, const cc_hlod_opts *o, cc_error *err) {
    if (cc_hlod_start(h, s, o, err)) return -1;
    return cc_hlod_step(h, UINT64_MAX, err) < 0 ? -1 : 0;
}

uint64_t cc_hlod_proxy_triangles(const cc_hlod_cell *c) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < c->nproxy; i++) n += c->proxy[i].mesh.ni / 3;
    return n;
}

uint64_t cc_hlod_proxy_bytes(const cc_hlod_cell *c) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < c->nproxy; i++) {
        const cc_mesh *m = &c->proxy[i].mesh;
        n += 12ull * m->nv + 4ull * m->ni + 24ull * m->ne + 8ull * m->nn;
    }
    return n;
}

/* ---- BPRX (hlod.h) ---- */

static void put_u32(Buf *b, uint32_t v) { buf_append(b, &v, 4); }

int cc_hlod_bprx(const cc_hlod *h, uint32_t rev, uint32_t nelements, Buf *out, cc_error *err) {
    if (h->work) return fail(err, "proxies not built yet");
    uint64_t nk = 0, np = 0, nv = 0, ni = 0, ns = 0;
    for (uint32_t i = 0; i < h->ncells; i++) {
        const cc_hlod_cell *c = &h->cells[i];
        nk += c->nchildren, np += c->nproxy;
        for (uint32_t k = 0; k < c->nproxy; k++) nv += c->proxy[k].mesh.nv, ni += c->proxy[k].mesh.ni, ns += c->proxy[k].mesh.ne;
    }
    if (nv > UINT32_MAX || ni > UINT32_MAX || ns > UINT32_MAX) return fail(err, "proxies too large");
    if (buf_reserve(out, 48 + 64 * (size_t)h->ncells + 4 * (nk + h->nelements) + 28 * np + 12 * nv + 4 * ni + 32 * ns))
        return fail(err, "out of memory");
    buf_append(out, "BPRX", 4);
    put_u32(out, 1), put_u32(out, rev), put_u32(out, nelements), put_u32(out, h->ncells);
    put_u32(out, (uint32_t)nk), put_u32(out, h->nelements), put_u32(out, (uint32_t)np);
    put_u32(out, (uint32_t)nv), put_u32(out, (uint32_t)ni), put_u32(out, (uint32_t)ns), put_u32(out, 0);
    uint32_t k0 = 0, p0 = 0;
    for (uint32_t i = 0; i < h->ncells; i++) {
        const cc_hlod_cell *c = &h->cells[i];
        put_u32(out, c->depth);
        buf_append(out, &c->error_mm, 4);
        buf_append(out, c->bounds, 24);
        put_u32(out, k0), put_u32(out, c->nchildren), put_u32(out, c->el0), put_u32(out, c->nel);
        put_u32(out, p0), put_u32(out, c->nproxy);
        buf_append(out, &c->original_triangles, 8);
        k0 += c->nchildren, p0 += c->nproxy;
    }
    for (uint32_t i = 0; i < h->ncells; i++) buf_append(out, h->cells[i].children, 4 * (size_t)h->cells[i].nchildren);
    buf_append(out, h->elements, 4 * (size_t)h->nelements);
    uint32_t v0 = 0, i0 = 0, s0 = 0;
    for (uint32_t i = 0; i < h->ncells; i++)
        for (uint32_t k = 0; k < h->cells[i].nproxy; k++) {
            const cc_hlod_part *p = &h->cells[i].proxy[k];
            uint32_t cat = (uint32_t)p->cat;
            put_u32(out, cat);
            put_u32(out, v0), put_u32(out, p->mesh.nv), put_u32(out, i0), put_u32(out, p->mesh.ni);
            put_u32(out, s0), put_u32(out, p->mesh.ne);
            v0 += p->mesh.nv, i0 += p->mesh.ni, s0 += p->mesh.ne;
        }
    for (uint32_t i = 0; i < h->ncells; i++)
        for (uint32_t k = 0; k < h->cells[i].nproxy; k++) buf_append(out, h->cells[i].proxy[k].mesh.pos, 12 * (size_t)h->cells[i].proxy[k].mesh.nv);
    v0 = 0;
    for (uint32_t i = 0; i < h->ncells; i++)
        for (uint32_t k = 0; k < h->cells[i].nproxy; k++) {
            const cc_mesh *m = &h->cells[i].proxy[k].mesh;
            for (uint32_t q = 0; q < m->ni; q++) put_u32(out, v0 + m->idx[q]);
            v0 += m->nv;
        }
    for (uint32_t i = 0; i < h->ncells; i++)
        for (uint32_t k = 0; k < h->cells[i].nproxy; k++) buf_append(out, h->cells[i].proxy[k].mesh.edges, 24 * (size_t)h->cells[i].proxy[k].mesh.ne);
    static const int8_t zero[8];
    for (uint32_t i = 0; i < h->ncells; i++)
        for (uint32_t k = 0; k < h->cells[i].nproxy; k++) {
            const cc_mesh *m = &h->cells[i].proxy[k].mesh;
            for (uint32_t q = 0; q < m->ne; q++) buf_append(out, q < m->nn ? m->normals[q] : zero, 8);
        }
    if (out->oom) return fail(err, "out of memory");
    return 0;
}
