/* IFC import (subset), a port of crates/bimble-ifc/src/import.rs and the parts of read.rs it
 * uses. See ifc_in.h. Arithmetic follows Rust's order of operations exactly; no contraction. */
#include "ifc_in.h"
#include <stdlib.h>
#include <string.h>
#include "guid.h"

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

/* f64::hypot. Exact when one side is 0 (all of Bimble's own axis-aligned rectangles);
 * otherwise sqrt(a²+b²), which can differ from libm's hypot in the last bit. */
static double hyp(double a, double b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    if (a == 0) return b;
    if (b == 0) return a;
    return cc_sqrt(a * a + b * b);
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

typedef struct {
    const cc_step_file *f;
    Aff *pl;          /* placement cache, by entity index */
    unsigned char *has_pl;
    Tallies warnings; /* Reader warnings (BTreeMap with counts) */
    Tallies unsup;    /* outside the C subset */
    const char *unsup_why; /* set by profile/curve when they meet something not ported */
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

/* ---- profiles: loops of 2D points (growable, malloc) */

typedef struct {
    double (*p)[2];
    size_t n, cap;
} Loop;
typedef struct {
    Loop *v;
    size_t n, cap;
} Loops;

static int loop_push(Loop *l, double x, double y) {
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 8;
        void *p = realloc(l->p, cap * sizeof *l->p);
        if (!p) return -1;
        l->p = p, l->cap = cap;
    }
    l->p[l->n][0] = x, l->p[l->n][1] = y;
    l->n++;
    return 0;
}
static int loops_push(Loops *ls, Loop l) {
    if (ls->n == ls->cap) {
        size_t cap = ls->cap ? ls->cap * 2 : 4;
        void *p = realloc(ls->v, cap * sizeof *ls->v);
        if (!p) return -1;
        ls->v = p, ls->cap = cap;
    }
    ls->v[ls->n++] = l;
    return 0;
}
static void loops_free(Loops *ls) {
    for (size_t i = 0; i < ls->n; i++) free(ls->v[i].p);
    free(ls->v);
    memset(ls, 0, sizeof *ls);
}
static double dist2(const double *a, const double *b) {
    double dx = a[0] - b[0], dy = a[1] - b[1];
    return dx * dx + dy * dy;
}

/* Closed 2D curve -> points without the repeated first (Reader::curve). IfcPolyline only. */
static int curve(R *r, const cc_sv *v, Loop *out) {
    memset(out, 0, sizeof *out);
    const cc_step_entity *e = get(r, v);
    if (!e) return 0;
    if (!is(e, "IFCPOLYLINE")) {
        if (is(e, "IFCINDEXEDPOLYCURVE") || is(e, "IFCCOMPOSITECURVE") || is(e, "IFCTRIMMEDCURVE") ||
            is(e, "IFCCIRCLE") || is(e, "IFCELLIPSE") || is(e, "IFCLINE"))
            r->unsup_why = e->name;
        else warn(r, "unsupported curve ", e->name, " (skipped)");
        return 0;
    }
    const cc_sv *ps;
    size_t n;
    if (!cc_sv_as_list(arg(e, 0), &ps, &n)) return 0;
    /* curve3: 3D points, consecutive duplicates (|a-b| < 1e-9) dropped, at least 2. */
    double (*p3)[3] = malloc((n ? n : 1) * sizeof *p3);
    size_t m = 0;
    if (!p3) return 0;
    for (size_t i = 0; i < n; i++) {
        double q[3];
        if (!point(r, &ps[i], q)) continue;
        if (m) {
            double d[3] = {q[0] - p3[m - 1][0], q[1] - p3[m - 1][1], q[2] - p3[m - 1][2]};
            if (cc_sqrt(dot(d, d)) < 1e-9) continue;
        }
        memcpy(p3[m++], q, sizeof q);
    }
    int ok = m >= 2;
    if (ok && m > 1) {
        double a[2] = {p3[0][0], p3[0][1]}, b[2] = {p3[m - 1][0], p3[m - 1][1]};
        if (dist2(a, b) < 1e-18) m--;
    }
    for (size_t i = 0; ok && i < m; i++) {
        double q[2] = {p3[i][0], p3[i][1]};
        if (out->n && dist2(out->p[out->n - 1], q) < 1e-18) continue;
        if (loop_push(out, q[0], q[1])) ok = 0;
    }
    free(p3);
    if (ok && out->n >= 3) return 1;
    free(out->p);
    memset(out, 0, sizeof *out);
    return 0;
}

static int profile(R *r, const cc_sv *v, Loops *out) {
    memset(out, 0, sizeof *out);
    const cc_step_entity *e = get(r, v);
    if (!e) return 0;
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
        x = x / 2.0, y = y / 2.0;
        Aff a = axis_placement(r, arg(e, 2));
        double pts[4][2] = {{-x, -y}, {x, -y}, {x, y}, {-x, y}};
        Loop l = {0};
        for (int k = 0; k < 4; k++) {
            double p[3] = {pts[k][0], pts[k][1], 0.0}, q[3];
            aff_apply(&a, p, q);
            if (loop_push(&l, q[0], q[1])) { free(l.p); return 0; }
        }
        if (loops_push(out, l)) { free(l.p); return 0; }
    } else {
        static const char *rust_has[] = {"IFCCIRCLEPROFILEDEF", "IFCCIRCLEHOLLOWPROFILEDEF",
                                         "IFCRECTANGLEHOLLOWPROFILEDEF", "IFCISHAPEPROFILEDEF",
                                         "IFCDERIVEDPROFILEDEF"};
        for (size_t k = 0; k < sizeof rust_has / sizeof *rust_has; k++)
            if (is(e, rust_has[k])) { r->unsup_why = e->name; return 0; }
        warn(r, "unsupported profile ", e->name, " (skipped)");
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
        double la = hyp(a[0], a[1]), lb = hyp(b[0], b[1]);
        double c = (a[0] * b[0] + a[1] * b[1]) / (la * lb);
        if (la < 1e-9 || lb < 1e-9 || (c < 0 ? -c : c) > 1e-6) return 0;
    }
    for (int i = 0; i < 2; i++) {
        double la = hyp(e[i][0], e[i][1]), lb = hyp(e[i + 2][0], e[i + 2][1]);
        double d = la - lb;
        if ((d < 0 ? -d : d) > 1e-6 * (la > lb ? la : lb)) return 0;
    }
    rc->centre[0] = (((l->p[0][0] + l->p[1][0]) + l->p[2][0]) + l->p[3][0]) / 4.0;
    rc->centre[1] = (((l->p[0][1] + l->p[1][1]) + l->p[2][1]) + l->p[3][1]) / 4.0;
    double la = hyp(e[0][0], e[0][1]), lb = hyp(e[1][0], e[1][1]);
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
    if (!profile(r, arg(solid, 0), &ls)) return 0;
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
    if (hyp(dir[0], dir[1]) > 1e-6) goto out;
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
    GSet seen = {0};
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
    if (!seen.v || !seen.used || !els) { free(nopen); goto oom; }

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
        r.unsup_why = NULL;
        int kind = solid ? extruded_element(&r, &m->arena, solid, &rel, cls, scale, &pe->el.placement, &pe->el.generator) : 0;
        if (!kind) {
            const char *shown = has_ent ? buf_cstr(&ent) : cname;
            if (r.unsup_why) unsup(&r, shown, ": body uses ", r.unsup_why);
            else unsup(&r, shown, ": body is not an extruded profile of the C subset (Rust: a mesh)", 0);
            rep->unsupported++;
            buf_free(&ent);
            continue;
        }
        if (kind == 1) rep->parametric++;
        else rep->extruded++;
        if (nopen[i]) {
            unsup(&r, cname, ": openings not cut (Rust keeps them as meshes)", 0);
            rep->unsupported += nopen[i];
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
        buf_puts(L, " extruded polygons, 0 meshes)\n");
        tally_sort(&by_class);
        for (size_t i = 0; i < by_class.n; i++) {
            buf_puts(L, "  ");
            buf_append(L, by_class.v[i].s, by_class.v[i].n);
            buf_puts(L, ": ");
            buf_u64(L, by_class.v[i].count);
            buf_putc(L, '\n');
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
