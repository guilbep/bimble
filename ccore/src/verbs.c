/* See verbs.h. Arguments are untrusted: unknown fields are refused, sizes and id counts bounded.
 * Argument errors copy serde's texts: fields are visited in sorted key order (serde_json's Value
 * map is a BTreeMap), then the first missing required field in declaration order. */
#include "verbs.h"
#include <string.h>
#include "guid.h"
#include "json_r.h"
#include "library.h"

/* ---- f64 maths without libm ----
 * Neither test/run.sh nor build.sh links libm, and freestanding wasm has none. cc_sqrt is exact
 * (IEEE sqrt is correctly rounded, so any correct method gives the same bits). cc_hypot is
 * glibc's (sysdeps/ieee754/dbl-64/e_hypot.c, glibc 2.35+) with its FMA kernel, the one glibc
 * uses where FMA is native (aarch64, where the goldens were made); the FMA is emulated in
 * software (exact product, then round-to-odd, Boldo and Melquiond 2008), so every compiler and
 * wasm give the same bits. Checked natively against glibc 2.43 on aarch64: 0 differences in
 * 2x10^7 random mm pairs; 1 in 2x10^7 random bit patterns, near 2^-511 where the exact product
 * underflows. An x86_64 glibc without FMA uses another kernel
 * and may differ from Rust there in the last bit. Ticket 13 (wasm) must check it bit-exact.
 * No contraction: a*b+c must round twice, as in Rust (tcc never contracts). */
#ifndef __TINYC__
#pragma STDC FP_CONTRACT OFF
#endif

static uint64_t bits_of(double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    return u;
}
static double of_bits(uint64_t u) {
    double x;
    memcpy(&x, &u, 8);
    return x;
}

/* cc_sqrt: the one in scene.c (correctly rounded; one definition for the whole core). */

/* a*b = p + *e exactly (Veltkamp split, Dekker product; no overflow in hypot's range). */
static double two_prod(double a, double b, double *e) {
    double p = a * b;
    double ca = 134217729.0 * a, cb = 134217729.0 * b;
    double ah = ca - (ca - a), al = a - ah, bh = cb - (cb - b), bl = b - bh;
    *e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
    return p;
}

/* a + b = s + *e exactly (Knuth). */
static double two_sum(double a, double b, double *e) {
    double s = a + b, bb = s - a;
    *e = (a - (s - bb)) + (b - bb);
    return s;
}

/* a + b rounded to odd. */
static double add_odd(double a, double b) {
    double e, s = two_sum(a, b, &e);
    uint64_t u = bits_of(s);
    if (e != 0.0 && !(u & 1)) u += (e > 0) == (s > 0) ? 1 : (uint64_t)-1;
    return of_bits(u);
}

/* fma(a, b, c), correctly rounded. */
static double fma_(double a, double b, double c) {
    double ul, uh = two_prod(a, b, &ul);
    double tl, th = two_sum(c, uh, &tl);
    return th + add_odd(tl, ul);
}

static double hypot_kernel(double ax, double ay) {
    double t1 = ay + ay, t2 = ax - ay;
    if (t1 >= ax) return cc_sqrt(fma_(t1, ax, t2 * t2));
    return cc_sqrt(fma_(ax, ax, ay * ay));
}

double cc_hypot(double x, double y) {
    const double SCALE = 0x1p-600, LARGE_VAL = 0x1p+511, TINY_VAL = 0x1p-511, EPS = 0x1p-54;
    int xinf = x - x != 0.0 && x == x, yinf = y - y != 0.0 && y == y;
    if (x - x != 0.0 || y - y != 0.0) { /* not finite */
        if (xinf || yinf) return of_bits(0x7FF0000000000000ull);
        return x + y;
    }
    x = x < 0 ? -x : x;
    y = y < 0 ? -y : y;
    double ax = x < y ? y : x, ay = x < y ? x : y;
    if (ax > LARGE_VAL) {
        if (ay <= ax * EPS) return ax + ay;
        return hypot_kernel(ax * SCALE, ay * SCALE) / SCALE;
    }
    if (ay < TINY_VAL) {
        if (ax >= ay / EPS) return ax + ay;
        return hypot_kernel(ax / SCALE, ay / SCALE) * SCALE;
    }
    if (ay <= ax * EPS) return ax + ay;
    return hypot_kernel(ax, ay);
}

/* ---- argument decoding, serde style ---- */

static int finite_(double x) { return x - x == 0.0; }
static double fabs_(double x) { return x < 0 ? -x : x; }

static int bad(Buf *err, const char *a, const char *b, const char *c) {
    buf_clear(err);
    buf_puts(err, "bad arguments: ");
    if (a) buf_puts(err, a);
    if (b) buf_puts(err, b);
    if (c) buf_puts(err, c);
    return -1;
}

