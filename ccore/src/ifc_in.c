/* IFC import (subset), a port of crates/bimble-ifc/src/import.rs and the parts of read.rs it
 * uses. See ifc_in.h. Arithmetic follows Rust's order of operations exactly; no contraction. */
#include "ifc_in.h"
#include <stdlib.h>
#include <string.h>
#include "earcut.h"
#include "guid.h"
#include "ifc_math.h"
#include "verbs.h"

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
#define strcmp s_cmp
#define strncmp s_ncmp
#define memchr m_chr
#define calloc zalloc

#ifdef __clang__
#pragma clang fp contract(off)
#endif

/* arena_dup aligned to 8 (arena_dup is byte-aligned). */
static void *ii_dup8(Arena *a, const void *src, size_t n) {
    void *d = arena_alloc(a, n ? n : 1, 8);
    if (d && n) memcpy(d, src, n);
    return d;
}

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

typedef struct {
    const cc_step_file *f;
    Aff *pl;          /* placement cache, by entity index */
    unsigned char *has_pl;
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

static Aff object_placement(R *r, uint64_t id, int depth) {
    const cc_step_entity *e = cc_step_get(r->f, id);
    size_t ix = e ? cc_step_index(r->f, e) : 0;
    if (e && r->has_pl[ix]) return r->pl[ix];
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
    r->pl[ix] = a;
    r->has_pl[ix] = 1;
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

typedef struct { /* read.rs Budget, single-threaded */
    size_t triangles_left, visits_left;
    int exhausted;
} Budget;

typedef struct {
    Tris *mb;
    double scale;
    Budget *budget;
} Emit;

static int has_budget(Emit *o) {
    if (o->budget->triangles_left == 0) o->budget->exhausted = 1;
    return !o->budget->exhausted && !o->mb->oom;
}
static void charge(Emit *o, size_t before) {
    size_t used = o->mb->n > before ? o->mb->n - before : 0;
    o->budget->triangles_left = o->budget->triangles_left > used ? o->budget->triangles_left - used : 0;
}
static int visit(Budget *b) {
    int ok = !b->exhausted && b->visits_left > 0;
    if (ok) b->visits_left--;
    else b->exhausted = 1;
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

/* IfcBooleanResult: no boolean kernel in the C core (Rust: manifold3d). An operand that is
 * empty is handled as boolean::apply does; otherwise the first operand is drawn uncut and the
 * report says so. */
static void boolean_result(R *r, const cc_step_entity *e, const Aff *tf, Emit *out, int depth) {
    int inter = enum_is(arg(e, 0), "INTERSECTION");
    uint64_t first;
    if (!cc_sv_as_ref(arg(e, 0 + 1), &first)) return;
    Tris a = {0};
    Emit ea = {&a, out->scale, out->budget};
    item(r, first, tf, &ea, depth + 1);
    if (!a.n) { tris_free(&a); return; }
    (void)inter;
    r->unsup_n++;
    for (size_t i = 0; i < a.n; i++) emit_triangle_mm(out, (const double(*)[3])a.t[i]);
    tris_free(&a);
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
    if (!visit(out->budget)) return;
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

/* ------------------------------------------------------------------ materials */

static int has_sub(const char *s, size_t n, const char *w) {
    size_t k = strlen(w);
    for (size_t i = 0; i + k <= n; i++)
        if (memcmp(s + i, w, k) == 0) return 1;
    return 0;
}

cc_category cc_material_classify(const char *name, size_t n) {
    char small[256];
    char *s = n < sizeof small ? small : malloc(n + 1);
    if (!s) return CC_CAT_OTHER;
    /* to_lowercase: ASCII, and Latin-1 capitals (U+00C0..U+00DE but U+00D7) in UTF-8. */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        s[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        if (c == 0xC3 && i + 1 < n) {
            unsigned char d = (unsigned char)name[i + 1];
            s[i + 1] = (char)(d >= 0x80 && d <= 0x9E && d != 0x97 ? d + 0x20 : d);
            i++;
        }
    }
    s[n] = 0;
#define HAS(...) has_any(s, n, (const char *[]){__VA_ARGS__, NULL})
    cc_category cat;
    int c_dig = n > 1 && s[0] == 'c' && s[1] >= '0' && s[1] <= '9';
    int lc_dig = n > 2 && s[0] == 'l' && s[1] == 'c' && s[2] >= '0' && s[2] <= '9';
    int gl_dig = n > 2 && s[0] == 'g' && s[1] == 'l' && s[2] >= '0' && s[2] <= '9';
    int d_dig = n > 1 && s[0] == 'd' && s[1] >= '0' && s[1] <= '9';
    int s_dig = n > 1 && s[0] == 's' && s[1] >= '0' && s[1] <= '9';
    int slash = memchr(s, '/', n) != NULL;
    struct { cc_category c; const char *w[12]; } t[] = {
        {CC_CAT_TIMBER, {"timber", "wood", "bois", "clt", "glulam", "lamell\xc3\xa9", "lamelle"}},
        {CC_CAT_STEEL, {"steel", "acier", "stahl", "metal", "m\xc3\xa9tal"}},
        {CC_CAT_MASONRY, {"brick", "brique", "masonry", "ma\xc3\xa7onnerie", "maconnerie", "block", "parpaing", "stone", "pierre"}},
        {CC_CAT_EARTH, {"earth", "terre", "soil", "ground", "terrain", "gravel", "gravier", "molasse"}},
        {CC_CAT_INSULATION, {"insulation", "isolation", "isolant", "mineral wool", "laine", "xps", "eps"}},
        {CC_CAT_GLASS, {"glass", "verre", "glazing", "vitrage"}},
    };
#undef HAS
    int any = 0;
    if (has_sub(s, n, "concrete") || has_sub(s, n, "b\xc3\xa9ton") || has_sub(s, n, "beton") ||
        (slash && (c_dig || lc_dig)))
        cat = CC_CAT_CONCRETE, any = 1;
    for (size_t k = 0; !any && k < sizeof t / sizeof *t; k++) {
        int hit = 0;
        for (int j = 0; t[k].w[j]; j++) hit |= has_sub(s, n, t[k].w[j]);
        if (t[k].c == CC_CAT_TIMBER) hit |= gl_dig || c_dig || d_dig;
        if (t[k].c == CC_CAT_STEEL) hit |= s_dig;
        if (hit) cat = t[k].c, any = 1;
    }
    if (!any) cat = CC_CAT_OTHER;
    if (s != small) free(s);
    return cat;
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

/* extruded_element: 0 none, 1 parametric, 2 polygon. */
static int extruded_element(R *r, Arena *ar, const cc_step_entity *solid, const Aff *rel, cc_ifc_class cls,
                            double scale, cc_placement *pl, cc_generator *g) {
    Loops ls;
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

/* displayed_bytes: bytes of the instances the display parses (all but property sets and
 * quantities), for Limits::for_text. */
static uint64_t displayed_bytes(const char *t, size_t n) {
    static const char *NOT_DISPLAYED[] = {
        "IFCPROPERTYSINGLEVALUE", "IFCPROPERTYENUMERATEDVALUE", "IFCPROPERTYLISTVALUE", "IFCPROPERTYBOUNDEDVALUE",
        "IFCPROPERTYTABLEVALUE", "IFCPROPERTYREFERENCEVALUE", "IFCCOMPLEXPROPERTY", "IFCPROPERTYSET",
        "IFCRELDEFINESBYPROPERTIES", "IFCELEMENTQUANTITY", "IFCQUANTITYLENGTH", "IFCQUANTITYAREA",
        "IFCQUANTITYVOLUME", "IFCQUANTITYCOUNT", "IFCQUANTITYWEIGHT", "IFCQUANTITYTIME"};
    uint64_t skipped = 0;
    int in_skipped = 0;
    size_t i = 0;
    for (;;) {
        size_t j = i;
        while (j < n && t[j] != '\n') j++;
        const char *line = t + i;
        size_t len = j - i;
        if (len && line[0] == '#') {
            size_t k = 0;
            while (k < len && line[k] != '=') k++;
            const char *rest = k < len ? line + k + 1 : line + len;
            size_t rn = k < len ? len - k - 1 : 0;
            while (rn && (*rest == ' ' || *rest == '\t' || *rest == '\r' || *rest == '\f' || *rest == '\n'))
                rest++, rn--;
            size_t e = 0;
            while (e < rn && ((rest[e] >= '0' && rest[e] <= '9') || (rest[e] >= 'a' && rest[e] <= 'z') ||
                              (rest[e] >= 'A' && rest[e] <= 'Z') || rest[e] == '_'))
                e++;
            in_skipped = 0;
            for (size_t q = 0; q < sizeof NOT_DISPLAYED / sizeof *NOT_DISPLAYED && !in_skipped; q++) {
                const char *nd = NOT_DISPLAYED[q];
                if (strlen(nd) != e) continue;
                size_t c = 0;
                while (c < e && (rest[c] >= 'a' && rest[c] <= 'z' ? rest[c] - 32 : rest[c]) == nd[c]) c++;
                in_skipped = c == e;
            }
        }
        if (in_skipped) skipped += len + 1;
        if (j >= n) break;
        i = j + 1;
    }
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
    r.pl = malloc(ne * sizeof *r.pl);
    r.has_pl = calloc(ne, 1);
    ElMat *em = calloc(ne, sizeof *em);
    Props props = {0};
    Pending *els = NULL;
    size_t nels = 0;
    cc_material *mats = NULL;
    size_t nmats = 0;
    size_t *st_ent = NULL; /* storey entity indices -> storey gid */
    cc_gid *st_gid = NULL;
    size_t nst = 0;
    GSet seen = {0}, opening_seen = {0};
    Budget budget = {0, 0, 0};
    Shapes shapes = {0};
    size_t *open_at = NULL;
    uint64_t *open_ids = NULL;
    int storeys_in_arena = 0;
    Cont *cont = calloc(ne, sizeof *cont);
    if (!cont) goto oom;
    if (!r.pl || !r.has_pl || !em) goto oom;

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
    }
    /* storeys sorted by id; the storey array moves into the arena at the end. */
    if (sort_by_gid(m->storeys, m->nstoreys, sizeof *m->storeys)) goto oom;
    cc_gid fallback = m->storeys[0].id;

    materials(&r, em);
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
    /* Georeference: Rust keeps it on the model; the C model has none. */
    if (first_of(&f, "IFCMAPCONVERSION") || building.x[0] != 1.0 || building.x[1] != 0.0 || building.t[0] != 0.0 ||
        building.t[1] != 0.0 || building.t[2] != 0.0)
        unsup(&r, "georeference (building frame or IfcMapConversion) not kept", 0, 0);

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
        if (!class_of(e->name, &cls)) {
            int skip = 0;
            for (size_t k = 0; k < sizeof SKIP_CLASSES / sizeof *SKIP_CLASSES; k++) skip |= !strcmp(e->name, SKIP_CLASSES[k]);
            const cc_step_entity *s6 = get(&r, arg(e, 6));
            if (skip || !is(s6, "IFCPRODUCTDEFINITIONSHAPE")) continue;
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
        const cc_step_entity *solid = extruded_solid(&r, e);
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
        /* Material: name -> derived id, category from the grade (first one wins). */
        const char *mn = em[i].set && em[i].n ? em[i].name : "Undefined";
        size_t mnn = em[i].set && em[i].n ? em[i].n : 9;
        cc_category cat = em[i].set ? em[i].cat : CC_CAT_OTHER;
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
        m->elements = ev, m->nelements = nels;
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
        m->elements = NULL, m->nelements = 0;
        m->materials = NULL, m->nmaterials = 0;
    }
    for (size_t i = 0; em && i < ne; i++) free(em[i].name);
    free(em);
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
    free(r.has_pl);
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
