/* See verbs.h. Arguments are untrusted: unknown fields are refused, sizes and id counts bounded.
 * Argument errors copy serde's texts: fields are visited in sorted key order (serde_json's Value
 * map is a BTreeMap), then the first missing required field in declaration order. */
#include "verbs.h"
#include <stdlib.h>
#include <string.h>
#include "ifc_math.h"
#include "json_w.h"
#include "verb_table.h"
#include "view_io.h"
#include "guid.h"
#include "json_r.h"
#include "library.h"
#include "annot.h"

#ifndef __TINYC__ /* tcc never contracts and warns on the pragma */
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#endif

static double of_bits(uint64_t u) {
    double x;
    memcpy(&x, &u, 8);
    return x;
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

/* ---- SLAB1: verbs::new_slab for the place tool's polygon (verbs.h) ---- */
int cc_verb_new_slab(const cc_model *m, const cc_gid *storey, const double (*pts)[2], size_t n, const double sizes[5],
                     const char *material_name, cc_cmds *out, Buf *err) {
    Sizes sz = {sizes[0], sizes[1], sizes[2], sizes[3], sizes[4]};
    cc_str name = {material_name, material_name ? strlen(material_name) : 0};
    return new_slab(m, storey, (double (*)[2])pts, n, &sz, material_name ? &name : NULL, out, err);
}
/* ---- end SLAB1 ---- */

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

int cc_vh_place(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
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

int cc_vh_move(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
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

int cc_vh_delete(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    (void)m;
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

int cc_vh_set_generator(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
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

int cc_vh_set_material(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
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

int cc_vh_set_note(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
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

/* A C addition (verb_table.h): Command::Rename of one element; the same name is no step. The
 * blank name check is Command::apply's ("the name is empty"), run by the history. */
int cc_vh_rename(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"id", "name"};
    static const int REQ[] = {1, 1};
    NoteArgs a; /* {id, a string} */
    memset(&a, 0, sizeof a);
    if (d_struct(err, args, "RenameArgs", F, 2, REQ, 1, note_field, &a)) return -1;
    const cc_element *e = cc_model_element(m, &a.id);
    if (!e) return no_element(err, &a.id);
    if (str_eq(e->name, a.text)) return 0;
    cc_command *c = cc_cmds_push(l, CC_CMD_RENAME);
    if (!c) return fail(err, "out of memory");
    c->id = a.id;
    c->name = a.text;
    return 0;
}

/* ---- more argument decoding ---- */

static int d_bool(Buf *err, const JrVal *v, int *out) {
    if (v->type != JR_BOOL) return bad_type(err, v, "a boolean");
    *out = v->b;
    return 0;
}

/* u32 from a JSON number: serde's texts for a float, a negative or a too large integer. */
static int d_u32(Buf *err, const JrVal *v, uint32_t *out) {
    if (v->type != JR_NUM || !v->num_int) return bad_type(err, v, "u32");
    uint64_t x = 0;
    int big = 0;
    for (size_t i = v->num_neg; i < v->n; i++) {
        if (x > 1000000000000ull) big = 1;
        else x = x * 10 + (uint64_t)(v->s[i] - '0');
    }
    if ((v->num_neg && x) || big || x > 0xFFFFFFFFull) {
        buf_clear(err);
        buf_puts(err, "bad arguments: invalid value: integer `");
        buf_append(err, v->s, v->n < 64 ? v->n : 64);
        buf_puts(err, "`, expected u32");
        return -1;
    }
    *out = (uint32_t)x;
    return 0;
}

/* A unit variant by name: "unknown variant `x`, expected one of …". */
static int d_variant(Buf *err, const JrVal *v, const char *enum_name, const char *const *names, int n, int *out) {
    if (v->type != JR_STR) {
        char what[64] = "enum ";
        size_t k = strlen(enum_name);
        memcpy(what + 5, enum_name, k + 1);
        return bad_type(err, v, what);
    }
    for (int i = 0; i < n; i++)
        if (strlen(names[i]) == v->n && memcmp(names[i], v->s, v->n) == 0) {
            *out = i;
            return 0;
        }
    buf_clear(err);
    buf_puts(err, "bad arguments: unknown variant `");
    buf_append(err, v->s, v->n < 200 ? v->n : 200);
    buf_puts(err, "`, expected ");
    one_of(err, names, n);
    return -1;
}

/* Characters of UTF-8 text (str::chars().count()). */
static size_t nchars(cc_str s) {
    size_t n = 0;
    for (size_t i = 0; i < s.n; i++) n += ((unsigned char)s.p[i] & 0xC0) != 0x80;
    return n;
}

static cc_str dup_str(Arena *a, const char *p, size_t n) {
    cc_str s = {n ? arena_dup(a, p, n) : "", n};
    return s;
}

static cc_str cat2(Arena *a, const char *x, cc_str y) {
    size_t k = strlen(x);
    char *p = arena_alloc(a, k + y.n + 1, 1);
    cc_str s = {p, 0};
    if (!p) return s;
    memcpy(p, x, k);
    if (y.n) memcpy(p + k, y.p, y.n);
    s.n = k + y.n;
    return s;
}

/* ---- copy (verbs.rs copy_by) ---- */

static int str_cmp(cc_str a, cc_str b) {
    size_t k = a.n < b.n ? a.n : b.n;
    int c = k ? memcmp(a.p, b.p, k) : 0;
    return c ? c : (a.n < b.n ? -1 : a.n > b.n);
}
static int str_qcmp(const void *x, const void *y) { return str_cmp(*(const cc_str *)x, *(const cc_str *)y); }

typedef struct {
    cc_str *v;
    size_t n, cap;
} Names;

static int names_has(const Names *sorted, const Names *added, cc_str s) {
    size_t lo = 0, hi = sorted->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = str_cmp(sorted->v[mid], s);
        if (!c) return 1;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    for (size_t i = 0; i < added->n; i++)
        if (str_eq(added->v[i], s)) return 1;
    return 0;
}

int cc_vh_copy(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"ids", "d"};
    static const int REQ[] = {1, 1};
    MoveArgs a;
    memset(&a, 0, sizeof a);
    a.a = &l->arena;
    if (d_struct(err, args, "MoveArgs", F, 2, REQ, 1, move_field, &a)) return -1;
    if (mm_check(err, "d", a.d, 3)) return -1;
    Names model = {0}, added = {0};
    int rc = 0;
    if (m->nelements) {
        model.v = malloc(m->nelements * sizeof *model.v);
        if (!model.v) return fail(err, "out of memory");
        for (size_t i = 0; i < m->nelements; i++) model.v[i] = m->elements[i].name;
        model.n = m->nelements;
        qsort(model.v, model.n, sizeof *model.v, str_qcmp);
    }
    Buf nb;
    buf_init(&nb);
    for (size_t i = 0; i < a.ids.n && !rc; i++) {
        const cc_element *e = cc_model_element(m, &a.ids.v[i]);
        if (!e) continue;
        cc_element c = *e;
        cc_guid id = cc_guid_new();
        memcpy(c.id.c, id.c, 22);
        if (e->nopenings) { /* new openings too: export derives their GlobalIds from the copy's */
            c.openings = arena_alloc(&l->arena, e->nopenings * sizeof *c.openings, 8);
            if (!c.openings) { rc = fail(err, "out of memory"); break; }
            memcpy(c.openings, e->openings, e->nopenings * sizeof *c.openings);
            for (size_t k = 0; k < e->nopenings; k++) c.openings[k].has_id = 0;
        }
        /* "B1" -> "B1 (2)", "B1 (3)", ... */
        for (uint64_t n = 2;; n++) {
            buf_clear(&nb);
            buf_append(&nb, e->name.p, e->name.n);
            buf_puts(&nb, " (");
            buf_u64(&nb, n);
            buf_putc(&nb, ')');
            if (nb.oom) { rc = fail(err, "out of memory"); break; }
            cc_str s = {nb.p, nb.len};
            if (!names_has(&model, &added, s)) {
                c.name = dup_str(&l->arena, nb.p, nb.len);
                break;
            }
        }
        if (rc) break;
        if (!c.name.p) { rc = fail(err, "out of memory"); break; }
        if (added.n == added.cap) {
            size_t cap = added.cap ? added.cap * 2 : 16;
            cc_str *v = realloc(added.v, cap * sizeof *v);
            if (!v) { rc = fail(err, "out of memory"); break; }
            added.v = v, added.cap = cap;
        }
        added.v[added.n++] = c.name;
        for (int k = 0; k < 3; k++) c.placement.origin[k] += a.d[k];
        cc_command *cmd = cc_cmds_push(l, CC_CMD_ADD_ELEMENT);
        if (!cmd) { rc = fail(err, "out of memory"); break; }
        cmd->element = c;
    }
    buf_free(&nb);
    free(model.v);
    free(added.v);
    return rc;
}

/* ---- rotate, mirror (dispatch.rs transform) ---- */

typedef struct {
    Ids ids;
    double angle;
    int has_about;
    double about[2];
    double point[2], dir[2]; /* mirror */
    Arena *a;
} XfArgs;

static int rotate_field(void *ctx, int i, const JrVal *v, Buf *err) {
    XfArgs *p = ctx;
    switch (i) {
    case 0: return d_ids(err, v, p->a, &p->ids);
    case 1: return d_f64(err, v, &p->angle);
    default:
        if (v->type == JR_NULL) return 0;
        p->has_about = 1;
        return d_vec(err, v, p->about, 2);
    }
}

static int axis_field(void *ctx, int i, const JrVal *v, Buf *err) {
    XfArgs *p = ctx;
    return d_vec(err, v, i == 0 ? p->point : p->dir, 2);
}

static int mirror_field(void *ctx, int i, const JrVal *v, Buf *err) {
    static const char *const F[] = {"point", "dir"};
    static const int REQ[] = {1, 1};
    XfArgs *p = ctx;
    if (i == 0) return d_ids(err, v, p->a, &p->ids);
    return d_struct(err, v, "MirrorAxis", F, 2, REQ, 1, axis_field, p);
}

/* A plan transform of a vector: [m00 x + m01 y + t0, m10 x + m11 y + t1, z]. */
typedef struct {
    int mirror;
    double c, s;      /* rotate */
    double ux, uy;    /* mirror */
} Xf;

static void xf_vec(const Xf *f, const double *v, double *out) {
    if (f->mirror) { /* 2 (v.u) u - v */
        double k = 2.0 * (v[0] * f->ux + v[1] * f->uy);
        out[0] = k * f->ux - v[0];
        out[1] = k * f->uy - v[1];
    } else {
        out[0] = f->c * v[0] - f->s * v[1];
        out[1] = f->s * v[0] + f->c * v[1];
    }
    out[2] = v[2];
}

/* One SetPlacement per element; `+ 0.0` turns -0.0 into 0.0 (canonical). */
static int transform(const cc_model *m, const Ids *ids, const Xf *f, double cx, double cy, cc_cmds *l, Buf *err) {
    size_t first = l->n;
    for (size_t i = 0; i < ids->n; i++) {
        const cc_element *e = cc_model_element(m, &ids->v[i]);
        if (!e) {
            l->n = first;
            return no_element(err, &ids->v[i]);
        }
        const cc_placement *p = &e->placement;
        cc_placement q;
        double o[3] = {p->origin[0] - cx, p->origin[1] - cy, p->origin[2]};
        xf_vec(f, o, q.origin);
        q.origin[0] += cx, q.origin[1] += cy;
        xf_vec(f, p->z_axis, q.z_axis);
        xf_vec(f, p->x_axis, q.x_axis);
        for (int k = 0; k < 3; k++) q.origin[k] += 0.0, q.z_axis[k] += 0.0, q.x_axis[k] += 0.0;
        cc_command *c = cc_cmds_push(l, CC_CMD_SET_PLACEMENT);
        if (!c) return fail(err, "out of memory");
        c->id = e->id;
        c->placement = q;
    }
    return 0;
}

/* f64::rem_euclid(360) for |x| <= 360 (fmod is exact there). */
static double rem360(double x) {
    double r = fabs_(x) < 360.0 ? x : x * 0.0; /* fmod(±360, 360) = ±0 */
    return r < 0.0 ? r + 360.0 : r;
}

int cc_vh_rotate(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"ids", "angle_deg", "about"};
    static const int REQ[] = {1, 1, 0};
    XfArgs a;
    memset(&a, 0, sizeof a);
    a.a = &l->arena;
    if (d_struct(err, args, "RotateArgs", F, 3, REQ, 1, rotate_field, &a)) return -1;
    if (!finite_(a.angle) || fabs_(a.angle) > 360.0) {
        buf_clear(err);
        buf_puts(err, "angle_deg: ");
        cc_buf_display_f64(err, a.angle);
        buf_puts(err, " is not within \xc2\xb1" "360");
        return -1;
    }
    if (a.has_about && mm_check(err, "about", a.about, 2)) return -1;
    double sum[2] = {0.0, 0.0};
    for (size_t i = 0; i < a.ids.n; i++) {
        const cc_element *e = cc_model_element(m, &a.ids.v[i]);
        if (!e) return no_element(err, &a.ids.v[i]);
        sum[0] += e->placement.origin[0];
        sum[1] += e->placement.origin[1];
    }
    double r = rem360(a.angle);
    if (r == 0.0 || a.ids.n == 0) return 0; /* a full turn or none: no command */
    double n = (double)a.ids.n;
    double cx = a.has_about ? a.about[0] : sum[0] / n, cy = a.has_about ? a.about[1] : sum[1] / n;
    Xf f = {0, 0, 0, 0, 0};
    /* cos_sin: exact for multiples of 90 degrees. */
    if (r == 90.0) f.c = 0.0, f.s = 1.0;
    else if (r == 180.0) f.c = -1.0, f.s = 0.0;
    else if (r == 270.0) f.c = 0.0, f.s = -1.0;
    else {
        double rad = r * (3.141592653589793 / 180.0); /* f64::to_radians */
        f.c = cc_cos(rad), f.s = cc_sin(rad);
    }
    return transform(m, &a.ids, &f, cx, cy, l, err);
}

int cc_vh_mirror(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"ids", "axis"};
    static const int REQ[] = {1, 1};
    XfArgs a;
    memset(&a, 0, sizeof a);
    a.a = &l->arena;
    if (d_struct(err, args, "MirrorArgs", F, 2, REQ, 1, mirror_field, &a)) return -1;
    if (mm_check(err, "axis.point", a.point, 2) || mm_check(err, "axis.dir", a.dir, 2)) return -1;
    double len = cc_hypot(a.dir[0], a.dir[1]);
    if (len == 0.0) return fail(err, "axis.dir must not be zero");
    Xf f = {1, 0, 0, a.dir[0] / len, a.dir[1] / len};
    return transform(m, &a.ids, &f, a.point[0], a.point[1], l, err);
}

/* ---- set_project_field ---- */

/* project::fold then to_lowercase, for what key_of keeps (ASCII letters and digits): the base
 * letters of accented Latin, lowercase; 0 for a character that becomes '_'. Two letters for
 * æ, œ, ß. */
static const char *fold_lower(uint32_t c) {
    if (c < 0x80) return NULL; /* key_of handles ASCII */
    if ((c >= 0xC0 && c <= 0xC5) || (c >= 0xE0 && c <= 0xE5) || (c >= 0x100 && c <= 0x105)) return "a";
    if (c == 0xC6 || c == 0xE6) return "ae";
    if (c == 0xC7 || c == 0xE7 || c == 0x106 || c == 0x107 || c == 0x10C || c == 0x10D) return "c";
    if ((c >= 0xC8 && c <= 0xCB) || (c >= 0xE8 && c <= 0xEB) || c == 0x112 || c == 0x113 || c == 0x116 ||
        c == 0x117 || c == 0x118 || c == 0x119 || c == 0x11A || c == 0x11B)
        return "e";
    if ((c >= 0xCC && c <= 0xCF) || (c >= 0xEC && c <= 0xEF) || c == 0x12A || c == 0x12B || c == 0x12E ||
        c == 0x12F || c == 0x131)
        return "i";
    if (c == 0x130) return "i"; /* İ lowercases to i + U+0307: the i is kept, the mark is a '_' */
    if (c == 0xD1 || c == 0xF1 || c == 0x143 || c == 0x144 || c == 0x147 || c == 0x148) return "n";
    if ((c >= 0xD2 && c <= 0xD6) || c == 0xD8 || (c >= 0xF2 && c <= 0xF6) || c == 0xF8 || c == 0x14C ||
        c == 0x14D || c == 0x150 || c == 0x151)
        return "o";
    if (c == 0x152 || c == 0x153) return "oe";
    if ((c >= 0xD9 && c <= 0xDC) || (c >= 0xF9 && c <= 0xFC) || c == 0x16A || c == 0x16B || c == 0x16E ||
        c == 0x16F || c == 0x170 || c == 0x171)
        return "u";
    if (c == 0xDD || c == 0x178 || c == 0xFD || c == 0xFF) return "y";
    if (c == 0xDF) return "ss";
    if (c == 0x160 || c == 0x161) return "s";
    if (c == 0x17D || c == 0x17E) return "z";
    if (c == 0x212A) return "k"; /* Kelvin sign lowercases to k */
    return NULL;
}

/* project::key_of: "Maître d'ouvrage" -> "maitre_d_ouvrage". Written into `k` (64 bytes max). */
static size_t key_of(cc_str label, char *k) {
    cc_str t = cc_str_trim(label);
    size_t n = 0;
    char buf[4096];
    for (size_t i = 0; i < t.n;) {
        unsigned char b = (unsigned char)t.p[i];
        uint32_t c;
        size_t len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        if (i + len > t.n) len = t.n - i;
        c = len == 1 ? b : (uint32_t)(b & (0x7F >> len));
        for (size_t j = 1; j < len; j++) c = (c << 6) | ((unsigned char)t.p[i + j] & 0x3F);
        i += len;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            if (n < sizeof buf) buf[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
            continue;
        }
        const char *f = fold_lower(c);
        int extra_mark = c == 0x130;
        if (f) {
            for (; *f; f++)
                if (n < sizeof buf) buf[n++] = *f;
        }
        if (!f || extra_mark) {
            if (n && buf[n - 1] != '_' && n < sizeof buf) buf[n++] = '_';
        }
    }
    while (n && buf[n - 1] == '_') n--;
    if (n > 64) n = 64;
    memcpy(k, buf, n);
    return n;
}

typedef struct {
    cc_str label, value;
} FieldArgs;

static int pfield_field(void *ctx, int i, const JrVal *v, Buf *err) {
    FieldArgs *p = ctx;
    return d_str(err, v, i == 0 ? &p->label : &p->value);
}

int cc_vh_set_project_field(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"label", "value"};
    static const int REQ[] = {1, 1};
    FieldArgs a;
    memset(&a, 0, sizeof a);
    if (d_struct(err, args, "SetProjectFieldArgs", F, 2, REQ, 1, pfield_field, &a)) return -1;
    char kb[64];
    size_t kn = key_of(a.label, kb);
    if (!kn) {
        buf_clear(err);
        cc_buf_debug_str(err, a.label.p, a.label.n);
        buf_puts(err, ": a label needs letters or digits");
        return -1;
    }
    cc_str key = {kb, kn};
    size_t n = m->nproject;
    cc_kv *kv = arena_alloc(&l->arena, (n + 1) * sizeof *kv, 8);
    if (!kv) return fail(err, "out of memory");
    size_t k = 0, at = 0;
    int found = 0, same = 0;
    for (size_t i = 0; i < n; i++) {
        int c = str_cmp(m->project[i].key, key);
        if (c == 0) {
            found = 1;
            same = a.value.n && str_eq(m->project[i].value, a.value);
            at = k;
            if (a.value.n) kv[k] = m->project[i], kv[k].value = a.value, k++;
            continue;
        }
        if (c > 0 && !found && a.value.n) {
            found = 2; /* inserted here */
            kv[k].key = dup_str(&l->arena, kb, kn);
            kv[k].value = a.value;
            k++;
        }
        kv[k++] = m->project[i];
    }
    if (!found && a.value.n) {
        kv[k].key = dup_str(&l->arena, kb, kn);
        kv[k].value = a.value;
        k++;
    }
    (void)at;
    /* Unchanged: no command, so no empty undo step. */
    if ((found == 1 && same) || (!found && !a.value.n)) return 0;
    if (found == 2 && !a.value.n) return 0;
    cc_model t;
    memset(&t, 0, sizeof t);
    t.project = kv;
    t.nproject = k;
    cc_error e;
    if (cc_model_validate(&t, &e)) return fail(err, e.msg); /* project::validate */
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_PROJECT);
    if (!c) return fail(err, "out of memory");
    c->project = kv;
    c->nproject = k;
    return 0;
}

/* ---- set_view (view.rs default_views, effective) ---- */

static double round_(double x) { /* f64::round: half away from zero */
    if (!(fabs_(x) < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x;
    double d = x - t;
    if (d >= 0.5) t += 1.0;
    else if (d <= -0.5) t -= 1.0;
    return t == 0.0 ? x * 0.0 : t;
}

static int64_t tkey(double x) { /* f64::total_cmp */
    int64_t k;
    memcpy(&k, &x, 8);
    return k ^ (int64_t)((uint64_t)(k >> 63) >> 1);
}

static cc_gid derived_view(const char *pre, const cc_gid *storey) {
    char seed[64];
    size_t n = strlen(pre);
    memcpy(seed, pre, n);
    if (storey) memcpy(seed + n, storey->c, 22), n += 22;
    cc_guid g = cc_guid_derived(seed, n);
    cc_gid out;
    memcpy(out.c, g.c, 22);
    return out;
}

/* The view `id` as view::effective gives it: stored, or a default one (built in `a`).
 * 1 found, 0 not, -1 out of memory. */
static int effective_view(const cc_model *m, const cc_gid *id, Arena *a, cc_view *out) {
    const cc_view *st = cc_model_view(m, id);
    if (st) {
        *out = *st;
        return 1;
    }
    memset(out, 0, sizeof *out);
    out->scale = 100;
    cc_gid g = derived_view("bimble view 3d", NULL);
    if (!cc_gid_cmp(&g, id)) {
        out->id = g;
        out->name.p = "3D", out->name.n = 2;
        out->kind = CC_VIEW_THREE_D;
        return 1;
    }
    size_t n = m->nstoreys;
    for (size_t i = 0; i < n; i++) {
        const cc_storey *s = &m->storeys[i];
        for (int look = 0; look < 2; look++) {
            g = derived_view(look ? "bimble view plan up " : "bimble view plan down ", &s->id);
            if (cc_gid_cmp(&g, id)) continue;
            /* The first storey after s (by elevation, ties by id: a stable sort of the id-ordered
             * storeys) more than 1 mm higher. */
            int any = 0;
            double h = 0.0;
            int64_t ks = tkey(s->elevation);
            size_t best = (size_t)-1;
            for (size_t j = 0; j < n; j++) {
                const cc_storey *o = &m->storeys[j];
                int64_t ko = tkey(o->elevation);
                if (!(ko > ks || (ko == ks && j > i))) continue;
                if (!(o->elevation - s->elevation > 1.0)) continue;
                if (best == (size_t)-1) { best = j; continue; }
                int64_t kb = tkey(m->storeys[best].elevation);
                if (ko < kb || (ko == kb && j < best)) best = j;
            }
            if (best != (size_t)-1) any = 1, h = m->storeys[best].elevation - s->elevation;
            out->id = g;
            out->name = cat2(a, look ? "Ceiling " : "Plan ", s->name);
            char *sid = arena_dup(a, s->id.c, 22);
            if (!out->name.p || !sid) return -1;
            out->kind = CC_VIEW_PLAN;
            out->storey.p = sid, out->storey.n = 22;
            out->look = look ? CC_LOOK_UP : CC_LOOK_DOWN;
            cc_range_plane top = {CC_LEVEL_ABOVE, 0.0}, cut = {CC_LEVEL_ASSOCIATED, look ? 2300.0 : 1200.0},
                           bottom = {CC_LEVEL_ASSOCIATED, 0.0},
                           depth = {look ? CC_LEVEL_ABOVE : CC_LEVEL_ASSOCIATED, 0.0};
            if (any && cut.offset_mm >= h) cut.offset_mm = round_(h / 2.0);
            out->range[CC_RANGE_TOP] = top, out->range[CC_RANGE_CUT] = cut;
            out->range[CC_RANGE_BOTTOM] = bottom, out->range[CC_RANGE_DEPTH] = depth;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    cc_gid id;
    int has_scale, has_tn, crop_given, has_crop, range_given;
    uint32_t scale;
    int tn;
    double crop[4]; /* min x y, max x y */
    int has_off[4];
    double off[4];
    int plane_i;
} ViewArgs;

static int plane_field(void *ctx, int i, const JrVal *v, Buf *err) {
    (void)i;
    ViewArgs *p = ctx;
    if (v->type == JR_NULL) return 0;
    p->has_off[p->plane_i] = 1;
    return d_f64(err, v, &p->off[p->plane_i]);
}

static int range_field(void *ctx, int i, const JrVal *v, Buf *err) {
    static const char *const F[] = {"offset_mm"};
    ViewArgs *p = ctx;
    if (v->type == JR_NULL) return 0;
    p->plane_i = i;
    return d_struct(err, v, "PlaneArgs", F, 1, NULL, 1, plane_field, p);
}

static int crop_field(void *ctx, int i, const JrVal *v, Buf *err) {
    ViewArgs *p = ctx;
    return d_vec(err, v, p->crop + 2 * i, 2);
}

static int view_field(void *ctx, int i, const JrVal *v, Buf *err) {
    static const char *const CROP[] = {"min", "max"};
    static const int CREQ[] = {1, 1};
    static const char *const RANGE[] = {"top", "cut", "bottom", "depth"};
    ViewArgs *p = ctx;
    switch (i) {
    case 0: return d_gid(err, v, &p->id);
    case 1:
        if (v->type == JR_NULL) return 0;
        p->has_scale = 1;
        return d_u32(err, v, &p->scale);
    case 2:
        if (v->type == JR_NULL) return 0;
        p->has_tn = 1;
        return d_bool(err, v, &p->tn);
    case 3: /* a present field, even null, is Some: null clears */
        p->crop_given = 1;
        if (v->type == JR_NULL) return 0;
        p->has_crop = 1;
        return d_struct(err, v, "CropArgs", CROP, 2, CREQ, 1, crop_field, p);
    default:
        if (v->type == JR_NULL) return 0;
        p->range_given = 1;
        return d_struct(err, v, "RangeArgs", RANGE, 4, NULL, 1, range_field, p);
    }
}

int cc_vh_set_view(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"id", "scale", "true_north", "crop", "range"};
    static const int REQ[] = {1, 0, 0, 0, 0};
    static const char *const PLANES[] = {"top", "cut", "bottom", "depth"};
    ViewArgs a;
    memset(&a, 0, sizeof a);
    if (d_struct(err, args, "SetViewArgs", F, 5, REQ, 1, view_field, &a)) return -1;
    cc_view old;
    int r = effective_view(m, &a.id, &l->arena, &old);
    if (r < 0) return fail(err, "out of memory");
    if (!r) {
        buf_clear(err);
        buf_puts(err, "no view GlobalId(");
        cc_buf_gid(err, &a.id);
        buf_putc(err, ')');
        return -1;
    }
    cc_view v = old;
    if (a.has_scale) v.scale = a.scale;
    if (a.has_tn) v.true_north = a.tn;
    if (a.crop_given) {
        if (a.has_crop) {
            if (mm_check(err, "crop", a.crop, 4)) return -1;
            v.has_crop = 1;
            memcpy(v.crop_min, a.crop, sizeof v.crop_min);
            memcpy(v.crop_max, a.crop + 2, sizeof v.crop_max);
        } else {
            v.has_crop = 0;
            memset(v.crop_min, 0, sizeof v.crop_min);
            memset(v.crop_max, 0, sizeof v.crop_max);
        }
    }
    if (a.range_given) {
        if (v.kind != CC_VIEW_PLAN) {
            buf_clear(err);
            buf_puts(err, "view ");
            buf_append(err, v.name.p, v.name.n);
            buf_puts(err, ": only a plan has a view range");
            return -1;
        }
        for (int k = 0; k < 4; k++)
            if (a.has_off[k]) {
                if (mm_check(err, PLANES[k], &a.off[k], 1)) return -1;
                v.range[k].offset_mm = a.off[k] + 0.0; /* no -0.0 */
            }
    }
    Buf why;
    buf_init(&why);
    if (cc_view_validate(m, &v, &why)) {
        buf_clear(err);
        buf_append(err, why.p, why.len);
        buf_free(&why);
        return -1;
    }
    buf_free(&why);
    /* The same values again: no command, so no empty undo step. */
    int same = v.scale == old.scale && !v.true_north == !old.true_north && v.has_crop == old.has_crop;
    if (same && v.has_crop)
        same = v.crop_min[0] == old.crop_min[0] && v.crop_min[1] == old.crop_min[1] &&
               v.crop_max[0] == old.crop_max[0] && v.crop_max[1] == old.crop_max[1];
    for (int k = 0; k < 4 && same; k++) same = v.range[k].offset_mm == old.range[k].offset_mm;
    if (same) return 0;
    cc_command *c = cc_cmds_push(l, cc_model_view(m, &v.id) ? CC_CMD_SET_VIEW : CC_CMD_ADD_VIEW);
    if (!c) return fail(err, "out of memory");
    c->view = v;
    return 0;
}

/* ---- dimensions and texts (C additions, ticket C4; annot.h): one set_view / add_view each ---- */

/* The view `id` for an annotation edit: effective, a 2D view. 0 ok, -1 with the error. */
static int ann_view(const cc_model *m, const cc_gid *id, Arena *a, cc_view *out, Buf *err) {
    int r = effective_view(m, id, a, out);
    if (r < 0) return fail(err, "out of memory");
    if (!r) {
        buf_clear(err);
        buf_puts(err, "no view GlobalId(");
        cc_buf_gid(err, id);
        buf_putc(err, ')');
        return -1;
    }
    if (out->kind == CC_VIEW_THREE_D) {
        buf_clear(err);
        buf_puts(err, "view ");
        buf_append(err, out->name.p, out->name.n);
        buf_puts(err, ": dimensions and texts go in 2D views");
        return -1;
    }
    return 0;
}

/* The elevation 2D points get: a plan's storey, else 0. */
static double ann_z(const cc_model *m, const cc_view *v) {
    if (v->kind != CC_VIEW_PLAN || v->storey.n != 22) return 0.0;
    cc_gid sid;
    memcpy(sid.c, v->storey.p, 22);
    const cc_storey *s = cc_model_storey(m, &sid);
    return s ? s->elevation : 0.0;
}

/* [x, y] or [x, y, z] (z: `z`). */
static int ann_point(Buf *err, const JrVal *v, const char *what, double z, double out[3]) {
    if (v->type != JR_ARR || (v->n != 2 && v->n != 3)) {
        buf_clear(err);
        buf_puts(err, "bad arguments: ");
        buf_puts(err, what);
        buf_puts(err, ": expected [x, y] or [x, y, z] in mm");
        return -1;
    }
    out[2] = z;
    for (size_t i = 0; i < v->n; i++)
        if (d_f64(err, &v->items[i], &out[i])) return -1;
    return mm_check(err, what, out, 3);
}

typedef struct {
    const cc_model *m;
    double z;
    int has_at, has_el, has_feat;
    double at[3];
    cc_gid el;
    cc_str feat;
} AnchorArgs;

static int anchor_field(void *ctx, int i, const JrVal *v, Buf *err) {
    AnchorArgs *p = ctx;
    if (v->type == JR_NULL) return 0;
    if (i == 0) return p->has_at = 1, ann_point(err, v, "at", p->z, p->at);
    if (i == 1) return p->has_el = 1, d_gid(err, v, &p->el);
    return p->has_feat = 1, d_str(err, v, &p->feat);
}

/* An end of a dimension: a point, or {at?, element?, feature?} (feature: of the element; its
 * point now is `at`). */
static int ann_anchor(const cc_model *m, const JrVal *v, const char *what, double z, Arena *ar, cc_anchor *out, Buf *err) {
    static const char *const F[] = {"at", "element", "feature"};
    memset(out, 0, sizeof *out);
    if (v->type == JR_ARR) return ann_point(err, v, what, z, out->at);
    AnchorArgs a;
    memset(&a, 0, sizeof a);
    a.m = m, a.z = z;
    if (d_struct(err, v, "AnchorArgs", F, 3, NULL, 1, anchor_field, &a)) return -1;
    if (a.has_el) {
        const cc_element *e = cc_model_element(m, &a.el);
        if (!e) return no_element(err, &a.el);
        out->has_element = 1, out->element = a.el;
        if (a.has_feat) {
            if (!cc_feature_name_ok(a.feat) || cc_feature_point(m, e, a.feat, out->at)) {
                buf_clear(err);
                buf_puts(err, what);
                buf_puts(err, ": the element has no feature ");
                cc_buf_debug_str(err, a.feat.p, a.feat.n < 64 ? a.feat.n : 64);
                buf_puts(err, " (\"origin\", \"end\", \"corner:N\")");
                return -1;
            }
            char *f = arena_dup(ar, a.feat.p, a.feat.n);
            if (!f) return fail(err, "out of memory");
            out->has_feature = 1, out->feature.p = f, out->feature.n = a.feat.n;
            return 0;
        }
    } else if (a.has_feat) {
        buf_clear(err);
        buf_puts(err, what);
        return buf_puts(err, ": a feature needs its element"), -1;
    }
    if (!a.has_at) {
        buf_clear(err);
        buf_puts(err, what);
        return buf_puts(err, ": needs at, or an element and its feature"), -1;
    }
    memcpy(out->at, a.at, sizeof a.at);
    return 0;
}

/* The view with its annotations replaced by `x` (n), validated, as one command. */
static int ann_store(const cc_model *m, const cc_view *old, cc_annotation *x, size_t n, cc_cmds *l, Buf *err) {
    cc_view v = *old;
    v.annotations = x, v.nannotations = n;
    Buf why;
    buf_init(&why);
    if (cc_view_validate(m, &v, &why)) {
        buf_clear(err);
        buf_append(err, why.p, why.len);
        buf_free(&why);
        return -1;
    }
    buf_free(&why);
    cc_command *c = cc_cmds_push(l, cc_model_view(m, &v.id) ? CC_CMD_SET_VIEW : CC_CMD_ADD_VIEW);
    if (!c) return fail(err, "out of memory");
    c->view = v;
    return 0;
}

/* The annotations of `v` and room for one more, in the arena. */
static cc_annotation *ann_copy(const cc_view *v, Arena *a) {
    cc_annotation *x = arena_alloc(a, (v->nannotations + 1) * sizeof *x, 8);
    if (x && v->nannotations) memcpy(x, v->annotations, v->nannotations * sizeof *x);
    return x;
}

static void ann_new_id(cc_annotation *x) {
    cc_guid id = cc_guid_new();
    memcpy(x->id.c, id.c, 22);
}

typedef struct {
    const cc_model *m;
    Arena *ar;
    cc_gid view, id;
    cc_view v;
    int has_view, has_off, has_axis, has_size, has_angle, has_by;
    const JrVal *a, *b, *at;
    double off, size, angle, by[2];
    int axis;
    cc_str text, kind;
    int has_kind;
} AnnArgs;

static int add_dim_field(void *ctx, int i, const JrVal *v, Buf *err) {
    static const char *const AX[] = {"aligned", "horizontal", "vertical"};
    AnnArgs *p = ctx;
    switch (i) {
    case 0: return d_gid(err, v, &p->view);
    case 1: return p->a = v, 0;
    case 2: return p->b = v, 0;
    case 3: return p->has_off = 1, d_f64(err, v, &p->off);
    case 4: return v->type == JR_NULL ? 0 : (p->has_axis = 1, d_variant(err, v, "DimAxis", AX, 3, &p->axis));
    default: return v->type == JR_NULL ? 0 : (p->has_kind = 1, d_str(err, v, &p->kind));
    }
}

int cc_vh_add_dimension(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"view", "a", "b", "offset", "axis", "kind"};
    static const int REQ[] = {1, 1, 1, 1, 0, 0};
    AnnArgs p;
    memset(&p, 0, sizeof p);
    if (d_struct(err, args, "AddDimensionArgs", F, 6, REQ, 1, add_dim_field, &p)) return -1;
    cc_view v;
    if (ann_view(m, &p.view, &l->arena, &v, err)) return -1;
    if (mm_check(err, "offset", &p.off, 1)) return -1;
    double z = ann_z(m, &v);
    cc_annotation *x = ann_copy(&v, &l->arena);
    if (!x) return fail(err, "out of memory");
    cc_annotation *d = &x[v.nannotations];
    memset(d, 0, sizeof *d);
    d->type = CC_ANN_DIMENSION;
    if (ann_anchor(m, p.a, "a", z, &l->arena, &d->a, err) || ann_anchor(m, p.b, "b", z, &l->arena, &d->b, err)) return -1;
    double u[2], qa[2], qb[2], len;
    if (cc_dim_line(d->a.at, d->b.at, p.off, (cc_dim_axis)p.axis, u, qa, qb, &len))
        return fail(err, "a dimension needs two points apart (along its axis)");
    d->offset_mm = p.off + 0.0;
    d->axis = (cc_dim_axis)p.axis;
    if (p.has_kind) d->has_kind = 1, d->kind = p.kind;
    ann_new_id(d);
    return ann_store(m, &v, x, v.nannotations + 1, l, err);
}

static int add_text_field(void *ctx, int i, const JrVal *v, Buf *err) {
    AnnArgs *p = ctx;
    switch (i) {
    case 0: return d_gid(err, v, &p->view);
    case 1: return p->at = v, 0;
    case 2: return d_str(err, v, &p->text);
    case 3: return v->type == JR_NULL ? 0 : (p->has_size = 1, d_f64(err, v, &p->size));
    case 4: return v->type == JR_NULL ? 0 : (p->has_angle = 1, d_f64(err, v, &p->angle));
    default: return v->type == JR_NULL ? 0 : (p->has_kind = 1, d_str(err, v, &p->kind));
    }
}

int cc_vh_add_text(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"view", "at", "text", "size_mm", "angle_deg", "kind"};
    static const int REQ[] = {1, 1, 1, 0, 0, 0};
    AnnArgs p;
    memset(&p, 0, sizeof p);
    if (d_struct(err, args, "AddTextArgs", F, 6, REQ, 1, add_text_field, &p)) return -1;
    cc_view v;
    if (ann_view(m, &p.view, &l->arena, &v, err)) return -1;
    cc_annotation *x = ann_copy(&v, &l->arena);
    if (!x) return fail(err, "out of memory");
    cc_annotation *t = &x[v.nannotations];
    memset(t, 0, sizeof *t);
    t->type = CC_ANN_TEXT;
    if (ann_point(err, p.at, "at", ann_z(m, &v), t->at)) return -1;
    t->text = p.text;
    t->size_mm = p.has_size ? p.size : 3.5; /* layout.rs Tool::Text: 3.5 without a text type */
    t->angle_deg = p.has_angle ? p.angle + 0.0 : 0.0;
    if (p.has_kind) t->has_kind = 1, t->kind = p.kind;
    ann_new_id(t);
    return ann_store(m, &v, x, v.nannotations + 1, l, err);
}

static int ann_id_field(void *ctx, int i, const JrVal *v, Buf *err) {
    AnnArgs *p = ctx;
    switch (i) {
    case 0: return d_gid(err, v, &p->view);
    case 1: return d_gid(err, v, &p->id);
    case 2: return v->type == JR_NULL ? 0 : (p->has_by = 1, d_vec(err, v, p->by, 2));
    default: return v->type == JR_NULL ? 0 : (p->has_off = 1, d_f64(err, v, &p->off));
    }
}

/* The annotation `id` of `v`: its index, or -1 with the error. */
static long ann_find(const cc_view *v, const cc_gid *id, Buf *err) {
    for (size_t i = 0; i < v->nannotations; i++)
        if (!cc_gid_cmp(&v->annotations[i].id, id)) return (long)i;
    buf_clear(err);
    buf_puts(err, "no dimension or text GlobalId(");
    cc_buf_gid(err, id);
    buf_puts(err, ") in view ");
    buf_append(err, v->name.p, v->name.n);
    return -1;
}

int cc_vh_move_annotation(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"view", "id", "by", "offset"};
    static const int REQ[] = {1, 1, 0, 0};
    AnnArgs p;
    memset(&p, 0, sizeof p);
    if (d_struct(err, args, "MoveAnnotationArgs", F, 4, REQ, 1, ann_id_field, &p)) return -1;
    cc_view v;
    if (ann_view(m, &p.view, &l->arena, &v, err)) return -1;
    long i = ann_find(&v, &p.id, err);
    if (i < 0) return -1;
    if ((p.has_by && mm_check(err, "by", p.by, 2)) || (p.has_off && mm_check(err, "offset", &p.off, 1))) return -1;
    cc_annotation *x = ann_copy(&v, &l->arena);
    if (!x) return fail(err, "out of memory");
    cc_annotation *a = &x[i];
    if (a->type == CC_ANN_TEXT) {
        if (p.has_off) return fail(err, "offset: a text has none (move it with by)");
        if (!p.has_by || (p.by[0] == 0.0 && p.by[1] == 0.0)) return 0;
        a->at[0] += p.by[0], a->at[1] += p.by[1];
    } else {
        double off = p.has_off ? p.off : a->offset_mm;
        if (p.has_by) { /* the line moves across itself by `by` (paper axes, as dimension_on_paper) */
            double pa[3], pb[3], u[2], qa[2], qb[2], len;
            cc_anchor_point(m, &a->a, pa), cc_anchor_point(m, &a->b, pb);
            if (!cc_dim_line(pa, pb, off, a->axis, u, qa, qb, &len)) {
                double up[2] = {u[0], -u[1]}, n[2] = {up[1], -up[0]}, byp[2] = {p.by[0], -p.by[1]};
                off += byp[0] * n[0] + byp[1] * n[1];
            }
        }
        if (off == a->offset_mm) return 0;
        a->offset_mm = off + 0.0;
    }
    return ann_store(m, &v, x, v.nannotations, l, err);
}

int cc_vh_delete_annotation(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"view", "id"};
    static const int REQ[] = {1, 1};
    AnnArgs p;
    memset(&p, 0, sizeof p);
    if (d_struct(err, args, "DeleteAnnotationArgs", F, 2, REQ, 1, ann_id_field, &p)) return -1;
    cc_view v;
    if (ann_view(m, &p.view, &l->arena, &v, err)) return -1;
    long i = ann_find(&v, &p.id, err);
    if (i < 0) return -1;
    cc_annotation *x = ann_copy(&v, &l->arena);
    if (!x) return fail(err, "out of memory");
    memmove(x + i, x + i + 1, (v.nannotations - (size_t)i - 1) * sizeof *x);
    return ann_store(m, &v, v.nannotations > 1 ? x : NULL, v.nannotations - 1, l, err);
}

