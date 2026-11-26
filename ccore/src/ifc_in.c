/* IFC import (subset), a port of crates/bimble-ifc/src/import.rs and the parts of read.rs it
 * uses. See ifc_in.h. Arithmetic follows Rust's order of operations exactly; no contraction. */
#include "ifc_in.h"
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#include <stdlib.h>
#include <string.h>
#include "earcut.h"
#include "georef.h"
#include "guid.h"
#include "ifc_math.h"
#include "verbs.h"

#include "ifc_geom.h"


/* ------------------------------------------------------------------ math */

double cc_sqrt_soft(double x) {
    uint64_t b;
    memcpy(&b, &x, 8);
    if (x != x || x == 0.0) return x;
    if (x < 0) return 0.0 / 0.0 * 0.0 + (x - x) / (x - x);
    int e = (int)((b >> 52) & 0x7FF);
    if (e == 0x7FF) return x; /* +inf */
    uint64_t m = b & ((1ull << 52) - 1);
    if (e) m |= 1ull << 52, e -= 1023;
    else {
        e = -1022;
        while (m < (1ull << 52)) m <<= 1, e--;
    }
    int ex = e - 52; /* x = m * 2^ex */
    if (ex & 1) m <<= 1, ex--;
    /* root = floor(sqrt(m * 2^52)), digit by digit over 53 bit pairs. */
    uint64_t rem = 0, root = 0;
    for (int k = 52; k >= 0; k--) {
        int sh = 2 * k - 52; /* bit pair k of m<<52 is bits (2k-52, 2k-51) of m */
        uint64_t two = sh >= 0 ? (m >> sh) & 3 : 0;
        rem = (rem << 2) | two;
        uint64_t trial = (root << 2) | 1;
        if (rem >= trial) rem -= trial, root = (root << 1) | 1;
        else root <<= 1;
    }
    if (rem > root) root++;
    int k2 = (ex - 52) / 2 + 52;
    if (root == (1ull << 53)) root >>= 1, k2++;
    uint64_t r = ((uint64_t)(k2 + 1023) << 52) | (root & ((1ull << 52) - 1));
    double y;
    memcpy(&y, &r, 8);
    return y;
}

/* The integer version everywhere: no libm to link (tcc, clang's errno fallback, wasm32). */


/* ------------------------------------------------------------------ property sets */

typedef struct {
    size_t host; /* entity index */
    size_t seq;
    char *key, *val;
    size_t nk, nv;
} Prop;
typedef struct {
    Prop *v;
    size_t n, cap;
} Props;

static int simple(const cc_sv *v, Buf *out) {
    const char *s;
    size_t n;
    int64_t i;
    double x;
    if (cc_sv_as_str(v, &s, &n)) { buf_append(out, s, n); return 1; }
    if (cc_sv_as_enum(v, &s)) { buf_puts(out, s); return 1; }
    if (cc_sv_as_i64(v, &i)) { buf_i64(out, i); return 1; }
    if (cc_sv_as_f64(v, &x)) { cc_buf_display_f64(out, x); return 1; }
    return 0;
}
static int property_value(R *r, const cc_sv *v, Buf *out) {
    if (v->kind == CC_SV_TYPED) return simple(v->u.typed.inner, out);
    if (v->kind == CC_SV_REF) {
        const cc_step_entity *e = get(r, v);
        return e && simple(arg(e, 0), out);
    }
    return 0;
}
/* 1 value in out, 0 empty ($), -1 kind not read. */
static int property(R *r, const cc_step_entity *p, Buf *out) {
    const char *nm = p->name;
    if (!strcmp(nm, "IFCPROPERTYSINGLEVALUE")) {
        if (arg(p, 2)->kind == CC_SV_NULL) return 0;
        return property_value(r, arg(p, 2), out) ? 1 : -1;
    }
    if (!strcmp(nm, "IFCPROPERTYLISTVALUE") || !strcmp(nm, "IFCPROPERTYENUMERATEDVALUE")) {
        const cc_sv *l;
        size_t n;
        if (!cc_sv_as_list(arg(p, 2), &l, &n)) return -1;
        for (size_t i = 0; i < n; i++) {
            if (i) buf_puts(out, "; ");
            if (!property_value(r, &l[i], out)) return -1;
        }
        return n ? 1 : 0;
    }
    if (!strcmp(nm, "IFCPROPERTYBOUNDEDVALUE")) {
        Buf hi, lo;
        buf_init(&hi);
        buf_init(&lo);
        int h = property_value(r, arg(p, 2), &hi), l = property_value(r, arg(p, 3), &lo), rc = 1;
        if (l && h) buf_append(out, lo.p, lo.len), buf_puts(out, "\xe2\x80\x93"), buf_append(out, hi.p, hi.len);
        else if (l) buf_puts(out, "\xe2\x89\xa5 "), buf_append(out, lo.p, lo.len);
        else if (h) buf_puts(out, "\xe2\x89\xa4 "), buf_append(out, hi.p, hi.len);
        else rc = 0;
        buf_free(&hi);
        buf_free(&lo);
        return rc;
    }
    if (!strcmp(nm, "IFCQUANTITYLENGTH") || !strcmp(nm, "IFCQUANTITYAREA") || !strcmp(nm, "IFCQUANTITYVOLUME") ||
        !strcmp(nm, "IFCQUANTITYCOUNT") || !strcmp(nm, "IFCQUANTITYWEIGHT") || !strcmp(nm, "IFCQUANTITYTIME")) {
        double x;
        if (!cc_sv_as_f64(arg(p, 3), &x)) return 0;
        cc_buf_display_f64(out, x);
        return 1;
    }
    return -1;
}

static char *dupb(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (d) {
        if (n) memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

static size_t property_sets(R *r, Props *out) {
    const cc_step_file *f = r->f;
    size_t skipped = 0;
    Buf key, val;
    buf_init(&key);
    buf_init(&val);
    for (size_t i = 0; i < f->n; i++) {
        const cc_step_entity *rel = &f->ents[i];
        if (!is(rel, "IFCRELDEFINESBYPROPERTIES")) continue;
        const cc_step_entity *ps = get(r, arg(rel, 5));
        if (!ps) continue;
        int q = is(ps, "IFCELEMENTQUANTITY");
        if (!is(ps, "IFCPROPERTYSET") && !q) continue;
        const char *pn = "";
        size_t pnn = 0;
        cc_sv_as_str(arg(ps, 2), &pn, &pnn);
        if (!pnn || (pnn == 16 && !memcmp(pn, "Bimble_Generator", 16)) ||
            (pnn == 25 && !memcmp(pn, "Bimble_ProjectInformation", 25)))
            continue;
        const cc_sv *l;
        size_t n;
        if (!cc_sv_as_list(arg(ps, q ? 5 : 4), &l, &n)) continue;
        for (size_t k = 0; k < n; k++) {
            const cc_step_entity *p = get(r, &l[k]);
            const char *nm;
            size_t nn;
            if (!p || !cc_sv_as_str(arg(p, 0), &nm, &nn)) continue;
            buf_clear(&val);
            int got = property(r, p, &val);
            if (got == 0) continue;
            if (got < 0) { skipped++; continue; }
            buf_clear(&key);
            if (!(pnn == 17 && !memcmp(pn, "Bimble_Properties", 17))) buf_append(&key, pn, pnn), buf_putc(&key, '/');
            buf_append(&key, nm, nn);
            const cc_sv *hs;
            size_t nh;
            if (!cc_sv_as_list(arg(rel, 4), &hs, &nh)) continue;
            for (size_t h = 0; h < nh; h++) {
                const cc_step_entity *he = get(r, &hs[h]);
                if (!he) continue;
                if (out->n == out->cap) {
                    size_t cap = out->cap ? out->cap * 2 : 64;
                    void *pp = realloc(out->v, cap * sizeof *out->v);
                    if (!pp) continue;
                    out->v = pp, out->cap = cap;
                }
                Prop *x = &out->v[out->n];
                x->host = cc_step_index(f, he);
                x->seq = out->n;
                x->key = dupb(key.p ? key.p : "", key.len), x->nk = key.len;
                x->val = dupb(val.p ? val.p : "", val.len), x->nv = val.len;
                if (x->key && x->val) out->n++;
                else free(x->key), free(x->val);
            }
        }
    }
    buf_free(&key);
    buf_free(&val);
    return skipped;
}

static int prop_cmp(const Prop *a, const Prop *b) {
    if (a->host != b->host) return a->host < b->host ? -1 : 1;
    size_t k = a->nk < b->nk ? a->nk : b->nk;
    int c = memcmp(a->key, b->key, k);
    if (c) return c;
    if (a->nk != b->nk) return a->nk < b->nk ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}
static void prop_sort(Prop *v, Prop *tmp, size_t n) {
    if (n < 2) return;
    size_t h = n / 2, i = 0, j = h, k = 0;
    prop_sort(v, tmp, h);
    prop_sort(v + h, tmp, n - h);
    while (i < h && j < n) tmp[k++] = prop_cmp(&v[j], &v[i]) < 0 ? v[j++] : v[i++];
    while (i < h) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof *v);
}

/* ------------------------------------------------------------------ geometry -> generator */

static int unit3(const double *v, double *o) {
    double n = cc_sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(n > 1e-12 && finite(n))) return 0;
    for (int k = 0; k < 3; k++) o[k] = v[k] / n;
    return 1;
}
static void toward_x(double *u) {
    if (u[0] < 0.0 || (u[0] == 0.0 && u[1] < 0.0)) u[0] = -u[0], u[1] = -u[1];
}

