/* Shared reader code of the IFC import (ifc_in.c) and the display reader (ifc_read.c): units,
 * placements, curves, profiles, representation items into world triangles (read.rs Reader and
 * Emit), materials, displayed_bytes, class names. Moved out of ifc_in.c unchanged (plus the
 * Budget element_max of read.rs). Internal: static definitions, included by those two files only
 * (each gets its own copy). */
#ifndef CCORE_IFC_GEOM_H
#define CCORE_IFC_GEOM_H
#include <stdlib.h>
#include <string.h>
#include "boolean.h"
#include "buf.h"
#include "earcut.h"
#include "ifc_in.h"
#include "ifc_math.h"
#include "scene.h"
#include "step.h"
#include "verbs.h"
#include "jobs.h"

/* The freestanding wasm32 shim has no strcmp, strncmp, memchr or calloc: local ones. */
static int s_cmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
static int s_ncmp(const char *a, const char *b, size_t n) {
    for (; n && *a && *a == *b; n--) a++, b++;
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
static const void *m_chr(const void *p, int c, size_t n) {
    const unsigned char *s = p;
    for (size_t i = 0; i < n; i++)
        if (s[i] == (unsigned char)c) return s + i;
    return NULL;
}
static void *zalloc(size_t n, size_t sz) {
    if (sz && n > (size_t)-1 / sz) return NULL;
    void *p = malloc(n * sz ? n * sz : 1);
    if (p) memset(p, 0, n * sz);
    return p;
}
#ifdef __wasm__
#define strcmp s_cmp
#define strncmp s_ncmp
#define calloc zalloc
#define memchr m_chr
#endif

#ifdef __clang__
#pragma clang fp contract(off)
#endif

/* arena_dup aligned to 8 (arena_dup is byte-aligned). */
static void *ii_dup8(Arena *a, const void *src, size_t n) {
    void *d = arena_alloc(a, n ? n : 1, 8);
    if (d && n) memcpy(d, src, n);
    return d;
}

typedef double V3[3];
typedef struct {
    V3 x, y, z, t;
} Aff;
static const Aff IDENT = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}};

static double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross(const double *a, const double *b, double *o) {
    double r[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    memcpy(o, r, sizeof r);
}
static int finite(double x) { return x == x && x - x == 0; }
static int normalize(const double *v, double *o) {
    double l = cc_sqrt(dot(v, v));
    if (!(l > 1e-12 && finite(l))) return 0;
    for (int k = 0; k < 3; k++) o[k] = v[k] / l;
    return 1;
}
static void aff_apply(const Aff *a, const double *p, double *o) {
    double r[3];
    for (int k = 0; k < 3; k++) r[k] = a->x[k] * p[0] + a->y[k] * p[1] + a->z[k] * p[2] + a->t[k];
    memcpy(o, r, sizeof r);
}
static void aff_rotate(const Aff *a, const double *v, double *o) {
    double r[3];
    for (int k = 0; k < 3; k++) r[k] = a->x[k] * v[0] + a->y[k] * v[1] + a->z[k] * v[2];
    memcpy(o, r, sizeof r);
}
static Aff aff_then(const Aff *a, const Aff *b) {
    Aff r;
    aff_rotate(a, b->x, r.x);
    aff_rotate(a, b->y, r.y);
    aff_rotate(a, b->z, r.z);
    aff_apply(a, b->t, r.t);
    return r;
}

/* ------------------------------------------------------------------ report tallies */

typedef struct {
    char *s;
    size_t n, count;
} Tally;
typedef struct {
    Tally *v;
    size_t n, cap;
} Tallies;
#define MAX_TALLY 4096 /* distinct lines kept; past it, counted under one line */

static void tally_add(Tallies *t, const char *s, size_t n, int list) {
    if (!list)
        for (size_t i = 0; i < t->n; i++)
            if (t->v[i].n == n && memcmp(t->v[i].s, s, n) == 0) { t->v[i].count++; return; }
    if (t->n >= MAX_TALLY) {
        s = "(more lines not shown)", n = strlen(s);
        for (size_t i = 0; i < t->n; i++)
            if (t->v[i].n == n && memcmp(t->v[i].s, s, n) == 0) { t->v[i].count++; return; }
    }
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 16;
        Tally *v = realloc(t->v, cap * sizeof *v);
        if (!v) return;
        t->v = v, t->cap = cap;
    }
    char *d = malloc(n + 1);
    if (!d) return;
    memcpy(d, s, n);
    d[n] = 0;
    t->v[t->n].s = d, t->v[t->n].n = n, t->v[t->n].count = 1;
    t->n++;
}
static void tally_buf(Tallies *t, Buf *b, int list) { tally_add(t, b->p ? b->p : "", b->len, list); }
static void tally_free(Tallies *t) {
    for (size_t i = 0; i < t->n; i++) free(t->v[i].s);
    free(t->v);
    memset(t, 0, sizeof *t);
}
static int tally_cmp(const Tally *a, const Tally *b) {
    size_t k = a->n < b->n ? a->n : b->n;
    int c = memcmp(a->s, b->s, k);
    return c ? c : (a->n > b->n) - (a->n < b->n);
}
static void tally_sort(Tallies *t) { /* insertion sort: small, bounded by MAX_TALLY */
    for (size_t i = 1; i < t->n; i++) {
        Tally x = t->v[i];
        size_t j = i;
        while (j && tally_cmp(&t->v[j - 1], &x) > 0) t->v[j] = t->v[j - 1], j--;
        t->v[j] = x;
    }
}

/* ------------------------------------------------------------------ reader */

#define MAX_NESTING 32

/* Placement cache: open addressing on the entity index (key ix + 1; 0 empty). */
typedef struct {
    size_t key;
    Aff a;
} PlSlot;

typedef struct {
    const cc_step_file *f;
    PlSlot *pl; /* placement cache (malloc; NULL until the first placement) */
    size_t plcap, pln;
    int pl_no_warned; /* 1: a placement whose chain warned is not cached (its users all warn:
                       * the count does not depend on which thread met it first) */
    Tallies warnings; /* Reader warnings (BTreeMap with counts) */
    Tallies unsup;    /* outside the C subset */
    double arc_tol;     /* arc tessellation tolerance, file units */
    double angle_scale; /* plane angle unit -> radians */
    uint64_t stack[MAX_NESTING + 8]; /* representation maps being expanded */
    size_t nstack;
    size_t unsup_n; /* booleans drawn uncut (no kernel) */
    Buf tmp;
} R;

static const cc_step_entity *get(R *r, const cc_sv *v) { return cc_step_deref(r->f, v); }
static const cc_sv *arg(const cc_step_entity *e, size_t i) { return cc_step_arg(e, i); }
static int is(const cc_step_entity *e, const char *name) { return e && strcmp(e->name, name) == 0; }
static int enum_is(const cc_sv *v, const char *s) {
    const char *p;
    return cc_sv_as_enum(v, &p) && strcmp(p, s) == 0;
}
static void warn(R *r, const char *a, const char *b, const char *c) {
    buf_clear(&r->tmp);
    buf_puts(&r->tmp, a);
    if (b) buf_puts(&r->tmp, b);
    if (c) buf_puts(&r->tmp, c);
    tally_buf(&r->warnings, &r->tmp, 0);
}

/* First entity of a type in id order (StepFile::by_type(..).next() on a dense file). */
static const cc_step_entity *first_of(const cc_step_file *f, const char *name) {
    for (size_t i = 0; i < f->n; i++)
        if (strcmp(f->ents[i].name, name) == 0) return &f->ents[i];
    return NULL;
}

static int length_unit_mm(R *r, const cc_step_entity *u, int depth, double *out) {
    if (depth > 4) return 0;
    if (is(u, "IFCSIUNIT") && enum_is(arg(u, 1), "LENGTHUNIT")) {
        const char *p = "";
        cc_sv_as_enum(arg(u, 2), &p);
        double pre = 1.0;
        if (!strcmp(p, "KILO")) pre = 1e3;
        else if (!strcmp(p, "HECTO")) pre = 1e2;
        else if (!strcmp(p, "DECA")) pre = 1e1;
        else if (!strcmp(p, "DECI")) pre = 1e-1;
        else if (!strcmp(p, "CENTI")) pre = 1e-2;
        else if (!strcmp(p, "MILLI")) pre = 1e-3;
        else if (!strcmp(p, "MICRO")) pre = 1e-6;
        *out = 1000.0 * pre;
        return 1;
    }
    if ((is(u, "IFCCONVERSIONBASEDUNIT") || is(u, "IFCCONVERSIONBASEDUNITWITHOFFSET")) &&
        enum_is(arg(u, 1), "LENGTHUNIT")) {
        const cc_step_entity *m = get(r, arg(u, 3));
        double factor, base;
        if (!m || !cc_sv_as_f64(arg(m, 0), &factor)) return 0;
        const cc_step_entity *b = get(r, arg(m, 1));
        if (!b || !length_unit_mm(r, b, depth + 1, &base)) return 0;
        *out = factor * base;
        return 1;
    }
    return 0;
}

static double length_scale_to_mm(R *r) {
    const cc_step_entity *p = first_of(r->f, "IFCPROJECT");
    const cc_step_entity *a = p ? get(r, arg(p, 8)) : NULL;
    const cc_sv *us;
    size_t n = 0;
    if (!a || !cc_sv_as_list(arg(a, 0), &us, &n)) n = 0;
    for (size_t i = 0; i < n; i++) {
        const cc_step_entity *u = get(r, &us[i]);
        double s;
        if (u && length_unit_mm(r, u, 0, &s)) return s;
    }
    warn(r, "no length unit in project; assuming millimetres", 0, 0);
    return 1.0;
}

static double plane_angle_to_radians(R *r) {
    const cc_step_entity *p = first_of(r->f, "IFCPROJECT");
    const cc_step_entity *a = p ? get(r, arg(p, 8)) : NULL;
    const cc_sv *us;
    size_t n = 0;
    if (!a || !cc_sv_as_list(arg(a, 0), &us, &n)) n = 0;
    for (size_t i = 0; i < n; i++) {
        const cc_step_entity *u = get(r, &us[i]);
        if (!u || !enum_is(arg(u, 1), "PLANEANGLEUNIT")) continue;
        if (is(u, "IFCSIUNIT")) return 1.0;
        if (is(u, "IFCCONVERSIONBASEDUNIT")) {
            const cc_step_entity *m = get(r, arg(u, 3));
            double f;
            if (m && cc_sv_as_f64(arg(m, 0), &f)) return f;
        }
    }
    return 1.0;
}

static int point(R *r, const cc_sv *v, double *o) {
    const cc_step_entity *e = get(r, v);
    const cc_sv *c;
    size_t n;
    if (!e || !cc_sv_as_list(arg(e, 0), &c, &n) || n < 2) return 0;
    if (!cc_sv_as_f64(&c[0], &o[0]) || !cc_sv_as_f64(&c[1], &o[1])) return 0;
    if (n < 3 || !cc_sv_as_f64(&c[2], &o[2])) o[2] = 0.0;
    return 1;
}
static int direction(R *r, const cc_sv *v, double *o) {
    double p[3];
    return point(r, v, p) && normalize(p, o);
}

static Aff axis_placement(R *r, const cc_sv *v) {
    const cc_step_entity *e = get(r, v);
    if (!e) return IDENT;
    Aff a;
    if (!point(r, arg(e, 0), a.t)) a.t[0] = a.t[1] = a.t[2] = 0;
    double zg[3], xg[3];
    int hz = 0, hx = 0;
    if (is(e, "IFCAXIS2PLACEMENT3D")) hz = direction(r, arg(e, 1), zg), hx = direction(r, arg(e, 2), xg);
    else if (is(e, "IFCAXIS2PLACEMENT2D")) hx = direction(r, arg(e, 1), xg);
    double z[3] = {0, 0, 1}, x[3];
    if (hz) memcpy(z, zg, sizeof z);
    int ok = 0;
    if (hx) {
        double d = dot(xg, z), w[3] = {xg[0] - z[0] * d, xg[1] - z[1] * d, xg[2] - z[2] * d};
        ok = normalize(w, x);
    }
    if (!ok) {
        double cand[3] = {1, 0, 0};
        if (!(1.0 - (z[0] < 0 ? -z[0] : z[0]) > 1e-9)) cand[0] = 0, cand[1] = 1;
        double d = dot(cand, z), w[3] = {cand[0] - z[0] * d, cand[1] - z[1] * d, cand[2] - z[2] * d};
        ok = normalize(w, x);
    }
    if (!ok) x[0] = 1, x[1] = 0, x[2] = 0;
    memcpy(a.z, z, sizeof z);
    memcpy(a.x, x, sizeof x);
    cross(z, x, a.y);
    return a;
}