/* ---- set_schedule (schedule.rs default_schedules, effective) ---- */

static cc_str CLS_COLUMN[] = {{"IfcColumn", 9}};
static cc_str CLS_BEAM[] = {{"IfcBeam", 7}};
static cc_str CLS_SLAB[] = {{"IfcSlab", 7}};
static cc_str CLS_WALL[] = {{"IfcWall", 7}, {"IfcWallStandardCase", 19}};
static cc_str CLS_FOOTING[] = {{"IfcFooting", 10}};
enum {
    F_LEVEL = CC_FIELD_LEVEL, F_LOC = CC_FIELD_LOCATION, F_NAME = CC_FIELD_NAME, F_CLASS = CC_FIELD_CLASS,
    F_MAT = CC_FIELD_MATERIAL, F_SEC = CC_FIELD_SECTION, F_LEN = CC_FIELD_LENGTH, F_WID = CC_FIELD_WIDTH,
    F_HEI = CC_FIELD_HEIGHT, F_AREA = CC_FIELD_AREA, F_VOL = CC_FIELD_VOLUME, F_MASS = CC_FIELD_MASS
};
static uint8_t FD_COL[] = {F_LEVEL, F_LOC, F_NAME, F_MAT, F_SEC, F_LEN, F_VOL};
static uint8_t FD_BEAM[] = {F_LEVEL, F_NAME, F_MAT, F_SEC, F_LEN, F_WID, F_HEI, F_VOL};
static uint8_t FD_STEEL_COL[] = {F_LEVEL, F_LOC, F_NAME, F_MAT, F_SEC, F_LEN, F_VOL, F_MASS};
static uint8_t FD_STEEL_BEAM[] = {F_LEVEL, F_NAME, F_MAT, F_SEC, F_LEN, F_WID, F_HEI, F_VOL, F_MASS};
static uint8_t FD_SLAB[] = {F_LEVEL, F_NAME, F_MAT, F_HEI, F_AREA, F_VOL};
static uint8_t FD_WALL[] = {F_LEVEL, F_NAME, F_MAT, F_WID, F_LEN, F_HEI, F_AREA, F_VOL};
static uint8_t FD_FOOTING[] = {F_LEVEL, F_LOC, F_NAME, F_MAT, F_SEC, F_HEI, F_VOL};
static uint8_t FD_ALL[] = {F_LEVEL, F_NAME, F_CLASS, F_MAT, F_VOL, F_MASS};
#define DEF(nm, cls, fam, fd)                                                                                    \
    {{nm, sizeof nm - 1}, {cls, sizeof cls / sizeof cls[0]}, fam >= 0, fam >= 0 ? fam : 0, fd,                \
     sizeof fd, 1, 1, {NULL, 0}, 0, 0, 0, {NULL, 0}, {NULL, 0}, 0, {NULL, 0}, 0}
