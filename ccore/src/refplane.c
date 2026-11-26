/* See refplane.h (WAVE1B, ADR 0075). */
#include "refplane.h"
#include <string.h>
#include "guid.h"
#include "math.h"
#include "strtod.h"
#include "verbs.h"

static int finite_(double x) { return x - x == 0.0; }
static double abs_(double x) { return x < 0 ? -x : x; }

int cc_class_shapeless(cc_ifc_class c) { return c == CC_IFC_ELEMENT_ASSEMBLY || c == CC_IFC_VIRTUAL_ELEMENT; }

static const cc_kv *prop(const cc_element *e, const char *key) {
    size_t kn = strlen(key);
    for (size_t i = 0; i < e->nproperties; i++)
        if (e->properties[i].key.n == kn && !memcmp(e->properties[i].key.p, key, kn)) return &e->properties[i];
    return NULL;
}
/* A size property: 1 with the value when present and valid, 0 absent, -1 bad. */
static int size_prop(const cc_element *e, const char *key, double *out) {
    const cc_kv *kv = prop(e, key);
    if (!kv) return 0;
    if (!kv->value.n || kv->value.n > 32) return -1;
    for (size_t i = 0; i < kv->value.n; i++) {
        char c = kv->value.p[i];
        if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')) return -1;
    }
    double v = cc_strtod(kv->value.p, kv->value.n);
    if (!finite_(v) || !(v >= 10.0 && v <= 1.0e6)) return -1;
    *out = v;
    return 1;
}

int cc_refplane_check(const cc_element *e, Buf *b) {
    if (e->generator.kind != CC_GEN_NONE) {
        buf_puts(b, "an IfcVirtualElement has no generator: it is a reference plane");
        return -1;
    }
    if (e->nopenings) {
        buf_puts(b, "an IfcVirtualElement has no openings");
        return -1;
    }
    double v;
    if (size_prop(e, CC_REFPLANE_WIDTH_KEY, &v) < 0 || size_prop(e, CC_REFPLANE_HEIGHT_KEY, &v) < 0) {
        buf_puts(b, "a reference plane's width and height are numbers within [10, 1000000] mm");
        return -1;
    }
    return 0;
}

int cc_refplane_prop_ok(cc_str key, const cc_str *value) {
    if (!value) return 0;
    cc_element e;
    memset(&e, 0, sizeof e);
    cc_kv kv = {key, *value};
    e.properties = &kv, e.nproperties = 1, e.generator.kind = CC_GEN_NONE;
    Buf b;
    buf_init(&b);
    int rc = cc_refplane_check(&e, &b);
    buf_free(&b);
    return rc;
}

void cc_refplane_size(const cc_element *e, double *w, double *h) {
    if (size_prop(e, CC_REFPLANE_WIDTH_KEY, w) != 1) *w = CC_REFPLANE_SIZE;
    if (size_prop(e, CC_REFPLANE_HEIGHT_KEY, h) != 1) *h = CC_REFPLANE_SIZE;
}

void cc_refplane_frame(const cc_element *e, double elev, double o[3], double n[3], double x[3], double y[3]) {
    const cc_placement *p = &e->placement;
    for (int k = 0; k < 3; k++) o[k] = p->origin[k], n[k] = p->z_axis[k], x[k] = p->x_axis[k];
    o[2] += elev;
    y[0] = n[1] * x[2] - n[2] * x[1], y[1] = n[2] * x[0] - n[0] * x[2], y[2] = n[0] * x[1] - n[1] * x[0];
}

void cc_refplane_corners(const cc_element *e, double elev, double c[4][3]) {
    double o[3], n[3], x[3], y[3], w, h;
    cc_refplane_frame(e, elev, o, n, x, y);
    cc_refplane_size(e, &w, &h);
    static const double S[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++) c[i][k] = o[k] + S[i][0] * w / 2.0 * x[k] + S[i][1] * h / 2.0 * y[k] + 0.0;
}

static int unit(double v[3]) {
    double l = cc_sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!finite_(l) || l < 1e-9) return -1;
    for (int k = 0; k < 3; k++) v[k] = v[k] / l + 0.0;
    return 0;
}

int cc_plane_from_points(const double p[3][3], double n[3], double x[3]) {
    double a[3], b[3];
    for (int k = 0; k < 3; k++) a[k] = p[1][k] - p[0][k], b[k] = p[2][k] - p[0][k];
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++)
            if (!finite_(p[i][k])) return -1;
    double la = cc_sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = cc_sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    n[0] = a[1] * b[2] - a[2] * b[1], n[1] = a[2] * b[0] - a[0] * b[2], n[2] = a[0] * b[1] - a[1] * b[0];
    double ln = cc_sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    /* sin of the angle at p0 under 1e-6, or a side under 1 mm: on one line */
    if (!(la >= 1.0 && lb >= 1.0 && ln >= 1e-6 * la * lb)) return -1;
    memcpy(x, a, sizeof a);
    if (unit(n) || unit(x)) return -1;
    return 0;
}

