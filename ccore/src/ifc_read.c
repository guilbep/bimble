/* The IFC display reader: read.rs read_str in C (see ifc_read.h). Ports, in Rust's order:
 *   read.rs   Parsed::new (units, containers, voids, materials, products, storey levels, grid
 *             axes, georef), Parsed::shapes, Parsed::piece, Parsed::scene_progress (batches,
 *             the cap, the warning of what is not shown)
 *   scene.rs  SceneBuilder::push / push_instance / finish (origin, build, share_copies)
 * The geometry (Reader, Emit, Budget) is ifc_geom.h, shared with the import. */
#include "ifc_read.h"
#include "georef.h"
#include "ifc_geom.h"
#include "fmt.h"
#include "json_w.h"
#include "scene_build.h"
#include "scene_bmsh.h"

#define ELEMENT_MAX_TRIANGLES ((size_t)50000000) /* read.rs */
#define BATCH 1024

/* Built with -DCC_IFC_PROFILE (and -D_POSIX_C_SOURCE=200809L): time and memory per phase on
 * stderr (ccore/results/ifc_read.md). */
#ifdef CC_IFC_PROFILE
#include <stdio.h>
#include <time.h>
static double prof_t0;
static double prof_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static void prof(const char *what) {
    FILE *f = fopen("/proc/self/status", "r");
    char l[256];
    long hwm = -1, rss = -1;
    while (f && fgets(l, sizeof l, f)) {
        if (!strncmp(l, "VmHWM:", 6)) hwm = strtol(l + 6, NULL, 10);
        if (!strncmp(l, "VmRSS:", 6)) rss = strtol(l + 6, NULL, 10);
    }
    if (f) fclose(f);
    double t = prof_now();
    fprintf(stderr, "PROF %-22s %8.1f ms  peak %5ld MB  rss %5ld MB\n", what, t - prof_t0, hwm / 1024, rss / 1024);
    prof_t0 = t;
}
#define PROF(what) prof(what)
#else
#define PROF(what) ((void)0)
#endif

/* ---- small helpers */

static size_t sat_add(size_t a, size_t b) { return a > (size_t)-1 - b ? (size_t)-1 : a + b; }
static size_t min_z(size_t a, size_t b) { return a < b ? a : b; }

static cc_str a_str(Arena *a, const char *s, size_t n, int *oom) {
    cc_str r = {"", 0};
    if (!n) return r;
    char *d = arena_alloc(a, n + 1, 1);
    if (!d) { *oom = 1; return r; }
    memcpy(d, s, n);
    r.p = d, r.n = n;
    return r;
}

static void r_init(R *r, const cc_step_file *f, double arc_tol, double angle_scale) {
    memset(r, 0, sizeof *r);
    r->f = f;
    r->pl_no_warned = 1;
    r->arc_tol = arc_tol;
    r->angle_scale = angle_scale;
    buf_init(&r->tmp);
}
static int r_ok(const R *r) { return r->f != NULL; }
static void r_free(R *r) {
    free(r->pl);
    tally_free(&r->warnings);
    tally_free(&r->unsup);
    buf_free(&r->tmp);
}

/* Adds every warning of `from` (with its count) to `to`, and empties `from`. */
static void tally_merge(Tallies *to, Tallies *from) {
    for (size_t i = 0; i < from->n; i++) {
        size_t before = to->n, j;
        for (j = 0; j < to->n; j++)
            if (to->v[j].n == from->v[i].n && !memcmp(to->v[j].s, from->v[i].s, from->v[i].n)) break;
        if (j == to->n) {
            tally_add(to, from->v[i].s, from->v[i].n, 1);
            if (to->n == before) continue; /* out of memory or past MAX_TALLY: dropped */
            to->v[j].count = from->v[i].count;
        } else to->v[j].count += from->v[i].count;
    }
    tally_free(from);
}

/* f64::total_cmp(a, b) < 0 */
static int total_less(double a, double b) {
    int64_t x, y;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    x ^= (int64_t)((uint64_t)(x >> 63) >> 1);
    y ^= (int64_t)((uint64_t)(y >> 63) >> 1);
    return x < y;
}

static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !memcmp(s + a - b, suf, b);
}

/* ---- Parsed::new parts */

/* containers(): entity index -> container name, for the entities a relation names. */
typedef struct {
    int has_direct, has_parent;
    uint64_t parent; /* aggregation parent id */
    const char *name;
    size_t nn;
    int has_name;
} Cont;
typedef struct {
    IxMap at;
    Cont *v;
    size_t n, cap;
    int oom;
} Conts;

static const Cont *cont_get(const Conts *c, size_t ix) {
    const size_t *k = ixmap_get(&c->at, ix);
    return k ? &c->v[*k] : NULL;
}
static Cont *cont_make(Conts *c, size_t ix) { /* NULL (and oom): out of memory */
    const size_t *k = ixmap_get(&c->at, ix);
    if (k) return &c->v[*k];
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 64;
        Cont *v = realloc(c->v, cap * sizeof *v);
        if (!v) { c->oom = 1; return NULL; }
        c->v = v, c->cap = cap;
    }
    if (ixmap_put(&c->at, ix, c->n)) { c->oom = 1; return NULL; }
    memset(&c->v[c->n], 0, sizeof *c->v);
    return &c->v[c->n++];
}
static void conts_free(Conts *c) {
    free(c->v);
    ixmap_free(&c->at);
}

static void containers(R *r, Conts *c) {
    const cc_step_file *f = r->f;
    for (size_t i = 0; i < f->n; i++) {
        const cc_step_entity *rel = &f->ents[i];
        if (!is(rel, "IFCRELCONTAINEDINSPATIALSTRUCTURE")) continue;
        const char *nm = "";
        size_t nn = 0;
        const cc_step_entity *s = get(r, arg(rel, 5));
        if (s && !cc_sv_as_str(arg(s, 2), &nm, &nn)) nm = "", nn = 0;
        const cc_sv *l;
        size_t n;
        if (!cc_sv_as_list(arg(rel, 4), &l, &n)) n = 0;
        for (size_t k = 0; k < n; k++) {
            const cc_step_entity *e = get(r, &l[k]);
            if (!e) continue; /* only entities of the file are elements */
            Cont *x = cont_make(c, cc_step_index(f, e));
            if (!x) return;
            x->has_direct = 1, x->name = nm, x->nn = nn;
        }
    }
    for (size_t i = 0; i < f->n; i++) {
        const cc_step_entity *rel = &f->ents[i];
        uint64_t p;
        const cc_sv *l;
        size_t n;
        if (!is(rel, "IFCRELAGGREGATES") || !cc_sv_as_ref(arg(rel, 4), &p) || !cc_sv_as_list(arg(rel, 5), &l, &n))
            continue;
        for (size_t k = 0; k < n; k++) {
            const cc_step_entity *e = get(r, &l[k]);
            if (!e) continue;
            Cont *x = cont_make(c, cc_step_index(f, e));
            if (!x) return;
            x->has_parent = 1, x->parent = p;
        }
    }
    /* out = direct, then each child takes the first direct container up its parents (8 steps). */
    for (size_t i = 0; i < c->n; i++) {
        Cont *x = &c->v[i];
        if (x->has_direct) { x->has_name = 1; continue; }
        if (!x->has_parent) continue;
        const Cont *cur = x;
        for (int s = 0; s < 8; s++) {
            if (cur->has_direct) {
                x->has_name = 1, x->name = cur->name, x->nn = cur->nn;
                break;
            }
            if (!cur->has_parent) break;
            const cc_step_entity *pe = cc_step_get(r->f, cur->parent);
            if (!pe || !(cur = cont_get(c, cc_step_index(f, pe)))) break;
        }
    }
}