#define DEF0(nm, fam, fd) {{nm, sizeof nm - 1}, {NULL, 0}, fam >= 0, fam >= 0 ? fam : 0, fd, sizeof fd, 1, 1, {NULL, 0}, 0, 0, 0, {NULL, 0}, {NULL, 0}, 0, {NULL, 0}, 0}
static const cc_schedule BUILTIN[] = {
    DEF("Concrete columns", CLS_COLUMN, CC_CAT_CONCRETE, FD_COL),
    DEF("Steel columns", CLS_COLUMN, CC_CAT_STEEL, FD_STEEL_COL),
    DEF("Timber columns", CLS_COLUMN, CC_CAT_TIMBER, FD_COL),
    DEF("Concrete beams", CLS_BEAM, CC_CAT_CONCRETE, FD_BEAM),
    DEF("Steel beams", CLS_BEAM, CC_CAT_STEEL, FD_STEEL_BEAM),
    DEF("Timber beams", CLS_BEAM, CC_CAT_TIMBER, FD_BEAM),
    DEF("Slabs", CLS_SLAB, -1, FD_SLAB),
    DEF("Walls", CLS_WALL, -1, FD_WALL),
    DEF("Footings", CLS_FOOTING, -1, FD_FOOTING),
    DEF0("All elements", -1, FD_ALL),
};
#undef DEF
#undef DEF0