/* "invalid type: <unexpected>, expected <what>". */
static int bad_type(Buf *err, const JrVal *v, const char *expected) {
    buf_clear(err);
    buf_puts(err, "bad arguments: invalid type: ");
    switch (v->type) {
    case JR_NULL: buf_puts(err, "null"); break;
    case JR_BOOL: buf_puts(err, v->b ? "boolean `true`" : "boolean `false`"); break;
    case JR_NUM:
        buf_puts(err, v->num_int ? "integer `" : "floating point `");
        buf_append(err, v->s, v->n < 64 ? v->n : 64);
        buf_putc(err, '`');
        break;
    case JR_STR:
        buf_puts(err, "string ");
        cc_buf_debug_str(err, v->s, v->n < 200 ? v->n : 200);
        break;
    case JR_ARR: buf_puts(err, "sequence"); break;
    default: buf_puts(err, "map"); break;
    }
    buf_puts(err, ", expected ");
    buf_puts(err, expected);
    return -1;
}

/* serde's OneOf: "`a`", "`a` or `b`", "one of `a`, `b`, `c`". */
static void one_of(Buf *b, const char *const *names, int n) {
    if (n == 0) {
        buf_puts(b, "there are no fields");
        return;
    }
    if (n > 2) buf_puts(b, "one of ");
    for (int i = 0; i < n; i++) {
        if (i) buf_puts(b, n == 2 ? " or " : ", ");
        buf_putc(b, '`');
        buf_puts(b, names[i]);
        buf_putc(b, '`');
    }
}

static int d_f64(Buf *err, const JrVal *v, double *out) {
    if (v->type != JR_NUM) return bad_type(err, v, "f64");
    *out = v->num;
    return 0;
}