typedef struct {
    double centre[2], w, h, u[2];
} Rect;

static int rectangle(const Loop *l, Rect *rc) {
    if (l->n != 4) return 0;
    double e[4][2];
    for (int i = 0; i < 4; i++) {
        const double *a = l->p[i], *b = l->p[(i + 1) % 4];
        e[i][0] = b[0] - a[0], e[i][1] = b[1] - a[1];
    }
    for (int i = 0; i < 4; i++) {
        const double *a = e[i], *b = e[(i + 1) % 4];
        double la = cc_hypot(a[0], a[1]), lb = cc_hypot(b[0], b[1]);
        double c = (a[0] * b[0] + a[1] * b[1]) / (la * lb);
        if (la < 1e-9 || lb < 1e-9 || (c < 0 ? -c : c) > 1e-6) return 0;
    }
    for (int i = 0; i < 2; i++) {
        double la = cc_hypot(e[i][0], e[i][1]), lb = cc_hypot(e[i + 2][0], e[i + 2][1]);
        double d = la - lb;
        if ((d < 0 ? -d : d) > 1e-6 * (la > lb ? la : lb)) return 0;
    }
    rc->centre[0] = (((l->p[0][0] + l->p[1][0]) + l->p[2][0]) + l->p[3][0]) / 4.0;
    rc->centre[1] = (((l->p[0][1] + l->p[1][1]) + l->p[2][1]) + l->p[3][1]) / 4.0;
    double la = cc_hypot(e[0][0], e[0][1]), lb = cc_hypot(e[1][0], e[1][1]);
    double da[2] = {e[0][0] / la, e[0][1] / la}, db[2] = {e[1][0] / lb, e[1][1] / lb};
    toward_x(da);
    toward_x(db);
    if (da[0] >= db[0]) rc->u[0] = da[0], rc->u[1] = da[1], rc->w = la, rc->h = lb;
    else rc->u[0] = db[0], rc->u[1] = db[1], rc->w = lb, rc->h = la;
    return 1;
}

static int copy_loop(Arena *a, const Loop *l, double scale, cc_loop *out) {
    out->n = l->n;
    out->pts = arena_alloc(a, (l->n ? l->n : 1) * sizeof *out->pts, 8);
    if (!out->pts) return 0;
    for (size_t i = 0; i < l->n; i++) out->pts[i][0] = l->p[i][0] * scale, out->pts[i][1] = l->p[i][1] * scale;
    return 1;
}

/* generator_for. 0 none; 1 parametric; 2 not. */
static int generator_for(Arena *ar, cc_ifc_class cls, const Aff *fr, Loops *ls, double depth, double scale,
                         cc_placement *pl, cc_generator *g) {
    const Loop *outer = &ls->v[0];
    Rect rc;
    int rect = ls->n == 1 && rectangle(outer, &rc);
    double z[3];
    if (!unit3(fr->z, z)) return 0;
    memcpy(pl->z_axis, z, sizeof z);
#define TO_WORLD(c, o)                                                     \
    for (int k = 0; k < 3; k++) (o)[k] = (fr->t[k] + fr->x[k] * (c)[0] + fr->y[k] * (c)[1]) * scale
#define AXIS(u, o)                                                                              \
    do {                                                                                        \
        double w_[3];                                                                           \
        for (int k = 0; k < 3; k++) w_[k] = fr->x[k] * (u)[0] + fr->y[k] * (u)[1];              \
        if (!unit3(w_, (o))) return 0;                                                          \
    } while (0)
    if (rect && (cls == CC_IFC_COLUMN || cls == CC_IFC_BEAM || cls == CC_IFC_FOOTING)) {
        TO_WORLD(rc.centre, pl->origin);
        AXIS(rc.u, pl->x_axis);
        if (cls == CC_IFC_BEAM) {
            g->kind = CC_GEN_RECT_BEAM;
            g->u.beam.width = rc.w * scale, g->u.beam.height = rc.h * scale, g->u.beam.length = depth * scale;
        } else {
            g->kind = cls == CC_IFC_COLUMN ? CC_GEN_RECT_COLUMN : CC_GEN_PAD_FOOTING;
            g->u.col.width = rc.w * scale, g->u.col.depth = rc.h * scale, g->u.col.height = depth * scale;
        }
        return 1;
    }
    if (rect && cls == CC_IFC_WALL) {
        double length, thick, u[2];
        if (rc.w >= rc.h) length = rc.w, thick = rc.h, u[0] = rc.u[0], u[1] = rc.u[1];
        else {
            length = rc.h, thick = rc.w, u[0] = -rc.u[1], u[1] = rc.u[0];
            toward_x(u);
        }
        double start[2] = {rc.centre[0] - u[0] * length / 2.0, rc.centre[1] - u[1] * length / 2.0};
        AXIS(u, pl->x_axis);
        TO_WORLD(start, pl->origin);
        g->kind = CC_GEN_WALL;
        g->u.wall.length = length * scale, g->u.wall.thickness = thick * scale, g->u.wall.height = depth * scale;
        return 1;
    }
    for (int k = 0; k < 3; k++) pl->origin[k] = fr->t[k] * scale;
    if (!unit3(fr->x, pl->x_axis)) return 0;
    if (cls == CC_IFC_SLAB && ls->n == 1) {
        g->kind = CC_GEN_SLAB;
        if (!copy_loop(ar, outer, scale, &g->u.slab.outline)) return 0;
        g->u.slab.thickness = depth * scale;
        return rect ? 1 : 2;
    }
    g->kind = CC_GEN_EXTRUDED_POLYGON;
    if (!copy_loop(ar, outer, scale, &g->u.poly.outline)) return 0;
    g->u.poly.nvoids = ls->n - 1;
    g->u.poly.voids = arena_alloc(ar, (ls->n ? ls->n : 1) * sizeof *g->u.poly.voids, 8);
    if (!g->u.poly.voids) return 0;
    for (size_t i = 1; i < ls->n; i++)
        if (!copy_loop(ar, &ls->v[i], scale, &g->u.poly.voids[i - 1])) return 0;
    g->u.poly.height = depth * scale;
    return 2;
#undef TO_WORLD
#undef AXIS
}

/* extruded_solid: the first IfcExtrudedAreaSolid of the shape's representations. */
static const cc_step_entity *extruded_solid(R *r, const cc_step_entity *e) {
    const cc_step_entity *shape = get(r, arg(e, 6));
    const cc_sv *reps, *its;
    size_t nr, ni;
    if (!shape || !cc_sv_as_list(arg(shape, 2), &reps, &nr)) return NULL;
    for (size_t i = 0; i < nr; i++) {
        const cc_step_entity *rep = get(r, &reps[i]);
        if (!rep) return NULL;
        if (!cc_sv_as_list(arg(rep, 3), &its, &ni)) continue;
        for (size_t j = 0; j < ni; j++) {
            const cc_step_entity *cur = get(r, &its[j]);
            if (!cur) continue;
            for (int k = 0; k < 4; k++) {
                if (is(cur, "IFCEXTRUDEDAREASOLID")) return cur;
                if (!is(cur, "IFCMAPPEDITEM")) break;
                const cc_step_entity *map = get(r, arg(cur, 0));
                if (!map) return NULL;
                const cc_step_entity *mrep = get(r, arg(map, 1));
                const cc_sv *l;
                size_t nl;
                if (!mrep || !cc_sv_as_list(arg(mrep, 3), &l, &nl) || !nl || l[0].kind != CC_SV_REF) return NULL;
                cur = get(r, &l[0]);
                if (!cur) return NULL;
            }
        }
    }
    return NULL;
}

/* only_item: 1 when `solid` is all the product's Body draws: one item in its body
 * representations, through mapped items with one item each (an item that maps a map already on
 * the way down is a cycle: the reader draws nothing for it, so it does not count) (ticket IFC3).
 * extruded_solid takes the first extrusion it meets (as import.rs does), so a Body of an
 * extrusion and anything else (a second extrusion, a brep, a revolved solid) lost the rest
 * without a word; such a product is now a mesh of its whole body. */
static int only_item(R *r, const cc_step_entity *e, const cc_step_entity *solid) {
    const cc_step_entity *shape = get(r, arg(e, 6));
    const cc_sv *reps, *its, *first = NULL;
    size_t nr, ni, count = 0;
    if (!shape || !cc_sv_as_list(arg(shape, 2), &reps, &nr)) return 0;
    for (size_t i = 0; i < nr && count < 2; i++) {
        const cc_step_entity *rep = get(r, &reps[i]);
        if (!rep || !is_body(rep) || !cc_sv_as_list(arg(rep, 3), &its, &ni)) continue;
        if (ni && !first) first = &its[0];
        count += ni;
    }
    if (count != 1) return 0;
    const cc_step_entity *cur = get(r, first), *maps[4];
    for (int k = 0; cur && k < 4; k++) {
        if (cur == solid) return 1;
        if (!is(cur, "IFCMAPPEDITEM")) return 0;
        const cc_step_entity *map = get(r, arg(cur, 0)), *mrep = map ? get(r, arg(map, 1)) : NULL;
        const cc_sv *l;
        size_t nl;
        if (!mrep || !cc_sv_as_list(arg(mrep, 3), &l, &nl)) return 0;
        maps[k] = map;
        const cc_step_entity *next = NULL;
        size_t live = 0;
        for (size_t j = 0; j < nl && live < 2; j++) {
            const cc_step_entity *it = get(r, &l[j]);
            if (!it) continue;
            if (is(it, "IFCMAPPEDITEM")) {
                const cc_step_entity *m2 = get(r, arg(it, 0));
                int cyc = 0;
                for (int q = 0; q <= k; q++) cyc |= m2 == maps[q];
                if (cyc) continue;
            }
            if (!live++) next = it;
        }
        if (live != 1) return 0;
        cur = next;
    }
    return cur == solid;
}