static int same_name(cc_str a, cc_str b) { /* str::eq_ignore_ascii_case */
    if (a.n != b.n) return 0;
    for (size_t i = 0; i < a.n; i++) {
        unsigned char x = (unsigned char)a.p[i], y = (unsigned char)b.p[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

/* schedule::effective(stored), then the first named `name`. */
static const cc_schedule *effective_schedule(const cc_model *m, cc_str name) {
    size_t nb = sizeof BUILTIN / sizeof BUILTIN[0];
    for (size_t i = 0; i < nb; i++) {
        const cc_schedule *d = &BUILTIN[i];
        for (size_t k = 0; k < m->nschedules; k++)
            if (same_name(m->schedules[k].name, d->name)) {
                d = &m->schedules[k];
                break;
            }
        if (same_name(d->name, name)) return d;
    }
    for (size_t k = 0; k < m->nschedules; k++) {
        int builtin = 0;
        for (size_t i = 0; i < nb && !builtin; i++) builtin = same_name(BUILTIN[i].name, m->schedules[k].name);
        if (!builtin && same_name(m->schedules[k].name, name)) return &m->schedules[k];
    }
    return NULL;
}

typedef struct {
    cc_str name;
    int has_group, group;
    int family_given, has_family, family;
    int has_columns;
    uint8_t *columns;
    size_t ncolumns;
    Arena *a;
} SchedArgs;

static int sched_field(void *ctx, int i, const JrVal *v, Buf *err) {
    SchedArgs *p = ctx;
    switch (i) {
    case 0: return d_str(err, v, &p->name);
    case 1:
        if (v->type == JR_NULL) return 0;
        p->has_group = 1;
        return d_bool(err, v, &p->group);
    case 2: {
        p->family_given = 1;
        if (v->type == JR_NULL) return 0;
        const char *names[CC_CAT_COUNT];
        for (int k = 0; k < CC_CAT_COUNT; k++) names[k] = cc_category_name((cc_category)k);
        p->has_family = 1;
        return d_variant(err, v, "MaterialCategory", names, CC_CAT_COUNT, &p->family);
    }
    default: {
        if (v->type == JR_NULL) return 0;
        if (v->type != JR_ARR) return bad_type(err, v, "a sequence");
        const char *names[CC_FIELD_RUST]; /* QTY1: Rust's set_schedule takes Rust's 13 */
        for (int k = 0; k < CC_FIELD_RUST; k++) names[k] = cc_sched_field_name(k);
        p->has_columns = 1;
        p->ncolumns = v->n;
        p->columns = v->n ? arena_alloc(p->a, v->n, 1) : NULL;
        if (v->n && !p->columns) return bad(err, "out of memory", 0, 0);
        for (size_t k = 0; k < v->n; k++) {
            int f;
            if (d_variant(err, &v->items[k], "Field", names, CC_FIELD_RUST, &f)) return -1;
            p->columns[k] = (uint8_t)f;
        }
        return 0;
    }
    }
}

int cc_vh_set_schedule(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"name", "group_by_level", "family", "columns"};
    static const int REQ[] = {1, 0, 0, 0};
    SchedArgs a;
    memset(&a, 0, sizeof a);
    a.a = &l->arena;
    if (d_struct(err, args, "SetScheduleArgs", F, 4, REQ, 1, sched_field, &a)) return -1;
    const cc_schedule *old = effective_schedule(m, a.name);
    if (!old) {
        buf_clear(err);
        buf_puts(err, "no schedule ");
        cc_buf_debug_str(err, a.name.p, a.name.n);
        return -1;
    }
    cc_schedule d = *old;
    if (a.has_group) d.group_by_level = a.group;
    if (a.family_given) d.has_family = a.has_family, d.family = a.has_family ? a.family : 0;
    if (a.has_columns) d.fields = a.columns, d.nfields = a.ncolumns;
    if (cc_schedule_eq(&d, old)) return 0;
    size_t n = m->nschedules;
    cc_schedule *v = arena_alloc(&l->arena, (n + 1) * sizeof *v, 8);
    if (!v) return fail(err, "out of memory");
    if (n) memcpy(v, m->schedules, n * sizeof *v);
    size_t k = 0;
    while (k < n && !same_name(v[k].name, a.name)) k++;
    v[k] = d;
    if (k == n) n++;
    Buf why;
    buf_init(&why);
    if (cc_schedules_validate(v, n, &why)) {
        buf_clear(err);
        buf_append(err, why.p, why.len);
        buf_free(&why);
        return -1;
    }
    buf_free(&why);
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_SCHEDULES);
    if (!c) return fail(err, "out of memory");
    c->schedules = v;
    c->nschedules = n;
    return 0;
}

/* ---- add_sheet, set_sheet (dispatch.rs, sheet.rs) ---- */

typedef struct {
    cc_gid id;
    int has[6]; /* id, number, name, paper, portrait, revision (set_sheet); add_sheet from 1 */
    cc_str number, name, paper, revision;
    int portrait;
    int off; /* 0: set_sheet field indices, 1: add_sheet (no id) */
} SheetArgs;

static int sheet_field(void *ctx, int i, const JrVal *v, Buf *err) {
    SheetArgs *p = ctx;
    i += p->off;
    if (i == 0) return p->has[0] = 1, d_gid(err, v, &p->id);
    if (v->type == JR_NULL) return 0;
    p->has[i] = 1;
    switch (i) {
    case 1: return d_str(err, v, &p->number);
    case 2: return d_str(err, v, &p->name);
    case 3: return d_str(err, v, &p->paper);
    case 4: return d_bool(err, v, &p->portrait);
    default: return d_str(err, v, &p->revision);
    }
}

/* check_sheet: valid, its number not used by another sheet. */
static int check_sheet(const cc_model *m, const cc_sheet *s, Buf *err) {
    Buf why;
    buf_init(&why);
    if (cc_sheet_validate(m, s, &why)) {
        buf_clear(err);
        buf_append(err, why.p, why.len);
        buf_free(&why);
        return -1;
    }
    buf_free(&why);
    for (size_t i = 0; i < m->nsheets; i++)
        if (cc_gid_cmp(&m->sheets[i].id, &s->id) && str_eq(m->sheets[i].number, s->number)) {
            buf_clear(err);
            buf_puts(err, "sheet number ");
            buf_append(err, s->number.p, s->number.n);
            buf_puts(err, " is already used");
            return -1;
        }
    return 0;
}

/* sheet::next_number: the highest trailing number (1 to 9 digits) plus one, zero padded to its
 * width, the first not used; "S-101" when no number ends in digits. */
static cc_str next_number(const cc_model *m, Arena *a) {
    int have = 0;
    uint64_t best = 0;
    size_t width = 0;
    cc_str prefix = {"", 0};
    for (size_t i = 0; i < m->nsheets; i++) {
        cc_str s = m->sheets[i].number;
        size_t d = 0;
        while (d < s.n && s.p[s.n - 1 - d] >= '0' && s.p[s.n - 1 - d] <= '9') d++;
        if (d == 0 || d > 9) continue;
        uint64_t v = 0;
        for (size_t k = s.n - d; k < s.n; k++) v = v * 10 + (uint64_t)(s.p[k] - '0');
        if (!have || v > best) have = 1, best = v, width = d, prefix.p = s.p, prefix.n = s.n - d;
    }
    if (!have) return dup_str(a, "S-101", 5);
    char buf[512];
    for (;;) {
        best++;
        char digits[24];
        size_t nd = 0;
        for (uint64_t v = best; v; v /= 10) digits[nd++] = (char)('0' + v % 10);
        size_t pn = prefix.n < sizeof buf - 40 ? prefix.n : sizeof buf - 40, n = pn;
        if (pn) memcpy(buf, prefix.p, pn);
        for (size_t k = nd; k < width; k++) buf[n++] = '0';
        while (nd) buf[n++] = digits[--nd];
        cc_str s = {buf, n};
        int used = 0;
        for (size_t i = 0; i < m->nsheets && !used; i++) used = str_eq(m->sheets[i].number, s);
        if (!used) return dup_str(a, buf, n);
    }
}

int cc_vh_add_sheet(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"number", "name", "paper", "portrait"};
    SheetArgs a;
    memset(&a, 0, sizeof a);
    a.off = 1;
    if (d_struct(err, args, "AddSheetArgs", F, 4, NULL, 1, sheet_field, &a)) return -1;
    cc_sheet s;
    memset(&s, 0, sizeof s);
    s.number = a.has[1] ? a.number : next_number(m, &l->arena);
    if (!s.number.p) return fail(err, "out of memory");
    cc_guid id = cc_guid_new(); /* Sheet::new */
    memcpy(s.id.c, id.c, 22);
    if (a.has[2]) s.name = a.name;
    else s.name.p = "New sheet", s.name.n = 9;
    if (a.has[3]) s.paper = a.paper;
    else s.paper.p = "A1", s.paper.n = 2;
    if (a.has[4]) s.portrait = a.portrait;
    if (check_sheet(m, &s, err)) return -1;
    cc_command *c = cc_cmds_push(l, CC_CMD_ADD_SHEET);
    if (!c) return fail(err, "out of memory");
    c->sheet = s;
    return 0;
}