static int d_vec(Buf *err, const JrVal *v, double *out, size_t n) {
    if (v->type != JR_ARR) return bad_type(err, v, n == 2 ? "an array of length 2" : "an array of length 3");
    if (v->n != n) {
        buf_clear(err);
        buf_puts(err, "bad arguments: invalid length ");
        buf_u64(err, v->n);
        buf_puts(err, v->n < n ? (n == 2 ? ", expected an array of length 2" : ", expected an array of length 3")
                               : ", expected fewer elements in array");
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        if (d_f64(err, &v->items[i], &out[i])) return -1;
    return 0;
}

static int d_gid(Buf *err, const JrVal *v, cc_gid *out) {
    if (v->type != JR_STR) return bad_type(err, v, "a string");
    cc_guid g;
    const char *msg = NULL;
    if (cc_guid_parse(v->s, v->n, &g, &msg)) return bad(err, msg, 0, 0);
    memcpy(out->c, g.c, 22);
    return 0;
}

static int d_str(Buf *err, const JrVal *v, cc_str *out) {
    if (v->type != JR_STR) return bad_type(err, v, "a string");
    out->p = v->s;
    out->n = v->n;
    return 0;
}

typedef struct {
    cc_gid *v;
    size_t n;
} Ids;

static int d_ids(Buf *err, const JrVal *v, Arena *a, Ids *out) {
    if (v->type != JR_ARR) return bad_type(err, v, "a sequence");
    out->n = v->n;
    out->v = v->n ? arena_alloc(a, v->n * sizeof *out->v, 8) : NULL;
    if (v->n && !out->v) return bad(err, "out of memory", 0, 0);
    for (size_t i = 0; i < v->n; i++)
        if (d_gid(err, &v->items[i], &out->v[i])) return -1;
    return 0;
}

/* The members of an object in sorted key order, the last of equal keys kept (a Value map).
 * idx[] gets member indices; returns the count. Insertion sort: argument objects are small. */
static size_t sorted_members(const JrVal *obj, size_t *idx) {
    size_t n = 0;
    for (size_t i = 0; i < obj->n; i++) {
        const JrMember *mi = &obj->members[i];
        size_t j = 0;
        int dup = 0;
        for (; j < n; j++) {
            const JrMember *mj = &obj->members[idx[j]];
            size_t k = mi->key_n < mj->key_n ? mi->key_n : mj->key_n;
            int c = memcmp(mi->key, mj->key, k);
            if (!c) c = mi->key_n < mj->key_n ? -1 : mi->key_n > mj->key_n;
            if (c == 0) { dup = 1; break; }
            if (c < 0) break;
        }
        if (dup) { idx[j] = i; continue; }
        memmove(idx + j + 1, idx + j, (n - j) * sizeof *idx);
        idx[j] = i;
        n++;
    }
    return n;
}

#define MAX_ARG_MEMBERS 64

/* A struct with deny_unknown_fields (or not): out[i] = the field's value, NULL when absent.
 * `field` (may be NULL) decodes each present field in sorted key order. Required fields are
 * those with required[i]. */
typedef int (*FieldFn)(void *ctx, int i, const JrVal *v, Buf *err);
static int d_struct(Buf *err, const JrVal *obj, const char *strct, const char *const *names, int nn,
                    const int *required, int deny, FieldFn field, void *ctx) {
    if (obj->type != JR_OBJ) {
        char what[96] = "struct ";
        size_t k = strlen(strct);
        if (k > 80) k = 80;
        memcpy(what + 7, strct, k);
        what[7 + k] = 0;
        return bad_type(err, obj, what);
    }
    size_t cap = obj->n ? obj->n : 1;
    size_t stack_idx[MAX_ARG_MEMBERS];
    size_t *idx = cap <= MAX_ARG_MEMBERS ? stack_idx : NULL;
    if (!idx) return bad(err, "too many fields", 0, 0); /* argument objects are small */
    size_t n = sorted_members(obj, idx);
    int seen[16] = {0};
    for (size_t s = 0; s < n; s++) {
        const JrMember *m = &obj->members[idx[s]];
        int i = 0;
        while (i < nn && !jr_key_is(m, names[i])) i++;
        if (i == nn) {
            if (!deny) continue;
            buf_clear(err);
            buf_puts(err, "bad arguments: unknown field `");
            buf_append(err, m->key, m->key_n < 200 ? m->key_n : 200);
            buf_puts(err, "`, expected ");
            one_of(err, names, nn);
            return -1;
        }
        seen[i] = 1;
        if (field && field(ctx, i, &m->val, err)) return -1;
    }
    for (int i = 0; i < nn; i++)
        if (required && required[i] && !seen[i]) return bad(err, "missing field `", names[i], "`");
    return 0;
}

/* ---- verb arguments ---- */

typedef enum { K_COLUMN, K_BEAM, K_WALL, K_SLAB } Kind;
static const char *const KIND_NAMES[] = {"column", "beam", "wall", "slab"};
static const char *const KIND_LABELS[] = {"Column", "Beam", "Wall", "Slab"};
static const char *const KIND_PREFIX[] = {"P", "B", "M", "D"};
static const cc_ifc_class KIND_CLASS[] = {CC_IFC_COLUMN, CC_IFC_BEAM, CC_IFC_WALL, CC_IFC_SLAB};

typedef struct {
    double column, beam_width, beam_height, wall, slab;
} Sizes;

typedef struct {
    Kind kind;
    cc_gid storey;
    double a[2], b[2];
    int has_b;
    Sizes sizes;
    cc_str material;
    int has_material;
} PlaceArgs;

static int sizes_field(void *ctx, int i, const JrVal *v, Buf *err) {
    double *f[] = {&((Sizes *)ctx)->column, &((Sizes *)ctx)->beam_width, &((Sizes *)ctx)->beam_height,
                   &((Sizes *)ctx)->wall, &((Sizes *)ctx)->slab};
    return d_f64(err, v, f[i]);
}

static int place_field(void *ctx, int i, const JrVal *v, Buf *err) {
    PlaceArgs *p = ctx;
    switch (i) {
    case 0: { /* kind */
        if (v->type != JR_STR) return bad_type(err, v, "enum Kind");
        for (int k = 0; k < 4; k++)
            if (strlen(KIND_NAMES[k]) == v->n && memcmp(KIND_NAMES[k], v->s, v->n) == 0) {
                p->kind = (Kind)k;
                return 0;
            }
        buf_clear(err);
        buf_puts(err, "bad arguments: unknown variant `");
        buf_append(err, v->s, v->n < 200 ? v->n : 200);
        buf_puts(err, "`, expected ");
        one_of(err, KIND_NAMES, 4);
        return -1;
    }
    case 1: return d_gid(err, v, &p->storey);
    case 2: return d_vec(err, v, p->a, 2);
    case 3:
        if (v->type == JR_NULL) return 0;
        p->has_b = 1;
        return d_vec(err, v, p->b, 2);
    case 4: {
        if (v->type == JR_NULL) return 0;
        static const char *const F[] = {"column", "beam_width", "beam_height", "wall", "slab"};
        return d_struct(err, v, "Sizes", F, 5, NULL, 0, sizes_field, &p->sizes);
    }
    case 5:
        if (v->type == JR_NULL) return 0;
        p->has_material = 1;
        return d_str(err, v, &p->material);
    default: return -1;
    }
}

typedef struct {
    Ids ids;
    double d[3];
    Arena *a;
} MoveArgs;

static int move_field(void *ctx, int i, const JrVal *v, Buf *err) {
    MoveArgs *p = ctx;
    return i == 0 ? d_ids(err, v, p->a, &p->ids) : d_vec(err, v, p->d, 3);
}

typedef struct {
    Ids ids;
    cc_str material;
    Arena *a;
} MatArgs;

static int mat_field(void *ctx, int i, const JrVal *v, Buf *err) {
    MatArgs *p = ctx;
    return i == 0 ? d_ids(err, v, p->a, &p->ids) : d_str(err, v, &p->material);
}

typedef struct {
    cc_gid id;
    cc_str text;
} NoteArgs;

static int note_field(void *ctx, int i, const JrVal *v, Buf *err) {
    NoteArgs *p = ctx;
    return i == 0 ? d_gid(err, v, &p->id) : d_str(err, v, &p->text);
}

typedef struct {
    cc_gid id;
    int has[5];
    double val[5]; /* width, height, length, depth, thickness */
} GenArgs;

static int params_field(void *ctx, int i, const JrVal *v, Buf *err) {
    GenArgs *p = ctx;
    if (v->type == JR_NULL) return 0;
    p->has[i] = 1;
    return d_f64(err, v, &p->val[i]);
}

static int gen_field(void *ctx, int i, const JrVal *v, Buf *err) {
    static const char *const F[] = {"width", "height", "length", "depth", "thickness"};
    GenArgs *p = ctx;
    if (i == 0) return d_gid(err, v, &p->id);
    return d_struct(err, v, "GeneratorParams", F, 5, NULL, 1, params_field, p);
}

/* ---- verbs.rs ---- */

static int fail(Buf *err, const char *msg) {
    buf_clear(err);
    buf_puts(err, msg);
    return -1;
}

/* verbs::size: "{what}: {v} is not a size within [10, 100000] mm". */
static int size_check(Buf *err, const char *what, double v) {
    if (v >= 10.0 && v <= 100000.0) return 0;
    buf_clear(err);
    buf_puts(err, what);
    buf_puts(err, ": ");
    cc_buf_display_f64(err, v);
    buf_puts(err, " is not a size within [10, 100000] mm");
    return -1;
}

/* dispatch::mm. */
static int mm_check(Buf *err, const char *what, const double *v, int n) {
    for (int i = 0; i < n; i++)
        if (!finite_(v[i]) || fabs_(v[i]) > CC_MAX_MM) {
            buf_clear(err);
            buf_puts(err, what);
            buf_puts(err, ": ");
            cc_buf_display_f64(err, v[i]);
            buf_puts(err, " is not a finite value within \xc2\xb1" "10000000 mm");
            return -1;
        }
    return 0;
}

static int no_element(Buf *err, const cc_gid *id) {
    buf_clear(err);
    buf_puts(err, "no element GlobalId(");
    cc_buf_gid(err, id);
    buf_putc(err, ')');
    return -1;
}

static double storey_height(const cc_model *m, const cc_gid *storey) {
    const cc_storey *s = cc_model_storey(m, storey);
    if (!s) return 3000.0;
    int any = 0;
    double best = 0.0;
    for (size_t i = 0; i < m->nstoreys; i++) {
        double d = m->storeys[i].elevation - s->elevation;
        if (d > 1.0 && (!any || d < best)) best = d, any = 1; /* fold with f64::min */
    }
    return any ? best : 3000.0;
}

static int cstr_eq(cc_str s, const char *c) { return s.n == strlen(c) && (s.n == 0 || memcmp(s.p, c, s.n) == 0); }
static int str_eq(cc_str a, cc_str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }

/* verbs::material: the material to use and, maybe, the AddMaterial that makes it. */
typedef struct {
    cc_gid id;
    int add; /* `mat` is to be added */
    cc_material mat;
} Mat;

static int material(const cc_model *m, const cc_str *name, Mat *out, Buf *err) {
    memset(out, 0, sizeof *out);
    if (!name) {
        for (size_t i = 0; i < m->nmaterials; i++)
            if (m->materials[i].category == CC_CAT_CONCRETE) {
                out->id = m->materials[i].id;
                return 0;
            }
        const cc_library_material *lm = cc_library_lookup("C30/37", 6);
        out->mat = cc_library_to_material(lm); /* GlobalId::new(), name C30/37, concrete, props */
        out->id = out->mat.id;
        out->add = 1;
        return 0;
    }
    for (size_t i = 0; i < m->nmaterials; i++)
        if (str_eq(m->materials[i].name, *name)) {
            out->id = m->materials[i].id;
            return 0;
        }
    const cc_library_material *lm = cc_library_lookup(name->p, name->n);
    if (!lm) {
        buf_clear(err);
        buf_puts(err, "unknown material: ");
        buf_append(err, name->p, name->n);
        return -1;
    }
    for (size_t i = 0; i < m->nmaterials; i++)
        if (cstr_eq(m->materials[i].name, lm->name) && m->materials[i].category == lm->category) {
            out->id = m->materials[i].id;
            return 0;
        }
    out->mat = cc_library_to_material(lm);
    out->id = out->mat.id;
    out->add = 1;
    return 0;
}

/* next_name: P1, P2, … the first not used by an element. Written into the arena. */
static cc_str next_name(const cc_model *m, const char *prefix, Arena *a) {
    char buf[32];
    size_t pn = strlen(prefix);
    memcpy(buf, prefix, pn);
    for (uint64_t k = 1;; k++) {
        char digits[24];
        int nd = 0;
        for (uint64_t v = k; v; v /= 10) digits[nd++] = (char)('0' + v % 10);
        size_t n = pn;
        while (nd) buf[n++] = digits[--nd];
        cc_str s = {buf, n};
        int used = 0;
        for (size_t i = 0; i < m->nelements && !used; i++) used = str_eq(m->elements[i].name, s);
        if (!used) {
            cc_str out = {arena_dup(a, buf, n), n};
            return out;
        }
    }
}

static int push_add_material(cc_cmds *l, const Mat *mat, Buf *err) {
    if (!mat->add) return 0;
    cc_command *c = cc_cmds_push(l, CC_CMD_ADD_MATERIAL);
    if (!c) return fail(err, "out of memory");
    c->material = mat->mat;
    return 0;
}

static int add_element(const cc_model *m, Kind kind, const cc_gid *storey, const Mat *mat, const cc_placement *p,
                       const cc_generator *g, cc_cmds *l, Buf *err) {
    cc_element e;
    memset(&e, 0, sizeof e);
    cc_guid id = cc_guid_new();
    memcpy(e.id.c, id.c, 22);
    e.cls = KIND_CLASS[kind];
    e.name = next_name(m, KIND_PREFIX[kind], &l->arena);
    if (!e.name.p) return fail(err, "out of memory");
    e.storey = *storey;
    e.material = mat->id;
    e.placement = *p;
    e.generator = *g;
    if (push_add_material(l, mat, err)) return -1;
    cc_command *c = cc_cmds_push(l, CC_CMD_ADD_ELEMENT);
    if (!c) return fail(err, "out of memory");
    c->element = e;
    return 0;
}

static cc_placement placement_at(double x, double y, double z) {
    cc_placement p = {{x, y, z}, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}};
    return p;
}