/* extruded_element: 0 none, 1 parametric, 2 polygon. */
static int extruded_element(R *r, Arena *ar, const cc_step_entity *solid, const Aff *rel, cc_ifc_class cls,
                            double scale, cc_placement *pl, cc_generator *g) {
    Loops ls;
    /* A composite of several profiles is several regions: not one extrusion (a mesh: the
     * reader sweeps it region by region; ticket IFC3). */
    const cc_step_entity *pd = get(r, arg(solid, 0));
    const cc_sv *pl_;
    size_t npl;
    if (is(pd, "IFCCOMPOSITEPROFILEDEF") && cc_sv_as_list(arg(pd, 2), &pl_, &npl) && npl > 1) return 0;
    if (!profile(r, arg(solid, 0), 0, &ls)) return 0;
    int rc = 0;
    if (!ls.n || ls.v[0].n < 3) goto out;
    Aff sp = axis_placement(r, arg(solid, 1));
    double dir[3], depth;
    if (!direction(r, arg(solid, 2), dir) || !cc_sv_as_f64(arg(solid, 3), &depth)) goto out;
    if (!finite(depth) || depth <= 0.0) goto out;
    Aff full = aff_then(rel, &sp);
    const double *pairs[3][2] = {{full.x, full.y}, {full.x, full.z}, {full.y, full.z}};
    for (int k = 0; k < 3; k++) {
        double la = cc_sqrt(dot(pairs[k][0], pairs[k][0])), lb = cc_sqrt(dot(pairs[k][1], pairs[k][1]));
        if (la < 1e-12 || lb < 1e-12 || dot(pairs[k][0], pairs[k][1]) / (la * lb) > 1e-6) goto out;
    }
    if (cc_hypot(dir[0], dir[1]) > 1e-6) goto out;
    int flip = dir[2] < 0.0;
    Aff fr = full;
    if (flip) {
        for (int k = 0; k < 3; k++) fr.y[k] = -full.y[k], fr.z[k] = -full.z[k];
        for (size_t i = 0; i < ls.n; i++)
            for (size_t j = 0; j < ls.v[i].n; j++) ls.v[i].p[j][1] = -ls.v[i].p[j][1];
    }
    if (signed_area(&ls.v[0]) < 0.0)
        for (size_t i = 0; i < ls.n; i++) loop_reverse(&ls.v[i]);
    for (size_t i = 1; i < ls.n; i++)
        if (signed_area(&ls.v[i]) > 0.0) loop_reverse(&ls.v[i]);
    rc = generator_for(ar, cls, &fr, &ls, depth, scale, pl, g);
out:
    loops_free(&ls);
    return rc;
}


/* ---- mesh generators */

/* mesh_from_tris: world triangles (mm) -> positions (deduplicated by bits, first-use order,
 * storey-relative) and indices, in the arena. 0 ok, -1 out of memory or too large. */
static int mesh_from_tris(Arena *ar, const Tris *t, double elev, cc_generator *g) {
    size_t nv = t->n * 3;
    if (t->n > ((size_t)UINT32_MAX) / 3) return -1;
    size_t cap = 16;
    while (cap < nv * 2) cap *= 2;
    uint32_t *tab = malloc(cap * sizeof *tab); /* index + 1, 0 empty */
    double(*pos)[3] = arena_alloc(ar, (nv ? nv : 1) * sizeof *pos, 8);
    uint32_t *idx = arena_alloc(ar, (nv ? nv : 1) * sizeof *idx, 4);
    if (!tab || !pos || !idx) { free(tab); return -1; }
    memset(tab, 0, cap * sizeof *tab);
    size_t np = 0;
    for (size_t i = 0; i < nv; i++) {
        const double *p = &t->t[i / 3][(i % 3) * 3];
        double q[3] = {p[0] + 0.0, p[1] + 0.0, p[2] - elev + 0.0};
        uint64_t b[3];
        memcpy(b, q, sizeof b);
        uint64_t h = (b[0] * 0x9E3779B97F4A7C15ull) ^ (b[1] * 0xC2B2AE3D27D4EB4Full) ^ (b[2] * 0x165667B19E3779F9ull);
        h ^= h >> 29;
        size_t k = (size_t)h & (cap - 1);
        uint32_t found = 0;
        while (tab[k]) {
            if (!memcmp(pos[tab[k] - 1], q, sizeof q)) { found = tab[k]; break; }
            k = (k + 1) & (cap - 1);
        }
        if (!found) {
            memcpy(pos[np], q, sizeof q);
            tab[k] = (uint32_t)(++np);
            found = tab[k];
        }
        idx[i] = found - 1;
    }
    free(tab);
    g->kind = CC_GEN_MESH;
    g->u.mesh.positions = pos;
    g->u.mesh.npositions = np;
    g->u.mesh.indices = idx;
    g->u.mesh.nindices = nv;
    return 0;
}

static void placement_at(cc_placement *p, double x, double y, double z) {
    p->origin[0] = x, p->origin[1] = y, p->origin[2] = z;
    p->z_axis[0] = 0, p->z_axis[1] = 0, p->z_axis[2] = 1;
    p->x_axis[0] = 1, p->x_axis[1] = 0, p->x_axis[2] = 0;
}

/* Meshes of representation maps, by entity index: 0 not met, 1 empty, 2 the mesh. */
typedef struct {
    unsigned char *state;
    cc_generator *mesh;
} Shapes;

/* mapped_copy: the element as a placed copy of its map's shared mesh, when its body is one
 * IfcMappedItem, nothing cuts it and the copy is only moved and turned. 1 yes, 0 no, -1 oom. */
static int mapped_copy(R *r, Arena *ar, const cc_step_entity *e, int has_openings, const Aff *rel, double scale,
                       Budget *budget, Shapes *shapes, cc_placement *pl, cc_generator *g) {
    if (has_openings) return 0;
    const cc_step_entity *mi = mapped_item_of(r, e);
    uint64_t map_id;
    if (!mi || !cc_sv_as_ref(arg(mi, 0), &map_id)) return 0;
    Aff op = transform_operator(r, arg(mi, 1));
    Aff t = aff_then(rel, &op);
    for (int k = 0; k < 3; k++) pl->origin[k] = t.t[k] * scale, pl->z_axis[k] = t.z[k], pl->x_axis[k] = t.x[k];
    double y[3];
    cross(pl->z_axis, pl->x_axis, y);
#define NEAR(a, b) (((a) - (b)) < 0 ? -((a) - (b)) < 1e-9 : ((a) - (b)) < 1e-9)
    int rigid = NEAR(dot(t.x, t.x), 1.0) && NEAR(dot(t.z, t.z), 1.0) && NEAR(dot(t.x, t.z), 0.0);
    for (int k = 0; k < 3; k++) rigid = rigid && NEAR(y[k], t.y[k]);
#undef NEAR
    if (!rigid) return 0;
    const cc_step_entity *map = cc_step_get(r->f, map_id);
    size_t ix = map ? cc_step_index(r->f, map) : 0;
    if (!map) return 0; /* Rust caches None under the id; same outcome */
    if (!shapes->state[ix]) {
        Tris mb = {0};
        Emit em = {&mb, scale, budget};
        map_shape(r, map_id, map, &em);
        shapes->state[ix] = 1;
        if (mb.oom) { tris_free(&mb); return -1; }
        if (mb.n) {
            if (mesh_from_tris(ar, &mb, 0.0, &shapes->mesh[ix])) { tris_free(&mb); return -1; }
            shapes->state[ix] = 2;
        }
        tris_free(&mb);
    }
    if (shapes->state[ix] != 2) return 0;
    *g = shapes->mesh[ix];
    return 1;
}


/* ---- WAVE1B (B3, ADR 0077 decision 18): an opening whose Body is one extrusion along its own +z of
 * a rectangle or a circle (profile at its origin) comes back parametric, in the host's frame `fr`
 * (selev: the host storey's elevation, mm): a box (rect_column) or a sleeve (the CC_SLEEVE_SIDES-gon
 * opening_add makes), so opening_set edits it again. 1 done (*o filled), 0 not this case (the mesh,
 * as before), -1 out of memory. */