int cc_vh_set_sheet(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"id", "number", "name", "paper", "portrait", "revision"};
    static const int REQ[] = {1, 0, 0, 0, 0, 0};
    SheetArgs a;
    memset(&a, 0, sizeof a);
    if (d_struct(err, args, "SetSheetArgs", F, 6, REQ, 1, sheet_field, &a)) return -1;
    const cc_sheet *old = cc_model_sheet(m, &a.id);
    if (!old) {
        buf_clear(err);
        buf_puts(err, "no sheet GlobalId(");
        cc_buf_gid(err, &a.id);
        buf_putc(err, ')');
        return -1;
    }
    cc_sheet s = *old;
    if (a.has[1]) s.number = a.number;
    if (a.has[2]) s.name = a.name;
    if (a.has[3]) s.paper = a.paper;
    if (a.has[4]) s.portrait = a.portrait;
    if (a.has[5]) {
        if (nchars(a.revision) > 200) return fail(err, "revision: at most 200 characters");
        cc_str key = {"revision", 8};
        cc_kv *kv = arena_alloc(&l->arena, (old->nfields + 1) * sizeof *kv, 8);
        if (!kv) return fail(err, "out of memory");
        size_t n = 0;
        int done = 0;
        for (size_t i = 0; i < old->nfields; i++) {
            int c = str_cmp(old->fields[i].key, key);
            if (c == 0) {
                done = 1;
                if (a.revision.n) kv[n] = old->fields[i], kv[n++].value = a.revision;
                continue;
            }
            if (c > 0 && !done) {
                done = 1;
                if (a.revision.n) kv[n].key = key, kv[n++].value = a.revision;
            }
            kv[n++] = old->fields[i];
        }
        if (!done && a.revision.n) kv[n].key = key, kv[n++].value = a.revision;
        s.fields = kv;
        s.nfields = n;
    }
    int same = str_eq(s.number, old->number) && str_eq(s.name, old->name) && str_eq(s.paper, old->paper) &&
               !s.portrait == !old->portrait && s.nfields == old->nfields;
    for (size_t i = 0; same && i < s.nfields; i++)
        same = str_eq(s.fields[i].key, old->fields[i].key) && str_eq(s.fields[i].value, old->fields[i].value);
    if (same) return 0;
    if (check_sheet(m, &s, err)) return -1;
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_SHEET);
    if (!c) return fail(err, "out of memory");
    c->sheet = s;
    return 0;
}