static double cross(const double *o, const double *a, const double *b) {
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}
static double min_(double a, double b) { return a < b ? a : (b < a ? b : (a != a ? b : a)); }
static double max_(double a, double b) { return a > b ? a : (b > a ? b : (a != a ? b : a)); }

static int on_seg(const double *a, const double *b, const double *q) {
    return cross(a, b, q) == 0.0 && q[0] >= min_(a[0], b[0]) && q[0] <= max_(a[0], b[0]) &&
           q[1] >= min_(a[1], b[1]) && q[1] <= max_(a[1], b[1]);
}

static int meet(const double *a, const double *b, const double *c, const double *d) {
    double d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c), d4 = cross(a, b, d);
    return (d1 * d2 < 0.0 && d3 * d4 < 0.0) || on_seg(c, d, a) || on_seg(c, d, b) || on_seg(a, b, c) ||
           on_seg(a, b, d);
}

static int pt_eq(const double *a, const double *b) { return a[0] == b[0] && a[1] == b[1]; }

/* verbs::is_simple. */
static int is_simple(double (*p)[2], size_t n) {
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++) {
            const double *a = p[i], *b = p[(i + 1) % n], *c = p[j], *d = p[(j + 1) % n];
            if (j == i + 1 || (i == 0 && j == n - 1)) {
                const double *shared, *x, *y;
                if (j == i + 1) shared = b, x = a, y = d;
                else shared = a, x = b, y = c;
                if (pt_eq(shared, x) || pt_eq(shared, y) || (cross(shared, x, y) == 0.0 && on_seg(shared, x, y)) ||
                    (cross(shared, x, y) == 0.0 && on_seg(shared, y, x)))
                    return 0;
                continue;
            }
            if (meet(a, b, c, d)) return 0;
        }
    return 1;
}