static size_t pl_hash(size_t ix, size_t cap) { return (size_t)(((uint64_t)ix * 0x9E3779B97F4A7C15ull) >> 20) & (cap - 1); }
static const Aff *pl_get(const R *r, size_t ix) {
    if (!r->pl) return NULL;
    for (size_t k = pl_hash(ix, r->plcap);; k = (k + 1) & (r->plcap - 1)) {
        if (!r->pl[k].key) return NULL;
        if (r->pl[k].key == ix + 1) return &r->pl[k].a;
    }
}
static void pl_put(R *r, size_t ix, const Aff *a) { /* out of memory: not cached */
    if (2 * (r->pln + 1) > r->plcap) {
        size_t cap = r->plcap ? r->plcap * 2 : 1024;
        PlSlot *t = calloc(cap, sizeof *t);
        if (!t) return;
        for (size_t i = 0; i < r->plcap; i++) {
            if (!r->pl[i].key) continue;
            size_t k = pl_hash(r->pl[i].key - 1, cap);
            while (t[k].key) k = (k + 1) & (cap - 1);
            t[k] = r->pl[i];
        }
        free(r->pl);
        r->pl = t, r->plcap = cap;
    }
    size_t k = pl_hash(ix, r->plcap);
    while (r->pl[k].key && r->pl[k].key != ix + 1) k = (k + 1) & (r->plcap - 1);
    if (!r->pl[k].key) r->pln++;
    r->pl[k].key = ix + 1, r->pl[k].a = *a;
}
static void pl_clear(R *r) {
    if (r->pl) memset(r->pl, 0, r->plcap * sizeof *r->pl);
    r->pln = 0;
}

static Aff object_placement(R *r, uint64_t id, int depth) {
    const cc_step_entity *e = cc_step_get(r->f, id);
    size_t ix = e ? cc_step_index(r->f, e) : 0;
    const Aff *hit = e ? pl_get(r, ix) : NULL;
    if (hit) return *hit;
    size_t total0 = 0;
    for (size_t k = 0; r->pl_no_warned && k < r->warnings.n; k++) total0 += r->warnings.v[k].count;
    if (depth > 64) {
        warn(r, "placement chain too deep or cyclic", 0, 0);
        return IDENT;
    }
    if (!e) return IDENT;
    Aff a;
    if (is(e, "IFCLOCALPLACEMENT")) {
        Aff rel = axis_placement(r, arg(e, 1));
        uint64_t parent;
        if (cc_sv_as_ref(arg(e, 0), &parent)) {
            Aff p = object_placement(r, parent, depth + 1);
            a = aff_then(&p, &rel);
        } else a = rel;
    } else {
        warn(r, "unsupported placement ", e->name, " (used identity)");
        a = IDENT;
    }
    size_t total1 = 0;
    for (size_t k = 0; r->pl_no_warned && k < r->warnings.n; k++) total1 += r->warnings.v[k].count;
    if (!r->pl_no_warned || total1 == total0) pl_put(r, ix, &a);
    return a;
}

/* ---- growable arrays: 2D loops, 3D point lists, triangles (malloc) */

typedef struct {
    double (*p)[2];
    size_t n, cap;
} Loop;
typedef struct {
    Loop *v;
    size_t n, cap;
} Loops;
typedef struct {
    double (*p)[3];
    size_t n, cap;
} P3;
typedef struct { /* bimble_geom::MeshBuilder: world triangles (mm) */
    double (*t)[9];
    size_t n, cap;
    int oom;
} Tris;

static double signed_area(const Loop *l);
static void loop_reverse(Loop *l);

static int grow(void **p, size_t *cap, size_t need, size_t sz) {
    if (need <= *cap) return 0;
    size_t c = *cap ? *cap : 8;
    while (c < need) {
        if (c > ((size_t)-1) / 2 / sz) return -1;
        c *= 2;
    }
    void *q = realloc(*p, c * sz);
    if (!q) return -1;
    *p = q, *cap = c;
    return 0;
}
static int loop_push(Loop *l, double x, double y) {
    if (grow((void **)&l->p, &l->cap, l->n + 1, sizeof *l->p)) return -1;
    l->p[l->n][0] = x, l->p[l->n][1] = y;
    l->n++;
    return 0;
}
static int loops_push(Loops *ls, Loop l) {
    if (grow((void **)&ls->v, &ls->cap, ls->n + 1, sizeof *ls->v)) return -1;
    ls->v[ls->n++] = l;
    return 0;
}
static void loops_free(Loops *ls) {
    for (size_t i = 0; i < ls->n; i++) free(ls->v[i].p);
    free(ls->v);
    memset(ls, 0, sizeof *ls);
}
static int p3_push(P3 *l, const double *q) {
    if (grow((void **)&l->p, &l->cap, l->n + 1, sizeof *l->p)) return -1;
    memcpy(l->p[l->n++], q, 3 * sizeof(double));
    return 0;
}
static void p3_free(P3 *l) {
    free(l->p);
    memset(l, 0, sizeof *l);
}
static double dist2(const double *a, const double *b) {
    double dx = a[0] - b[0], dy = a[1] - b[1];
    return dx * dx + dy * dy;
}
static double norm3(const double *v) { return cc_sqrt(dot(v, v)); }
static double dist3(const double *a, const double *b) {
    double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return norm3(d);
}
/* Vec::dedup_by(|a, b| norm(sub(a, b)) < 1e-9): drop points within 1e-9 of the last kept. */
static void p3_dedup(P3 *l) {
    size_t w = 0;
    for (size_t i = 0; i < l->n; i++) {
        if (w && dist3(l->p[i], l->p[w - 1]) < 1e-9) continue;
        if (w != i) memcpy(l->p[w], l->p[i], sizeof l->p[0]);
        w++;
    }
    l->n = w;
}
static void p3_reverse(P3 *l) {
    for (size_t i = 0, j = l->n ? l->n - 1 : 0; i < j; i++, j--) {
        double t[3];
        memcpy(t, l->p[i], sizeof t);
        memcpy(l->p[i], l->p[j], sizeof t);
        memcpy(l->p[j], t, sizeof t);
    }
}

/* f64::min / f64::max (a NaN side is ignored). */
static double fmin_rs(double a, double b) { return a != a ? b : b != b ? a : a < b ? a : b; }
static double fmax_rs(double a, double b) { return a != a ? b : b != b ? a : a > b ? a : b; }
/* f64::rem_euclid */
static double rem_euclid(double a, double b) {
    double r = cc_fmod(a, b);
    return r < 0.0 ? r + (b < 0 ? -b : b) : r;
}
/* f64::round: half away from zero. */
static double round_rs(double x) {
    double ax = x < 0 ? -x : x;
    if (!(ax < 4503599627370496.0)) return x; /* 2^52: already an integer (or NaN, inf) */
    double t = (double)(int64_t)x;
    double d = x - t;
    if (d >= 0.5) t += 1.0;
    else if (d <= -0.5) t -= 1.0;
    if (t == 0.0) return x < 0 || (x == 0 && 1.0 / x < 0) ? -0.0 : 0.0;
    return t;
}

static const double TAU = 6.283185307179586;

/* ---- arcs */

/* arc_segments: chords within tol of a circle of radius r over sweep, at most 15° each. */
static size_t arc_segments(double r, double sweep, double tol) {
    const double MAX_STEP = 3.141592653589793 / 12.0;
    double step = tol > 0.0 && tol < r ? fmin_rs(2.0 * cc_acos(1.0 - tol / r), MAX_STEP) : MAX_STEP;
    double q = (sweep < 0 ? -sweep : sweep) / fmax_rs(step, 1e-3);
    /* .ceil() as usize, saturating; NaN -> 0; then clamp(8, 1024) */
    size_t n;
    if (!(q > 0)) n = 0;
    else if (q >= 1e18) n = (size_t)-1;
    else {
        uint64_t t = (uint64_t)q;
        n = (size_t)((double)t < q ? t + 1 : t);
    }
    return n < 8 ? 8 : n > 1024 ? 1024 : n;
}

/* arc: points from a through b to c (inclusive), within tol. Appends to out. */
static int arc2(const double *a, const double *b, const double *c, double tol, Loop *out) {
    double d = 2.0 * (a[0] * (b[1] - c[1]) + b[0] * (c[1] - a[1]) + c[0] * (a[1] - b[1]));
    if ((d < 0 ? -d : d) < 1e-12) {
        if (loop_push(out, a[0], a[1]) || loop_push(out, b[0], b[1]) || loop_push(out, c[0], c[1])) return -1;
        return 0;
    }
    double sa = a[0] * a[0] + a[1] * a[1], sb = b[0] * b[0] + b[1] * b[1], sc = c[0] * c[0] + c[1] * c[1];
    double ux = (sa * (b[1] - c[1]) + sb * (c[1] - a[1]) + sc * (a[1] - b[1])) / d;
    double uy = (sa * (c[0] - b[0]) + sb * (a[0] - c[0]) + sc * (b[0] - a[0])) / d;
    double r = cc_sqrt((a[0] - ux) * (a[0] - ux) + (a[1] - uy) * (a[1] - uy));
    double angA = cc_atan2(a[1] - uy, a[0] - ux), angB = cc_atan2(b[1] - uy, b[0] - ux),
           angC = cc_atan2(c[1] - uy, c[0] - ux);
    double a0 = angA, ab = rem_euclid(angB - angA, TAU), ac = rem_euclid(angC - angA, TAU);
    double sweep = ab < ac ? ac : ac - TAU;
    size_t n = arc_segments(r, sweep, tol);
    size_t start = out->n;
    for (size_t i = 0; i <= n; i++) {
        double t = a0 + sweep * (double)i / (double)n;
        if (loop_push(out, ux + r * cc_cos(t), uy + r * cc_sin(t))) return -1;
    }
    out->p[start][0] = a[0], out->p[start][1] = a[1];
    out->p[start + n][0] = c[0], out->p[start + n][1] = c[1];
    return 0;
}

/* arc3: the arc through three 3D points, in its own plane. Appends to out, skipping the first
 * `skip` points. */