static int parametric_opening(R *r, Arena *ar, const cc_step_entity *op, const Aff *fr, double selev, double scale, cc_opening *o) {
    const cc_step_entity *solid = extruded_solid(r, op);
    if (!solid) return 0;
    { /* the Body's one item is the solid itself (no mapping in between) */
        const cc_step_entity *shape = get(r, arg(op, 6));
        const cc_sv *reps, *its;
        size_t nr, ni, count = 0;
        const cc_step_entity *first = NULL;
        if (!shape || !cc_sv_as_list(arg(shape, 2), &reps, &nr)) return 0;
        for (size_t i = 0; i < nr; i++) {
            const cc_step_entity *rep = get(r, &reps[i]);
            if (!rep || !is_body(rep) || !cc_sv_as_list(arg(rep, 3), &its, &ni)) continue;
            if (ni && !first) first = get(r, &its[0]);
            count += ni;
        }
        if (count != 1 || first != solid) return 0;
    }
    const cc_step_entity *pd = get(r, arg(solid, 0));
    int rect = is(pd, "IFCRECTANGLEPROFILEDEF"), circ = is(pd, "IFCCIRCLEPROFILEDEF");
    if (!rect && !circ) return 0;
    const cc_sv *ppos = arg(pd, 2);
    if (ppos && ppos->kind == CC_SV_REF) { /* a profile placed anywhere but its origin: not this case */
        Aff pp = axis_placement(r, ppos);
        if (pp.t[0] != 0.0 || pp.t[1] != 0.0 || pp.x[0] != 1.0 || pp.x[1] != 0.0) return 0;
    }
    double a, b = 0.0, depth, dir[3];
    if (!cc_sv_as_f64(arg(pd, 3), &a) || (rect && !cc_sv_as_f64(arg(pd, 4), &b))) return 0;
    if (!direction(r, arg(solid, 2), dir) || !cc_sv_as_f64(arg(solid, 3), &depth)) return 0;
    if (!(dir[0] == 0.0 && dir[1] == 0.0 && dir[2] > 0.0) || !finite(a) || !finite(b) || !finite(depth) || !(a > 0) ||
        (rect && !(b > 0)) || !(depth > 0))
        return 0;
    uint64_t plid;
    Aff opa = IDENT;
    if (cc_sv_as_ref(arg(op, 5), &plid)) opa = object_placement(r, plid, 0);
    Aff sp = axis_placement(r, arg(solid, 1));
    Aff full = aff_then(&opa, &sp);
    double w[3] = {full.t[0] * scale, full.t[1] * scale, full.t[2] * scale - selev}, zx[3], xx[3];
    if (!unit3(full.z, zx) || !unit3(full.x, xx)) return 0;
    memset(o, 0, sizeof *o);
    aff_local(fr, w, o->placement.origin);
    for (int k = 0; k < 3; k++) {
        o->placement.z_axis[k] = dot(zx, k == 0 ? fr->x : k == 1 ? fr->y : fr->z) + 0.0;
        o->placement.x_axis[k] = dot(xx, k == 0 ? fr->x : k == 1 ? fr->y : fr->z) + 0.0;
    }
    for (int k = 0; k < 3; k++) { /* exact where it is (an axis-aligned frame stays exact) */
        double *v[2] = {o->placement.z_axis, o->placement.x_axis};
        for (int j = 0; j < 2; j++)
            if (v[j][k] < 1e-12 && v[j][k] > -1e-12) v[j][k] = 0.0;
    }
    if (rect) {
        o->generator.kind = CC_GEN_RECT_COLUMN;
        o->generator.u.col.width = a * scale, o->generator.u.col.depth = b * scale, o->generator.u.col.height = depth * scale;
        return 1;
    }
    double rr = a * scale, (*pts)[2] = arena_alloc(ar, CC_SLEEVE_SIDES * sizeof *pts, 8);
    if (!pts) return -1;
    for (int i = 0; i < CC_SLEEVE_SIDES; i++) { /* as opening_add draws it (verbs_concrete.c) */
        double t = 2.0 * 3.141592653589793 * (double)i / (double)CC_SLEEVE_SIDES;
        pts[i][0] = rr * cc_cos(t) + 0.0, pts[i][1] = rr * cc_sin(t) + 0.0;
    }
    o->generator.kind = CC_GEN_EXTRUDED_POLYGON;
    o->generator.u.poly.outline.pts = pts, o->generator.u.poly.outline.n = CC_SLEEVE_SIDES;
    o->generator.u.poly.height = depth * scale;
    return 1;
}

static int class_of(const char *n, cc_ifc_class *c) {
    static const struct { const char *n; cc_ifc_class c; } S[] = {
        {"IFCCOLUMN", CC_IFC_COLUMN}, {"IFCBEAM", CC_IFC_BEAM}, {"IFCSLAB", CC_IFC_SLAB},
        {"IFCWALL", CC_IFC_WALL}, {"IFCWALLSTANDARDCASE", CC_IFC_WALL}, {"IFCWALLELEMENTEDCASE", CC_IFC_WALL},
        {"IFCFOOTING", CC_IFC_FOOTING},
    };
    for (size_t k = 0; k < sizeof S / sizeof *S; k++)
        if (!strcmp(S[k].n, n)) { *c = S[k].c; return 1; }
    return 0;
}

/* ------------------------------------------------------------------ import */

static int gid_from_sv(const cc_sv *v, cc_gid *out) {
    const char *s;
    size_t n;
    if (!cc_sv_as_str(v, &s, &n) || !cc_gid_valid(s, n)) return 0;
    memcpy(out->c, s, 22);
    return 1;
}
static cc_gid derived(const char *s, size_t n) {
    cc_guid g = cc_guid_derived(s, n);
    cc_gid o;
    memcpy(o.c, g.c, 22);
    return o;
}
static cc_str astr(Arena *a, const char *s, size_t n) {
    cc_str r = {"", 0};
    char *d = arena_alloc(a, n + 1, 1);
    if (d) {
        if (n) memcpy(d, s, n);
        r.p = d, r.n = n;
    }
    return r;
}

static int set_err(cc_error *err, const char *pre, const char *msg) {
    size_t a = strlen(pre), b = strlen(msg), cap = sizeof err->msg - 1;
    if (a > cap) a = cap;
    memcpy(err->msg, pre, a);
    if (b > cap - a) b = cap - a;
    memcpy(err->msg + a, msg, b);
    err->msg[a + b] = 0;
    err->offset = CC_NO_OFFSET;
    return -1;
}

typedef struct {
    cc_element el;
    size_t ent; /* entity index */
} Pending;

static int gid_cmpv(const void *a, const void *b) { return memcmp(a, b, 22); }

/* Sort an array of records starting with a cc_gid (stable merge sort, no qsort in wasm). */
static void gsort(char *v, char *tmp, size_t n, size_t sz) {
    if (n < 2) return;
    size_t h = n / 2, i = 0, j = h, k = 0;
    gsort(v, tmp, h, sz);
    gsort(v + h * sz, tmp, n - h, sz);
    while (i < h && j < n) {
        if (gid_cmpv(v + j * sz, v + i * sz) < 0) memcpy(tmp + k++ * sz, v + j++ * sz, sz);
        else memcpy(tmp + k++ * sz, v + i++ * sz, sz);
    }
    while (i < h) memcpy(tmp + k++ * sz, v + i++ * sz, sz);
    while (j < n) memcpy(tmp + k++ * sz, v + j++ * sz, sz);
    memcpy(v, tmp, n * sz);
}
static int sort_by_gid(void *v, size_t n, size_t sz) {
    if (n < 2) return 0;
    char *tmp = malloc(n * sz);
    if (!tmp) return -1;
    gsort(v, tmp, n, sz);
    free(tmp);
    return 0;
}

/* Open addressing set of GlobalIds (duplicate detection). */
typedef struct {
    cc_gid *v;
    unsigned char *used;
    size_t cap;
} GSet;
static size_t ghash(const cc_gid *g) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 22; i++) h = (h ^ (unsigned char)g->c[i]) * 1099511628211ull;
    return (size_t)h;
}
static int gset_insert(GSet *s, const cc_gid *g) { /* 1 inserted, 0 present */
    size_t i = ghash(g) & (s->cap - 1);
    while (s->used[i]) {
        if (!memcmp(s->v[i].c, g->c, 22)) return 0;
        i = (i + 1) & (s->cap - 1);
    }
    s->used[i] = 1;
    s->v[i] = *g;
    return 1;
}

static int gset_has(GSet *s, const cc_gid *g) {
    size_t i = ghash(g) & (s->cap - 1);
    while (s->used[i]) {
        if (!memcmp(s->v[i].c, g->c, 22)) return 1;
        i = (i + 1) & (s->cap - 1);
    }
    return 0;
}

typedef struct {
    uint64_t direct, parent;
    int has, has_parent;
} Cont;

static void unsup(R *r, const char *a, const char *b, const char *c) {
    buf_clear(&r->tmp);
    buf_puts(&r->tmp, a);
    if (b) buf_puts(&r->tmp, b);
    if (c) buf_puts(&r->tmp, c);
    tally_buf(&r->unsup, &r->tmp, 0);
}