/* storey_levels() */
static int storey_levels(R *r, double scale, Arena *a, cc_ifc_storey **out, size_t *nout) {
    const cc_step_file *f = r->f;
    size_t n = 0, cap = 0;
    cc_ifc_storey *v = NULL;
    int oom = 0;
    for (size_t i = 0; i < f->n; i++) {
        const cc_step_entity *st = &f->ents[i];
        if (!is(st, "IFCBUILDINGSTOREY")) continue;
        const char *nm = "", *id = "";
        size_t nn = 0, ni = 0;
        if (!cc_sv_as_str(arg(st, 2), &nm, &nn)) nm = "", nn = 0;
        uint64_t pl;
        double z;
        int has;
        if (cc_sv_as_ref(arg(st, 5), &pl)) z = object_placement(r, pl, 0).t[2], has = 1;
        else has = cc_sv_as_f64(arg(st, 9), &z);
        if (!has || !finite(z)) continue;
        if (!cc_sv_as_str(arg(st, 0), &id, &ni)) id = "", ni = 0;
        if (n == cap && grow((void **)&v, &cap, n + 1, sizeof *v)) { free(v); return -1; }
        v[n].name = a_str(a, nm, nn, &oom);
        v[n].elevation_mm = z * scale;
        v[n].id = a_str(a, id, ni, &oom);
        n++;
    }
    /* SceneBuilder::finish: sort_by total_cmp of the elevation (stable). */
    for (size_t i = 1; i < n; i++) {
        cc_ifc_storey x = v[i];
        size_t j = i;
        while (j && total_less(x.elevation_mm, v[j - 1].elevation_mm)) v[j] = v[j - 1], j--;
        v[j] = x;
    }
    *out = n ? ii_dup8(a, v, n * sizeof *v) : NULL;
    *nout = n;
    free(v);
    return oom || (n && !*out) ? -1 : 0;
}

/* grid_axes() */
static int grid_axes(R *r, double scale, Arena *a, cc_ifc_grid **out, size_t *nout) {
    const size_t MAX_AXES = 10000;
    const cc_step_file *f = r->f;
    size_t n = 0, cap = 0;
    cc_ifc_grid *v = NULL;
    int oom = 0;
    for (size_t i = 0; i < f->n && n < MAX_AXES; i++) {
        const cc_step_entity *g = &f->ents[i];
        if (!is(g, "IFCGRID")) continue;
        uint64_t pl;
        Aff place = cc_sv_as_ref(arg(g, 5), &pl) ? object_placement(r, pl, 0) : IDENT;
        for (int li = 7; li <= 9; li++) {
            const cc_sv *l;
            size_t nl;
            if (!cc_sv_as_list(arg(g, (size_t)li), &l, &nl)) nl = 0;
            for (size_t k = 0; k < nl; k++) {
                if (n >= MAX_AXES) goto done;
                const cc_step_entity *ax = get(r, &l[k]);
                if (!ax) continue;
                const char *tag = "";
                size_t tn = 0;
                if (!cc_sv_as_str(arg(ax, 0), &tag, &tn)) tag = "", tn = 0;
                const cc_step_entity *curve = get(r, arg(ax, 1));
                if (!curve) continue;
                double pa[3], pb[3];
                int has_a = 0, has_b = 0;
                if (is(curve, "IFCPOLYLINE")) {
                    const cc_sv *ps;
                    size_t np;
                    if (!cc_sv_as_list(arg(curve, 0), &ps, &np)) np = 0;
                    for (size_t q = 0; q < np; q++) {
                        double p[3];
                        if (!point(r, &ps[q], p)) continue;
                        if (!has_a) memcpy(pa, p, sizeof p), has_a = 1;
                        memcpy(pb, p, sizeof p), has_b = 1;
                    }
                } else if (is(curve, "IFCINDEXEDPOLYCURVE")) {
                    const cc_step_entity *pl2 = get(r, arg(curve, 0));
                    const cc_sv *cs;
                    size_t nc;
                    if (pl2 && cc_sv_as_list(arg(pl2, 0), &cs, &nc) && nc) {
                        const cc_sv *ends[2] = {&cs[0], &cs[nc - 1]};
                        double got[2][3];
                        int ok[2] = {0, 0}, m = 0;
                        for (int e = 0; e < 2; e++) {
                            const cc_sv *c;
                            size_t cn;
                            if (!cc_sv_as_list(ends[e], &c, &cn) || cn < 2) continue;
                            if (!cc_sv_as_f64(&c[0], &got[e][0]) || !cc_sv_as_f64(&c[1], &got[e][1])) continue;
                            if (cn < 3 || !cc_sv_as_f64(&c[2], &got[e][2])) got[e][2] = 0.0;
                            ok[e] = 1;
                        }
                        /* [first, last].filter_map(at): the points that parse, in order. */
                        for (int e = 0; e < 2; e++)
                            if (ok[e]) {
                                if (!m) memcpy(pa, got[e], sizeof pa);
                                memcpy(pb, got[e], sizeof pb);
                                m++;
                            }
                        has_a = has_b = m > 0;
                    }
                }
                if (!has_a || !has_b) continue;
                double wa[3], wb[3];
                aff_apply(&place, pa, wa);
                aff_apply(&place, pb, wb);
                double A[2] = {wa[0] * scale, wa[1] * scale}, B[2] = {wb[0] * scale, wb[1] * scale};
                int dup = 0;
                for (size_t q = 0; q < n && !dup; q++)
                    dup = v[q].tag.n == tn && (!tn || !memcmp(v[q].tag.p, tag, tn)) && v[q].start_mm[0] == A[0] &&
                          v[q].start_mm[1] == A[1] && v[q].end_mm[0] == B[0] && v[q].end_mm[1] == B[1];
                int same = A[0] == B[0] && A[1] == B[1];
                if (dup || same || !finite(A[0]) || !finite(A[1]) || !finite(B[0]) || !finite(B[1])) continue;
                if (n == cap && grow((void **)&v, &cap, n + 1, sizeof *v)) { free(v); return -1; }
                v[n].tag = a_str(a, tag, tn, &oom);
                memcpy(v[n].start_mm, A, sizeof A);
                memcpy(v[n].end_mm, B, sizeof B);
                n++;
            }
        }
    }
done:
    *out = n ? ii_dup8(a, v, n * sizeof *v) : NULL;
    *nout = n;
    free(v);
    return oom || (n && !*out) ? -1 : 0;
}

static double to_degrees(double x) { return x * 57.2957795130823208767981548141051703; }

/* georef() with Georef::from_frame */
static void georef(R *r, double scale, Arena *a, cc_ifc_georef *g, double base_mm[3], int *oom) {
    memset(g, 0, sizeof *g);
    const cc_step_entity *b = first_of(r->f, "IFCBUILDING");
    uint64_t pl;
    Aff bf = b && cc_sv_as_ref(arg(b, 5), &pl) ? object_placement(r, pl, 0) : IDENT;
    double rot = to_degrees(cc_atan2(bf.x[1], bf.x[0]));
    g->rotation_deg = round_rs(rot * 1e6) / 1e6 + 0.0;
    for (int k = 0; k < 3; k++) base_mm[k] = bf.t[k] * scale + 0.0; /* LINKF: the building's origin, world mm */
    double e = bf.t[0] * scale / 1000.0, n = bf.t[1] * scale / 1000.0;
    if (e >= 2480000.0 && e <= 2840000.0 && n >= 1070000.0 && n <= 1300000.0) {
        g->has_crs = 1;
        g->crs = a_str(a, "EPSG:2056", 9, oom);
    }
    const cc_step_entity *m = first_of(r->f, "IFCMAPCONVERSION");
    if (!m) return;
    double v;
    g->east_m = cc_sv_as_f64(arg(m, 2), &v) ? v : 0.0;
    g->north_m = cc_sv_as_f64(arg(m, 3), &v) ? v : 0.0;
    g->height_m = cc_sv_as_f64(arg(m, 4), &v) ? v : 0.0;
    double ax = cc_sv_as_f64(arg(m, 5), &v) ? v : 1.0, o = cc_sv_as_f64(arg(m, 6), &v) ? v : 0.0;
    g->map_rotation_deg = round_rs(to_degrees(cc_atan2(o, ax)) * 1e6) / 1e6 + 0.0;
    const cc_step_entity *crs = get(r, arg(m, 1));
    const char *s;
    size_t sn;
    g->has_crs = 0;
    g->crs.p = "", g->crs.n = 0;
    if (crs && cc_sv_as_str(arg(crs, 0), &s, &sn)) g->has_crs = 1, g->crs = a_str(a, s, sn, oom);
}

