/* The native app's scenes (native.h): models through the C core into the renderer's CPU arrays.
 *
 * The open model comes as the page asks it (cc_mesh_get_form version 3: each shared shape once, a
 * placement per copy; src/scene_bmsh.h), a link through an nv_src (its rows and meshes, decoded here
 * from its cache's blocks one batch at a time), a link's proxies as its cache's BPRX (src/hlod.h). Every
 * input is untrusted: ranges checked, nothing read past what is there. */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "native.h"

double nv_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

void *nv_push(nv_vec *v, size_t sz, size_t count) {
    v->sz = sz;
    if (v->n + count > v->cap || !v->p) { /* never NULL, even for 0 more */
        size_t cap = v->cap ? v->cap : 1024;
        while (cap < v->n + count) cap *= 2;
        void *p = realloc(v->p, cap * sz);
        if (!p) return NULL;
        v->p = p, v->cap = cap;
    }
    void *r = (char *)v->p + v->n * sz;
    v->n += count;
    return r;
}

void nv_vec_free(nv_vec *v) {
    free(v->p);
    memset(v, 0, sizeof *v);
}

void nv_scene_init(nv_scene *sc, uint32_t elem0, const double origin0[3]) {
    memset(sc, 0, sizeof *sc);
    sc->elem0 = elem0;
    if (origin0) memcpy(sc->origin0, origin0, sizeof sc->origin0);
    for (int i = 0; i < 3; i++) sc->bmin[i] = 1e300, sc->bmax[i] = -1e300;
}

void nv_scene_free(nv_scene *sc) {
    nv_vec *all[] = {&sc->wv, &sc->wi, &sc->we, &sc->welems, &sc->sv, &sc->si, &sc->se, &sc->shapes, &sc->plc, &sc->colour,
                     &sc->ids, &sc->items, &sc->cells, &sc->roots, &sc->kids, &sc->pv, &sc->pi, &sc->pe, &sc->item_of};
    for (size_t k = 0; k < sizeof all / sizeof *all; k++) nv_vec_free(all[k]);
}

/* The palettes (glviewport.ts CATEGORY_SRGB, bimble-core style.rs:302; LINK_SRGB). */
static const uint8_t CATEGORY_SRGB[8][3] = {
    {214, 214, 210}, {150, 170, 196}, {226, 196, 150}, {206, 150, 128},
    {236, 220, 120}, {176, 160, 132}, {176, 214, 226}, {238, 238, 236},
};
static const uint8_t LINK_SRGB[12][3] = {
    {0xc0, 0x50, 0x4d}, {0x4f, 0x81, 0xbd}, {0x9b, 0xbb, 0x59}, {0x80, 0x64, 0xa2}, {0x4b, 0xac, 0xc6}, {0xf7, 0x96, 0x46},
    {0x2c, 0x4d, 0x75}, {0x7f, 0x7f, 0x7f}, {0xd9, 0x96, 0x94}, {0x95, 0xb3, 0xd7}, {0xc3, 0xd6, 0x9b}, {0xb2, 0xa2, 0xc7},
};
static uint32_t pack(const uint8_t c[3]) { return c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | 0xff000000u; }
uint32_t nv_category_colour(int cat) { return pack(CATEGORY_SRGB[cat >= 0 && cat < 8 ? cat : 7]); }
uint32_t nv_link_colour(int k) { return pack(LINK_SRGB[k >= 0 && k < 12 ? k : 7]); }

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

#define FAIL(...) do { snprintf(err, errn, __VA_ARGS__); goto done; } while (0)
typedef float nv_f3[3];
typedef char nv_id[22];
#define PUSH(v, T, n) ((T *)nv_push(&(v), sizeof(T), (n)))