int cc_ifc_import(cc_model *m, cc_ifc_report *rep, const char *text, size_t n, cc_error *err) {
    cc_model_init(m);
    memset(rep, 0, sizeof *rep);
    buf_init(&rep->lines);
    cc_step_file f;
    if (cc_step_parse(&f, text, n, err)) {
        char msg[sizeof err->msg];
        memcpy(msg, err->msg, sizeof msg);
        size_t off = err->offset;
        set_err(err, "parse: ", msg);
        err->offset = off;
        cc_step_free(&f);
        return -1;
    }
    rep->entities = f.n;
    rep->malformed = f.skipped;
    int ifc = 0;
    for (size_t i = 0; i < f.nschemas; i++) ifc |= strncmp(f.schemas[i], "IFC", 3) == 0;
    if (!ifc) {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "not an IFC file (schema ");
        Buf j;
        buf_init(&j);
        for (size_t i = 0; i < f.nschemas; i++) {
            if (i) buf_putc(&j, ',');
            buf_puts(&j, f.schemas[i]);
        }
        cc_buf_debug_str(&b, j.p ? j.p : "", j.len);
        buf_putc(&b, ')');
        set_err(err, "", buf_cstr(&b));
        buf_free(&b);
        buf_free(&j);
        cc_step_free(&f);
        return -1;
    }

    int rc = -1;
    Tallies skipped = {0}, warnings = {0}, by_class = {0};
    R r;
    memset(&r, 0, sizeof r);
    r.f = &f;
    buf_init(&r.tmp);
    Buf b;
    buf_init(&b);
    size_t ne = f.n ? f.n : 1;
    ElMats em = {0};
    Props props = {0};
    Pending *els = NULL;
    size_t nels = 0;
    cc_material *mats = NULL;
    size_t nmats = 0;
    size_t *st_ent = NULL; /* storey entity indices -> storey gid */
    cc_gid *st_gid = NULL;
    size_t nst = 0;
    GSet seen = {0}, opening_seen = {0};
    Budget budget = {0, 0, 0, (size_t)-1};
    Shapes shapes = {0};
    size_t *open_at = NULL;
    uint64_t *open_ids = NULL;
    int storeys_in_arena = 0;
    Cont *cont = calloc(ne, sizeof *cont);
    if (!cont) goto oom;

    if (f.skipped) {
        buf_clear(&b);
        buf_u64(&b, f.skipped);
        buf_puts(&b, " malformed entity instance(s) skipped");
        tally_buf(&warnings, &b, 1);
    }
    double scale = length_scale_to_mm(&r);
    r.arc_tol = 0.5 / scale; /* read.rs ARC_TOLERANCE_MM */
    r.angle_scale = plane_angle_to_radians(&r);

    /* Storeys. */
    const cc_step_entity *bld = first_of(&f, "IFCBUILDING");
    Aff building = IDENT;
    uint64_t plid;
    if (bld && cc_sv_as_ref(arg(bld, 5), &plid)) building = object_placement(&r, plid, 0);
    size_t nmoved = 0;
    Buf moved;
    buf_init(&moved);
    for (size_t i = 0; i < f.n; i++) {
        const cc_step_entity *st = &f.ents[i];
        if (!is(st, "IFCBUILDINGSTOREY")) continue;
        const char *nm = "";
        size_t nn = 0;
        cc_sv_as_str(arg(st, 2), &nm, &nn);
        double elev = 0.0;
        if (cc_sv_as_ref(arg(st, 5), &plid)) {
            Aff a = object_placement(&r, plid, 0);
            int same = 1;
            for (int k = 0; k < 3; k++) {
                double dx = a.x[k] - building.x[k], dy = a.y[k] - building.y[k];
                same &= (dx < 0 ? -dx : dx) < 1e-9 && (dy < 0 ? -dy : dy) < 1e-9;
            }
            double d0 = (a.t[0] - building.t[0]) * scale, d1 = (a.t[1] - building.t[1]) * scale;
            if ((d0 < 0 ? -d0 : d0) > 1e-3 || (d1 < 0 ? -d1 : d1) > 1e-3 || !same) {
                if (nmoved < 4) {
                    if (nmoved) buf_puts(&moved, ", ");
                    buf_append(&moved, nm, nn);
                }
                nmoved++;
            }
            elev = a.t[2] * scale;
        } else {
            double e9;
            elev = (cc_sv_as_f64(arg(st, 9), &e9) ? e9 : 0.0) * scale;
        }
        cc_gid g;
        if (!gid_from_sv(arg(st, 0), &g)) {
            buf_clear(&b);
            buf_puts(&b, "storey ");
            cc_buf_debug_str(&b, nm, nn);
            buf_puts(&b, ": invalid GlobalId; a new one was derived");
            tally_buf(&warnings, &b, 1);
            buf_clear(&b);
            buf_puts(&b, "storey/");
            buf_u64(&b, st->id);
            g = derived(b.p, b.len);
        }
        size_t k;
        for (k = 0; k < m->nstoreys; k++)
            if (!memcmp(m->storeys[k].id.c, g.c, 22)) break;
        if (k == m->nstoreys) {
            cc_storey *v = realloc(m->storeys, (m->nstoreys + 1) * sizeof *v);
            if (!v) goto oom;
            m->storeys = v;
            m->nstoreys++;
        }
        m->storeys[k].id = g;
        m->storeys[k].name = astr(&m->arena, nm, nn);
        m->storeys[k].elevation = elev;
        m->storeys[k].not_storey = 0; /* every IfcBuildingStorey is a storey (ADR 0056) */
        size_t *se = realloc(st_ent, (nst + 1) * sizeof *se);
        if (se) st_ent = se;
        cc_gid *sg = realloc(st_gid, (nst + 1) * sizeof *sg);
        if (sg) st_gid = sg;
        if (!se || !sg) goto oom;
        st_ent[nst] = i, st_gid[nst] = g, nst++;
    }
    if (nmoved) {
        buf_clear(&b);
        buf_u64(&b, nmoved);
        buf_puts(&b, " storey(s) placed offset or turned in plan from their building (");
        buf_append(&b, moved.p, moved.len);
        if (nmoved > 4) buf_puts(&b, ", \xe2\x80\xa6");
        buf_puts(&b, "): their elements keep their world position");
        tally_buf(&warnings, &b, 1);
    }
    buf_free(&moved);
    if (!m->nstoreys) {
        tally_add(&warnings, "no IfcBuildingStorey in the file; elements go into a synthetic storey", 69, 1);
        m->storeys = malloc(sizeof *m->storeys);
        if (!m->storeys) goto oom;
        m->nstoreys = 1;
        m->storeys[0].id = derived("storey/synthetic", 16);
        m->storeys[0].name = astr(&m->arena, "(no storey)", 11);
        m->storeys[0].elevation = 0.0;
        m->storeys[0].not_storey = 0;
    }
    /* storeys sorted by id; the storey array moves into the arena at the end. */
    if (sort_by_gid(m->storeys, m->nstoreys, sizeof *m->storeys)) goto oom;
    cc_gid fallback = m->storeys[0].id;

    materials(&r, &em);
    size_t psets_skipped = property_sets(&r, &props);
    if (props.n) {
        Prop *tmp = malloc(props.n * sizeof *tmp);
        if (!tmp) goto oom;
        prop_sort(props.v, tmp, props.n);
        free(tmp);
    }
    if (psets_skipped) {
        buf_clear(&b);
        buf_u64(&b, psets_skipped);
        buf_puts(&b, " property value(s) not imported (unsupported kind)");
        tally_buf(&warnings, &b, 1);
    }
    /* Openings: host entity index -> count (IfcRelVoidsElement). */
    size_t *nopen = calloc(ne, sizeof *nopen);
    if (!nopen) goto oom;
    for (size_t i = 0; i < f.n; i++) {
        const cc_step_entity *rv = &f.ents[i];
        uint64_t h, o;
        if (!is(rv, "IFCRELVOIDSELEMENT") || !cc_sv_as_ref(arg(rv, 4), &h) || !cc_sv_as_ref(arg(rv, 5), &o)) continue;
        const cc_step_entity *he = cc_step_get(&f, h);
        if (he) nopen[cc_step_index(&f, he)]++;
    }
    /* Their ids, per host in id order (read.rs voids). */
    open_at = calloc(ne + 1, sizeof *open_at);
    if (!open_at) { free(nopen); goto oom; }
    for (size_t i = 0; i < f.n; i++) open_at[i + 1] = open_at[i] + nopen[i];
    open_ids = malloc((open_at[f.n] ? open_at[f.n] : 1) * sizeof *open_ids);
    size_t *fill = calloc(ne, sizeof *fill);
    if (!open_ids || !fill) { free(fill); free(nopen); goto oom; }
    for (size_t i = 0; i < f.n; i++) {
        const cc_step_entity *rv = &f.ents[i];
        uint64_t h, o;
        if (!is(rv, "IFCRELVOIDSELEMENT") || !cc_sv_as_ref(arg(rv, 4), &h) || !cc_sv_as_ref(arg(rv, 5), &o)) continue;
        const cc_step_entity *he = cc_step_get(&f, h);
        if (!he) continue;
        size_t hi = cc_step_index(&f, he);
        uint64_t *v = open_ids + open_at[hi];
        size_t j = fill[hi]++;
        while (j && v[j - 1] > o) v[j] = v[j - 1], j--;
        v[j] = o;
    }
    free(fill);

    for (size_t i = 0; i < f.n; i++) { /* HashMap inserts: a later relation overwrites */
        const cc_step_entity *rl = &f.ents[i];
        int agg = is(rl, "IFCRELAGGREGATES");
        if (!agg && !is(rl, "IFCRELCONTAINEDINSPATIALSTRUCTURE")) continue;
        uint64_t to;
        const cc_sv *l;
        size_t nl;
        if (!cc_sv_as_ref(arg(rl, agg ? 4 : 5), &to) || !cc_sv_as_list(arg(rl, agg ? 5 : 4), &l, &nl)) continue;
        for (size_t j = 0; j < nl; j++) {
            const cc_step_entity *ce = get(&r, &l[j]);
            if (!ce) continue;
            Cont *c = &cont[cc_step_index(&f, ce)];
            if (agg) c->parent = to, c->has_parent = 1;
            else c->direct = to, c->has = 1;
        }
    }
    const cc_step_entity *proj = first_of(&f, "IFCPROJECT");
    if (!proj || !gid_from_sv(arg(proj, 0), &m->project_id)) m->project_id = derived("project/imported", 16);
    m->project_name = astr(&m->arena, "Imported model", 14);
    /* Georeference (read.rs Reader::georef, ADR 0045): the first IfcBuilding's frame turns the
     * plans and, in LV95's range, names the survey system; an IfcMapConversion gives the survey
     * offset, the map's turn and the system by its target's name (metres: other map units are
     * not read, as Rust). */
    {
        double at[3] = {building.t[0] * scale, building.t[1] * scale, building.t[2] * scale}, xa[2] = {building.x[0], building.x[1]};
        cc_georef_from_frame(&m->georef, xa, at);
        const cc_step_entity *mc = first_of(&f, "IFCMAPCONVERSION");
        if (mc) {
            cc_georef *g = &m->georef;
            double v;
            g->east_m = cc_sv_as_f64(arg(mc, 2), &v) ? v : 0.0;
            g->north_m = cc_sv_as_f64(arg(mc, 3), &v) ? v : 0.0;
            g->height_m = cc_sv_as_f64(arg(mc, 4), &v) ? v : 0.0;
            double ax = cc_sv_as_f64(arg(mc, 5), &v) ? v : 1.0, o = cc_sv_as_f64(arg(mc, 6), &v) ? v : 0.0;
            g->map_rotation_deg = round_rs(cc_atan2(o, ax) * (180.0 / 3.14159265358979323846) * 1e6) / 1e6 + 0.0;
            const cc_step_entity *crs = get(&r, arg(mc, 1));
            const char *cs;
            size_t csn;
            g->has_crs = 0, g->crs.p = NULL, g->crs.n = 0;
            if (crs && cc_sv_as_str(arg(crs, 0), &cs, &csn)) {
                g->has_crs = 1, g->crs = astr(&m->arena, cs, csn);
                if (g->crs.n != csn) goto oom;
            }
        }
        /* A file's values may be anything: what the model cannot hold is dropped, not refused. */
        if (cc_georef_validate(&m->georef, NULL) < 0) memset(&m->georef, 0, sizeof m->georef);
    }

    seen.cap = 64;
    while (seen.cap < 2 * f.n) seen.cap *= 2;
    seen.v = malloc(seen.cap * sizeof *seen.v);
    seen.used = calloc(seen.cap, 1);
    els = malloc(ne * sizeof *els);
    opening_seen.cap = seen.cap;
    opening_seen.v = malloc(seen.cap * sizeof *opening_seen.v);
    opening_seen.used = calloc(seen.cap, 1);
    shapes.state = calloc(ne, 1);
    shapes.mesh = calloc(ne, sizeof *shapes.mesh);
    if (!seen.v || !seen.used || !els || !opening_seen.v || !opening_seen.used || !shapes.state || !shapes.mesh) {
        free(nopen);
        goto oom;
    }
    {
        /* Limits::for_text: 10 triangles per displayed byte (at least 1 M), visits / 10 (5 M). */
        uint64_t db = displayed_bytes(text, n);
        uint64_t mt = db > UINT64_MAX / 10 ? UINT64_MAX : db * 10;
        budget.triangles_left = mt < 1000000 ? 1000000 : mt > (uint64_t)(size_t)-1 ? (size_t)-1 : (size_t)mt;
        budget.visits_left = db / 10 < 5000000 ? 5000000 : (size_t)(db / 10);
    }

    for (size_t i = 0; i < f.n; i++) {
        const cc_step_entity *e = &f.ents[i];
        cc_ifc_class cls;
        int has_ent = 0;
        Buf ent;
        buf_init(&ent);
        /* WAVE1B (ADR 0075): an IfcVirtualElement with ObjectType REFERENCE_PLANE is a reference plane;
         * other virtual elements (space boundaries' helpers) are skipped as before. */
        int refplane = 0;
        if (!strcmp(e->name, "IFCVIRTUALELEMENT")) {
            const char *ot;
            size_t otn;
            if (cc_sv_as_str(arg(e, 4), &ot, &otn) && otn == 15 && !memcmp(ot, "REFERENCE_PLANE", 15)) refplane = 1;
        }
        if (refplane) cls = CC_IFC_VIRTUAL_ELEMENT;
        else if (!class_of(e->name, &cls)) {
            int skip = 0;
            for (size_t k = 0; k < sizeof SKIP_CLASSES / sizeof *SKIP_CLASSES; k++) skip |= !strcmp(e->name, SKIP_CLASSES[k]);
            const cc_step_entity *s6 = get(&r, arg(e, 6));
            /* IFC2X3 allows the supertype IfcProductRepresentation (the same attributes): taken
             * like IfcProductDefinitionShape, as the display reader does (ticket IFC3,
             * DEVIATIONS.md section 10; Rust skips these products without a word). */
            if (skip || !(is(s6, "IFCPRODUCTDEFINITIONSHAPE") || is(s6, "IFCPRODUCTREPRESENTATION"))) continue;
            cls = CC_IFC_PROXY;
            if (!strcmp(e->name, "IFCBUILDINGELEMENTPROXY")) {
                const char *t;
                size_t tn;
                if (cc_sv_as_str(arg(e, 4), &t, &tn) && tn >= 3 && !memcmp(t, "Ifc", 3)) {
                    int alpha = 1;
                    for (size_t k = 0; k < tn; k++)
                        alpha &= (t[k] >= 'a' && t[k] <= 'z') || (t[k] >= 'A' && t[k] <= 'Z');
                    if (alpha) has_ent = 1, buf_append(&ent, t, tn);
                }
            } else {
                has_ent = 1;
                class_name(e->name, &ent);
            }
        }
        const char *cname = cc_ifc_class_name(cls);
        const char *nm = "";
        size_t nn = 0;
        cc_sv_as_str(arg(e, 2), &nm, &nn);
        cc_gid gid;
        if (!gid_from_sv(arg(e, 0), &gid)) {
            buf_clear(&b);
            buf_puts(&b, cname);
            buf_puts(&b, ": invalid GlobalId");
            tally_buf(&skipped, &b, 0);
            buf_free(&ent);
            continue;
        }
        if (gset_has(&seen, &gid)) {
            buf_clear(&b);
            buf_puts(&b, cname);
            buf_puts(&b, ": duplicate GlobalId");
            tally_buf(&skipped, &b, 0);
            buf_clear(&b);
            buf_puts(&b, cname);
            buf_putc(&b, ' ');
            cc_buf_debug_str(&b, nm, nn);
            buf_puts(&b, ": GlobalId ");
            cc_buf_gid(&b, &gid);
            buf_puts(&b, " already used; skipped");
            tally_buf(&warnings, &b, 1);
            buf_free(&ent);
            continue;
        }
        /* Storey: containment, then up to 8 aggregation steps (ContainedStoreys::storey_of). */
        cc_gid storey = fallback;
        if (cont[i].has) {
            uint64_t cur = cont[i].direct;
            for (int step = 0; step <= 8; step++) {
                if (step) {
                    const cc_step_entity *ce = cc_step_get(&f, cur);
                    if (!ce || !cont[cc_step_index(&f, ce)].has_parent) break;
                    cur = cont[cc_step_index(&f, ce)].parent;
                }
                const cc_step_entity *ce = cc_step_get(&f, cur);
                size_t k;
                for (k = 0; ce && k < nst; k++)
                    if (st_ent[k] == cc_step_index(&f, ce)) break;
                if (ce && k < nst) { storey = st_gid[k]; break; }
            }
        }
        double selev = 0.0;
        for (size_t k = 0; k < m->nstoreys; k++)
            if (!memcmp(m->storeys[k].id.c, storey.c, 22)) selev = m->storeys[k].elevation;
        Aff rel = IDENT;
        if (cc_sv_as_ref(arg(e, 5), &plid)) rel = object_placement(&r, plid, 0);
        rel.t[2] -= selev / scale;

        Pending *pe = &els[nels];
        memset(pe, 0, sizeof *pe);
        if (refplane) { /* its frame from the placement (unit axes, x made perpendicular), no shape */
            double zz[3], xx[3];
            if (!unit3(rel.z, zz) || !unit3(rel.x, xx)) {
                tally_add(&skipped, "IfcVirtualElement: a reference plane without a usable placement", 61, 0);
                buf_free(&ent);
                continue;
            }
            double d0 = dot(xx, zz);
            for (int k = 0; k < 3; k++) xx[k] -= d0 * zz[k];
            if (!unit3(xx, xx)) {
                tally_add(&skipped, "IfcVirtualElement: a reference plane without a usable placement", 61, 0);
                buf_free(&ent);
                continue;
            }
            for (int k = 0; k < 3; k++)
                pe->el.placement.origin[k] = rel.t[k] * scale + 0.0, pe->el.placement.z_axis[k] = zz[k] + 0.0,
                pe->el.placement.x_axis[k] = xx[k] + 0.0;
            pe->el.generator.kind = CC_GEN_NONE;
            goto refplane_done;
        }
        const cc_step_entity *solid = extruded_solid(&r, e);
        if (solid && !only_item(&r, e, solid)) solid = NULL;
        int kind = solid ? extruded_element(&r, &m->arena, solid, &rel, cls, scale, &pe->el.placement, &pe->el.generator) : 0;
        int mc = 0;
        if (kind == 1) rep->parametric++;
        else if (kind == 2) rep->extruded++;
        else if ((mc = mapped_copy(&r, &m->arena, e, nopen[i] > 0, &rel, scale, &budget, &shapes, &pe->el.placement,
                                   &pe->el.generator)) != 0) {
            if (mc < 0) { buf_free(&ent); goto oom_loop; }
            rep->meshes++;
        } else {
            Tris mb = {0};
            Emit em = {&mb, scale, &budget};
            product_body(&r, e, &em);
            if (mb.oom) { tris_free(&mb); buf_free(&ent); goto oom_loop; }
            if (!mb.n) {
                tris_free(&mb);
                /* Why: no shape, no 3D body, or a body not read. */
                const cc_step_entity *sh = get(&r, arg(e, 6));
                const cc_sv *reps;
                size_t nr = 0;
                if (!sh || !cc_sv_as_list(arg(sh, 2), &reps, &nr)) nr = 0;
                Buf ids;
                buf_init(&ids);
                size_t nids = 0;
                int body = 0;
                for (size_t k = 0; k < nr; k++) {
                    const cc_step_entity *rp = get(&r, &reps[k]);
                    const char *id;
                    size_t idn;
                    if (!rp || !cc_sv_as_str(arg(rp, 1), &id, &idn)) continue;
                    if (nids++) buf_puts(&ids, ", ");
                    buf_append(&ids, id, idn);
                    body |= idn == 4 && !memcmp(id, "Body", 4);
                }
                buf_clear(&b);
                if (has_ent) buf_append(&b, ent.p, ent.len);
                else buf_puts(&b, cname);
                if (!nids) buf_puts(&b, ": no shape in the file");
                else if (!body) buf_puts(&b, ": no 3D body, only "), buf_append(&b, ids.p, ids.len);
                else buf_puts(&b, ": a body Bimble can't read yet");
                tally_buf(&skipped, &b, 0);
                buf_free(&ids);
                buf_free(&ent);
                continue;
            }
            if (mesh_from_tris(&m->arena, &mb, selev, &pe->el.generator)) { tris_free(&mb); buf_free(&ent); goto oom_loop; }
            tris_free(&mb);
            double(*ps)[3] = pe->el.generator.u.mesh.positions;
            double o[3] = {round_rs(ps[0][0]), round_rs(ps[0][1]), round_rs(ps[0][2])};
            for (size_t k = 0; k < pe->el.generator.u.mesh.npositions; k++)
                for (int c = 0; c < 3; c++) ps[k][c] = ps[k][c] - o[c];
            placement_at(&pe->el.placement, o[0], o[1], o[2]);
            rep->meshes++;
        }
        if (nopen[i]) {
            /* element_openings: each opening's body in world mm, into element-local mm. */
            cc_opening *ov = arena_alloc(&m->arena, nopen[i] * sizeof *ov, 8);
            if (!ov) { buf_free(&ent); goto oom_loop; }
            size_t no = 0;
            const cc_placement *hp = &pe->el.placement;
            Aff fr;
            memcpy(fr.x, hp->x_axis, sizeof fr.x);
            cross(hp->z_axis, hp->x_axis, fr.y);
            memcpy(fr.z, hp->z_axis, sizeof fr.z);
            memcpy(fr.t, hp->origin, sizeof fr.t);
            for (size_t k = 0; k < nopen[i]; k++) {
                const cc_step_entity *op = cc_step_get(&f, open_ids[open_at[i] + k]);
                if (!op) continue;
                { /* WAVE1B (B3): a box or a cylinder comes back parametric */
                    cc_opening *po = &ov[no];
                    int pr = parametric_opening(&r, &m->arena, op, &fr, selev, scale, po);
                    if (pr < 0) { buf_free(&ent); goto oom_loop; }
                    if (pr) {
                        cc_gid og;
                        if (gid_from_sv(arg(op, 0), &og) && gset_insert(&opening_seen, &og)) po->has_id = 1, po->id = og;
                        no++;
                        continue;
                    }
                }
                Tris mb = {0};
                Emit em = {&mb, scale, &budget};
                product_body(&r, op, &em);
                if (mb.oom) { tris_free(&mb); buf_free(&ent); goto oom_loop; }
                if (!mb.n) {
                    tris_free(&mb);
                    buf_clear(&b);
                    buf_puts(&b, cname);
                    buf_putc(&b, ' ');
                    buf_append(&b, nm, nn);
                    buf_puts(&b, ": opening without usable geometry");
                    tally_buf(&skipped, &b, 0);
                    continue;
                }
                for (size_t t = 0; t < mb.n; t++)
                    for (int c = 0; c < 3; c++) {
                        double *p = &mb.t[t][c * 3], q[3] = {p[0], p[1], p[2] - selev};
                        aff_local(&fr, q, p);
                    }
                cc_opening *o = &ov[no];
                memset(o, 0, sizeof *o);
                if (mesh_from_tris(&m->arena, &mb, 0.0, &o->generator)) { tris_free(&mb); buf_free(&ent); goto oom_loop; }
                tris_free(&mb);
                placement_at(&o->placement, 0.0, 0.0, 0.0);
                cc_gid og;
                if (gid_from_sv(arg(op, 0), &og)) {
                    if (!gset_insert(&opening_seen, &og)) {
                        buf_clear(&b);
                        buf_puts(&b, cname);
                        buf_putc(&b, ' ');
                        buf_append(&b, nm, nn);
                        buf_puts(&b, ": opening GlobalId ");
                        cc_buf_gid(&b, &og);
                        buf_puts(&b, " already used; a new one is derived on export");
                        tally_buf(&warnings, &b, 1);
                    } else o->has_id = 1, o->id = og;
                }
                no++;
            }
            pe->el.openings = no ? ov : NULL;
            pe->el.nopenings = no;
            rep->openings += no;
        }
    refplane_done:
        gset_insert(&seen, &gid);
        pe->el.id = gid;
        pe->el.cls = cls;
        pe->el.name = astr(&m->arena, nm, nn);
        pe->el.storey = storey;
        pe->ent = i;
        if (has_ent) pe->el.has_ifc_entity = 1, pe->el.ifc_entity = astr(&m->arena, ent.p ? ent.p : "", ent.len);
        /* Properties: sorted by (host, key, seq); the last of equal keys wins. */
        {
            size_t lo = 0, hi = props.n;
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (props.v[mid].host < i) lo = mid + 1;
                else hi = mid;
            }
            size_t cnt = 0;
            for (size_t k = lo; k < props.n && props.v[k].host == i; k++)
                if (k + 1 >= props.n || props.v[k + 1].host != i || props.v[k + 1].nk != props.v[k].nk ||
                    memcmp(props.v[k + 1].key, props.v[k].key, props.v[k].nk))
                    cnt++;
            if (cnt) {
                pe->el.properties = arena_alloc(&m->arena, cnt * sizeof(cc_kv), 8);
                if (!pe->el.properties) { buf_free(&ent); free(nopen); goto oom; }
                size_t w = 0;
                for (size_t k = lo; k < props.n && props.v[k].host == i; k++)
                    if (k + 1 >= props.n || props.v[k + 1].host != i || props.v[k + 1].nk != props.v[k].nk ||
                        memcmp(props.v[k + 1].key, props.v[k].key, props.v[k].nk)) {
                        pe->el.properties[w].key = astr(&m->arena, props.v[k].key, props.v[k].nk);
                        pe->el.properties[w].value = astr(&m->arena, props.v[k].val, props.v[k].nv);
                        w++;
                    }
                pe->el.nproperties = cnt;
                rep->properties += cnt;
            }
        }
        /* ---- WAVE1B (B3, ADR 0077 decision 18): the entity's PredefinedType STRIP_FOOTING (IfcFooting)
         * or BASESLAB (IfcSlab) kept as the property Bimble/PredefinedType when the file has none;
         * a strip footing read as a pad (the rectangle extruded) becomes the wall generator along
         * its long side, as footing_add makes it. ---- */
        if (cls == CC_IFC_FOOTING || cls == CC_IFC_SLAB) {
            const char *pt;
            size_t ptn;
            const char *want = cls == CC_IFC_FOOTING ? "STRIP_FOOTING" : "BASESLAB";
            size_t wn = strlen(want);
            const cc_sv *pa = arg(e, 8);
            if (pa && cc_sv_as_enum(pa, &pt) && (ptn = pa->u.s.n) == wn && !memcmp(pt, want, wn) && !cc_element_predefined(&pe->el)) {
                size_t np = pe->el.nproperties, kn = strlen(CC_PREDEFINED_KEY), at = 0;
                cc_kv *nv = arena_alloc(&m->arena, (np + 1) * sizeof *nv, 8);
                if (!nv) { buf_free(&ent); free(nopen); goto oom; }
                while (at < np) { /* sorted by key bytes */
                    cc_str k = pe->el.properties[at].key;
                    size_t mn = k.n < kn ? k.n : kn;
                    int c = memcmp(k.p, CC_PREDEFINED_KEY, mn);
                    if (c > 0 || (c == 0 && k.n > kn)) break;
                    at++;
                }
                if (at) memcpy(nv, pe->el.properties, at * sizeof *nv);
                nv[at].key = astr(&m->arena, CC_PREDEFINED_KEY, kn), nv[at].value = astr(&m->arena, want, wn);
                if (np > at) memcpy(nv + at + 1, pe->el.properties + at, (np - at) * sizeof *nv);
                pe->el.properties = nv, pe->el.nproperties = np + 1;
            }
            const cc_str *pv = cc_element_predefined(&pe->el);
            cc_generator *g = &pe->el.generator;
            if (cls == CC_IFC_FOOTING && pv && pv->n == 13 && !memcmp(pv->p, "STRIP_FOOTING", 13) && g->kind == CC_GEN_PAD_FOOTING) {
                double w = g->u.col.width, d = g->u.col.depth, h = g->u.col.height;
                cc_placement *pl = &pe->el.placement;
                double y[3];
                cross(pl->z_axis, pl->x_axis, y);
                double len = w, th = d, *ax = pl->x_axis;
                if (d > w) { /* the long side along y: x turns to it */
                    len = d, th = w;
                    memcpy(pl->x_axis, y, sizeof y);
                }
                for (int k = 0; k < 3; k++) pl->origin[k] = pl->origin[k] - ax[k] * len / 2.0 + 0.0;
                g->kind = CC_GEN_WALL;
                g->u.wall.length = len, g->u.wall.thickness = th, g->u.wall.height = h;
            }
        }
        /* Material: name -> derived id, category from the grade (first one wins). */
        const ElMat *mi = elmat_get(&em, i);
        if (refplane && !(mi && mi->set) && nmats) { /* WAVE1B: a plane is of no material; the element needs one */
            pe->el.material = mats[0].id;
            buf_clear(&b);
            buf_puts(&b, cname);
            tally_buf(&by_class, &b, 0);
            rep->elements++;
            nels++;
            buf_free(&ent);
            continue;
        }
        const char *mn = mi && mi->set && mi->n ? mi->name : "Undefined";
        size_t mnn = mi && mi->set && mi->n ? mi->n : 9;
        cc_category cat = mi && mi->set ? mi->cat : CC_CAT_OTHER;
        buf_clear(&b);
        buf_puts(&b, "material/");
        buf_append(&b, mn, mnn);
        cc_gid mid = derived(b.p, b.len);
        size_t k;
        for (k = 0; k < nmats; k++)
            if (!memcmp(mats[k].id.c, mid.c, 22)) break;
        if (k == nmats) {
            cc_material *v = realloc(mats, (nmats + 1) * sizeof *v);
            if (!v) { buf_free(&ent); free(nopen); goto oom; }
            mats = v;
            memset(&mats[nmats], 0, sizeof *mats);
            mats[nmats].id = mid;
            mats[nmats].name = astr(&m->arena, mn, mnn);
            mats[nmats].category = cat;
            nmats++;
        }
        pe->el.material = mid;
        buf_clear(&b);
        if (has_ent) buf_append(&b, ent.p, ent.len);
        else buf_puts(&b, cname);
        tally_buf(&by_class, &b, 0);
        rep->elements++;
        nels++;
        buf_free(&ent);
    }
    free(nopen);
    nopen = NULL;
    if (0) {
    oom_loop:
        free(nopen);
        goto oom;
    }
    if (budget.exhausted)
        tally_add(&warnings,
                  "model too large or nested too deeply: geometry stopped at the import limits; what was left is "
                  "skipped",
                  strlen("model too large or nested too deeply: geometry stopped at the import limits; what was "
                         "left is skipped"),
                  1);
    if (r.unsup_n) {
        buf_clear(&b);
        buf_puts(&b, "boolean result drawn uncut: no boolean kernel in the C core (Rust cuts it)");
        for (size_t k = 0; k < r.unsup_n; k++) tally_buf(&r.unsup, &b, 0);
        rep->unsupported += r.unsup_n;
    }

    tally_sort(&r.warnings);
    for (size_t i = 0; i < r.warnings.n; i++) {
        buf_clear(&b);
        buf_append(&b, r.warnings.v[i].s, r.warnings.v[i].n);
        if (r.warnings.v[i].count > 1) buf_puts(&b, " (\xc3\x97"), buf_u64(&b, r.warnings.v[i].count), buf_putc(&b, ')');
        tally_buf(&warnings, &b, 1);
    }

    /* Report lines, as ImportReport::lines. */
    {
        Buf *L = &rep->lines;
        buf_u64(L, rep->elements);
        buf_puts(L, " elements (");
        buf_u64(L, rep->parametric);
        buf_puts(L, " parametric, ");
        buf_u64(L, rep->extruded);
        buf_puts(L, " extruded polygons, ");
        buf_u64(L, rep->meshes);
        buf_puts(L, " meshes)\n");
        tally_sort(&by_class);
        for (size_t i = 0; i < by_class.n; i++) {
            buf_puts(L, "  ");
            buf_append(L, by_class.v[i].s, by_class.v[i].n);
            buf_puts(L, ": ");
            buf_u64(L, by_class.v[i].count);
            buf_putc(L, '\n');
        }
        if (rep->openings) {
            buf_puts(L, "openings: ");
            buf_u64(L, rep->openings);
            buf_puts(L, " cut on import (as meshes, subtracted at draw time)\n");
        }
        if (rep->properties) {
            buf_puts(L, "property values: ");
            buf_u64(L, rep->properties);
            buf_putc(L, '\n');
        }
        tally_sort(&skipped);
        for (size_t i = 0; i < skipped.n; i++) {
            buf_puts(L, "skipped: ");
            buf_append(L, skipped.v[i].s, skipped.v[i].n);
            buf_puts(L, " (\xc3\x97");
            buf_u64(L, skipped.v[i].count);
            buf_puts(L, ")\n");
        }
        for (size_t i = 0; i < warnings.n; i++) {
            buf_puts(L, "warning: ");
            buf_append(L, warnings.v[i].s, warnings.v[i].n);
            buf_putc(L, '\n');
        }
        tally_sort(&r.unsup);
        for (size_t i = 0; i < r.unsup.n; i++) {
            buf_puts(L, "unsupported: ");
            buf_append(L, r.unsup.v[i].s, r.unsup.v[i].n);
            buf_puts(L, " (\xc3\x97");
            buf_u64(L, r.unsup.v[i].count);
            buf_puts(L, ")\n");
        }
    }

    if (!rep->elements) {
        set_err(err, "", "no element with geometry found");
        goto done;
    }
    /* Into the model: arrays sorted by id, in the arena. */
    {
        cc_element *ev = arena_alloc(&m->arena, nels * sizeof *ev, 8);
        if (!ev) goto oom;
        for (size_t i = 0; i < nels; i++) ev[i] = els[i].el;
        if (sort_by_gid(ev, nels, sizeof *ev) || sort_by_gid(mats, nmats, sizeof *mats)) goto oom;
        m->elements = ev, m->nelements = nels, m->cap_elements = 0;
        cc_material *mv = ii_dup8(&m->arena, mats, nmats * sizeof *mats);
        cc_storey *sv = ii_dup8(&m->arena, m->storeys, m->nstoreys * sizeof *sv);
        if (!mv || !sv) goto oom;
        free(m->storeys);
        m->storeys = sv;
        storeys_in_arena = 1;
        m->materials = mv, m->nmaterials = nmats;
    }
    if (cc_model_validate(m, err)) {
        char msg[sizeof err->msg];
        memcpy(msg, err->msg, sizeof msg);
        set_err(err, "imported model invalid: ", msg);
        goto done;
    }
    cc_model_canonical(m);
    rc = 0;
    goto done;