/* ---- meshing */

typedef struct {
    uint64_t id;  /* map entity id */
    cc_mesh mesh; /* in its own coordinates (built at origin 0) */
    int seen;     /* counted once in the stored triangles */
} Shape;

typedef struct {
    size_t ent;         /* entity index of the product */
    int instance;       /* 1: shape `shape` placed by `tf` (f64 world mm) */
    size_t shape;
    Aff tf;
    Tris tris;          /* own triangles (world mm) */
    size_t triangles;   /* Piece::triangles */
    Tallies warnings;
    Tallies cut;        /* C only: openings and booleans left uncut */
    int oom;
} Piece;

static void piece_free(Piece *p) {
    tris_free(&p->tris);
    tally_free(&p->warnings);
    tally_free(&p->cut);
}

/* Sorts map ids (bottom-up merge sort, no libc qsort in the wasm build). */
static int sort_ids(uint64_t *v, size_t n) {
    uint64_t *t = malloc((n ? n : 1) * sizeof *t);
    if (!t) return -1;
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
            while (i < mid && j < hi) t[k++] = v[j] < v[i] ? v[j++] : v[i++];
            while (i < mid) t[k++] = v[i++];
            while (j < hi) t[k++] = v[j++];
        }
        memcpy(v, t, n * sizeof *v);
    }
    free(t);
    return 0;
}
static long shape_find(const Shape *v, size_t n, uint64_t id) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (v[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && v[lo].id == id ? (long)lo : -1;
}

typedef struct {
    const cc_step_file *f;
    double scale, arc_tol, angle_scale;
    /* Openings (IfcRelVoidsElement): (host entity index, opening id) pairs sorted by host, then
     * opening id. */
    const size_t *void_host;
    const uint64_t *void_ids;
    size_t nvoid;
    Shape *shapes;
    size_t nshapes;
    Tallies cut_warnings; /* C only: openings and booleans left uncut (the pieces' `cut`) */
} Ctx;

/* The openings of host `ent`: void_ids[*at .. *at + count). */
static size_t voids_of(const Ctx *c, size_t ent, size_t *at) {
    size_t lo = 0, hi = c->nvoid;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (c->void_host[mid] < ent) lo = mid + 1;
        else hi = mid;
    }
    size_t e = lo;
    while (e < c->nvoid && c->void_host[e] == ent) e++;
    *at = lo;
    return e - lo;
}

typedef struct {
    size_t host;
    uint64_t id;
} VoidPair;

/* Sorts by host, then opening id (bottom-up merge sort: no libc qsort in the wasm build). */
static int sort_voids(VoidPair *v, size_t n) {
    VoidPair *t = malloc((n ? n : 1) * sizeof *t);
    if (!t) return -1;
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                t[k++] = v[j].host < v[i].host || (v[j].host == v[i].host && v[j].id < v[i].id) ? v[j++] : v[i++];
            while (i < mid) t[k++] = v[i++];
            while (j < hi) t[k++] = v[j++];
        }
        memcpy(v, t, n * sizeof *v);
    }
    free(t);
    return 0;
}

/* Parsed::mapped_item */
static const cc_step_entity *mapped_item(const Ctx *c, R *r, const cc_step_entity *e) {
    size_t at;
    if (voids_of(c, cc_step_index(c->f, e), &at)) return NULL;
    return mapped_item_of(r, e);
}

/* Reader::cut: subtract cutters from `tris` (mm). If the whole set fails (e.g. one opening is not
 * a closed solid), retry one cutter at a time and keep the ones that work. 0 ok, -1 out of memory. */
static int cut(R *w, Tris *tris, const Tris *cutters, size_t n, const char *what) {
    cc_tri_set *refs = malloc(n * sizeof *refs);
    if (!refs) return -1;
    for (size_t i = 0; i < n; i++) refs[i] = tri_set(&cutters[i]);
    Tris res;
    int rc = tris_apply(CC_BOOL_DIFFERENCE, tris, refs, n, &res, NULL);
    if (rc == 0) {
        tris_free(tris);
        *tris = res;
    } else if (rc > 0) {
        size_t failed = 0;
        for (size_t i = 0; i < n && rc >= 0; i++) {
            rc = tris_apply(CC_BOOL_DIFFERENCE, tris, &refs[i], 1, &res, NULL);
            if (rc == 0) {
                tris_free(tris);
                *tris = res;
            } else if (rc > 0) failed++;
        }
        if (failed && rc >= 0) {
            Buf b;
            buf_init(&b);
            buf_puts(&b, what);
            buf_puts(&b, ": ");
            buf_u64(&b, failed);
            buf_puts(&b, " opening(s) not cut (geometry not a closed solid)");
            tally_buf(&w->warnings, &b, 0);
            buf_free(&b);
        }
    }
    free(refs);
    return rc < 0 ? -1 : 0;
}

/* Parsed::piece. 0 ok, -1 out of memory. */
static int piece(Ctx *c, R *w, size_t ent, Budget *budget, Piece *out) {
    const cc_step_file *f = c->f;
    const cc_step_entity *e = &f->ents[ent];
    memset(out, 0, sizeof *out);
    out->ent = ent;
    const cc_step_entity *mi = mapped_item(c, w, e);
    uint64_t mid, pl;
    long sh;
    if (mi && cc_sv_as_ref(arg(mi, 0), &mid) && (sh = shape_find(c->shapes, c->nshapes, mid)) >= 0 &&
        cc_sv_as_ref(arg(e, 5), &pl)) {
        Aff p = object_placement(w, pl, 0);
        Aff op = transform_operator(w, arg(mi, 1));
        Aff t = aff_then(&p, &op);
        for (int k = 0; k < 3; k++) t.t[k] = t.t[k] * c->scale;
        out->instance = 1;
        out->shape = (size_t)sh;
        out->tf = t;
        out->triangles = c->shapes[sh].mesh.ni / 3;
        out->warnings = w->warnings;
        memset(&w->warnings, 0, sizeof w->warnings);
        return 0;
    }
    Emit em = {&out->tris, c->scale, budget};
    size_t unsup0 = w->unsup_n;
    product_body(w, e, &em);
    size_t vat, nv = voids_of(c, ent, &vat);
    if (nv && out->tris.n) {
        /* Openings (IfcRelVoidsElement): each opening's body meshed (same budget), then subtracted
         * from the host (Reader::cut). Without a kernel the host is drawn uncut (C only warning). */
        Tris *ob = calloc(nv, sizeof *ob);
        size_t cutters = 0;
        if (!ob) out->tris.oom = 1;
        for (size_t k = 0; ob && k < nv; k++) {
            const cc_step_entity *oe = cc_step_get(f, c->void_ids[vat + k]);
            if (!oe) continue;
            Emit eo = {&ob[cutters], c->scale, budget};
            product_body(w, oe, &eo);
            if (ob[cutters].oom) out->tris.oom = 1;
            if (ob[cutters].n) cutters++;
            else tris_free(&ob[cutters]);
        }
        Buf b;
        buf_init(&b);
        class_name(e->name, &b);
        if (cutters && !cc_boolean_kernel()) {
            buf_puts(&b, ": ");
            buf_u64(&b, cutters);
            buf_puts(&b, " opening(s) not cut (no boolean kernel in the C core)");
            tally_buf(&out->cut, &b, 0);
        } else if (cutters && cut(w, &out->tris, ob, cutters, buf_cstr(&b)))
            out->tris.oom = 1;
        buf_free(&b);
        for (size_t k = 0; ob && k < cutters; k++) tris_free(&ob[k]);
        free(ob);
    }
    if (w->unsup_n > unsup0)
        for (size_t k = unsup0; k < w->unsup_n; k++)
            tally_add(&out->cut, "boolean result drawn as its first operand (no boolean kernel in the C core)",
                      75, 0);
    out->triangles = out->tris.n;
    out->warnings = w->warnings;
    memset(&w->warnings, 0, sizeof w->warnings);
    return out->tris.oom ? -1 : 0;
}