void cc_plane_x_axis(const double n[3], double x[3]) {
    /* world x minus its part along n; when n is (nearly) x, world y */
    double w[3] = {1, 0, 0};
    if (abs_(n[0]) > 0.9) w[0] = 0, w[1] = 1;
    double d = w[0] * n[0] + w[1] * n[1] + w[2] * n[2];
    for (int k = 0; k < 3; k++) x[k] = w[k] - d * n[k];
    unit(x);
}

int cc_ray_meets_plane(const double o[3], const double d[3], const double po[3], const double n[3], double q[3]) {
    double dn = d[0] * n[0] + d[1] * n[1] + d[2] * n[2];
    if (abs_(dn) < 1e-12) return -1;
    double t = ((po[0] - o[0]) * n[0] + (po[1] - o[1]) * n[1] + (po[2] - o[2]) * n[2]) / dn;
    if (!(t >= 0.0) || !finite_(t)) return -1;
    for (int k = 0; k < 3; k++) q[k] = o[k] + t * d[k];
    return 0;
}

void cc_plane_project(const double po[3], const double n[3], const double q[3], double out[3]) {
    double s = (q[0] - po[0]) * n[0] + (q[1] - po[1]) * n[1] + (q[2] - po[2]) * n[2];
    for (int k = 0; k < 3; k++) out[k] = q[k] - s * n[k] + 0.0;
}

/* ---- the verb ---- */

static int fail(Buf *err, const char *msg) {
    buf_clear(err);
    buf_puts(err, msg);
    return -1;
}

static int vec3(const JrVal *v, const char *what, double out[3], Buf *err) {
    if (!v || v->type != JR_ARR || v->n != 3) goto bad;
    for (int k = 0; k < 3; k++) {
        if (v->items[k].type != JR_NUM || !finite_(v->items[k].num) || abs_(v->items[k].num) > CC_MAX_MM) goto bad;
        out[k] = v->items[k].num + 0.0;
    }
    return 0;
bad:
    buf_clear(err);
    buf_puts(err, what);
    buf_puts(err, ": [x, y, z] in mm within \xc2\xb1" "10000000 is required");
    return -1;
}
static int size_arg(const JrVal *v, const char *what, double *out, Buf *err) {
    if (!v) {
        *out = CC_REFPLANE_SIZE;
        return 0;
    }
    if (v->type != JR_NUM || !(v->num >= 10.0 && v->num <= 1.0e6)) {
        buf_clear(err);
        buf_puts(err, what);
        buf_puts(err, ": a size within [10, 1000000] mm");
        return -1;
    }
    *out = v->num + 0.0;
    return 0;
}

static int str_eq(cc_str a, const char *p, size_t n) { return a.n == n && (n == 0 || !memcmp(a.p, p, n)); }

/* The first name "RP <k>" no element has. */
static cc_str next_name(const cc_model *m, Arena *a) {
    char buf[32] = "RP ";
    for (uint64_t k = 1;; k++) {
        char d[24];
        int nd = 0;
        for (uint64_t v = k; v; v /= 10) d[nd++] = (char)('0' + v % 10);
        size_t n = 3;
        while (nd) buf[n++] = d[--nd];
        int used = 0;
        for (size_t i = 0; i < m->nelements && !used; i++) used = str_eq(m->elements[i].name, buf, n);
        if (!used) return (cc_str){arena_dup(a, buf, n), n};
    }
}

static cc_str num_text(Arena *a, double v) {
    Buf b;
    buf_init(&b);
    cc_buf_display_f64(&b, v);
    cc_str s = {NULL, 0};
    if (!b.oom) s.p = arena_dup(a, b.p, b.len), s.n = b.len;
    buf_free(&b);
    return s;
}