/* verbs::new_slab. pts are copied into the arena. */
static int new_slab(const cc_model *m, const cc_gid *storey, double (*pts)[2], size_t n, const Sizes *sizes,
                    const cc_str *mat_name, cc_cmds *l, Buf *err) {
    if (!cc_model_storey(m, storey)) return fail(err, "choose a storey");
    if (n < 3) return fail(err, "a slab outline needs at least 3 points");
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < 2; k++)
            if (!finite_(pts[i][k]) || fabs_(pts[i][k]) > CC_MAX_MM)
                return fail(err, "a slab outline point is out of range");
    if (!is_simple(pts, n)) return fail(err, "the slab outline crosses itself");
    double area2 = -0.0;
    for (size_t i = 0; i < n; i++) {
        const double *p = pts[i], *q = pts[(i + 1) % n];
        area2 += p[0] * q[1] - q[0] * p[1];
    }
    if (fabs_(area2) < 2.0 * 100.0) return fail(err, "the slab outline is too small");
    double h = storey_height(m, storey);
    Mat mat;
    if (material(m, mat_name, &mat, err)) return -1;
    double x0 = of_bits(0x7FF0000000000000ull), y0 = x0; /* fold from f64::INFINITY */
    for (size_t i = 0; i < n; i++) x0 = min_(x0, pts[i][0]), y0 = min_(y0, pts[i][1]);
    double(*o)[2] = arena_alloc(&l->arena, n * sizeof *o, 8);
    if (!o) return fail(err, "out of memory");
    for (size_t i = 0; i < n; i++) {
        size_t k = area2 < 0.0 ? n - 1 - i : i;
        o[i][0] = pts[k][0] - x0 + 0.0;
        o[i][1] = pts[k][1] - y0 + 0.0;
    }
    cc_placement p = placement_at(x0, y0, h - sizes->slab);
    cc_generator g;
    memset(&g, 0, sizeof g);
    g.kind = CC_GEN_SLAB;
    g.u.slab.outline.pts = o;
    g.u.slab.outline.n = n;
    g.u.slab.thickness = sizes->slab;
    return add_element(m, K_SLAB, storey, &mat, &p, &g, l, err);
}