static void category_of(const cc_step_entity *e, const ElMat *m, cc_category *cat, cc_str *mat) {
    int set = m && m->set;
    *cat = set ? m->cat : CC_CAT_OTHER;
    mat->p = set && m->name ? m->name : "";
    mat->n = set ? m->n : 0;
    if (!mat->n && is(e, "IFCGEOGRAPHICELEMENT")) *cat = CC_CAT_EARTH;
}

static cc_ifc_class class_enum(const char *n) {
    if (!strcmp(n, "IFCCOLUMN")) return CC_IFC_COLUMN;
    if (!strcmp(n, "IFCBEAM")) return CC_IFC_BEAM;
    if (!strcmp(n, "IFCSLAB")) return CC_IFC_SLAB;
    if (!strcmp(n, "IFCWALL") || !strcmp(n, "IFCWALLSTANDARDCASE")) return CC_IFC_WALL;
    if (!strcmp(n, "IFCFOOTING")) return CC_IFC_FOOTING;
    return CC_IFC_PROXY;
}

static int set_err(cc_error *err, const char *a, const char *b) {
    size_t n = 0;
    for (const char *p = a; *p && n + 1 < sizeof err->msg; p++) err->msg[n++] = *p;
    for (const char *p = b; p && *p && n + 1 < sizeof err->msg; p++) err->msg[n++] = *p;
    err->msg[n] = 0;
    err->offset = CC_NO_OFFSET;
    return -1;
}

/* Box of the f32 positions (Aabb::of_points): f32::min / max. */
static float fmin32(float a, float b) { return a != a ? b : b != b ? a : a < b ? a : b; }
static float fmax32(float a, float b) { return a != a ? b : b != b ? a : a > b ? a : b; }

/* ---- parallel jobs (cc_jobs): each task writes its own slot, merged in order after */

#define PROD_CHUNK ((size_t)1 << 16)
typedef struct {
    const cc_step_file *f;
    unsigned char *isp; /* per entity: a product with a body */
} ProdJob;

static void prod_job(void *ctx, size_t k, size_t w) {
    (void)w;
    ProdJob *j = ctx;
    const cc_step_file *f = j->f;
    for (size_t i = k * PROD_CHUNK; i < f->n && i < (k + 1) * PROD_CHUNK; i++) {
        const cc_step_entity *e = &f->ents[i];
        int skip = 0;
        for (size_t q = 0; q < sizeof SKIP_CLASSES / sizeof *SKIP_CLASSES && !skip; q++)
            skip = !strcmp(e->name, SKIP_CLASSES[q]);
        const cc_step_entity *sh = skip ? NULL : cc_step_deref(f, arg(e, 6)), *pl = sh ? cc_step_deref(f, arg(e, 5)) : NULL;
        /* IFC2X3 allows the supertype IfcProductRepresentation (same attributes: Name,
         * Description, Representations): read alike (ticket IFC2, DEVIATIONS.md section 10). */
        j->isp[i] = sh && (is(sh, "IFCPRODUCTDEFINITIONSHAPE") || is(sh, "IFCPRODUCTREPRESENTATION")) && pl &&
                    ends_with(pl->name, "PLACEMENT");
    }
}

#define SCRATCH_KEEP ((size_t)1 << 20) /* a worker's scratch past a job: at most one 1 MiB block */

typedef struct {
    Ctx *c;
    R *rs;          /* per worker */
    Arena *arenas;  /* per worker: the meshes (adopted by the scene arena) */
    Arena *scratch; /* per worker */
    Budget *budget;
    Tallies *warn;  /* per shape */
    int *bad;       /* per shape */
} ShapeJob;

/* Parsed::shapes, one map. */
static void shape_job(void *ctx, size_t k, size_t w) {
    ShapeJob *j = ctx;
    Ctx *c = j->c;
    R *r = &j->rs[w];
    Tris mb = {0};
    const cc_step_entity *map = cc_step_get(c->f, c->shapes[k].id);
    if (map) {
        Emit e = {&mb, c->scale, j->budget};
        map_shape(r, c->shapes[k].id, map, &e);
    }
    j->warn[k] = r->warnings;
    memset(&r->warnings, 0, sizeof r->warnings);
    double zero[3] = {0, 0, 0};
    arena_reset(&j->scratch[w]);
    j->bad[k] = mb.oom || cc_scene_mesh_build((double(*)[3][3])mb.t, (uint32_t)mb.n, zero, &j->arenas[w],
                                              &j->scratch[w], &c->shapes[k].mesh);
    tris_free(&mb);
    arena_trim(&j->scratch[w], SCRATCH_KEEP);
}

typedef struct {
    Ctx *c;
    R *rs; /* per worker */
    Budget *budget;
    const size_t *ids;
    Piece *res;
} PieceJob;

static void piece_job(void *ctx, size_t k, size_t w) {
    PieceJob *j = ctx;
    if (piece(j->c, &j->rs[w], j->ids[k], j->budget, &j->res[k])) j->res[k].oom = 1;
}

typedef struct {
    size_t ent;
    int instance;
    size_t shape;
    Aff tf;
    Tris tris;
    cc_mesh mesh; /* built from tris (not an instance) */
    int bad;
} Item;

typedef struct {
    Item *items;
    const double *origin;
    Arena *arenas, *scratch; /* per worker */
} MeshJob;

/* MeshBuilder::build of an element's own triangles. */
static void mesh_job(void *ctx, size_t k, size_t w) {
    MeshJob *j = ctx;
    Item *it = &j->items[k];
    if (it->instance) return;
    arena_reset(&j->scratch[w]);
    it->bad = cc_scene_mesh_build((double(*)[3][3])it->tris.t, (uint32_t)it->tris.n, j->origin, &j->arenas[w],
                                  &j->scratch[w], &it->mesh);
    tris_free(&it->tris);
    arena_trim(&j->scratch[w], SCRATCH_KEEP);
}

/* ---- read */

/* LINKF: progress (ifc_read.h cc_ifc_progress), relaxed atomics: another thread reads it. */
static void pg_set(cc_ifc_progress *pg, size_t v) {
    if (pg) cc_atomic_store(&pg->done, v);
}
static int pg_cancelled(cc_ifc_progress *pg) { return pg && cc_atomic_load(&pg->cancel); }