int cc_vh_reference_plane_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"storey", "name", "origin", "normal", "x_axis", "points", "width", "height"};
    const JrVal *f[8] = {0};
    if (!args || args->type != JR_OBJ) return fail(err, "bad arguments: expected an object");
    for (size_t j = 0; j < args->n; j++) {
        const JrMember *mb = &args->members[j];
        int k = -1;
        for (int i = 0; i < 8 && k < 0; i++)
            if (jr_key_is(mb, F[i])) k = i;
        if (k < 0) {
            buf_clear(err);
            buf_puts(err, "bad arguments: unknown field `");
            buf_append(err, mb->key, mb->key_n < 64 ? mb->key_n : 64);
            buf_puts(err, "`");
            return -1;
        }
        if (f[k]) return fail(err, "bad arguments: a field twice");
        f[k] = mb->val.type == JR_NULL ? NULL : &mb->val;
    }
    cc_gid storey;
    if (!f[0] || f[0]->type != JR_STR || !cc_gid_valid(f[0]->s, f[0]->n)) return fail(err, "storey: a GlobalId is required");
    memcpy(storey.c, f[0]->s, 22);
    const cc_storey *st = cc_model_storey(m, &storey);
    if (!st || st->not_storey) return fail(err, "choose a storey");
    size_t count = 0;
    for (size_t i = 0; i < m->nelements; i++) count += m->elements[i].cls == CC_IFC_VIRTUAL_ELEMENT;
    if (count >= CC_REFPLANES_MAX) return fail(err, "too many reference planes (1000)");
    double o[3], n[3], x[3], w, h;
    if (size_arg(f[6], "width", &w, err) || size_arg(f[7], "height", &h, err)) return -1;
    if (f[5]) {
        if (f[2] || f[3] || f[4]) return fail(err, "give either points or origin and normal");
        if (f[5]->type != JR_ARR || f[5]->n != 3) return fail(err, "points: three points [x, y, z] are required");
        double p[3][3];
        for (int i = 0; i < 3; i++)
            if (vec3(&f[5]->items[i], "points", p[i], err)) return -1;
        if (cc_plane_from_points((const double(*)[3])p, n, x)) return fail(err, "the three points are on one line");
        memcpy(o, p[0], sizeof o);
    } else {
        if (vec3(f[2], "origin", o, err) || vec3(f[3], "normal", n, err)) return -1;
        if (unit(n)) return fail(err, "normal: not a direction");
        if (f[4]) {
            if (vec3(f[4], "x_axis", x, err)) return -1;
            double d = x[0] * n[0] + x[1] * n[1] + x[2] * n[2];
            for (int k = 0; k < 3; k++) x[k] -= d * n[k];
            if (unit(x)) return fail(err, "x_axis: along the normal");
        } else cc_plane_x_axis(n, x);
    }
    cc_element e;
    memset(&e, 0, sizeof e);
    cc_guid id = cc_guid_new();
    memcpy(e.id.c, id.c, 22);
    e.cls = CC_IFC_VIRTUAL_ELEMENT;
    e.storey = storey;
    e.generator.kind = CC_GEN_NONE;
    if (f[1]) {
        if (f[1]->type != JR_STR || !f[1]->n || f[1]->n > 200) return fail(err, "name: a text of 1 to 200 bytes");
        for (size_t i = 0; i < m->nelements; i++)
            if (m->elements[i].cls == CC_IFC_VIRTUAL_ELEMENT && str_eq(m->elements[i].name, f[1]->s, f[1]->n))
                return fail(err, "a reference plane has this name already");
        e.name = (cc_str){arena_dup(&l->arena, f[1]->s, f[1]->n), f[1]->n};
    } else e.name = next_name(m, &l->arena);
    /* the material: the model's first (an element needs one; a plane is exported without it) */
    if (!m->nmaterials) return fail(err, "the model has no material");
    e.material = m->materials[0].id;
    for (int k = 0; k < 3; k++) e.placement.origin[k] = o[k], e.placement.z_axis[k] = n[k], e.placement.x_axis[k] = x[k];
    e.placement.origin[2] = o[2] - st->elevation + 0.0;
    cc_kv *kv = arena_alloc(&l->arena, 2 * sizeof *kv, 8);
    if (!kv || !e.name.p) return fail(err, "out of memory");
    /* sorted by key bytes: Height < Width */
    kv[0].key = (cc_str){CC_REFPLANE_HEIGHT_KEY, strlen(CC_REFPLANE_HEIGHT_KEY)}, kv[0].value = num_text(&l->arena, h);
    kv[1].key = (cc_str){CC_REFPLANE_WIDTH_KEY, strlen(CC_REFPLANE_WIDTH_KEY)}, kv[1].value = num_text(&l->arena, w);
    if (!kv[0].value.p || !kv[1].value.p) return fail(err, "out of memory");
    e.properties = kv, e.nproperties = 2;
    cc_command *c = cc_cmds_push(l, CC_CMD_ADD_ELEMENT);
    if (!c) return fail(err, "out of memory");
    c->element = e;
    return 0;
}