oom:
    set_err(err, "", "out of memory");
done:
    if (!storeys_in_arena) free(m->storeys);
    if (rc) { /* an empty model on error */
        m->storeys = NULL, m->nstoreys = 0;
        m->elements = NULL, m->nelements = 0, m->cap_elements = 0;
        m->materials = NULL, m->nmaterials = 0;
        memset(&m->georef, 0, sizeof m->georef);
    }
    elmats_free(&em);
    for (size_t i = 0; i < props.n; i++) free(props.v[i].key), free(props.v[i].val);
    free(props.v);
    free(els);
    free(mats);
    free(st_ent);
    free(cont);
    free(st_gid);
    free(seen.v);
    free(seen.used);
    free(opening_seen.v);
    free(opening_seen.used);
    free(shapes.state);
    free(shapes.mesh);
    free(open_at);
    free(open_ids);
    free(r.pl);
    tally_free(&r.warnings);
    tally_free(&r.unsup);
    tally_free(&skipped);
    tally_free(&warnings);
    tally_free(&by_class);
    buf_free(&r.tmp);
    buf_free(&b);
    cc_step_free(&f);
    return rc;
}

void cc_ifc_report_free(cc_ifc_report *rep) {
    buf_free(&rep->lines);
    memset(rep, 0, sizeof *rep);
}