static int read_parsed(cc_step_file *f, const char *text, size_t n, cc_ifc_scene *out, cc_error *err, cc_ifc_progress *pg) {
    cc_scene *s = &out->scene;
    Arena *a = &s->arena;
    int rc = -1, oom = 0;
    size_t ne = f->n ? f->n : 1;
    R r;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.f = f;
    r_init(&r, f, 0.5, 1.0);
    Conts cont = {0};
    ElMats em = {0};
    VoidPair *vp = NULL;
    size_t *void_host = NULL;
    uint64_t *void_ids = NULL;
    size_t *ids = NULL, nids = 0;
    Piece *res = NULL;
    size_t nres = 0;
    Tallies warnings = {0};
    /* Per worker: a reader, an arena for meshes (adopted by the scene's), a scratch arena. */
    size_t nw = cc_jobs_threads();
    R *rs = calloc(nw, sizeof *rs);
    Arena *warena = calloc(nw, sizeof *warena), *wscratch = calloc(nw, sizeof *wscratch);
    Tallies *swarn = NULL;
    int *sbad = NULL;
    Item *items = NULL;
    size_t nitems = 0;
    if (!rs || !warena || !wscratch) goto oom;
    for (size_t w = 0; w < nw; w++) {
        arena_init(&warena[w], 0);
        arena_init(&wscratch[w], 0);
    }
    if (!r_ok(&r)) goto oom;

    /* Parsed::new */
    if (f->skipped) {
        Buf b;
        buf_init(&b);
        buf_u64(&b, f->skipped);
        buf_puts(&b, " malformed entity instance(s) skipped");
        tally_buf(&r.warnings, &b, 0);
        buf_free(&b);
    }
    double scale = length_scale_to_mm(&r);
    r.arc_tol = 0.5 / scale; /* ARC_TOLERANCE_MM / scale */
    r.angle_scale = plane_angle_to_radians(&r);
    c.scale = scale, c.arc_tol = r.arc_tol, c.angle_scale = r.angle_scale;
    containers(&r, &cont);
    if (cont.oom) goto oom;
    PROF("  containers");
    { /* voids(): per host, opening ids sorted */
        size_t nv = 0, cv = 0;
        for (size_t i = 0; i < f->n; i++) {
            const cc_step_entity *rv = &f->ents[i];
            uint64_t h, o;
            if (!is(rv, "IFCRELVOIDSELEMENT") || !cc_sv_as_ref(arg(rv, 4), &h) || !cc_sv_as_ref(arg(rv, 5), &o))
                continue;
            const cc_step_entity *he = cc_step_get(f, h);
            if (!he) continue;
            if (nv == cv && grow((void **)&vp, &cv, nv + 1, sizeof *vp)) goto oom;
            vp[nv].host = cc_step_index(f, he), vp[nv].id = o;
            nv++;
        }
        if (sort_voids(vp, nv)) goto oom;
        void_host = malloc((nv ? nv : 1) * sizeof *void_host);
        void_ids = malloc((nv ? nv : 1) * sizeof *void_ids);
        if (!void_host || !void_ids) goto oom;
        for (size_t k = 0; k < nv; k++) void_host[k] = vp[k].host, void_ids[k] = vp[k].id;
        free(vp);
        vp = NULL;
        c.void_host = void_host, c.void_ids = void_ids, c.nvoid = nv;
    }
    PROF("  voids");
    materials(&r, &em);
    PROF("  materials");
    ids = malloc(ne * sizeof *ids);
    if (!ids) goto oom;
    { /* products with a body, in id order (flags by chunks of entities in parallel) */
        unsigned char *isp = malloc(ne);
        if (!isp) goto oom;
        ProdJob pj = {f, isp};
        cc_jobs_run((f->n + PROD_CHUNK - 1) / PROD_CHUNK, prod_job, &pj);
        for (size_t i = 0; i < f->n; i++)
            if (isp[i]) ids[nids++] = i;
        free(isp);
    }
    PROF("  ids");
    if (storey_levels(&r, scale, a, &out->storeys, &out->nstoreys)) goto oom;
    if (grid_axes(&r, scale, a, &out->grids, &out->ngrids)) goto oom;
    georef(&r, scale, a, &out->georef, out->base_point_mm, &oom);
    warnings = r.warnings;
    memset(&r.warnings, 0, sizeof r.warnings);
    r_free(&r);
    r_init(&r, f, c.arc_tol, c.angle_scale);
    if (!r_ok(&r)) goto oom;

    /* Limits::for_text */
    PROF("parsed::new");
    PROF("  pre-limits");
    uint64_t db = displayed_bytes(text, n);
    PROF("  displayed_bytes");
    uint64_t mt64 = db > UINT64_MAX / 10 ? UINT64_MAX : db * 10;
    size_t max_triangles = mt64 > (uint64_t)(size_t)-1 ? (size_t)-1 : (size_t)mt64;
    if (max_triangles < 1000000) max_triangles = 1000000;
    size_t max_visits = db / 10 < 5000000 ? 5000000 : (size_t)(db / 10);
    size_t cap = max_triangles;
    size_t element_max = sat_add(min_z(cap, ELEMENT_MAX_TRIANGLES), 1);

    /* Parsed::shapes: each map that an element is meshed once (map id order). */
    {
        size_t nm = 0;
        c.shapes = calloc(nids ? nids : 1, sizeof *c.shapes);
        uint64_t *mids = malloc((nids ? nids : 1) * sizeof *mids);
        if (!c.shapes || !mids) { free(mids); goto oom; }
        for (size_t k = 0; k < nids; k++) {
            const cc_step_entity *mi = mapped_item(&c, &r, &f->ents[ids[k]]);
            uint64_t m;
            if (mi && cc_sv_as_ref(arg(mi, 0), &m)) mids[nm++] = m;
        }
        if (sort_ids(mids, nm)) { free(mids); goto oom; }
        size_t u = 0;
        for (size_t k = 0; k < nm; k++)
            if (!u || c.shapes[u - 1].id != mids[k]) c.shapes[u++].id = mids[k];
        free(mids);
        c.nshapes = u;
        Budget sb = {(size_t)-1, max_visits, 0, element_max};
        swarn = calloc(c.nshapes ? c.nshapes : 1, sizeof *swarn);
        sbad = calloc(c.nshapes ? c.nshapes : 1, sizeof *sbad);
        if (!swarn || !sbad) goto oom;
        for (size_t w = 0; w < nw; w++) r_init(&rs[w], f, c.arc_tol, c.angle_scale);
        ShapeJob sj = {&c, rs, warena, wscratch, &sb, swarn, sbad};
        PROF("  shapes setup");
        cc_jobs_run(c.nshapes, shape_job, &sj);
        PROF("  shapes run");
        int bad = 0;
        for (size_t k = 0; k < c.nshapes; k++) bad |= sbad[k], tally_merge(&r.warnings, &swarn[k]);
        tally_merge(&warnings, &r.warnings);
        if (bad) goto oom;
    }
    PROF("shapes");

    /* Parsed::scene_progress: batches in file order, the cap applied in order. */
    size_t total = 0, shown = 0, drawn = 0, visits_left = max_visits;
    int visits_exhausted = 0;
    items = calloc(nids ? nids : 1, sizeof *items);
    res = calloc(BATCH, sizeof *res);
    if (!items || !res) goto oom;
    for (size_t b0 = 0; b0 < nids; b0 += BATCH) {
        size_t bn = min_z(BATCH, nids - b0);
        pg_set(pg, 300 + 650 * b0 / nids); /* LINKF */
        if (pg_cancelled(pg)) goto cancelled;
        /* Fresh readers per batch (read.rs: map_init per batch). */
        for (size_t w = 0; w < nw; w++) {
            r_free(&rs[w]);
            r_init(&rs[w], f, c.arc_tol, c.angle_scale);
        }
        Budget g = {sat_add(sat_add(cap - total, cap), 1), visits_left, 0, element_max};
        PieceJob pj = {&c, rs, &g, ids + b0, res};
        cc_jobs_run(bn, piece_job, &pj);
        nres = bn;
        int poom = 0;
        for (size_t k = 0; k < nres; k++) poom |= res[k].oom, tally_merge(&c.cut_warnings, &res[k].cut);
        if (poom) goto oom;
        Budget used = g;
        if (g.exhausted) { /* meshed again in order on one thread, stopping past the cap */
            for (size_t k = 0; k < nres; k++) piece_free(&res[k]);
            nres = 0;
            r_free(&r);
            r_init(&r, f, c.arc_tol, c.angle_scale);
            if (!r_ok(&r)) goto oom;
            Budget g2 = {sat_add(sat_add(cap - total, cap), 1), visits_left, 0, element_max};
            size_t sum = total;
            for (size_t k = 0; k < bn; k++) {
                int prc = piece(&c, &r, ids[b0 + k], &g2, &res[k]);
                nres++;
                tally_merge(&c.cut_warnings, &res[k].cut);
                if (prc) goto oom;
                sum += res[k].triangles;
                if (sum > cap) break;
            }
            used = g2;
        }
        visits_left = used.visits_left;
        if (!visits_left) visits_exhausted = 1;
        int stop = 0;
        for (size_t k = 0; k < nres && !stop; k++) {
            Piece *p = &res[k];
            tally_merge(&warnings, &p->warnings);
            size_t nadd, drawn_n = p->triangles;
            if (!p->instance) nadd = p->tris.n;
            else if (!c.shapes[p->shape].seen) nadd = 1 + p->triangles;
            else nadd = 1;
            if (p->instance) c.shapes[p->shape].seen = 1;
            if (sat_add(total, nadd) > cap || sat_add(drawn, drawn_n) > max_triangles) {
                stop = 1;
                break;
            }
            total += nadd, drawn += drawn_n, shown++;
            /* SceneBuilder::push / push_instance: empty bodies are not kept. */
            if (p->instance ? c.shapes[p->shape].mesh.ni > 0 : p->tris.n > 0) {
                Item *it = &items[nitems++];
                it->ent = p->ent, it->instance = p->instance, it->shape = p->shape, it->tf = p->tf;
                it->tris = p->tris;
                memset(&p->tris, 0, sizeof p->tris);
            }
        }
        for (size_t k = 0; k < nres; k++) piece_free(&res[k]);
        nres = 0;
        if (stop || visits_exhausted) break;
    }
    tally_merge(&warnings, &c.cut_warnings);
    PROF("pieces (batches)");
    if (shown < nids || visits_exhausted) {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "model too large or nested too deeply: ");
        buf_u64(&b, nids - shown);
        buf_puts(&b, " element(s) not shown after ");
        buf_u64(&b, total);
        buf_puts(&b, " triangles / ");
        buf_u64(&b, max_visits - visits_left);
        buf_puts(&b, " items");
        size_t j;
        for (j = 0; j < warnings.n; j++)
            if (warnings.v[j].n == b.len && !memcmp(warnings.v[j].s, b.p, b.len)) break;
        if (j < warnings.n) warnings.v[j].count = 1;
        else tally_buf(&warnings, &b, 1);
        buf_free(&b);
    }

    /* SceneBuilder::finish: the origin is the middle of the world box, rounded to mm. */
    double lo[3] = {1.0 / 0.0, 1.0 / 0.0, 1.0 / 0.0}, hi[3] = {-1.0 / 0.0, -1.0 / 0.0, -1.0 / 0.0};
    for (size_t i = 0; i < nitems; i++) {
        const Item *it = &items[i];
        if (!it->instance) {
            for (size_t t = 0; t < it->tris.n; t++)
                for (int k = 0; k < 9; k++) lo[k % 3] = fmin_rs(lo[k % 3], it->tris.t[t][k]), hi[k % 3] = fmax_rs(hi[k % 3], it->tris.t[t][k]);
            continue;
        }
        const cc_mesh *m = &c.shapes[it->shape].mesh;
        float bl[3] = {1.0f / 0.0f, 1.0f / 0.0f, 1.0f / 0.0f}, bh[3] = {-1.0f / 0.0f, -1.0f / 0.0f, -1.0f / 0.0f};
        for (uint32_t v = 0; v < m->nv; v++)
            for (int k = 0; k < 3; k++) bl[k] = fmin32(bl[k], m->pos[v][k]), bh[k] = fmax32(bh[k], m->pos[v][k]);
        for (int corner = 0; corner < 8; corner++) {
            double cc[3], w3[3];
            for (int k = 0; k < 3; k++) cc[k] = (double)((corner >> k & 1) == 0 ? bl[k] : bh[k]);
            aff_apply(&it->tf, cc, w3);
            for (int k = 0; k < 3; k++) lo[k] = fmin_rs(lo[k], w3[k]), hi[k] = fmax_rs(hi[k], w3[k]);
        }
    }
    for (int k = 0; k < 3; k++) s->origin[k] = finite(lo[0]) ? round_rs((lo[k] + hi[k]) / 2.0) : 0.0;
    PROF("origin");
    s->shapes = arena_alloc(a, (c.nshapes + 1) * sizeof *s->shapes, 8);
    s->elements = arena_alloc(a, (nitems + 1) * sizeof *s->elements, 8);
    out->info = arena_alloc(a, (nitems + 1) * sizeof *out->info, 8);
    if (!s->shapes || !s->elements || !out->info) goto oom;
    for (size_t k = 0; k < c.nshapes; k++) s->shapes[k] = c.shapes[k].mesh;
    s->nshapes = c.nshapes;
    {
        MeshJob mj = {items, s->origin, warena, wscratch};
        cc_jobs_run(nitems, mesh_job, &mj);
    }
    Buf cls;
    buf_init(&cls);
    size_t *drawn_ent = malloc((nitems + 1) * sizeof *drawn_ent); /* out->empty_classes */
    if (!drawn_ent) { buf_free(&cls); goto oom; }
    for (size_t i = 0; i < nitems; i++) {
        Item *it = &items[i];
        cc_scene_element *se = &s->elements[s->nelements];
        memset(se, 0, sizeof *se);
        if (it->instance) {
            se->shape = (int32_t)it->shape;
            for (int k = 0; k < 3; k++) {
                se->to_scene.x[k] = (float)it->tf.x[k];
                se->to_scene.y[k] = (float)it->tf.y[k];
                se->to_scene.z[k] = (float)it->tf.z[k];
                se->to_scene.t[k] = (float)(it->tf.t[k] - s->origin[k]);
            }
        } else {
            se->shape = -1;
            if (it->bad) { buf_free(&cls); free(drawn_ent); goto oom; }
            se->mesh = it->mesh;
            if (!se->mesh.ni) continue; /* all degenerate: dropped */
        }
        /* Parsed::info */
        const cc_step_entity *e = &f->ents[it->ent];
        cc_ifc_info *in = &out->info[s->nelements];
        const char *t;
        size_t tn;
        if (!cc_sv_as_str(arg(e, 0), &t, &tn)) t = "", tn = 0;
        in->global_id = a_str(a, t, tn, &oom);
        if (cc_gid_valid(t, tn)) memcpy(se->id.c, t, 22);
        buf_clear(&cls);
        class_name(e->name, &cls);
        in->class_name = a_str(a, cls.p ? cls.p : "", cls.len, &oom);
        oom |= cls.oom;
        if (!cc_sv_as_str(arg(e, 2), &t, &tn)) t = "", tn = 0;
        in->name = a_str(a, t, tn, &oom);
        const Cont *ct = cont_get(&cont, it->ent);
        in->storey = ct && ct->has_name ? a_str(a, ct->name, ct->nn, &oom) : (cc_str){"", 0};
        cc_str mat;
        category_of(e, elmat_get(&em, it->ent), &se->category, &mat);
        in->material = a_str(a, mat.p, mat.n, &oom);
        se->class_name = in->class_name;
        se->cls = class_enum(e->name);
        se->model_index = s->nelements;
        drawn_ent[s->nelements] = it->ent;
        s->nelements++;
    }
    buf_free(&cls);
    { /* out->empty_classes: the shown products not drawn (ids and drawn_ent are both in id order) */
        Tallies em0 = {0};
        Buf eb;
        buf_init(&eb);
        for (size_t k = 0, d = 0; k < shown && k < nids; k++) {
            if (d < s->nelements && drawn_ent[d] == ids[k]) { d++; continue; }
            const char *nm = f->ents[ids[k]].name;
            tally_add(&em0, nm, strlen(nm), 0);
        }
        tally_sort(&em0);
        for (size_t k = 0; k < em0.n; k++) {
            buf_append(&eb, em0.v[k].s, em0.v[k].n);
            buf_putc(&eb, ' ');
            buf_u64(&eb, em0.v[k].count);
            buf_putc(&eb, '\n');
        }
        out->empty_classes = a_str(a, eb.p ? eb.p : "", eb.len, &oom);
        oom |= eb.oom;
        buf_free(&eb);
        tally_free(&em0);
        free(drawn_ent);
    }
    PROF("mesh build");
    if (oom || cc_scene_share_copies(s)) goto oom;
    PROF("share_copies");
    for (size_t k = 0; k < s->nelements; k++) out->triangles += cc_scene_local_mesh(s, k)->ni / 3;
    out->products = nids;
    out->entities = f->n;

    /* Warnings, sorted, "(×N)" when repeated. */
    tally_sort(&warnings);
    s->warnings = arena_alloc(a, (warnings.n + 1) * sizeof *s->warnings, 8);
    if (!s->warnings) goto oom;
    {
        Buf b;
        buf_init(&b);
        for (size_t k = 0; k < warnings.n; k++) {
            buf_clear(&b);
            buf_append(&b, warnings.v[k].s, warnings.v[k].n);
            if (warnings.v[k].count > 1) {
                buf_puts(&b, " (\xc3\x97");
                buf_u64(&b, warnings.v[k].count);
                buf_putc(&b, ')');
            }
            s->warnings[s->nwarnings++] = a_str(a, b.p ? b.p : "", b.len, &oom);
        }
        oom |= b.oom;
        buf_free(&b);
    }
    {
        Buf b;
        buf_init(&b);
        for (size_t k = 0; k < f->nschemas; k++) {
            if (k) buf_puts(&b, ", ");
            buf_puts(&b, f->schemas[k]);
        }
        out->schema = a_str(a, b.p ? b.p : "", b.len, &oom);
        oom |= b.oom;
        buf_free(&b);
    }
    if (oom) goto oom;
    rc = 0;
    goto done;