/* verbs::new_element. */
static int new_element(const cc_model *m, Kind kind, const cc_gid *storey, const double *a, const double *b,
                       const Sizes *sizes, const cc_str *mat_name, cc_cmds *l, Buf *err) {
    if (!cc_model_storey(m, storey)) return fail(err, "choose a storey");
    double h = storey_height(m, storey);
    Mat mat;
    if (material(m, mat_name, &mat, err)) return -1;
    double dx = b[0] - a[0], dy = b[1] - a[1];
    double len = cc_hypot(dx, dy);
    if (kind != K_COLUMN && len < 10.0) return fail(err, "the two points are too close");
    double dir[3] = {dx / len, dy / len, 0.0};
    cc_placement p;
    cc_generator g;
    memset(&g, 0, sizeof g);
    switch (kind) {
    case K_COLUMN:
        p = placement_at(a[0], a[1], 0.0);
        g.kind = CC_GEN_RECT_COLUMN;
        g.u.col.width = sizes->column, g.u.col.depth = sizes->column, g.u.col.height = h;
        break;
    case K_BEAM: {
        cc_placement q = {{a[0], a[1], h - sizes->beam_height / 2.0}, {dir[0], dir[1], dir[2]}, {-dir[1], dir[0], 0.0}};
        p = q;
        g.kind = CC_GEN_RECT_BEAM;
        g.u.beam.width = sizes->beam_width, g.u.beam.height = sizes->beam_height, g.u.beam.length = len;
        break;
    }
    case K_WALL: {
        cc_placement q = {{a[0], a[1], 0.0}, {0.0, 0.0, 1.0}, {dir[0], dir[1], dir[2]}};
        p = q;
        g.kind = CC_GEN_WALL;
        g.u.wall.length = len, g.u.wall.thickness = sizes->wall, g.u.wall.height = h;
        break;
    }
    default: { /* K_SLAB */
        double x0 = min_(a[0], b[0]), x1 = max_(a[0], b[0]);
        double y0 = min_(a[1], b[1]), y1 = max_(a[1], b[1]);
        if (x1 - x0 < 10.0 || y1 - y0 < 10.0) return fail(err, "the slab rectangle is too thin");
        double pts[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        return new_slab(m, storey, pts, 4, sizes, mat_name, l, err);
    }
    }
    return add_element(m, kind, storey, &mat, &p, &g, l, err);
}

/* ---- dispatch::run ---- */

static int run_place(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"kind", "storey", "a", "b", "sizes", "material"};
    static const int REQ[] = {1, 1, 1, 0, 0, 0};
    PlaceArgs p;
    memset(&p, 0, sizeof p);
    Sizes def = {300.0, 250.0, 500.0, 200.0, 250.0};
    p.sizes = def;
    if (d_struct(err, args, "PlaceArgs", F, 6, REQ, 1, place_field, &p)) return -1;
    if (mm_check(err, "a", p.a, 2)) return -1;
    double b[2] = {p.has_b ? p.b[0] : p.a[0], p.has_b ? p.b[1] : p.a[1]};
    if (mm_check(err, "b", b, 2)) return -1;
    double s[5] = {p.sizes.column, p.sizes.beam_width, p.sizes.beam_height, p.sizes.wall, p.sizes.slab};
    for (int i = 0; i < 5; i++)
        if (size_check(err, "sizes", s[i])) return -1;
    if (!p.has_b && p.kind != K_COLUMN) {
        buf_clear(err);
        buf_puts(err, KIND_LABELS[p.kind]);
        buf_puts(err, ": b is required");
        return -1;
    }
    return new_element(m, p.kind, &p.storey, p.a, b, &p.sizes, p.has_material ? &p.material : NULL, l, err);
}