static void place(const float *m, const float p[3], float out[3]) { /* m: x[3], y[3], z[3], t[3] */
    for (int i = 0; i < 3; i++) out[i] = m[i] * p[0] + m[3 + i] * p[1] + m[6 + i] * p[2] + m[9 + i];
}
/* The source's placement m (its own scene coordinates) composed with the part's: R(c, s) m + off. */
static void compose(const nv_load_opts *o, const float m[12], float out[12]) {
    for (int col = 0; col < 4; col++) {
        double x = m[3 * col], y = m[3 * col + 1], z = m[3 * col + 2];
        out[3 * col] = (float)(o->c * x - o->s * y), out[3 * col + 1] = (float)(o->s * x + o->c * y), out[3 * col + 2] = (float)z;
    }
    for (int i = 0; i < 3; i++) out[9 + i] = (float)(out[9 + i] + o->off[i]);
}
static void grow(float bb[6], const float p[3]) {
    for (int i = 0; i < 3; i++) {
        if (p[i] < bb[i]) bb[i] = p[i];
        if (p[i] > bb[3 + i]) bb[3 + i] = p[i];
    }
}
static void item_from_box(nv_item *it, const float bb[6]) {
    float r2 = 0;
    for (int i = 0; i < 3; i++) {
        it->c[i] = (bb[i] + bb[3 + i]) / 2;
        it->e[i] = (bb[3 + i] - bb[i]) / 2;
        r2 += it->e[i] * it->e[i];
    }
    it->c[3] = sqrtf(r2);
    it->e[3] = 0;
}
static void scene_grow(nv_scene *sc, const float bb[6]) {
    if (bb[0] > bb[3]) return;
    for (int i = 0; i < 3; i++) {
        if (bb[i] < sc->bmin[i]) sc->bmin[i] = bb[i];
        if (bb[3 + i] > sc->bmax[i]) sc->bmax[i] = bb[3 + i];
    }
}

/* ---- one load: the copies of each shape, where each shape went ---- */
typedef struct {
    nv_scene *sc;
    const nv_load_opts *o;
    uint32_t nshape;
    uint32_t *ncopies, *shape_of;
    float *shape_bb; /* own coordinates */
} Ld;

static void ld_free(Ld *L) { free(L->ncopies), free(L->shape_of), free(L->shape_bb); }
static int ld_init(Ld *L, nv_scene *sc, const nv_load_opts *o, uint32_t nshape) {
    memset(L, 0, sizeof *L);
    L->sc = sc, L->o = o, L->nshape = nshape;
    L->ncopies = calloc(nshape + 1, 4), L->shape_of = malloc((nshape + 1) * 4ull), L->shape_bb = malloc((nshape + 1) * 24ull);
    if (!L->ncopies || !L->shape_of || !L->shape_bb) return -1;
    for (uint32_t s = 0; s <= nshape; s++) {
        L->shape_of[s] = UINT32_MAX;
        for (int i = 0; i < 3; i++) L->shape_bb[6 * s + i] = 1e30f, L->shape_bb[6 * s + 3 + i] = -1e30f;
    }
    return 0;
}

/* Element k (local) of the load: its mesh `pos` (nv vertices), `idx` (ni, minus `ibase`), `seg` (ns
 * segments of 6 floats), as its own (shape < 0, in the source's scene coordinates) or as shape `shape`
 * (its own coordinates) placed by m. Returns 0, -1 out of memory, -2 damaged. */