oom:
    set_err(err, "out of memory", "");
    goto done;
cancelled: /* LINKF */
    set_err(err, "cancelled", "");
done:
    for (size_t k = 0; k < nres; k++) piece_free(&res[k]);
    for (size_t k = 0; items && k < nitems; k++) tris_free(&items[k].tris);
    free(res);
    free(items);
    r_free(&r);
    tally_free(&warnings);
    tally_free(&c.cut_warnings);
    free(c.shapes);
    for (size_t k = 0; swarn && k < c.nshapes; k++) tally_free(&swarn[k]);
    free(swarn);
    free(sbad);
    for (size_t w = 0; w < nw; w++) {
        if (rs) r_free(&rs[w]);
        if (warena) arena_adopt(a, &warena[w]);
        if (wscratch) arena_free(&wscratch[w]);
    }
    free(rs);
    free(warena);
    free(wscratch);
    conts_free(&cont);
    elmats_free(&em);
    free(vp);
    free(void_host);
    free(void_ids);
    free(ids);
    return rc;
}

int cc_ifc_read(const char *text, size_t n, cc_ifc_scene *out, cc_error *err) {
    return cc_ifc_read_progress(text, n, out, err, NULL);
}

int cc_ifc_read_progress(const char *text, size_t n, cc_ifc_scene *out, cc_error *err, cc_ifc_progress *pg) {
    memset(out, 0, sizeof *out);
    arena_init(&out->scene.arena, 0);
    out->schema.p = "";
    cc_step_file f;
#ifdef CC_IFC_PROFILE
    prof_t0 = prof_now();
#endif
    if (cc_step_parse_skipping(&f, text, n, NOT_DISPLAYED, N_NOT_DISPLAYED, err)) {
        char msg[sizeof err->msg];
        memcpy(msg, err->msg, sizeof msg);
        msg[sizeof msg - 1] = 0;
        size_t off = err->offset;
        set_err(err, "parse: ", msg);
        err->offset = off;
        cc_step_free(&f);
        /* The other IFC encodings, by their first bytes (ticket IFC2): say which. */
        size_t k = n >= 3 && !memcmp(text, "\xef\xbb\xbf", 3) ? 3 : 0;
        while (k < n && k < 4096 && (text[k] == ' ' || text[k] == '\t' || text[k] == '\r' || text[k] == '\n')) k++;
        if (n >= 4 && !memcmp(text, "PK\x03\x04", 4))
            set_err(err, "unsupported format: ifcZIP (a zip archive; unzip it and open the .ifc inside)", 0);
        else if (k < n && text[k] == '<')
            set_err(err, "unsupported format: ifcXML (only IFC-SPF, ISO 10303-21 text, is read)", 0);
        else if (k < n && (text[k] == '{' || text[k] == '['))
            set_err(err, "unsupported format: ifcJSON (only IFC-SPF, ISO 10303-21 text, is read)", 0);
        return -1;
    }
    PROF("step parse");
    pg_set(pg, 300); /* LINKF */
#ifdef CC_IFC_PROFILE
    fprintf(stderr, "PROF entities %zu\n", f.n);
#endif
    int ifc = 0;
    for (size_t i = 0; i < f.nschemas; i++) ifc |= strncmp(f.schemas[i], "IFC", 3) == 0;
    if (!ifc) {
        Buf b, j;
        buf_init(&b), buf_init(&j);
        buf_puts(&b, "not an IFC file (schema ");
        for (size_t i = 0; i < f.nschemas; i++) {
            if (i) buf_putc(&j, ',');
            buf_puts(&j, f.schemas[i]);
        }
        cc_buf_debug_str(&b, j.p ? j.p : "", j.len);
        buf_putc(&b, ')');
        set_err(err, buf_cstr(&b), "");
        buf_free(&b), buf_free(&j);
        cc_step_free(&f);
        return -1;
    }
    int rc = pg_cancelled(pg) ? (set_err(err, "cancelled", ""), -1) : read_parsed(&f, text, n, out, err, pg);
    if (!rc) pg_set(pg, 950);
    PROF("geometry+scene total");
    cc_step_free(&f);
    return rc;
}