/* ---- dispatch::run ---- */

static void unknown_verb(const char *name, Buf *err) {
    buf_clear(err);
    buf_puts(err, "unknown verb ");
    cc_buf_debug_str(err, name, strlen(name));
    buf_puts(err, "; known: ");
    for (size_t i = 0; i < cc_verb_count; i++) {
        if (i) buf_puts(err, ", ");
        buf_puts(err, cc_verb_table[i].name);
    }
}

int cc_verb_run_ex(const cc_model *m, const char *name, const char *args, size_t n, int dry_run, int64_t if_rev,
                   uint64_t current_rev, cc_cmds *out, Buf *err) {
    (void)dry_run; /* a verb never changes the model: the caller applies, or not */
    const cc_verb_def *def = cc_verb_find(name);
    if (!def) {
        unknown_verb(name, err);
        return -1;
    }
    if (if_rev >= 0 && (uint64_t)if_rev != current_rev) {
        buf_clear(err);
        buf_puts(err, "conflict: the model changed (rev ");
        buf_u64(err, current_rev);
        buf_puts(err, ", expected ");
        buf_u64(err, (uint64_t)if_rev);
        buf_putc(err, ')');
        return CC_VERB_CONFLICT;
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
    int rc = def->run(m, v, out, err);
    if (rc) out->n = before; /* nothing half made */
    return rc;
}

int cc_verb_run(const cc_model *m, const char *name, const char *args, size_t n, cc_cmds *out, Buf *err) {
    return cc_verb_run_ex(m, name, args, n, 0, -1, 0, out, err);
}