static int arc3(const double *a, const double *b, const double *c, double tol, size_t skip, P3 *out) {
    double ba[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ca[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    double nn[3], e1[3], n[3], e2[3];
    cross(ba, ca, nn);
    if (!normalize(ba, e1) || !normalize(nn, n)) {
        const double *v[3] = {a, b, c};
        for (size_t k = skip; k < 3; k++)
            if (p3_push(out, v[k])) return -1;
        return 0;
    }
    cross(n, e1, e2);
    double q[3][2];
    const double *v[3] = {a, b, c};
    for (int k = 0; k < 3; k++) {
        double d[3] = {v[k][0] - a[0], v[k][1] - a[1], v[k][2] - a[2]};
        q[k][0] = dot(d, e1), q[k][1] = dot(d, e2);
    }
    Loop l = {0};
    if (arc2(q[0], q[1], q[2], tol, &l)) { free(l.p); return -1; }
    for (size_t i = skip; i < l.n; i++) {
        double p[3];
        for (int k = 0; k < 3; k++) p[k] = a[k] + e1[k] * l.p[i][0] + e2[k] * l.p[i][1];
        if (p3_push(out, p)) { free(l.p); return -1; }
    }
    free(l.p);
    return 0;
}

/* fillet: the given corners replaced with arcs of radius r. */
static int fillet(const double (*poly)[2], size_t n, const size_t *corners, size_t nc, double r, double tol,
                  Loop *out) {
    for (size_t i = 0; i < n; i++) {
        const double *c = poly[i];
        int is_c = 0;
        for (size_t k = 0; k < nc; k++) is_c |= corners[k] == i;
        if (!is_c) {
            if (loop_push(out, c[0], c[1])) return -1;
            continue;
        }
        const double *p = poly[(i + n - 1) % n], *q = poly[(i + 1) % n];
        double l1 = cc_sqrt((c[0] - p[0]) * (c[0] - p[0]) + (c[1] - p[1]) * (c[1] - p[1]));
        double l2 = cc_sqrt((q[0] - c[0]) * (q[0] - c[0]) + (q[1] - c[1]) * (q[1] - c[1]));
        if (r >= l1 || r >= l2 || l1 == 0.0 || l2 == 0.0) {
            if (loop_push(out, c[0], c[1])) return -1;
            continue;
        }
        double d1[2] = {(c[0] - p[0]) / l1, (c[1] - p[1]) / l1};
        double d2[2] = {(q[0] - c[0]) / l2, (q[1] - c[1]) / l2};
        double t1[2] = {c[0] - d1[0] * r, c[1] - d1[1] * r};
        double t2[2] = {c[0] + d2[0] * r, c[1] + d2[1] * r};
        double center[2] = {t1[0] + d2[0] * r, t1[1] + d2[1] * r};
        double k = cc_sqrt((c[0] - center[0]) * (c[0] - center[0]) + (c[1] - center[1]) * (c[1] - center[1]));
        double mid[2] = {center[0] + (c[0] - center[0]) / k * r, center[1] + (c[1] - center[1]) / k * r};
        if (arc2(t1, mid, t2, tol, out)) return -1;
    }
    return 0;
}

/* ---- transforms */

/* IfcCartesianTransformationOperator3D (+ NonUniform) -> Affine. */
static Aff transform_operator(R *r, const cc_sv *v) {
    const cc_step_entity *e = get(r, v);
    if (!e) return IDENT;
    double a1[3], a2[3], a3[3], t[3], s, s2, s3;
    int h1 = direction(r, arg(e, 0), a1), h2 = direction(r, arg(e, 1), a2);
    if (!point(r, arg(e, 2), t)) t[0] = t[1] = t[2] = 0;
    if (!cc_sv_as_f64(arg(e, 3), &s)) s = 1.0;
    int h3 = direction(r, arg(e, 4), a3);
    s2 = s3 = s;
    if (is(e, "IFCCARTESIANTRANSFORMATIONOPERATOR3DNONUNIFORM")) {
        if (!cc_sv_as_f64(arg(e, 5), &s2)) s2 = s;
        if (!cc_sv_as_f64(arg(e, 6), &s3)) s3 = s;
    }
    double z[3] = {0, 0, 1}, x[3], y[3];
    if (h3) memcpy(z, a3, sizeof z);
    int ok = 0;
    if (h1) {
        double d = dot(a1, z), w[3] = {a1[0] - z[0] * d, a1[1] - z[1] * d, a1[2] - z[2] * d};
        ok = normalize(w, x);
    }
    if (!ok) {
        double w[3] = {1.0 - z[0] * z[0], 0.0 - z[1] * z[0], 0.0 - z[2] * z[0]};
        ok = normalize(w, x);
    }
    if (!ok) x[0] = 0, x[1] = 1, x[2] = 0;
    double yy[3] = {0, 1, 0};
    const double *yv = h2 ? a2 : yy;
    {
        double dz = dot(yv, z), dx = dot(yv, x);
        double w1[3] = {yv[0] - z[0] * dz, yv[1] - z[1] * dz, yv[2] - z[2] * dz};
        double w[3] = {w1[0] - x[0] * dx, w1[1] - x[1] * dx, w1[2] - x[2] * dx};
        if (!normalize(w, y)) cross(z, x, y);
    }
    Aff a;
    for (int k = 0; k < 3; k++) a.x[k] = x[k] * s, a.y[k] = y[k] * s2, a.z[k] = z[k] * s3, a.t[k] = t[k];
    return a;
}

static Aff transform_operator_2d(R *r, const cc_sv *v) {
    const cc_step_entity *e = get(r, v);
    if (!e) return IDENT;
    double x[3], t[3], s, y[3];
    if (!direction(r, arg(e, 0), x)) x[0] = 1, x[1] = 0, x[2] = 0;
    if (!point(r, arg(e, 2), t)) t[0] = t[1] = t[2] = 0;
    if (!cc_sv_as_f64(arg(e, 3), &s)) s = 1.0;
    double zz[3] = {0, 0, 1};
    cross(zz, x, y);
    Aff a;
    for (int k = 0; k < 3; k++) a.x[k] = x[k] * s, a.y[k] = y[k] * s, a.z[k] = zz[k], a.t[k] = t[k];
    return a;
}

static double aff_det(const Aff *a) {
    double c[3];
    cross(a->y, a->z, c);
    return dot(a->x, c);
}

/* Affine::local: world point -> local coordinates (orthogonal axes). */
static void aff_local(const Aff *a, const double *p, double *o) {
    double d[3] = {p[0] - a->t[0], p[1] - a->t[1], p[2] - a->t[2]};
    const double *ax[3] = {a->x, a->y, a->z};
    double r[3];
    for (int k = 0; k < 3; k++) r[k] = dot(d, ax[k]) / fmax_rs(dot(ax[k], ax[k]), 1e-300);
    memcpy(o, r, sizeof r);
}

/* ---- curves */

static int curve3(R *r, const cc_sv *v, int depth, P3 *out);

static int pick_param(const cc_sv *v, R *r, int *has_pt, double *pt, int *has_par, double *par) {
    const cc_sv *l;
    size_t n;
    *has_pt = *has_par = 0;
    if (!cc_sv_as_list(v, &l, &n)) return 0;
    for (size_t i = 0; i < n; i++) {
        const cc_sv *x = &l[i];
        if (x->kind == CC_SV_TYPED && !strcmp(x->u.typed.name, "IFCPARAMETERVALUE"))
            *has_par = cc_sv_as_f64(x->u.typed.inner, par);
        else if (x->kind == CC_SV_REF) *has_pt = point(r, x, pt);
    }
    return 0;
}

/* IfcTrimmedCurve on a line, circle or ellipse. */
static int trimmed_curve(R *r, const cc_step_entity *e, int depth, P3 *out) {
    const cc_step_entity *basis = get(r, arg(e, 0));
    if (!basis) return 0;
    int sense = !enum_is(arg(e, 3), "F");
    int prefer = enum_is(arg(e, 4), "PARAMETER");
    int hp[2], hq[2];
    double pt[2][3], par[2];
    pick_param(arg(e, 1), r, &hp[0], pt[0], &hq[0], &par[0]);
    pick_param(arg(e, 2), r, &hp[1], pt[1], &hq[1], &par[1]);
    if (is(basis, "IFCLINE")) {
        double p0[3], d[3], m;
        const cc_step_entity *vec;
        if (!point(r, arg(basis, 0), p0) || !(vec = get(r, arg(basis, 1))) || !direction(r, arg(vec, 0), d)) return 0;
        if (!cc_sv_as_f64(arg(vec, 1), &m)) m = 1.0;
        double res[2][3];
        for (int k = 0; k < 2; k++) {
            if ((prefer && hq[k]) || (!prefer && !hp[k] && hq[k])) {
                for (int c = 0; c < 3; c++) res[k][c] = p0[c] + d[c] * m * par[k];
            } else if (hp[k]) memcpy(res[k], pt[k], sizeof res[k]);
            else return 0;
        }
        return p3_push(out, res[0]) || p3_push(out, res[1]) ? -1 : 1;
    }
    if (is(basis, "IFCCIRCLE") || is(basis, "IFCELLIPSE")) {
        Aff pos = axis_placement(r, arg(basis, 0));
        double r1, r2;
        if (!cc_sv_as_f64(arg(basis, 1), &r1)) return 0;
        if (is(basis, "IFCELLIPSE")) {
            if (!cc_sv_as_f64(arg(basis, 2), &r2)) return 0;
        } else r2 = r1;
        double ang[2];
        for (int k = 0; k < 2; k++) {
            if ((prefer && hq[k]) || (!prefer && !hp[k] && hq[k])) ang[k] = par[k] * r->angle_scale;
            else if (hp[k]) {
                double d[3] = {pt[k][0] - pos.t[0], pt[k][1] - pos.t[1], pt[k][2] - pos.t[2]};
                ang[k] = cc_atan2(dot(d, pos.y) / r2, dot(d, pos.x) / r1);
            } else return 0;
        }
        double a1 = ang[0], a2 = ang[1], sweep;
        if (sense) {
            double s = rem_euclid(a2 - a1, TAU);
            sweep = s < 1e-12 ? TAU : s;
        } else {
            double s = rem_euclid(a1 - a2, TAU);
            sweep = -(s < 1e-12 ? TAU : s);
        }
        size_t n = arc_segments(fmax_rs(r1, r2), sweep, r->arc_tol);
        size_t start = out->n;
        for (size_t i = 0; i <= n; i++) {
            double a = a1 + sweep * (double)i / (double)n;
            double p[3] = {r1 * cc_cos(a), r2 * cc_sin(a), 0.0}, q[3];
            aff_apply(&pos, p, q);
            if (p3_push(out, q)) return -1;
        }
        if (hp[0] && !prefer) memcpy(out->p[start], pt[0], sizeof pt[0]);
        if (hp[1] && !prefer) memcpy(out->p[start + n], pt[1], sizeof pt[1]);
        return 1;
    }
    warn(r, "trimmed ", basis->name, " drawn untrimmed");
    return curve3(r, arg(e, 0), depth + 1, out);
}

/* Reader::curve3: any bounded curve -> points (2D curves get z = 0), consecutive duplicates
 * removed, at least 2. 1 ok, 0 none (out empty), -1 out of memory. */
static int curve3(R *r, const cc_sv *v, int depth, P3 *out) {
    memset(out, 0, sizeof *out);
    if (depth > 16) {
        warn(r, "curve nesting too deep", 0, 0);
        return 0;
    }
    const cc_step_entity *e = get(r, v);
    if (!e) return 0;
    int rc = 1;
    if (is(e, "IFCPOLYLINE")) {
        const cc_sv *ps;
        size_t n;
        if (!cc_sv_as_list(arg(e, 0), &ps, &n)) return 0;
        for (size_t i = 0; i < n && rc == 1; i++) {
            double q[3];
            if (point(r, &ps[i], q) && p3_push(out, q)) rc = -1;
        }
    } else if (is(e, "IFCINDEXEDPOLYCURVE")) {
        const cc_step_entity *pl = get(r, arg(e, 0));
        const cc_sv *cl, *segs;
        size_t ncl, nseg;
        if (!pl || !cc_sv_as_list(arg(pl, 0), &cl, &ncl)) return 0;
        P3 coords = {0};
        for (size_t i = 0; i < ncl && rc == 1; i++) {
            const cc_sv *c;
            size_t nc;
            double q[3];
            if (!cc_sv_as_list(&cl[i], &c, &nc) || nc < 2 || !cc_sv_as_f64(&c[0], &q[0]) || !cc_sv_as_f64(&c[1], &q[1]))
                continue;
            if (nc < 3 || !cc_sv_as_f64(&c[2], &q[2])) q[2] = 0.0;
            if (p3_push(&coords, q)) rc = -1;
        }
        if (rc == 1 && !cc_sv_as_list(arg(e, 1), &segs, &nseg)) {
            for (size_t i = 0; i < coords.n && rc == 1; i++)
                if (p3_push(out, coords.p[i])) rc = -1;
        } else
            for (size_t s = 0; rc == 1 && s < nseg; s++) {
                if (segs[s].kind != CC_SV_TYPED) continue;
                const cc_sv *idx;
                size_t ni;
                if (!cc_sv_as_list(segs[s].u.typed.inner, &idx, &ni)) ni = 0;
                if (!ni || ni > 3 * 1024 * 1024) continue;
                size_t *at = malloc(ni * sizeof *at);
                if (!at) { rc = -1; break; }
                size_t ok = 0;
                for (size_t k = 0; k < ni; k++) {
                    int64_t ix;
                    if (cc_sv_as_i64(&idx[k], &ix) && ix >= 1 && (uint64_t)(ix - 1) < coords.n) at[ok++] = (size_t)(ix - 1);
                }
                if (ok == ni) {
                    if (!out->n && p3_push(out, coords.p[at[0]])) rc = -1;
                    if (rc == 1 && !strcmp(segs[s].u.typed.name, "IFCARCINDEX") && ni == 3) {
                        if (arc3(coords.p[at[0]], coords.p[at[1]], coords.p[at[2]], r->arc_tol, 1, out)) rc = -1;
                    } else
                        for (size_t k = 1; rc == 1 && k < ni; k++)
                            if (p3_push(out, coords.p[at[k]])) rc = -1;
                }
                free(at);
            }
        p3_free(&coords);
    } else if (is(e, "IFCCOMPOSITECURVE") || is(e, "IFCCOMPOSITECURVEONSURFACE")) {
        const cc_sv *segs;
        size_t ns;
        if (!cc_sv_as_list(arg(e, 0), &segs, &ns)) return 0;
        for (size_t i = 0; i < ns && rc == 1; i++) {
            const cc_step_entity *s = get(r, &segs[i]);
            P3 p;
            int c = s ? curve3(r, arg(s, 2), depth + 1, &p) : 0;
            if (c != 1) { rc = c; break; }
            if (enum_is(arg(s, 1), "F")) p3_reverse(&p);
            size_t from = 0;
            if (out->n && p.n && dist3(out->p[out->n - 1], p.p[0]) < 1e-9) from = 1;
            for (size_t k = from; k < p.n && rc == 1; k++)
                if (p3_push(out, p.p[k])) rc = -1;
            p3_free(&p);
        }
    } else if (is(e, "IFCTRIMMEDCURVE")) {
        rc = trimmed_curve(r, e, depth, out);
    } else if (is(e, "IFCCIRCLE") || is(e, "IFCELLIPSE")) {
        Aff pos = axis_placement(r, arg(e, 0));
        double r1, r2;
        if (!cc_sv_as_f64(arg(e, 1), &r1)) return 0;
        if (is(e, "IFCELLIPSE")) {
            if (!cc_sv_as_f64(arg(e, 2), &r2)) return 0;
        } else r2 = r1;
        size_t n = arc_segments(fmax_rs(r1, r2), TAU, r->arc_tol);
        for (size_t i = 0; i < n && rc == 1; i++) {
            double a = (double)i / (double)n * TAU;
            double p[3] = {r1 * cc_cos(a), r2 * cc_sin(a), 0.0}, q[3];
            aff_apply(&pos, p, q);
            if (p3_push(out, q)) rc = -1;
        }
    } else {
        warn(r, "unsupported curve ", e->name, " (skipped)");
        return 0;
    }
    if (rc == 1) {
        p3_dedup(out);
        if (out->n >= 2) return 1;
        rc = 0;
    }
    p3_free(out);
    return rc;
}

/* Reader::curve: closed 2D curve -> points without the repeated first, at least 3. */
static int curve(R *r, const cc_sv *v, Loop *out) {
    memset(out, 0, sizeof *out);
    P3 p;
    int c = curve3(r, v, 0, &p);
    if (c != 1) return 0;
    size_t m = p.n;
    if (m > 1) {
        double a[2] = {p.p[0][0], p.p[0][1]}, b[2] = {p.p[m - 1][0], p.p[m - 1][1]};
        if (dist2(a, b) < 1e-18) m--;
    }
    int ok = 1;
    for (size_t i = 0; ok && i < m; i++) {
        double q[2] = {p.p[i][0], p.p[i][1]};
        if (out->n && dist2(q, out->p[out->n - 1]) < 1e-18) continue;
        if (loop_push(out, q[0], q[1])) ok = 0;
    }
    p3_free(&p);
    if (ok && out->n >= 3) return 1;
    free(out->p);
    memset(out, 0, sizeof *out);
    return 0;
}

/* ---- profiles */

static void loops_place(R *r, Loops *ls, const cc_sv *pos) {
    Aff a = axis_placement(r, pos);
    for (size_t i = 0; i < ls->n; i++)
        for (size_t j = 0; j < ls->v[i].n; j++) {
            double p[3] = {ls->v[i].p[j][0], ls->v[i].p[j][1], 0.0}, q[3];
            aff_apply(&a, p, q);
            ls->v[i].p[j][0] = q[0], ls->v[i].p[j][1] = q[1];
        }
}
static int ring(Loop *l, double rad, size_t n) {
    for (size_t i = 0; i < n; i++) {
        double a = (double)i / (double)n * TAU;
        if (loop_push(l, rad * cc_cos(a), rad * cc_sin(a))) return -1;
    }
    return 0;
}
static int push_rect(Loops *ls, double a, double b) {
    Loop l = {0};
    if (loop_push(&l, -a, -b) || loop_push(&l, a, -b) || loop_push(&l, a, b) || loop_push(&l, -a, b) ||
        loops_push(ls, l)) {
        free(l.p);
        return -1;
    }
    return 0;
}

/* Reader::profile: loops in profile coordinates (outer first, then holes). 1 ok, 0 none. */
static int profile(R *r, const cc_sv *v, int depth, Loops *out) {
    memset(out, 0, sizeof *out);
    const cc_step_entity *e = get(r, v);
    if (!e) return 0;
    int bad = 0; /* out of memory or a `?` that failed */
    if (is(e, "IFCARBITRARYCLOSEDPROFILEDEF") || is(e, "IFCARBITRARYPROFILEDEFWITHVOIDS")) {
        Loop l;
        if (!curve(r, arg(e, 2), &l)) return 0;
        if (loops_push(out, l)) { free(l.p); return 0; }
        const cc_sv *hs;
        size_t nh;
        if (is(e, "IFCARBITRARYPROFILEDEFWITHVOIDS") && cc_sv_as_list(arg(e, 3), &hs, &nh))
            for (size_t i = 0; i < nh; i++)
                if (curve(r, &hs[i], &l) && loops_push(out, l)) free(l.p);
    } else if (is(e, "IFCRECTANGLEPROFILEDEF")) {
        double x, y;
        if (!cc_sv_as_f64(arg(e, 3), &x) || !cc_sv_as_f64(arg(e, 4), &y)) return 0;
        bad = push_rect(out, x / 2.0, y / 2.0);
        if (!bad) loops_place(r, out, arg(e, 2));
    } else if (is(e, "IFCCIRCLEPROFILEDEF")) {
        double rad;
        if (!cc_sv_as_f64(arg(e, 3), &rad)) return 0;
        Loop l = {0};
        if (ring(&l, rad, arc_segments(rad, TAU, r->arc_tol)) || loops_push(out, l)) free(l.p), bad = 1;
        if (!bad) loops_place(r, out, arg(e, 2));
    } else if (is(e, "IFCCIRCLEHOLLOWPROFILEDEF")) {
        double rad, t;
        if (!cc_sv_as_f64(arg(e, 3), &rad) || !cc_sv_as_f64(arg(e, 4), &t)) return 0;
        size_t n = arc_segments(rad, TAU, r->arc_tol);
        Loop l = {0};
        if (ring(&l, rad, n) || loops_push(out, l)) free(l.p), bad = 1;
        if (!bad && t > 0.0 && t < rad) {
            Loop h = {0};
            if (ring(&h, rad - t, n) || loops_push(out, h)) free(h.p), bad = 1;
        }
        if (!bad) loops_place(r, out, arg(e, 2));
    } else if (is(e, "IFCRECTANGLEHOLLOWPROFILEDEF")) {
        double x, y, t;
        if (!cc_sv_as_f64(arg(e, 3), &x) || !cc_sv_as_f64(arg(e, 4), &y) || !cc_sv_as_f64(arg(e, 5), &t)) return 0;
        x = x / 2.0, y = y / 2.0;
        bad = push_rect(out, x, y);
        if (!bad && t > 0.0 && t < fmin_rs(x, y)) bad = push_rect(out, x - t, y - t);
        if (!bad) loops_place(r, out, arg(e, 2));
    } else if (is(e, "IFCISHAPEPROFILEDEF")) {
        double w, h, tw, tf, rad;
        if (!cc_sv_as_f64(arg(e, 3), &w) || !cc_sv_as_f64(arg(e, 4), &h) || !cc_sv_as_f64(arg(e, 5), &tw) ||
            !cc_sv_as_f64(arg(e, 6), &tf))
            return 0;
        w = w / 2.0, h = h / 2.0, tw = tw / 2.0;
        double c[12][2] = {{-w, -h}, {w, -h},     {w, -h + tf},  {tw, -h + tf},  {tw, h - tf},   {w, h - tf},
                           {w, h},   {-w, h},     {-w, h - tf},  {-tw, h - tf},  {-tw, -h + tf}, {-w, -h + tf}};
        Loop l = {0};
        if (cc_sv_as_f64(arg(e, 7), &rad) && finite(rad) && rad > 0.0) {
            static const size_t corners[4] = {3, 4, 9, 10};
            if (fillet((const double(*)[2])c, 12, corners, 4, rad, r->arc_tol, &l)) bad = 1;
        } else
            for (int k = 0; k < 12 && !bad; k++) bad = loop_push(&l, c[k][0], c[k][1]);
        if (bad || loops_push(out, l)) free(l.p), bad = 1;
        if (!bad) loops_place(r, out, arg(e, 2));
    } else if (is(e, "IFCDERIVEDPROFILEDEF") && depth < 4) {
        if (!profile(r, arg(e, 2), depth + 1, out)) return 0;
        Aff op = transform_operator_2d(r, arg(e, 3));
        for (size_t i = 0; i < out->n; i++)
            for (size_t j = 0; j < out->v[i].n; j++) {
                double p[3] = {out->v[i].p[j][0], out->v[i].p[j][1], 0.0}, q[3];
                aff_apply(&op, p, q);
                out->v[i].p[j][0] = q[0], out->v[i].p[j][1] = q[1];
            }
    } else {
        warn(r, "unsupported profile ", e->name, " (skipped)");
        return 0;
    }
    if (bad) {
        loops_free(out);
        return 0;
    }
    size_t w = 0;
    for (size_t i = 0; i < out->n; i++) {
        if (out->v[i].n >= 3) out->v[w++] = out->v[i];
        else free(out->v[i].p);
    }
    out->n = w;
    if (w) return 1;
    loops_free(out);
    return 0;
}

/* ---- meshes: MeshBuilder, the triangle budget, Emit */

static void tris_add(Tris *m, const double (*t)[3]) { /* MeshBuilder::add_triangle */
    for (int k = 0; k < 9; k++)
        if (!finite(t[k / 3][k % 3])) return;
    if (grow((void **)&m->t, &m->cap, m->n + 1, sizeof *m->t)) { m->oom = 1; return; }
    for (int k = 0; k < 9; k++) m->t[m->n][k] = t[k / 3][k % 3];
    m->n++;
}
static void tris_free(Tris *m) {
    free(m->t);
    memset(m, 0, sizeof *m);
}

/* MeshBuilder::add_polygon: a planar polygon (outer + holes), triangulated in its own plane by
 * earcut after dropping the dominant axis of the Newell normal; winding follows the outer loop. */
static void add_polygon(Tris *m, const double (*outer)[3], size_t no, const P3 *holes, size_t nh) {
    if (no < 3) return;
    double nv[3] = {0, 0, 0};
    for (size_t i = 0; i < no; i++) {
        const double *a = outer[i], *b = outer[(i + 1) % no];
        nv[0] += (a[1] - b[1]) * (a[2] + b[2]);
        nv[1] += (a[2] - b[2]) * (a[0] + b[0]);
        nv[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    double len = norm3(nv);
    if (len != len || len <= 1e-12) return;
    double n[3] = {nv[0] / len, nv[1] / len, nv[2] / len};
    /* max_by(total_cmp) of |n[k]|: the last of equal maxima. */
    int ax = 0;
    uint64_t best = 0;
    for (int k = 0; k < 3; k++) {
        double a = n[k] < 0 ? -n[k] : n[k];
        uint64_t bits;
        memcpy(&bits, &a, 8);
        bits &= ~(1ull << 63);
        if (k == 0 || bits >= best) best = bits, ax = k;
    }
    int u = ax == 0 ? 1 : ax == 1 ? 2 : 0, v = ax == 0 ? 2 : ax == 1 ? 0 : 1;
    size_t np = no;
    for (size_t h = 0; h < nh; h++)
        if (holes[h].n >= 3) np += holes[h].n;
    double (*pts)[3] = malloc(np * sizeof *pts);
    double *flat = malloc(np * 2 * sizeof *flat);
    size_t *hidx = malloc((nh ? nh : 1) * sizeof *hidx), nhi = 0;
    if (!pts || !flat || !hidx) {
        m->oom = 1;
        goto out;
    }
    size_t k = 0;
    for (size_t i = 0; i < no; i++, k++) memcpy(pts[k], outer[i], sizeof pts[k]);
    for (size_t h = 0; h < nh; h++) {
        if (holes[h].n < 3) continue;
        hidx[nhi++] = k;
        for (size_t i = 0; i < holes[h].n; i++, k++) memcpy(pts[k], holes[h].p[i], sizeof pts[k]);
    }
    for (size_t i = 0; i < np; i++) flat[2 * i] = pts[i][u], flat[2 * i + 1] = pts[i][v];
    uint32_t *idx = NULL;
    size_t ni = 0;
    if (cc_earcut(flat, np * 2, hidx, nhi, &idx, &ni)) goto out;
    for (size_t t = 0; t + 2 < ni + 0 || t + 3 <= ni; t += 3) {
        double tri[3][3];
        memcpy(tri[0], pts[idx[t]], sizeof tri[0]);
        memcpy(tri[1], pts[idx[t + 1]], sizeof tri[1]);
        memcpy(tri[2], pts[idx[t + 2]], sizeof tri[2]);
        double e1[3] = {tri[1][0] - tri[0][0], tri[1][1] - tri[0][1], tri[1][2] - tri[0][2]};
        double e2[3] = {tri[2][0] - tri[0][0], tri[2][1] - tri[0][1], tri[2][2] - tri[0][2]};
        double c[3];
        cross(e1, e2, c);
        if (dot(c, n) < 0.0) {
            double s[3];
            memcpy(s, tri[1], sizeof s);
            memcpy(tri[1], tri[2], sizeof s);
            memcpy(tri[2], s, sizeof s);
        }
        tris_add(m, (const double(*)[3])tri);
    }
    free(idx);
out:
    free(pts);
    free(flat);
    free(hidx);
}

typedef struct { /* read.rs Budget: shared by the workers of a job (relaxed atomics) */
    size_t triangles_left, visits_left;
    int exhausted;
    size_t element_max; /* most triangles one element may take (SIZE_MAX: no limit) */
} Budget;
static int budget_exhausted(Budget *b) { return cc_atomic_load(&b->exhausted); }

typedef struct {
    Tris *mb;
    double scale;
    Budget *budget;
} Emit;

static int has_budget(Emit *o) {
    Budget *b = o->budget;
    if (cc_atomic_load(&b->triangles_left) == 0) cc_atomic_store(&b->exhausted, 1);
    return !cc_atomic_load(&b->exhausted) && !o->mb->oom && o->mb->n < b->element_max;
}
static void charge(Emit *o, size_t before) {
    size_t used = o->mb->n > before ? o->mb->n - before : 0;
    if (!used) return; /* nothing to take (the CAS would store the same value) */
    size_t v = cc_atomic_load(&o->budget->triangles_left);
    while (!cc_atomic_cas(&o->budget->triangles_left, &v, v > used ? v - used : 0)) {
    }
}
static int visit(Budget *b) {
    int ok = !cc_atomic_load(&b->exhausted);
    if (ok) {
        size_t v = cc_atomic_load(&b->visits_left);
        while (v > 0 && !cc_atomic_cas(&b->visits_left, &v, v - 1)) {
        }
        ok = v > 0;
    }
    if (!ok) cc_atomic_store(&b->exhausted, 1);
    return ok;
}
/* Emit::polygon: file units -> mm, then add_polygon. */
static void emit_polygon(Emit *o, const double (*outer)[3], size_t no, const P3 *holes, size_t nh) {
    if (!has_budget(o)) return;
    size_t before = o->mb->n;
    double s = o->scale;
    P3 so = {0};
    P3 *sh = calloc(nh ? nh : 1, sizeof *sh);
    int bad = !sh;
    for (size_t i = 0; !bad && i < no; i++) {
        double q[3] = {outer[i][0] * s, outer[i][1] * s, outer[i][2] * s};
        bad = p3_push(&so, q);
    }
    for (size_t h = 0; !bad && h < nh; h++)
        for (size_t i = 0; !bad && i < holes[h].n; i++) {
            double q[3] = {holes[h].p[i][0] * s, holes[h].p[i][1] * s, holes[h].p[i][2] * s};
            bad = p3_push(&sh[h], q);
        }
    if (bad) o->mb->oom = 1;
    else add_polygon(o->mb, (const double(*)[3])so.p, so.n, sh, nh);
    p3_free(&so);
    for (size_t h = 0; sh && h < nh; h++) p3_free(&sh[h]);
    free(sh);
    charge(o, before);
}
static void emit_triangle_mm(Emit *o, const double (*t)[3]) {
    if (!has_budget(o)) return;
    size_t before = o->mb->n;
    tris_add(o->mb, t);
    charge(o, before);
}
static void emit_triangle(Emit *o, const double *a, const double *b, const double *c) {
    double s = o->scale;
    double t[3][3] = {{a[0] * s, a[1] * s, a[2] * s}, {b[0] * s, b[1] * s, b[2] * s}, {c[0] * s, c[1] * s, c[2] * s}};
    emit_triangle_mm(o, (const double(*)[3])t);
}

/* ---- representation items */

static int is_body(const cc_step_entity *rep) {
    const char *id = "", *ty = "";
    size_t ni = 0, nt = 0;
    cc_sv_as_str(arg(rep, 1), &id, &ni);
    cc_sv_as_str(arg(rep, 2), &ty, &nt);
    if ((ni == 4 && !memcmp(id, "Body", 4)) || (ni == 10 && !memcmp(id, "Facetation", 10))) return 1;
    if (ni) return 0;
    static const char *NOT[] = {"Curve2D", "Curve3D", "GeometricCurveSet", "Annotation2D", "BoundingBox", "Point"};
    for (size_t k = 0; k < sizeof NOT / sizeof *NOT; k++)
        if (strlen(NOT[k]) == nt && !memcmp(NOT[k], ty, nt)) return 0;
    return 1;
}

static void item(R *r, uint64_t id, const Aff *tf, Emit *out, int depth);

static void extrusion(R *r, const cc_step_entity *e, const Aff *tf, Emit *out) {
    Loops ls;
    if (!profile(r, arg(e, 0), 0, &ls)) return;
    Aff ap = axis_placement(r, arg(e, 1));
    Aff pos = aff_then(tf, &ap);
    double dir[3], depth;
    P3 *bottom = NULL, *top = NULL;
    if (!direction(r, arg(e, 2), dir) || !cc_sv_as_f64(arg(e, 3), &depth) || !finite(depth) || !(depth > 0.0))
        goto out;
    double d[3] = {dir[0] * depth, dir[1] * depth, dir[2] * depth};
    int flip = (d[2] < 0.0) != (aff_det(&pos) < 0.0);
    for (size_t i = 0; i < ls.n; i++) {
        int ccw = signed_area(&ls.v[i]) > 0.0;
        if (ccw == (i > 0)) loop_reverse(&ls.v[i]);
        if (flip) loop_reverse(&ls.v[i]);
    }
    bottom = calloc(ls.n, sizeof *bottom);
    top = calloc(ls.n, sizeof *top);
    if (!bottom || !top) goto out;
    for (size_t i = 0; i < ls.n; i++)
        for (size_t j = 0; j < ls.v[i].n; j++) {
            const double *p = ls.v[i].p[j];
            double b[3] = {p[0], p[1], 0.0}, t[3] = {p[0] + d[0], p[1] + d[1], d[2]}, q[3];
            aff_apply(&pos, b, q);
            if (p3_push(&bottom[i], q)) goto out;
            aff_apply(&pos, t, q);
            if (p3_push(&top[i], q)) goto out;
        }
    {
        P3 rev = {0};
        for (size_t j = bottom[0].n; j-- > 0;)
            if (p3_push(&rev, bottom[0].p[j])) { p3_free(&rev); goto out; }
        emit_polygon(out, (const double(*)[3])rev.p, rev.n, bottom + 1, ls.n - 1);
        p3_free(&rev);
    }
    emit_polygon(out, (const double(*)[3])top[0].p, top[0].n, top + 1, ls.n - 1);
    for (size_t l = 0; l < ls.n; l++) {
        size_t n = bottom[l].n;
        for (size_t i = 0; i < n; i++) {
            size_t j = (i + 1) % n;
            emit_triangle(out, bottom[l].p[i], bottom[l].p[j], top[l].p[j]);
            emit_triangle(out, bottom[l].p[i], top[l].p[j], top[l].p[i]);
        }
    }
out:
    for (size_t i = 0; i < ls.n; i++) {
        if (bottom) p3_free(&bottom[i]);
        if (top) p3_free(&top[i]);
    }
    free(bottom);
    free(top);
    loops_free(&ls);
}

/* point_list_3d: coordinates with three values each. */
static int point_list_3d(R *r, const cc_sv *v, P3 *out, const Aff *tf) {
    memset(out, 0, sizeof *out);
    const cc_step_entity *e = get(r, v);
    const cc_sv *l;
    size_t n;
    if (!e || !cc_sv_as_list(arg(e, 0), &l, &n)) return 0;
    for (size_t i = 0; i < n; i++) {
        const cc_sv *c;
        size_t nc;
        double p[3], q[3];
        if (!cc_sv_as_list(&l[i], &c, &nc) || nc < 3 || !cc_sv_as_f64(&c[0], &p[0]) || !cc_sv_as_f64(&c[1], &p[1]) ||
            !cc_sv_as_f64(&c[2], &p[2]))
            continue;
        aff_apply(tf, p, q);
        if (p3_push(out, q)) return -1;
    }
    return 0;
}

/* PnIndex: the integers of a list (filter_map as_i64), or none. */
typedef struct {
    int has;
    int64_t *v;
    size_t n;
} Pn;
static int pn_read(const cc_sv *v, Pn *pn) {
    const cc_sv *l;
    size_t n;
    memset(pn, 0, sizeof *pn);
    if (!cc_sv_as_list(v, &l, &n)) return 0;
    pn->has = 1;
    pn->v = malloc((n ? n : 1) * sizeof *pn->v);
    if (!pn->v) return -1;
    for (size_t i = 0; i < n; i++)
        if (cc_sv_as_i64(&l[i], &pn->v[pn->n])) pn->n++;
    return 0;
}
static const double *resolve(const P3 *coords, const Pn *pn, int64_t k) {
    if (pn->has) {
        if (k < 1 || (uint64_t)(k - 1) >= pn->n) return NULL;
        k = pn->v[k - 1];
    }
    if (k < 1 || (uint64_t)(k - 1) >= coords->n) return NULL;
    return coords->p[k - 1];
}
static int loop_of(const cc_sv *v, const P3 *coords, const Pn *pn, P3 *out) {
    const cc_sv *l;
    size_t n;
    memset(out, 0, sizeof *out);
    if (!cc_sv_as_list(v, &l, &n)) return 0;
    for (size_t i = 0; i < n; i++) {
        int64_t k;
        const double *p = cc_sv_as_i64(&l[i], &k) ? resolve(coords, pn, k) : NULL;
        if (!p || p3_push(out, p)) {
            p3_free(out);
            return 0;
        }
    }
    return 1;
}

static void polygonal_face_set(R *r, const cc_step_entity *e, const Aff *tf, Emit *out) {
    P3 coords;
    Pn pn;
    if (point_list_3d(r, arg(e, 0), &coords, tf) || pn_read(arg(e, 3), &pn)) {
        out->mb->oom = 1;
        p3_free(&coords);
        return;
    }
    const cc_sv *fs;
    size_t nf;
    if (!cc_sv_as_list(arg(e, 2), &fs, &nf)) nf = 0;
    for (size_t i = 0; i < nf; i++) {
        const cc_step_entity *face = get(r, &fs[i]);
        P3 outer;
        if (!face || !loop_of(arg(face, 0), &coords, &pn, &outer)) continue;
        P3 *holes = NULL;
        size_t nh = 0;
        const cc_sv *hl;
        size_t nhl;
        if (is(face, "IFCINDEXEDPOLYGONALFACEWITHVOIDS") && cc_sv_as_list(arg(face, 1), &hl, &nhl) && nhl) {
            holes = calloc(nhl, sizeof *holes);
            for (size_t h = 0; holes && h < nhl; h++)
                if (loop_of(&hl[h], &coords, &pn, &holes[nh])) nh++;
        }
        emit_polygon(out, (const double(*)[3])outer.p, outer.n, holes, nh);
        for (size_t h = 0; h < nh; h++) p3_free(&holes[h]);
        free(holes);
        p3_free(&outer);
    }
    free(pn.v);
    p3_free(&coords);
}

static void triangulated_face_set(R *r, const cc_step_entity *e, const Aff *tf, Emit *out) {
    P3 coords;
    Pn pn;
    if (point_list_3d(r, arg(e, 0), &coords, tf) || pn_read(arg(e, 4), &pn)) {
        out->mb->oom = 1;
        p3_free(&coords);
        return;
    }
    const cc_sv *ts;
    size_t nt;
    if (!cc_sv_as_list(arg(e, 3), &ts, &nt)) nt = 0;
    for (size_t i = 0; i < nt; i++) {
        const cc_sv *t;
        size_t n;
        if (!cc_sv_as_list(&ts[i], &t, &n) || n != 3) continue;
        const double *p[3];
        int ok = 1;
        for (int k = 0; k < 3 && ok; k++) {
            int64_t ix;
            p[k] = cc_sv_as_i64(&t[k], &ix) ? resolve(&coords, &pn, ix) : NULL;
            ok = p[k] != NULL;
        }
        if (ok) emit_triangle(out, p[0], p[1], p[2]);
    }
    free(pn.v);
    p3_free(&coords);
}

/* IfcFace list (faceted B-rep, shells). */
static void faces(R *r, const cc_sv *list, const Aff *tf, Emit *out) {
    const cc_sv *fs;
    size_t nf;
    if (!cc_sv_as_list(list, &fs, &nf)) return;
    for (size_t i = 0; i < nf; i++) {
        const cc_step_entity *face = get(r, &fs[i]);
        if (!face) continue;
        const cc_sv *bs;
        size_t nb;
        if (!cc_sv_as_list(arg(face, 0), &bs, &nb)) nb = 0;
        P3 outer = {0};
        int has_outer = 0;
        P3 *holes = calloc(nb ? nb : 1, sizeof *holes);
        size_t nh = 0;
        if (!holes) { out->mb->oom = 1; return; }
        for (size_t j = 0; j < nb; j++) {
            const cc_step_entity *bound = get(r, &bs[j]), *lp;
            if (!bound || !(lp = get(r, arg(bound, 0)))) continue;
            if (!is(lp, "IFCPOLYLOOP")) {
                warn(r, "unsupported face loop ", lp->name, " (skipped)");
                continue;
            }
            P3 pts = {0};
            const cc_sv *ps;
            size_t np;
            if (!cc_sv_as_list(arg(lp, 0), &ps, &np)) np = 0;
            for (size_t k = 0; k < np; k++) {
                double q[3], w[3];
                if (!point(r, &ps[k], q)) continue;
                aff_apply(tf, q, w);
                if (p3_push(&pts, w)) out->mb->oom = 1;
            }
            if (enum_is(arg(bound, 1), "F")) p3_reverse(&pts);
            if (is(bound, "IFCFACEOUTERBOUND") && !has_outer) outer = pts, has_outer = 1;
            else holes[nh++] = pts;
        }
        size_t h0 = 0;
        if (!has_outer && nh) outer = holes[0], has_outer = 1, h0 = 1;
        if (has_outer) emit_polygon(out, (const double(*)[3])outer.p, outer.n, holes + h0, nh - h0);
        p3_free(&outer);
        for (size_t h = h0; h < nh; h++) p3_free(&holes[h]);
        free(holes);
    }
}

/* tube: a tube of radius r with n sides along a polyline (IfcSweptDiskSolid). */
static void tube(P3 *p, double r, size_t n, Emit *out) {
    p3_dedup(p);
    if (p->n < 2) return;
    int closed = dist3(p->p[0], p->p[p->n - 1]) < 1e-9;
    if (closed) p->n--;
    size_t m = p->n;
    double(*rings)[3] = malloc(m * n * sizeof *rings);
    if (!rings) { out->mb->oom = 1; return; }
    double prev_n[3];
    int has_prev = 0;
    for (size_t i = 0; i < m; i++) {
        const double *pi = p->p[i];
        double din[3], dout[3], t[3];
        int hin = 0, hout = 0, ht = 0;
#define SEG(i_, o_) do { size_t a_ = (i_), b_ = (a_ + 1) % m; double s_[3] = {p->p[b_][0] - p->p[a_][0], p->p[b_][1] - p->p[a_][1], p->p[b_][2] - p->p[a_][2]}; h_ = normalize(s_, (o_)); } while (0)
        int h_;
        if (i > 0 || closed) { SEG((i + m - 1) % m, din); hin = h_; }
        if (i + 1 < m || closed) { SEG(i, dout); hout = h_; }
#undef SEG
        if (hin && hout) {
            double s[3] = {din[0] + dout[0], din[1] + dout[1], din[2] + dout[2]};
            ht = normalize(s, t);
            if (!ht) memcpy(t, dout, sizeof t), ht = 1;
        } else if (hin) memcpy(t, din, sizeof t), ht = 1;
        else if (hout) memcpy(t, dout, sizeof t), ht = 1;
        if (!ht) { free(rings); return; }
        double seed[3];
        if (has_prev) memcpy(seed, prev_n, sizeof seed);
        else if ((t[2] < 0 ? -t[2] : t[2]) < 0.9) seed[0] = 0, seed[1] = 0, seed[2] = 1;
        else seed[0] = 1, seed[1] = 0, seed[2] = 0;
        double ds = dot(seed, t), w[3] = {seed[0] - t[0] * ds, seed[1] - t[1] * ds, seed[2] - t[2] * ds}, nrm[3];
        if (!normalize(w, nrm)) { free(rings); return; }
        memcpy(prev_n, nrm, sizeof nrm), has_prev = 1;
        double bin[3], bd[3], stretch = 1.0;
        cross(t, nrm, bin);
        int hb = 0;
        if (hin && hout) {
            double c = dot(din, t);
            c = c != c ? c : c < 0.3 ? 0.3 : c > 1.0 ? 1.0 : c;
            double ab[3], tab[3];
            cross(din, dout, ab);
            cross(t, ab, tab);
            hb = normalize(tab, bd);
            stretch = 1.0 / c;
        }
        for (size_t j = 0; j < n; j++) {
            double a = (double)j / (double)n * TAU;
            double ca = cc_cos(a), sa = cc_sin(a), off[3];
            for (int k = 0; k < 3; k++) off[k] = r * (ca * nrm[k] + sa * bin[k]);
            if (hb) {
                double extra = dot(off, bd) * (stretch - 1.0);
                for (int k = 0; k < 3; k++) off[k] = off[k] + extra * bd[k];
            }
            for (int k = 0; k < 3; k++) rings[i * n + j][k] = pi[k] + off[k];
        }
    }
    size_t pairs = closed ? m : m - 1;
    for (size_t i = 0; i < pairs; i++) {
        const double(*a)[3] = rings + i * n, (*b)[3] = rings + ((i + 1) % m) * n;
        for (size_t j = 0; j < n; j++) {
            size_t k = (j + 1) % n;
            emit_triangle(out, a[j], b[k], b[j]);
            emit_triangle(out, a[j], a[k], b[k]);
        }
    }
    if (!closed) {
        double(*start)[3] = malloc(n * sizeof *start);
        if (!start) { out->mb->oom = 1; free(rings); return; }
        for (size_t j = 0; j < n; j++) memcpy(start[j], rings[n - 1 - j], sizeof start[j]);
        emit_polygon(out, (const double(*)[3])start, n, NULL, 0);
        emit_polygon(out, (const double(*)[3])(rings + (m - 1) * n), n, NULL, 0);
        free(start);
    }
    free(rings);
}

static void swept_disk(R *r, const cc_step_entity *e, const Aff *tf, Emit *out) {
    P3 path;
    if (curve3(r, arg(e, 0), 0, &path) != 1) return;
    double rad, ri;
    if (!cc_sv_as_f64(arg(e, 1), &rad) || !finite(rad) || !(rad > 0)) { p3_free(&path); return; }
    if (cc_sv_as_f64(arg(e, 2), &ri) && ri > 0) warn(r, "hollow swept disk drawn solid (inner radius ignored)", 0, 0);
    for (size_t i = 0; i < path.n; i++) aff_apply(tf, path.p[i], path.p[i]);
    tube(&path, rad, arc_segments(rad, TAU, r->arc_tol), out);
    p3_free(&path);
}

/* Reader::solid_tris: triangles (mm, world) of one representation item, evaluated on its own. */
static void solid_tris(R *r, uint64_t id, const Aff *tf, Emit *out, int depth, Tris *mb) {
    Emit tmp = {mb, out->scale, out->budget};
    item(r, id, tf, &tmp, depth);
}

/* prism_tris: closed prism (mm), a 2D loop in pos's XY plane from z0 to z1 along its Z. */
static void prism_tris(const Loop *loop2d, const Aff *pos, double z0, double z1, double scale, Tris *mb) {
    Loop l = {0};
    if (grow((void **)&l.p, &l.cap, loop2d->n + 1, sizeof *l.p)) { mb->oom = 1; return; }
    memcpy(l.p, loop2d->p, loop2d->n * sizeof *l.p);
    l.n = loop2d->n;
    if (signed_area(&l) < 0.0) loop_reverse(&l);
    size_t n = l.n;
    double (*bottom)[3] = malloc((n + 1) * sizeof *bottom), (*top)[3] = malloc((n + 1) * sizeof *top);
    if (!bottom || !top) {
        mb->oom = 1;
        goto out;
    }
#define PRISM_P(q_, z_, o_) do { double v_[3] = {(q_)[0], (q_)[1], (z_)}; aff_apply(pos, v_, (o_)); \
        for (int k_ = 0; k_ < 3; k_++) (o_)[k_] *= scale; } while (0)
    for (size_t i = 0; i < n; i++) {
        PRISM_P(l.p[n - 1 - i], z0, bottom[i]);
        PRISM_P(l.p[i], z1, top[i]);
    }
    size_t first = mb->n;
    add_polygon(mb, (const double(*)[3])bottom, n, NULL, 0);
    add_polygon(mb, (const double(*)[3])top, n, NULL, 0);
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;
        double bi[3], bj[3], ti[3], tj[3];
        PRISM_P(l.p[i], z0, bi);
        PRISM_P(l.p[j], z0, bj);
        PRISM_P(l.p[i], z1, ti);
        PRISM_P(l.p[j], z1, tj);
        double t1[3][3], t2[3][3];
        memcpy(t1[0], bi, 24), memcpy(t1[1], bj, 24), memcpy(t1[2], tj, 24);
        memcpy(t2[0], bi, 24), memcpy(t2[1], tj, 24), memcpy(t2[2], ti, 24);
        tris_add(mb, (const double(*)[3])t1);
        tris_add(mb, (const double(*)[3])t2);
    }
#undef PRISM_P
    if (aff_det(pos) < 0.0) /* a mirroring placement turns the solid inside out */
        for (size_t t = first; t < mb->n; t++)
            for (int k = 0; k < 3; k++) {
                double s = mb->t[t][3 + k];
                mb->t[t][3 + k] = mb->t[t][6 + k];
                mb->t[t][6 + k] = s;
            }
out:
    free(bottom);
    free(top);
    free(l.p);
}

static cc_tri_set tri_set(const Tris *m) {
    cc_tri_set s = {(const double(*)[9])m->t, m->n};
    return s;
}
/* boolean::apply into a Tris (replacing it on success). Returns CC_BOOL_*, -1 out of memory;
 * err (CC_BOOL_ERR bytes) gets the text. */
static int tris_apply(cc_bool_op op, const Tris *a, const cc_tri_set *b, size_t nb, Tris *res, char *err) {
    double (*o)[9];
    size_t no;
    int rc = cc_boolean_apply(op, tri_set(a), b, nb, &o, &no, err);
    if (rc) return rc;
    memset(res, 0, sizeof *res);
    res->t = o, res->n = no, res->cap = no;
    return 0;
}

/* Reader::half_space: a half-space as a finite solid (mm, world), large enough to contain
 * `operand`. With AgreementFlag TRUE the material is on the -normal side of the plane. */
static void half_space(R *r, const cc_step_entity *h, const Aff *tf, const Tris *operand, double scale, Tris *res) {
    memset(res, 0, sizeof *res);
    const cc_step_entity *plane = get(r, arg(h, 0));
    if (!is(plane, "IFCPLANE")) {
        warn(r, "", h->name, ": unsupported base surface (not cut)");
        return;
    }
    Aff ap = axis_placement(r, arg(plane, 0));
    Aff pos = aff_then(tf, &ap);
    int material_below = !enum_is(arg(h, 1), "F");
    double lo[3] = {1.0 / 0.0, 1.0 / 0.0, 1.0 / 0.0}, hi[3] = {-1.0 / 0.0, -1.0 / 0.0, -1.0 / 0.0};
    for (size_t t = 0; t < operand->n; t++)
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++) {
                double v = operand->t[t][3 * c + k] / scale;
                lo[k] = fmin_rs(lo[k], v), hi[k] = fmax_rs(hi[k], v);
            }
    double d[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    double l = 2.0 * norm3(d) + 1.0;
    double mid[3] = {(lo[0] + hi[0]) / 2.0, (lo[1] + hi[1]) / 2.0, (lo[2] + hi[2]) / 2.0}, c[3];
    aff_local(&pos, mid, c);
    Loop rect = {0};
    if (loop_push(&rect, c[0] - l, c[1] - l) || loop_push(&rect, c[0] + l, c[1] - l) ||
        loop_push(&rect, c[0] + l, c[1] + l) || loop_push(&rect, c[0] - l, c[1] + l)) {
        free(rect.p);
        res->oom = 1;
        return;
    }
    double z0, z1;
    if (material_below) z0 = -l + fmin_rs(c[2], 0.0), z1 = 0.0;
    else z0 = 0.0, z1 = l + fmax_rs(c[2], 0.0);
    prism_tris(&rect, &pos, z0, z1, scale, res);
    free(rect.p);
    if (!is(h, "IFCPOLYGONALBOUNDEDHALFSPACE")) return;
    /* IfcPolygonalBoundedHalfSpace(.., Position, PolygonalBoundary): also inside the boundary prism
     * (extruded along Position's Z). A boundary whose end does not come back to its start (by
     * 0.1 mm) is cut by its plane alone, as IfcOpenShell does. */
    Aff pp = axis_placement(r, arg(h, 2));
    Aff ppos = aff_then(tf, &pp);
    P3 ends;
    int closed = 0;
    if (curve3(r, arg(h, 3), 0, &ends) == 1) {
        if (ends.n) {
            const double *a = ends.p[0], *b = ends.p[ends.n - 1];
            double dd[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
            closed = norm3(dd) * scale < 0.1;
        }
        p3_free(&ends);
    }
    Loop poly = {0};
    int have = closed && curve(r, arg(h, 3), &poly);
    if (!have) {
        warn(r, "polygonal half-space: boundary not closed; cut by its plane alone", 0, 0);
        return;
    }
    double lc[3];
    aff_local(&ppos, mid, lc);
    double cz = lc[2];
    Tris prism = {0};
    prism_tris(&poly, &ppos, cz - l, cz + l, scale, &prism);
    free(poly.p);
    if (prism.oom) res->oom = 1;
    cc_tri_set b = tri_set(&prism);
    Tris cut;
    char err[CC_BOOL_ERR];
    int rc = tris_apply(CC_BOOL_INTERSECTION, res, &b, 1, &cut, err);
    if (rc == 0) {
        tris_free(res);
        *res = cut;
    } else if (rc < 0) res->oom = 1;
    else warn(r, "polygonal half-space: boundary not a closed polygon (", err, "); cut by its plane alone");
    tris_free(&prism);
}

/* IfcBooleanResult / IfcBooleanClippingResult (Operator, FirstOperand, SecondOperand): read.rs
 * Reader::boolean, through cc_boolean_apply. Without a kernel (cc_boolean_kernel() NULL: wasm,
 * a build without manifold) the first operand is drawn uncut and the report says so. */
static void boolean_result(R *r, const cc_step_entity *e, const Aff *tf, Emit *out, int depth) {
    cc_bool_op op = enum_is(arg(e, 0), "UNION")          ? CC_BOOL_UNION
                    : enum_is(arg(e, 0), "INTERSECTION") ? CC_BOOL_INTERSECTION
                                                         : CC_BOOL_DIFFERENCE;
    uint64_t first;
    if (!cc_sv_as_ref(arg(e, 1), &first)) return;
    Tris a = {0};
    solid_tris(r, first, tf, out, depth + 1, &a);
    if (a.oom) out->mb->oom = 1;
    if (!a.n) { tris_free(&a); return; }
    if (!cc_boolean_kernel()) {
        r->unsup_n++;
        for (size_t i = 0; i < a.n; i++) emit_triangle_mm(out, (const double(*)[3])a.t[i]);
        tris_free(&a);
        return;
    }
    const cc_step_entity *h = get(r, arg(e, 2));
    int half = h && (is(h, "IFCHALFSPACESOLID") || is(h, "IFCBOXEDHALFSPACE") || is(h, "IFCPOLYGONALBOUNDEDHALFSPACE"));
    Tris b = {0};
    uint64_t second;
    if (h && half) half_space(r, h, tf, &a, out->scale, &b);
    else if (h && cc_sv_as_ref(arg(e, 2), &second)) solid_tris(r, second, tf, out, depth + 1, &b);
    if (b.oom) out->mb->oom = 1;
    cc_tri_set bs = tri_set(&b);
    Tris res;
    char err[CC_BOOL_ERR];
    int rc = tris_apply(op, &a, &bs, 1, &res, err);
    const Tris *draw = &a;
    if (rc < 0) out->mb->oom = 1;
    else if (rc == 0 && !res.n && half && op == CC_BOOL_DIFFERENCE)
        /* A clipping half-space that leaves nothing (Revit: a wall's top plane put below its
         * bottom) is an export error: drawn unclipped, as IfcOpenShell does, rather than lost. */
        warn(r, "a clipping plane removed the whole solid; drawn unclipped", 0, 0);
    else if (rc == 0) draw = &res;
    else warn(r, "boolean cut failed, drawn uncut (", err, ")");
    for (size_t i = 0; i < draw->n; i++) emit_triangle_mm(out, (const double(*)[3])draw->t[i]);
    if (rc == 0) tris_free(&res);
    tris_free(&a);
    tris_free(&b);
}

static int map_on_stack(R *r, uint64_t id) {
    for (size_t i = 0; i < r->nstack; i++)
        if (r->stack[i] == id) return 1;
    return 0;
}

static void item(R *r, uint64_t id, const Aff *tf, Emit *out, int depth) {
    if (depth > MAX_NESTING) {
        warn(r, "representation nesting too deep", 0, 0);
        return;
    }
    /* An element at its own limit stops here too: nothing it visits would be kept. */
    if (out->mb->n >= out->budget->element_max || !visit(out->budget)) return;
    const cc_step_entity *e = cc_step_get(r->f, id);
    if (!e) return;
    if (is(e, "IFCEXTRUDEDAREASOLID")) extrusion(r, e, tf, out);
    else if (is(e, "IFCPOLYGONALFACESET")) polygonal_face_set(r, e, tf, out);
    else if (is(e, "IFCTRIANGULATEDFACESET") || is(e, "IFCTRIANGULATEDIRREGULARNETWORK"))
        triangulated_face_set(r, e, tf, out);
    else if (is(e, "IFCFACETEDBREP") || is(e, "IFCFACETEDBREPWITHVOIDS")) {
        const cc_step_entity *shell = get(r, arg(e, 0));
        if (shell) faces(r, arg(shell, 0), tf, out);
    } else if (is(e, "IFCSHELLBASEDSURFACEMODEL") || is(e, "IFCFACEBASEDSURFACEMODEL")) {
        const cc_sv *l;
        size_t n;
        if (!cc_sv_as_list(arg(e, 0), &l, &n)) n = 0;
        for (size_t i = 0; i < n; i++) {
            const cc_step_entity *shell = get(r, &l[i]);
            if (shell) faces(r, arg(shell, 0), tf, out);
        }
    } else if (is(e, "IFCMAPPEDITEM")) {
        uint64_t map_id;
        const cc_step_entity *map, *rep;
        if (!cc_sv_as_ref(arg(e, 0), &map_id) || !(map = cc_step_get(r->f, map_id))) return;
        if (map_on_stack(r, map_id)) {
            warn(r, "representation map contains itself (cycle skipped)", 0, 0);
            return;
        }
        Aff origin = axis_placement(r, arg(map, 0));
        Aff target = transform_operator(r, arg(e, 1));
        Aff t1 = aff_then(tf, &target);
        Aff t = aff_then(&t1, &origin);
        if (!(rep = get(r, arg(map, 1)))) return;
        if (r->nstack >= sizeof r->stack / sizeof *r->stack) return; /* bounded by MAX_NESTING */
        r->stack[r->nstack++] = map_id;
        const cc_sv *l;
        size_t n;
        if (!cc_sv_as_list(arg(rep, 3), &l, &n)) n = 0;
        for (size_t i = 0; i < n; i++) {
            uint64_t it;
            if (cc_sv_as_ref(&l[i], &it)) item(r, it, &t, out, depth + 1);
        }
        r->nstack--;
    } else if (is(e, "IFCBOOLEANCLIPPINGRESULT") || is(e, "IFCBOOLEANRESULT")) boolean_result(r, e, tf, out, depth);
    else if (is(e, "IFCSWEPTDISKSOLID") || is(e, "IFCSWEPTDISKSOLIDPOLYGONAL")) swept_disk(r, e, tf, out);
    else if (is(e, "IFCSTYLEDITEM")) {
    } else warn(r, "unsupported geometry ", e->name, " (skipped)");
}

/* map_shape: a representation map's items in the map's own coordinates. */
static void map_shape(R *r, uint64_t map_id, const cc_step_entity *map, Emit *out) {
    Aff origin = axis_placement(r, arg(map, 0));
    const cc_step_entity *rep = get(r, arg(map, 1));
    if (!rep) return;
    r->stack[r->nstack++] = map_id;
    const cc_sv *l;
    size_t n;
    if (!cc_sv_as_list(arg(rep, 3), &l, &n)) n = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t it;
        if (cc_sv_as_ref(&l[i], &it)) item(r, it, &origin, out, 1);
    }
    r->nstack--;
}

/* product_body: the Body representation of a product, placed in the world. */
static void product_body(R *r, const cc_step_entity *e, Emit *out) {
    uint64_t pl;
    const cc_step_entity *shape = get(r, arg(e, 6));
    if (!cc_sv_as_ref(arg(e, 5), &pl) || !shape) return;
    Aff placement = object_placement(r, pl, 0);
    const cc_sv *reps;
    size_t nr;
    if (!cc_sv_as_list(arg(shape, 2), &reps, &nr)) nr = 0;
    for (size_t i = 0; i < nr; i++) {
        const cc_step_entity *rep = get(r, &reps[i]);
        if (!rep || !is_body(rep)) continue;
        const cc_sv *its;
        size_t ni;
        if (!cc_sv_as_list(arg(rep, 3), &its, &ni)) ni = 0;
        for (size_t j = 0; j < ni; j++) {
            uint64_t it;
            if (cc_sv_as_ref(&its[j], &it)) item(r, it, &placement, out, 0);
        }
    }
}

/* mapped_item_of: the single IfcMappedItem the element's Body is, when that is all it is. */
static const cc_step_entity *mapped_item_of(R *r, const cc_step_entity *e) {
    const cc_step_entity *shape = get(r, arg(e, 6));
    const cc_sv *reps;
    size_t nr;
    if (!shape || !cc_sv_as_list(arg(shape, 2), &reps, &nr)) return NULL;
    const cc_sv *first = NULL;
    size_t count = 0;
    for (size_t i = 0; i < nr && count < 2; i++) {
        const cc_step_entity *rep = get(r, &reps[i]);
        if (!rep || !is_body(rep)) continue;
        const cc_sv *its;
        size_t ni;
        if (!cc_sv_as_list(arg(rep, 3), &its, &ni)) ni = 0;
        for (size_t j = 0; j < ni && count < 2; j++) {
            if (!count) first = &its[j];
            count++;
        }
    }
    if (!first) return NULL;
    const cc_step_entity *only = get(r, first);
    return only && count == 1 && is(only, "IFCMAPPEDITEM") ? only : NULL;
}

typedef struct {
    const char *name;
    size_t n;
    int has_cat;
    cc_category cat;
} MatName;
typedef struct {
    MatName *v;
    size_t n, cap;
} MatNames;

static void mn_push(MatNames *m, const char *s, size_t n, int has, cc_category c) {
    if (m->n >= 4096) return;
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 4;
        void *p = realloc(m->v, cap * sizeof *m->v);
        if (!p) return;
        m->v = p, m->cap = cap;
    }
    m->v[m->n].name = s, m->v[m->n].n = n, m->v[m->n].has_cat = has, m->v[m->n].cat = c;
    m->n++;
}

static void material_names(R *r, const cc_step_entity *m, int depth, MatNames *out) {
    if (depth > 6 || !m) return;
    const cc_sv *l;
    size_t n;
#define SUB(v) material_names(r, get(r, (v)), depth + 1, out)
#define SUBS(v) \
    if (cc_sv_as_list((v), &l, &n)) for (size_t i = 0; i < n; i++) SUB(&l[i])
    if (is(m, "IFCMATERIAL")) {
        const char *s = "", *h;
        size_t sn = 0, hn;
        cc_sv_as_str(arg(m, 0), &s, &sn);
        cc_category from = cc_material_classify(s, sn), hint = CC_CAT_OTHER;
        if (cc_sv_as_str(arg(m, 2), &h, &hn)) hint = cc_material_classify(h, hn);
        if (from != CC_CAT_OTHER) mn_push(out, s, sn, 1, from);
        else mn_push(out, s, sn, hint != CC_CAT_OTHER, hint);
    } else if (is(m, "IFCMATERIALLAYERSETUSAGE") || is(m, "IFCMATERIALPROFILESETUSAGE") ||
               is(m, "IFCMATERIALPROFILESETUSAGETAPERING") || is(m, "IFCMATERIALLAYER") ||
               is(m, "IFCMATERIALLAYERWITHOFFSETS")) {
        SUB(arg(m, 0));
    } else if (is(m, "IFCMATERIALLAYERSET") || is(m, "IFCMATERIALLIST")) {
        SUBS(arg(m, 0));
    } else if (is(m, "IFCMATERIALPROFILESET") || is(m, "IFCMATERIALCONSTITUENTSET")) {
        SUBS(arg(m, 2));
    } else if (is(m, "IFCMATERIALPROFILE") || is(m, "IFCMATERIALPROFILEWITHOFFSETS") ||
               is(m, "IFCMATERIALCONSTITUENT")) {
        SUB(arg(m, 2));
    }
#undef SUB
#undef SUBS
}

/* Per entity index: material display name (malloc) and category; set when known. */
typedef struct {
    char *name;
    size_t n;
    cc_category cat;
    int set;
} ElMat;

static void materials(R *r, ElMat *em) {
    const cc_step_file *f = r->f;
    Buf b;
    buf_init(&b);
    for (size_t i = 0; i < f->n; i++) {
        const cc_step_entity *rel = &f->ents[i];
        if (!is(rel, "IFCRELASSOCIATESMATERIAL")) continue;
        const cc_step_entity *m = get(r, arg(rel, 5));
        if (!m) continue;
        MatNames names = {0};
        material_names(r, m, 0, &names);
        if (!names.n) { free(names.v); continue; }
        cc_category cat = CC_CAT_OTHER;
        for (size_t k = 0; k < names.n; k++) {
            cc_category c = names.v[k].has_cat ? names.v[k].cat : cc_material_classify(names.v[k].name, names.v[k].n);
            if (c != CC_CAT_OTHER) { cat = c; break; }
        }
        buf_clear(&b);
        for (size_t k = 0; k < names.n; k++) {
            int dup = 0;
            for (size_t j = 0; j < k && !dup; j++)
                dup = names.v[j].n == names.v[k].n && !memcmp(names.v[j].name, names.v[k].name, names.v[k].n);
            if (dup) continue;
            if (b.len) buf_puts(&b, " / ");
            buf_append(&b, names.v[k].name, names.v[k].n);
        }
        free(names.v);
        const cc_sv *os;
        size_t no;
        if (!cc_sv_as_list(arg(rel, 4), &os, &no)) continue;
        for (size_t k = 0; k < no; k++) {
            const cc_step_entity *o = get(r, &os[k]);
            if (!o) continue; /* only entities of the file can be elements */
            ElMat *x = &em[cc_step_index(f, o)];
            free(x->name);
            x->name = malloc(b.len + 1);
            x->n = x->name ? b.len : 0;
            if (x->name && b.len) memcpy(x->name, b.p, b.len);
            x->cat = cat;
            x->set = 2; /* direct */
        }
    }
    buf_free(&b);
    /* By type: products of a type with a material get it, unless they have their own. */
    for (size_t i = 0; i < f->n; i++) {
        const cc_step_entity *rel = &f->ents[i];
        if (!is(rel, "IFCRELDEFINESBYTYPE")) continue;
        const cc_step_entity *t = get(r, arg(rel, 5));
        if (!t) continue;
        ElMat *tm = &em[cc_step_index(f, t)];
        if (tm->set != 2) continue;
        const cc_sv *os;
        size_t no;
        if (!cc_sv_as_list(arg(rel, 4), &os, &no)) continue;
        for (size_t k = 0; k < no; k++) {
            const cc_step_entity *o = get(r, &os[k]);
            if (!o) continue;
            ElMat *x = &em[cc_step_index(f, o)];
            if (x->set) continue;
            x->name = malloc(tm->n + 1);
            x->n = x->name ? tm->n : 0;
            if (x->name && tm->n) memcpy(x->name, tm->name, tm->n);
            x->cat = tm->cat;
            x->set = 1;
        }
    }
}

/* displayed_bytes: bytes of the instances the display parses (all but property sets and
 * quantities), for Limits::for_text. */
/* read.rs NOT_DISPLAYED: types the display never reads, not even parsed (cc_step_parse_skipping). */
static const char *const NOT_DISPLAYED[] = {
    "IFCPROPERTYSINGLEVALUE", "IFCPROPERTYENUMERATEDVALUE", "IFCPROPERTYLISTVALUE", "IFCPROPERTYBOUNDEDVALUE",
    "IFCPROPERTYTABLEVALUE", "IFCPROPERTYREFERENCEVALUE", "IFCCOMPLEXPROPERTY", "IFCPROPERTYSET",
    "IFCRELDEFINESBYPROPERTIES", "IFCELEMENTQUANTITY", "IFCQUANTITYLENGTH", "IFCQUANTITYAREA",
    "IFCQUANTITYVOLUME", "IFCQUANTITYCOUNT", "IFCQUANTITYWEIGHT", "IFCQUANTITYTIME"};
#define N_NOT_DISPLAYED (sizeof NOT_DISPLAYED / sizeof *NOT_DISPLAYED)

/* The NOT_DISPLAYED state a line starting with '#' sets. */
static int db_hash_line(const char *line, size_t len) {
    size_t k = 0;
    while (k < len && line[k] != '=') k++;
    const char *rest = k < len ? line + k + 1 : line + len;
    size_t rn = k < len ? len - k - 1 : 0;
    while (rn && (*rest == ' ' || *rest == '\t' || *rest == '\r' || *rest == '\f' || *rest == '\n')) rest++, rn--;
    size_t e = 0;
    while (e < rn && ((rest[e] >= '0' && rest[e] <= '9') || (rest[e] >= 'a' && rest[e] <= 'z') ||
                      (rest[e] >= 'A' && rest[e] <= 'Z') || rest[e] == '_'))
        e++;
    for (size_t q = 0; q < N_NOT_DISPLAYED; q++) {
        const char *nd = NOT_DISPLAYED[q];
        if (strlen(nd) != e) continue;
        size_t c = 0;
        while (c < e && (rest[c] >= 'a' && rest[c] <= 'z' ? rest[c] - 32 : rest[c]) == nd[c]) c++;
        if (c == e) return 1;
    }
    return 0;
}

#define DB_CHUNK ((size_t)4 << 20)
typedef struct {
    const char *t;
    size_t n;
    uint64_t *prefix, *rest; /* per chunk: bytes of the lines before its first '#' line, and after */
    int *has_hash, *last;    /* per chunk: a '#' line seen; the state after its last one */
} DbJob;

/* The first line start at or after `at` (lines start at 0 and after each '\n'; n is one when the
 * text ends with '\n'). */
static size_t db_line_start(const char *t, size_t n, size_t at) {
    if (at == 0) return 0;
    if (at > n) return n + 1;
    while (at <= n && t[at - 1] != '\n') at++;
    return at;
}

static void db_job(void *ctx, size_t k, size_t w) {
    (void)w;
    DbJob *d = ctx;
    const char *t = d->t;
    size_t n = d->n, i = db_line_start(t, n, k * DB_CHUNK), stop = db_line_start(t, n, (k + 1) * DB_CHUNK);
    uint64_t pre = 0, rest = 0;
    int seen = 0, st = 0;
    while (i < stop && i <= n) {
        size_t j = i;
        const char *nl = i < n ? memchr(t + i, '\n', n - i) : NULL;
        j = nl ? (size_t)(nl - t) : n;
        size_t len = j - i;
        if (len && t[i] == '#') seen = 1, st = db_hash_line(t + i, len);
        if (!seen) pre += len + 1;
        else if (st) rest += len + 1;
        i = j + 1;
    }
    d->prefix[k] = pre, d->rest[k] = rest, d->has_hash[k] = seen, d->last[k] = st;
}

/* displayed_bytes: bytes of the instances the display parses (all but property sets and
 * quantities), for Limits::for_text. A line continues the state of the last line starting with
 * '#'. Chunks of lines in parallel, combined in order. */
static uint64_t displayed_bytes(const char *t, size_t n) {
    size_t nc = n / DB_CHUNK + 1;
    DbJob d = {t, n, calloc(nc, sizeof(uint64_t)), calloc(nc, sizeof(uint64_t)), calloc(nc, sizeof(int)),
               calloc(nc, sizeof(int))};
    uint64_t skipped = 0;
    if (d.prefix && d.rest && d.has_hash && d.last) {
        cc_jobs_run(nc, db_job, &d);
        int st = 0;
        for (size_t k = 0; k < nc; k++) {
            if (st) skipped += d.prefix[k];
            skipped += d.rest[k];
            if (d.has_hash[k]) st = d.last[k];
        }
    } else { /* out of memory: serial */
        int st = 0;
        for (size_t i = 0;;) {
            size_t j = i;
            while (j < n && t[j] != '\n') j++;
            if (j > i && t[i] == '#') st = db_hash_line(t + i, j - i);
            if (st) skipped += j - i + 1;
            if (j >= n) break;
            i = j + 1;
        }
    }
    free(d.prefix), free(d.rest), free(d.has_hash), free(d.last);
    return (uint64_t)n > skipped ? (uint64_t)n - skipped : 0;
}

/* ------------------------------------------------------------------ classes */

static const char *CLASS_NAMES[] = {
    "IfcFlowController", "IfcFlowMovingDevice", "IfcFlowStorageDevice", "IfcFlowTreatmentDevice",
    "IfcEnergyConversionDevice", "IfcDistributionControlElement", "IfcDistributionFlowElement",
    "IfcDistributionChamberElement", "IfcBeam", "IfcBeamStandardCase", "IfcColumn", "IfcColumnStandardCase",
    "IfcWall", "IfcWallStandardCase", "IfcWallElementedCase", "IfcSlab", "IfcSlabStandardCase",
    "IfcSlabElementedCase", "IfcFooting", "IfcPile", "IfcRoof", "IfcStair", "IfcStairFlight", "IfcRamp",
    "IfcRampFlight", "IfcRailing", "IfcDoor", "IfcWindow", "IfcCovering", "IfcCurtainWall", "IfcPlate",
    "IfcPlateStandardCase", "IfcMember", "IfcMemberStandardCase", "IfcBuildingElementProxy",
    "IfcBuildingElementPart", "IfcReinforcingBar", "IfcReinforcingMesh", "IfcTendon", "IfcTendonAnchor",
    "IfcMechanicalFastener", "IfcFastener", "IfcDiscreteAccessory", "IfcFurnishingElement", "IfcFurniture",
    "IfcSystemFurnitureElement", "IfcFlowSegment", "IfcFlowTerminal", "IfcFlowFitting",
    "IfcDistributionElement", "IfcGeographicElement", "IfcElementAssembly", "IfcChimney",
    "IfcShadingDevice", "IfcTransportElement", "IfcEarthworksFill", "IfcEarthworksCut", "IfcCourse",
    "IfcPavement", "IfcKerb", "IfcBearing", "IfcDeepFoundation", "IfcCaissonFoundation", "IfcPipeSegment",
    "IfcDuctSegment", "IfcCableCarrierSegment", "IfcCableSegment", "IfcSanitaryTerminal", "IfcLightFixture",
};

static void class_name(const char *upper, Buf *out) {
    for (size_t k = 0; k < sizeof CLASS_NAMES / sizeof *CLASS_NAMES; k++) {
        const char *c = CLASS_NAMES[k];
        size_t i = 0;
        while (c[i] && upper[i] && (c[i] >= 'a' && c[i] <= 'z' ? c[i] - 32 : c[i]) == upper[i]) i++;
        if (!c[i] && !upper[i]) { buf_puts(out, c); return; }
    }
    const char *rest = strncmp(upper, "IFC", 3) == 0 ? upper + 3 : upper;
    buf_puts(out, "Ifc");
    for (size_t i = 0; rest[i]; i++) {
        char c = rest[i];
        if (i == 0) buf_putc(out, c >= 'a' && c <= 'z' ? (char)(c - 32) : c);
        else buf_putc(out, c >= 'A' && c <= 'Z' ? (char)(c + 32) : c);
    }
}

static const char *SKIP_CLASSES[] = {
    "IFCOPENINGELEMENT", "IFCOPENINGSTANDARDCASE", "IFCSPACE", "IFCSITE", "IFCBUILDING", "IFCBUILDINGSTOREY",
    "IFCVIRTUALELEMENT", "IFCANNOTATION", "IFCGRID", "IFCSPATIALZONE", "IFCEXTERNALSPATIALELEMENT",
};
static double signed_area(const Loop *l) {
    double s = 0;
    for (size_t i = 0; i < l->n; i++) {
        const double *a = l->p[i], *b = l->p[(i + 1) % l->n];
        s += a[0] * b[1] - b[0] * a[1];
    }
    return s / 2.0;
}
static void loop_reverse(Loop *l) {
    for (size_t i = 0, j = l->n ? l->n - 1 : 0; i < j; i++, j--) {
        double t0 = l->p[i][0], t1 = l->p[i][1];
        l->p[i][0] = l->p[j][0], l->p[i][1] = l->p[j][1];
        l->p[j][0] = t0, l->p[j][1] = t1;
    }
}
#endif