void cc_ifc_scene_free(cc_ifc_scene *s) {
    cc_scene_free(&s->scene);
    memset(s, 0, sizeof *s);
}

/* ---- text */

static void hex_n(Buf *b, uint64_t u, int digits) {
    static const char H[] = "0123456789abcdef";
    for (int k = digits - 1; k >= 0; k--) buf_putc(b, H[(u >> (4 * k)) & 15]);
}
static void hex64(Buf *b, double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    hex_n(b, u, 16);
}
static void hex32(Buf *b, float x) {
    uint32_t u;
    memcpy(&u, &x, 4);
    hex_n(b, u, 8);
}
static void jstr(Buf *b, cc_str s) { json_escape(b, s.p ? s.p : "", s.n); }

void cc_ifc_georef_json(Buf *b, const cc_ifc_georef *g) { cc_georef_json(b, g); }

int cc_ifc_scene_meta(const cc_ifc_scene *s, cc_viewfile_meta *mt) {
    cc_viewfile_meta_init(mt);
    Arena *a = &mt->arena;
    size_t ne = s->scene.nelements;
    cc_view_info *info = arena_alloc(a, (ne + 1) * sizeof *info, 8);
    cc_view_storey *st = arena_alloc(a, (s->nstoreys + 1) * sizeof *st, 8);
    cc_view_grid *gr = arena_alloc(a, (s->ngrids + 1) * sizeof *gr, 8);
    if (!info || !st || !gr) return -1;
    for (size_t k = 0; k < ne; k++) {
        const cc_ifc_info *x = &s->info[s->scene.elements[k].model_index];
        info[k].global_id = x->global_id;
        info[k].name = x->name;
        info[k].storey = x->storey;
        info[k].material = x->material;
        info[k].volume_fix_m3 = 0.0; /* WAVE1B (B1): a model's sleeves only */
    }
    for (size_t i = 0; i < s->nstoreys; i++) {
        st[i].name = s->storeys[i].name;
        st[i].elevation = s->storeys[i].elevation_mm;
        st[i].id = s->storeys[i].id;
    }
    for (size_t i = 0; i < s->ngrids; i++) {
        const cc_ifc_grid *g = &s->grids[i];
        gr[i].tag = g->tag;
        memcpy(gr[i].start, g->start_mm, sizeof gr[i].start);
        memcpy(gr[i].end, g->end_mm, sizeof gr[i].end);
    }
    Buf j;
    buf_init(&j);
    cc_ifc_georef_json(&j, &s->georef);
    int rc = 0;
    if (j.oom) rc = -1;
    else if (!(j.len == 2 && j.p[0] == '{' && j.p[1] == '}')) { /* Georef::is_default */
        char *t = arena_dup(a, j.p, j.len);
        if (!t) rc = -1;
        else mt->has_georef = 1, mt->georef.p = t, mt->georef.n = j.len;
    }
    buf_free(&j);
    mt->info = info, mt->ninfo = ne;
    mt->storeys = st, mt->nstoreys = s->nstoreys;
    mt->grids = gr, mt->ngrids = s->ngrids;
    return rc;
}

static void put_k(Buf *b, size_t k) { /* {k:03} */
    if (k < 100) buf_putc(b, '0');
    if (k < 10) buf_putc(b, '0');
    buf_u64(b, k);
}