static int run_move(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"ids", "d"};
    static const int REQ[] = {1, 1};
    MoveArgs a;
    memset(&a, 0, sizeof a);
    a.a = &l->arena;
    if (d_struct(err, args, "MoveArgs", F, 2, REQ, 1, move_field, &a)) return -1;
    if (mm_check(err, "d", a.d, 3)) return -1;
    for (size_t i = 0; i < a.ids.n; i++) {
        const cc_element *e = cc_model_element(m, &a.ids.v[i]);
        if (!e) continue;
        cc_command *c = cc_cmds_push(l, CC_CMD_SET_PLACEMENT);
        if (!c) return fail(err, "out of memory");
        c->id = e->id;
        c->placement = e->placement;
        for (int k = 0; k < 3; k++) c->placement.origin[k] += a.d[k];
    }
    return 0;
}

static int ids_field(void *ctx, int i, const JrVal *v, Buf *err) {
    (void)i;
    MoveArgs *p = ctx;
    return d_ids(err, v, p->a, &p->ids);
}

static int run_delete(const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"ids"};
    static const int REQ[] = {1};
    MoveArgs a;
    memset(&a, 0, sizeof a);
    a.a = &l->arena;
    if (d_struct(err, args, "IdsArgs", F, 1, REQ, 1, ids_field, &a)) return -1;
    for (size_t i = 0; i < a.ids.n; i++) {
        cc_command *c = cc_cmds_push(l, CC_CMD_REMOVE_ELEMENT);
        if (!c) return fail(err, "out of memory");
        c->id = a.ids.v[i];
    }
    return 0;
}

static int run_set_generator(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"id", "params"};
    static const int REQ[] = {1, 1};
    static const char *const K[] = {"width", "height", "length", "depth", "thickness"};
    GenArgs a;
    memset(&a, 0, sizeof a);
    if (d_struct(err, args, "SetGeneratorArgs", F, 2, REQ, 1, gen_field, &a)) return -1;
    const cc_element *e = cc_model_element(m, &a.id);
    if (!e) return no_element(err, &a.id);
    cc_generator g = e->generator;
    int n = 0;
    for (int k = 0; k < 5; k++) {
        if (!a.has[k]) continue;
        if (size_check(err, K[k], a.val[k])) return -1;
        /* The generator's serde form: a number field of that name? */
        double *slot = NULL;
        switch (g.kind) {
        case CC_GEN_RECT_COLUMN:
        case CC_GEN_PAD_FOOTING:
            slot = k == 0 ? &g.u.col.width : k == 3 ? &g.u.col.depth : k == 1 ? &g.u.col.height : NULL;
            break;
        case CC_GEN_RECT_BEAM:
            slot = k == 0 ? &g.u.beam.width : k == 1 ? &g.u.beam.height : k == 2 ? &g.u.beam.length : NULL;
            break;
        case CC_GEN_WALL:
            slot = k == 2 ? &g.u.wall.length : k == 4 ? &g.u.wall.thickness : k == 1 ? &g.u.wall.height : NULL;
            break;
        case CC_GEN_SLAB: slot = k == 4 ? &g.u.slab.thickness : NULL; break;
        case CC_GEN_EXTRUDED_POLYGON: slot = k == 1 ? &g.u.poly.height : NULL; break;
        default: break;
        }
        if (!slot) {
            buf_clear(err);
            buf_puts(err, K[k]);
            buf_puts(err, ": not a size of this \"");
            buf_puts(err, cc_gen_kind_name(g.kind));
            buf_putc(err, '"');
            return -1;
        }
        *slot = a.val[k];
        n++;
    }
    if (!n) return fail(err, "params: no size given");
    /* The same sizes again: no command (f64 ==, so -0.0 equals 0.0, as Rust's PartialEq). */
    const cc_generator *o = &e->generator;
    int same = 0;
    switch (g.kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
        same = g.u.col.width == o->u.col.width && g.u.col.depth == o->u.col.depth && g.u.col.height == o->u.col.height;
        break;
    case CC_GEN_RECT_BEAM:
        same = g.u.beam.width == o->u.beam.width && g.u.beam.height == o->u.beam.height &&
               g.u.beam.length == o->u.beam.length;
        break;
    case CC_GEN_WALL:
        same = g.u.wall.length == o->u.wall.length && g.u.wall.thickness == o->u.wall.thickness &&
               g.u.wall.height == o->u.wall.height;
        break;
    case CC_GEN_SLAB: same = g.u.slab.thickness == o->u.slab.thickness; break; /* same outline */
    case CC_GEN_EXTRUDED_POLYGON: same = g.u.poly.height == o->u.poly.height; break;
    default: same = 1; break;
    }
    if (same) return 0;
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_GENERATOR);
    if (!c) return fail(err, "out of memory");
    c->id = a.id;
    c->generator = g;
    return 0;
}