static int add_elem(Ld *L, uint32_t k, const char id[22], int cat, int32_t shape, const float m_src[12], const float *pos, uint32_t nv,
                    const uint32_t *idx, uint32_t ni, uint32_t ibase, const float *seg, uint32_t ns) {
    nv_scene *sc = L->sc;
    static const float IDENT[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    float m[12];
    compose(L->o, shape >= 0 ? m_src : IDENT, m);
    uint32_t elem = sc->elem0 + k;
    ((uint32_t *)sc->colour.p)[k] = L->o->colour >= 0 ? nv_link_colour(L->o->colour) : nv_category_colour(cat);
    memcpy(((nv_id *)sc->ids.p)[k], id, 22);
    sc->tris_total += ni / 3;
    for (uint32_t i = 0; i < ni; i++)
        if (idx[i] < ibase || idx[i] - ibase >= nv) return -2;
    int instanced = shape >= 0 && (uint32_t)shape <= L->nshape && L->ncopies[shape] >= L->o->min_copies;
    float bb[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    if (!instanced) { /* into the merged buffers, placed */
        uint32_t vbase = (uint32_t)sc->wv.n;
        nv_vtx *wv = PUSH(sc->wv, nv_vtx, nv);
        uint32_t *wi = PUSH(sc->wi, uint32_t, ni);
        nv_vtx *we = PUSH(sc->we, nv_vtx, 2ull * ns);
        nv_welem *we1 = PUSH(sc->welems, nv_welem, 1);
        nv_item *it = PUSH(sc->items, nv_item, 1);
        if (!wv || !wi || !we || !we1 || !it) return -1;
        for (uint32_t v = 0; v < nv; v++) place(m, pos + 3ull * v, wv[v].p), wv[v].elem = elem, grow(bb, wv[v].p);
        for (uint32_t i = 0; i < ni; i++) wi[i] = idx[i] - ibase + vbase;
        for (uint32_t q = 0; q < ns; q++) {
            place(m, seg + 6ull * q, we[2 * q].p), place(m, seg + 6ull * q + 3, we[2 * q + 1].p);
            we[2 * q].elem = we[2 * q + 1].elem = elem;
        }
        we1->first_index = (uint32_t)(sc->wi.n - ni), we1->nidx = ni;
        we1->first_edge = (uint32_t)(sc->we.n - 2ull * ns), we1->nedge = 2 * ns;
        item_from_box(it, bb);
        it->k[0] = (uint32_t)(sc->welems.n - 1), it->k[1] = UINT32_MAX, it->k[2] = 0, it->k[3] = ni / 3;
        sc->tris_unique += ni / 3;
    } else {
        if (L->shape_of[shape] == UINT32_MAX) { /* the shape's first copy writes it into the shape buffers */
            uint32_t vbase = (uint32_t)sc->sv.n;
            nv_f3 *sv = PUSH(sc->sv, nv_f3, nv);
            uint32_t *si = PUSH(sc->si, uint32_t, ni);
            nv_f3 *se = PUSH(sc->se, nv_f3, 2ull * ns);
            nv_shape *sh = PUSH(sc->shapes, nv_shape, 1);
            if (!sv || !si || !se || !sh) return -1;
            if (nv) memcpy(sv, pos, 12ull * nv);
            for (uint32_t i = 0; i < ni; i++) si[i] = idx[i] - ibase + vbase;
            if (ns) memcpy(se, seg, 24ull * ns);
            sh->first_index = (uint32_t)(sc->si.n - ni), sh->nidx = ni;
            sh->first_edge = (uint32_t)(sc->se.n - 2ull * ns), sh->nedge = 2 * ns;
            sh->first_inst = 0, sh->ninst = 0; /* nv_finish */
            L->shape_of[shape] = (uint32_t)(sc->shapes.n - 1);
            sc->tris_unique += ni / 3;
        }
        const float *lb = &L->shape_bb[6 * shape]; /* the copy's box: the shape's box placed */
        float lc[3], le[3], c[3];
        for (int i = 0; i < 3; i++) lc[i] = (lb[i] + lb[3 + i]) / 2, le[i] = (lb[3 + i] - lb[i]) / 2;
        if (lb[0] > lb[3]) for (int i = 0; i < 3; i++) lc[i] = 0, le[i] = 0;
        place(m, lc, c);
        for (int i = 0; i < 3; i++) {
            float ex = fabsf(m[i]) * le[0] + fabsf(m[3 + i]) * le[1] + fabsf(m[6 + i]) * le[2];
            bb[i] = c[i] - ex, bb[3 + i] = c[i] + ex;
        }
        nv_plc *pc = PUSH(sc->plc, nv_plc, 1);
        nv_item *it = PUSH(sc->items, nv_item, 1);
        if (!pc || !it) return -1;
        for (int i = 0; i < 3; i++) pc->x[i] = m[i], pc->y[i] = m[3 + i], pc->z[i] = m[6 + i], pc->t[i] = m[9 + i];
        pc->x[3] = pc->y[3] = pc->z[3] = 0;
        memcpy(&pc->t[3], &elem, 4);
        item_from_box(it, bb);
        it->k[0] = L->shape_of[shape], it->k[1] = (uint32_t)(sc->plc.n - 1), it->k[2] = 0, it->k[3] = ni / 3;
    }
    ((uint32_t *)sc->item_of.p)[k] = (uint32_t)(sc->items.n - 1);
    scene_grow(sc, bb);
    return 0;
}

static int rows_alloc(nv_scene *sc, uint32_t nE) {
    sc->nelements = nE;
    uint32_t *col = PUSH(sc->colour, uint32_t, nE);
    nv_id *ids = PUSH(sc->ids, nv_id, nE);
    uint32_t *io = PUSH(sc->item_of, uint32_t, nE);
    if (!col || !ids || !io) return -1;
    memset(ids, '0', 22ull * nE);
    for (uint32_t k = 0; k < nE; k++) io[k] = UINT32_MAX, col[k] = nv_category_colour(7);
    return 0;
}

/* ---- the open model: BMSH version 3 ---- */
int nv_load_bytes(nv_scene *sc, const char *p8, size_t n, const nv_load_opts *o, char *err, size_t errn) {
    int rc = -1;
    Ld L;
    memset(&L, 0, sizeof L);
    const uint8_t *b = (const uint8_t *)p8;
    if (n < 52 || memcmp(b, "BMSH", 4) || rd32(b + 4) != 3) { snprintf(err, errn, "not BMSH version 3"); return -1; }
    uint32_t nE = rd32(b + 12), nV = rd32(b + 16), nI = rd32(b + 20), nS = rd32(b + 24);
    size_t o_rows = 52, o_pos = o_rows + 48ull * nE, o_idx = o_pos + 12ull * nV, o_seg = o_idx + 4ull * nI,
           o_nrm = o_seg + 24ull * nS, o_plc = o_nrm + 8ull * nS;
    if (o_plc + 52ull * nE > n || nE > n) { snprintf(err, errn, "BMSH truncated"); return -1; }
    const float *pos = (const float *)(b + o_pos);
    const uint32_t *idx = (const uint32_t *)(b + o_idx);
    const float *seg = (const float *)(b + o_seg);
    uint32_t nshape = 0;
    for (uint32_t k = 0; k < nE; k++) {
        uint32_t s = rd32(b + o_plc + 52ull * k);
        if (s > nshape) nshape = s;
    }
    if (nshape > nE) FAIL("BMSH: bad shape index");
    if (ld_init(&L, sc, o, nshape) || rows_alloc(sc, nE)) FAIL("out of memory");
    for (uint32_t k = 0; k < nE; k++) { /* BMSH 3: shape s >= 1 is shared, its first copy's row holds its mesh */
        uint32_t s = rd32(b + o_plc + 52ull * k);
        if (!s) continue;
        if (!L.ncopies[s]) {
            const uint8_t *row = b + o_rows + 48ull * k;
            uint32_t fv = rd32(row + 24), cnt = rd32(row + 28);
            if ((uint64_t)fv + cnt > nV) FAIL("BMSH: vertex range");
            for (uint32_t v = fv; v < fv + cnt; v++) grow(&L.shape_bb[6 * s], pos + 3ull * v);
        }
        L.ncopies[s]++;
    }
    for (uint32_t k = 0; k < nE; k++) {
        const uint8_t *row = b + o_rows + 48ull * k;
        uint32_t fv = rd32(row + 24), cv = rd32(row + 28), fi = rd32(row + 32), ci = rd32(row + 36), fs = rd32(row + 40),
                 cs = rd32(row + 44);
        if ((uint64_t)fv + cv > nV || (uint64_t)fi + ci > nI || (uint64_t)fs + cs > nS) FAIL("BMSH: element ranges");
        const uint8_t *pl = b + o_plc + 52ull * k;
        uint32_t s = rd32(pl);
        float m[12];
        memcpy(m, pl + 4, 48);
        int r = add_elem(&L, k, (const char *)row, row[22] < 8 ? row[22] : 7, s ? (int32_t)s : -1, m, pos + 3ull * fv, cv, idx + fi, ci,
                         fv, seg + 6ull * fs, cs);
        if (r == -1) FAIL("out of memory");
        if (r == -2) FAIL("BMSH: index out of its element");
    }
    rc = 0;
done:
    ld_free(&L);
    return rc;
}

/* ---- a source (a link) ---- */
#define BATCH 512 /* elements per lock: the session waits at most one batch (a few ms) */
int nv_load_src(nv_scene *sc, const nv_src *src, const nv_load_opts *o, char *err, size_t errn) {
    int rc = -1;
    Ld L;
    memset(&L, 0, sizeof L);
    size_t nE = src->nelements, nS = src->nshapes;
    if (nE > 0xffffffu * 16 || nS > nE + 1) { snprintf(err, errn, "too many elements"); return -1; }
    int locked = 0;
    if (ld_init(&L, sc, o, (uint32_t)nS) || rows_alloc(sc, (uint32_t)nE)) FAIL("out of memory");
    /* the copies per shape (rows only) */
    src->lock(src->ctx), locked = 1;
    for (size_t k = 0; k < nE; k++) {
        char id[22];
        int cat;
        int32_t s;
        float m[12];
        src->row(src->ctx, k, id, &cat, &s, m);
        if (s >= 0 && (size_t)s < nS) L.ncopies[s]++;
    }
    for (size_t s = 0; s < nS; s++)
        if (L.ncopies[s] && src->shape_box(src->ctx, s, &L.shape_bb[6 * s]) != 0)
            for (int i = 0; i < 3; i++) L.shape_bb[6 * s + i] = 1e30f, L.shape_bb[6 * s + 3 + i] = -1e30f;
    src->unlock(src->ctx), locked = 0;
    for (size_t k0 = 0; k0 < nE; k0 += BATCH) {
        src->lock(src->ctx), locked = 1;
        if (src->cancelled(src->ctx)) FAIL("cancelled");
        for (size_t k = k0; k < nE && k < k0 + BATCH; k++) {
            char id[22];
            int cat;
            int32_t s;
            float m[12];
            src->row(src->ctx, k, id, &cat, &s, m);
            if (s >= (int32_t)nS) s = -1;
            cc_mesh me;
            memset(&me, 0, sizeof me);
            int got = s >= 0 ? src->shape_mesh(src->ctx, (size_t)s, &me) : src->own_mesh(src->ctx, k, &me);
            if (got != 0) memset(&me, 0, sizeof me); /* drawn as nothing; its row stays (an id to pick, a property) */
            int r = add_elem(&L, (uint32_t)k, id, cat, s, m, me.pos ? &me.pos[0][0] : NULL, me.pos ? me.nv : 0, me.idx, me.idx ? me.ni : 0, 0,
                             me.edges ? &me.edges[0][0] : NULL, me.edges ? me.ne : 0);
            if (r == -1) FAIL("out of memory");
            if (r == -2) FAIL("damaged mesh (an index out of its element)");
        }
        src->unlock(src->ctx), locked = 0;
    }
    rc = 0;
done:
    if (locked) src->unlock(src->ctx);
    ld_free(&L);
    return rc;
}

/* ---- proxies: BPRX (src/hlod.h) ---- */
int nv_load_bprx(nv_scene *sc, const char *p8, size_t n, uint32_t nE, const nv_load_opts *o, char *err, size_t errn) {
    const uint8_t *q = (const uint8_t *)p8;
    if (n < 48 || memcmp(q, "BPRX", 4)) { snprintf(err, errn, "not BPRX"); return -1; }
    uint32_t nC = rd32(q + 16), nK = rd32(q + 20), nL = rd32(q + 24), nP = rd32(q + 28), pV = rd32(q + 32), pI = rd32(q + 36), pS = rd32(q + 40);
    size_t c_cells = 48, c_kids = c_cells + 64ull * nC, c_list = c_kids + 4ull * nK, c_parts = c_list + 4ull * nL, c_pos = c_parts + 28ull * nP,
           c_idx = c_pos + 12ull * pV, c_seg = c_idx + 4ull * pI;
    if (c_seg + 24ull * pS > n || nC > n || nK > n) { snprintf(err, errn, "BPRX truncated"); return -1; }
    uint32_t cbase = (uint32_t)sc->cells.n, kbase = (uint32_t)sc->kids.n;
    nv_cell *cells = PUSH(sc->cells, nv_cell, nC);
    uint32_t *kids = PUSH(sc->kids, uint32_t, nK);
    if ((nC && !cells) || (nK && !kids)) { snprintf(err, errn, "out of memory"); return -1; }
    for (uint32_t k = 0; k < nK; k++) {
        uint32_t c = rd32(q + c_kids + 4ull * k);
        if (c >= nC) { snprintf(err, errn, "BPRX: child"); return -1; }
        kids[k] = cbase + c;
    }
    if (nC) {
        uint32_t *r = PUSH(sc->roots, uint32_t, 1);
        if (!r) { snprintf(err, errn, "out of memory"); return -1; }
        *r = cbase;
    }
    const float *ppos = (const float *)(q + c_pos);
    const uint32_t *pidx = (const uint32_t *)(q + c_idx);
    const float *pseg = (const float *)(q + c_seg);
    static const float IDENT[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    float m[12];
    compose(o, IDENT, m);
    for (uint32_t c = 0; c < nC; c++) {
        const uint8_t *r = q + c_cells + 64ull * c;
        nv_cell *cl = &((nv_cell *)sc->cells.p)[cbase + c];
        memset(cl, 0, sizeof *cl);
        cl->error_mm = rdf(r + 4);
        { /* the cell's box placed: its 8 corners */
            float lo[3], hi[3], bb[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
            for (int i = 0; i < 3; i++) lo[i] = rdf(r + 8 + 4 * i), hi[i] = rdf(r + 20 + 4 * i);
            for (int k = 0; k < 8; k++) {
                float p[3] = {k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2]}, w[3];
                place(m, p, w);
                grow(bb, w);
            }
            memcpy(cl->bounds, bb, sizeof bb);
        }
        uint32_t fk = rd32(r + 32), nk = rd32(r + 36), el0 = rd32(r + 40), nel = rd32(r + 44), fp = rd32(r + 48), np = rd32(r + 52);
        if ((uint64_t)fk + nk > nK || (uint64_t)el0 + nel > nL || (uint64_t)fp + np > nP) { snprintf(err, errn, "BPRX: cell ranges"); return -1; }
        cl->first_child = kbase + fk, cl->nchildren = nk, cl->leaf = nk == 0;
        if (cl->leaf && nE && sc->item_of.n == nE)
            for (uint32_t j = el0; j < el0 + nel; j++) {
                uint32_t el = rd32(q + c_list + 4ull * j);
                if (el >= nE) { snprintf(err, errn, "BPRX: element"); return -1; }
                uint32_t it = ((uint32_t *)sc->item_of.p)[el];
                if (it != UINT32_MAX) ((nv_item *)sc->items.p)[it].k[2] = cbase + c;
            }
        cl->pfirst_index = (uint32_t)sc->pi.n, cl->pfirst_edge = (uint32_t)sc->pe.n;
        for (uint32_t pp = fp; pp < fp + np; pp++) {
            const uint8_t *pr = q + c_parts + 28ull * pp;
            uint32_t cat = pr[0] < 8 ? pr[0] : 7;
            uint32_t fv = rd32(pr + 4), cv = rd32(pr + 8), fi = rd32(pr + 12), ci = rd32(pr + 16), fs = rd32(pr + 20), cs = rd32(pr + 24);
            if ((uint64_t)fv + cv > pV || (uint64_t)fi + ci > pI || (uint64_t)fs + cs > pS) { snprintf(err, errn, "BPRX: part ranges"); return -1; }
            uint32_t vbase = (uint32_t)sc->pv.n;
            nv_vtx *pv = PUSH(sc->pv, nv_vtx, cv);
            uint32_t *pi = PUSH(sc->pi, uint32_t, ci);
            nv_vtx *pe = PUSH(sc->pe, nv_vtx, 2ull * cs);
            if (!pv || !pi || !pe) { snprintf(err, errn, "out of memory"); return -1; }
            uint32_t tint = cat; /* the palette slots 0..7 of the colour table (a coloured link's proxies keep them) */
            for (uint32_t v = 0; v < cv; v++) place(m, ppos + 3ull * (fv + v), pv[v].p), pv[v].elem = tint;
            for (uint32_t i = 0; i < ci; i++) {
                uint32_t a = pidx[fi + i];
                if (a < fv || a >= fv + cv) { snprintf(err, errn, "BPRX: index"); return -1; }
                pi[i] = a - fv + vbase;
            }
            for (uint32_t s2 = 0; s2 < cs; s2++) {
                place(m, pseg + 6ull * (fs + s2), pe[2 * s2].p), place(m, pseg + 6ull * (fs + s2) + 3, pe[2 * s2 + 1].p);
                pe[2 * s2].elem = pe[2 * s2 + 1].elem = tint;
            }
        }
        cl->pnidx = (uint32_t)sc->pi.n - cl->pfirst_index, cl->pnedge = (uint32_t)sc->pe.n - cl->pfirst_edge;
        cl->has_proxy = cl->pnidx > 0;
        if (cl->has_proxy) {
            float bb[6];
            memcpy(bb, cl->bounds, sizeof bb);
            scene_grow(sc, bb);
        }
    }
    return 0;
}

/* After the last load: the copies grouped by shape (each shape's instances a range, as the indirect
 * draws want them), the items renumbered, and each copy's command (merged elements first, then one per
 * shape). 0 ok, -1 out of memory. */
int nv_finish(nv_scene *sc) {
    size_t ns = sc->shapes.n, np = sc->plc.n;
    nv_shape *sh = sc->shapes.p;
    nv_item *it = sc->items.p;
    uint32_t *at = calloc(ns + 1, 4);
    nv_plc *out = malloc((np + 1) * sizeof *out);
    if (!at || !out) { free(at), free(out); return -1; }
    for (size_t k = 0; k < sc->items.n; k++)
        if (it[k].k[1] != UINT32_MAX) sh[it[k].k[0]].ninst++;
    uint32_t acc = 0;
    for (size_t s = 0; s < ns; s++) sh[s].first_inst = acc, at[s] = acc, acc += sh[s].ninst;
    const nv_plc *in = sc->plc.p;
    uint32_t nw = (uint32_t)sc->welems.n;
    for (size_t k = 0; k < sc->items.n; k++) {
        if (it[k].k[1] == UINT32_MAX) continue;
        uint32_t s = it[k].k[0], dst = at[s]++;
        out[dst] = in[it[k].k[1]];
        it[k].k[1] = dst;
        it[k].k[0] = nw + s;
    }
    free(sc->plc.p);
    sc->plc.p = out, sc->plc.cap = np + 1;
    free(at);
    nv_vec_free(&sc->item_of);
    return 0;
}