void cc_ifc_scene_txt(Buf *b, const cc_ifc_scene *s) {
    const cc_scene *sc = &s->scene;
    buf_puts(b, "schema ");
    jstr(b, s->schema);
    buf_puts(b, "\norigin");
    for (int k = 0; k < 3; k++) buf_putc(b, ' '), hex64(b, sc->origin[k]);
    buf_puts(b, "\ngeoref ");
    cc_ifc_georef_json(b, &s->georef);
    buf_puts(b, "\nstoreys ");
    buf_u64(b, s->nstoreys);
    buf_putc(b, '\n');
    for (size_t k = 0; k < s->nstoreys; k++) {
        buf_puts(b, "storey ");
        hex64(b, s->storeys[k].elevation_mm);
        buf_putc(b, ' ');
        jstr(b, s->storeys[k].id);
        buf_putc(b, ' ');
        jstr(b, s->storeys[k].name);
        buf_putc(b, '\n');
    }
    buf_puts(b, "grids ");
    buf_u64(b, s->ngrids);
    buf_putc(b, '\n');
    for (size_t k = 0; k < s->ngrids; k++) {
        const cc_ifc_grid *g = &s->grids[k];
        buf_puts(b, "grid ");
        hex64(b, g->start_mm[0]), buf_putc(b, ' '), hex64(b, g->start_mm[1]), buf_putc(b, ' ');
        hex64(b, g->end_mm[0]), buf_putc(b, ' '), hex64(b, g->end_mm[1]), buf_putc(b, ' ');
        jstr(b, g->tag);
        buf_putc(b, '\n');
    }
    buf_puts(b, "elements ");
    buf_u64(b, sc->nelements);
    buf_puts(b, " triangles ");
    buf_u64(b, s->triangles);
    buf_puts(b, s->triangles <= 20000 ? " meshes yes\n" : " meshes no\n");
    for (size_t k = 0; k < sc->nelements; k++) {
        const cc_scene_element *e = &sc->elements[k];
        const cc_ifc_info *in = &s->info[e->model_index];
        const cc_mesh *m = cc_scene_local_mesh(sc, k);
        const char *cat = cc_category_name(e->category);
        put_k(b, k);
        buf_putc(b, ' ');
        buf_append(b, in->global_id.p, in->global_id.n);
        buf_putc(b, ' ');
        buf_append(b, in->class_name.p, in->class_name.n);
        buf_putc(b, ' ');
        buf_u64(b, (uint64_t)e->category);
        buf_putc(b, ' ');
        buf_putc(b, (char)(cat[0] - 'a' + 'A'));
        buf_puts(b, cat + 1);
        buf_puts(b, " v="), buf_u64(b, m->nv);
        buf_puts(b, " i="), buf_u64(b, m->ni / 3);
        buf_puts(b, " e="), buf_u64(b, m->ne);
        buf_puts(b, " n="), buf_u64(b, m->nn);
        buf_puts(b, " name="), jstr(b, in->name);
        buf_puts(b, " storey="), jstr(b, in->storey);
        buf_puts(b, " material="), jstr(b, in->material);
        buf_putc(b, '\n');
    }
    for (size_t k = 0; k < sc->nwarnings; k++) {
        buf_puts(b, "warning ");
        buf_append(b, sc->warnings[k].p, sc->warnings[k].n);
        buf_putc(b, '\n');
    }
}

void cc_ifc_mesh_txt(Buf *b, const cc_ifc_scene *s, size_t k) {
    const cc_scene_element *e = &s->scene.elements[k];
    const cc_mesh *m = cc_scene_local_mesh(&s->scene, k);
    if (e->shape >= 0) {
        const float *cols[4] = {e->to_scene.x, e->to_scene.y, e->to_scene.z, e->to_scene.t};
        buf_putc(b, 't');
        for (int c = 0; c < 4; c++)
            for (int i = 0; i < 3; i++) buf_putc(b, ' '), hex32(b, cols[c][i]);
        buf_putc(b, '\n');
    }
    for (uint32_t i = 0; i < m->nv; i++) {
        buf_putc(b, 'v');
        for (int j = 0; j < 3; j++) buf_putc(b, ' '), hex32(b, m->pos[i][j]);
        buf_putc(b, '\n');
    }
    for (uint32_t i = 0; i + 2 < m->ni; i += 3) {
        buf_putc(b, 'i');
        for (int j = 0; j < 3; j++) buf_putc(b, ' '), buf_u64(b, m->idx[i + j]);
        buf_putc(b, '\n');
    }
    for (uint32_t i = 0; i < m->ne; i++) {
        buf_putc(b, 'e');
        for (int j = 0; j < 6; j++) buf_putc(b, ' '), hex32(b, m->edges[i][j]);
        buf_putc(b, '\n');
    }
    for (uint32_t i = 0; i < m->nn; i++) {
        buf_putc(b, 'n');
        for (int j = 0; j < 8; j++) buf_putc(b, ' '), buf_i64(b, m->normals[i][j]);
        buf_putc(b, '\n');
    }
}

void cc_ifc_error_txt(Buf *b, const cc_error *err) {
    buf_puts(b, "error ");
    buf_puts(b, err->msg);
    buf_putc(b, '\n');
}

/* ---- BMSH (host/mesh.c's layout, version 1) */

static void put_u32(Buf *b, uint32_t v) { buf_append(b, &v, 4); }

int cc_ifc_scene_bmsh(const cc_ifc_scene *is, uint32_t rev, Buf *out, cc_error *err) {
    const cc_scene *s = &is->scene;
    int rc = -1;
    Arena tmp;
    arena_init(&tmp, 0);
    size_t ne = s->nelements;
    cc_mesh *w = arena_alloc(&tmp, (ne + 1) * sizeof *w, 16);
    if (!w) { set_err(err, "out of memory", ""); goto done; }
    uint64_t nv = 0, ni = 0, ns = 0;
    for (size_t k = 0; k < ne; k++) {
        if (cc_scene_world_mesh(s, k, &tmp, &w[k])) { set_err(err, "out of memory", ""); goto done; }
        nv += w[k].nv, ni += w[k].ni, ns += w[k].ne;
    }
    if (ne > UINT32_MAX || nv > UINT32_MAX || ni > UINT32_MAX || ns > UINT32_MAX) {
        set_err(err, "scene too large for BMSH", "");
        goto done;
    }
    buf_append(out, "BMSH", 4);
    put_u32(out, 1);
    put_u32(out, rev);
    put_u32(out, (uint32_t)ne), put_u32(out, (uint32_t)nv), put_u32(out, (uint32_t)ni), put_u32(out, (uint32_t)ns);
    buf_append(out, s->origin, 24);
    uint32_t fv = 0, fi = 0, fs = 0;
    for (size_t k = 0; k < ne; k++) {
        const cc_scene_element *e = &s->elements[k];
        cc_str id = is->info[e->model_index].global_id;
        for (size_t i = 0; i < 22; i++) buf_putc(out, i < id.n ? id.p[i] : '0');
        buf_putc(out, (char)(unsigned char)e->category);
        buf_putc(out, 0);
        put_u32(out, fv), put_u32(out, w[k].nv), put_u32(out, fi), put_u32(out, w[k].ni);
        put_u32(out, fs), put_u32(out, w[k].ne);
        fv += w[k].nv, fi += w[k].ni, fs += w[k].ne;
    }
    for (size_t k = 0; k < ne; k++) buf_append(out, w[k].pos, (size_t)w[k].nv * 12);
    fv = 0;
    for (size_t k = 0; k < ne; k++) {
        for (uint32_t i = 0; i < w[k].ni; i++) put_u32(out, fv + w[k].idx[i]);
        fv += w[k].nv;
    }
    for (size_t k = 0; k < ne; k++) buf_append(out, w[k].edges, (size_t)w[k].ne * 24);
    static const int8_t zero[8];
    for (size_t k = 0; k < ne; k++)
        for (uint32_t i = 0; i < w[k].ne; i++) buf_append(out, i < w[k].nn ? w[k].normals[i] : zero, 8);
    if (out->oom) { set_err(err, "out of memory", ""); goto done; }
    rc = 0;
done:
    arena_free(&tmp);
    return rc;
}

/* ---- 3D1: BMSH version 3 (scene_bmsh.h), the ids as cc_ifc_scene_bmsh writes them */
static void ifc_bmsh_id(const void *ctx, const cc_scene *s, size_t k, char out[22]) {
    const cc_ifc_scene *is = ctx;
    cc_str id = is->info[s->elements[k].model_index].global_id;
    for (size_t i = 0; i < 22; i++) out[i] = i < id.n ? id.p[i] : '0';
}

int cc_ifc_scene_bmsh3(const cc_ifc_scene *is, uint32_t rev, Buf *out, cc_error *err) {
    return cc_scene_bmsh3(&is->scene, ifc_bmsh_id, is, rev, out, err);
}