static int run_set_material(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"ids", "material"};
    static const int REQ[] = {1, 1};
    MatArgs a;
    memset(&a, 0, sizeof a);
    a.a = &l->arena;
    if (d_struct(err, args, "SetMaterialArgs", F, 2, REQ, 1, mat_field, &a)) return -1;
    Mat mat;
    if (material(m, &a.material, &mat, err)) return -1;
    size_t first = l->n;
    for (size_t i = 0; i < a.ids.n; i++) {
        const cc_element *e = cc_model_element(m, &a.ids.v[i]);
        if (!e) {
            l->n = first;
            return no_element(err, &a.ids.v[i]);
        }
        if (cc_gid_cmp(&e->material, &mat.id) == 0) continue;
        cc_command *c = cc_cmds_push(l, CC_CMD_SET_ELEMENT_MATERIAL);
        if (!c) return fail(err, "out of memory");
        c->id = a.ids.v[i];
        c->material_id = mat.id;
    }
    if (l->n > first && mat.add) { /* the AddMaterial goes first */
        if (!cc_cmds_push(l, CC_CMD_ADD_MATERIAL)) return fail(err, "out of memory");
        memmove(&l->v[first + 1], &l->v[first], (l->n - 1 - first) * sizeof *l->v);
        memset(&l->v[first], 0, sizeof *l->v);
        l->v[first].op = CC_CMD_ADD_MATERIAL;
        l->v[first].material = mat.mat;
    }
    return 0;
}

static int run_set_note(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"id", "text"};
    static const int REQ[] = {1, 1};
    NoteArgs a;
    memset(&a, 0, sizeof a);
    if (d_struct(err, args, "SetNoteArgs", F, 2, REQ, 1, note_field, &a)) return -1;
    const cc_element *e = cc_model_element(m, &a.id);
    if (!e) return no_element(err, &a.id);
    cc_str text = cc_str_trim(a.text);
    const cc_str *old = NULL;
    for (size_t i = 0; i < e->nproperties; i++)
        if (cstr_eq(e->properties[i].key, "Note")) old = &e->properties[i].value;
    if (text.n == 0 ? old == NULL : (old && str_eq(*old, text))) return 0;
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_PROPERTY);
    if (!c) return fail(err, "out of memory");
    c->id = a.id;
    c->key.p = "Note";
    c->key.n = 4;
    if (text.n) {
        c->value = text;
        c->has_value = 1;
    }
    return 0;
}

/* strcmp(a, b) == 0 (freestanding wasm has no strcmp). */
static int streq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static const char *const RUST_VERBS[] = {"place", "move", "copy", "delete", "set_generator", "set_view",
                                         "set_schedule", "set_material", "set_note", "set_project_field",
                                         "rotate", "mirror", "add_sheet", "set_sheet"};

int cc_verb_run(const cc_model *m, const char *name, const char *args, size_t n, cc_cmds *out, Buf *err) {
    size_t nn = strlen(name);
    int known = -1;
    for (int i = 0; i < 14; i++)
        if (streq(RUST_VERBS[i], name)) known = i;
    if (known < 0) {
        buf_clear(err);
        buf_puts(err, "unknown verb ");
        cc_buf_debug_str(err, name, nn);
        buf_puts(err, "; known: ");
        for (int i = 0; i < 14; i++) {
            if (i) buf_puts(err, ", ");
            buf_puts(err, RUST_VERBS[i]);
        }
        return -1;
    }
    /* Strings of the arguments (ids, a note's text) end up in commands: parse into out's arena. */
    JrError je;
    const JrVal *v = NULL;
    JrVal empty;
    memset(&empty, 0, sizeof empty);
    empty.type = JR_OBJ;
    if (n) {
        v = jr_parse(&out->arena, args, n, &je);
        if (!v) return bad(err, je.msg, 0, 0);
    }
    if (!v || v->type == JR_NULL) v = &empty;
    /* dispatch::parse: a huge id list costs nothing to refuse. */
    if (v->type == JR_OBJ) {
        const JrVal *ids = NULL;
        for (size_t i = 0; i < v->n; i++)
            if (jr_key_is(&v->members[i], "ids")) ids = &v->members[i].val;
        if (ids && ids->type == JR_ARR && ids->n > CC_MAX_IDS) {
            buf_clear(err);
            buf_u64(err, ids->n);
            buf_puts(err, " ids: at most 10000 per call");
            return -1;
        }
    }
    size_t before = out->n;
    int rc;
    if (streq(name, "place")) rc = run_place(m, v, out, err);
    else if (streq(name, "move")) rc = run_move(m, v, out, err);
    else if (streq(name, "delete")) rc = run_delete(v, out, err);
    else if (streq(name, "set_generator")) rc = run_set_generator(m, v, out, err);
    else if (streq(name, "set_material")) rc = run_set_material(m, v, out, err);
    else if (streq(name, "set_note")) rc = run_set_note(m, v, out, err);
    else {
        buf_clear(err);
        buf_puts(err, "the verb ");
        cc_buf_debug_str(err, name, nn);
        buf_puts(err, " is not in the C core");
        return -1;
    }
    if (rc) out->n = before; /* nothing half made */
    return rc;
}
