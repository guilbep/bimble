/* CONC1 (ADR 0077): footings, raft, openings and sleeves as verbs (verbs_concrete.h). C additions:
 * Rust v0.19.0 has the pad_footing generator and openings as data (model.rs Opening), no verb or
 * tool that makes them; the references are hand-computed in test/concrete_test.c. */
#include "verbs_concrete.h"
#include <string.h>
#include "guid.h"
#include "ifc_math.h"
#include "library.h"
#include "math.h"
#include "verbs.h"

#define EPS 1e-6
#define CC_PI 3.141592653589793

static int finite_(double x) { return x - x == 0.0; }
static double abs_(double x) { return x < 0 ? -x : x; }
static double min_(double a, double b) { return a < b ? a : b; }
static double max_(double a, double b) { return a > b ? a : b; }

static int fail(Buf *err, const char *msg) {
    buf_clear(err);
    buf_puts(err, msg);
    return -1;
}
static int fail2(Buf *err, const char *a, const char *b) {
    buf_clear(err);
    buf_puts(err, a);
    buf_puts(err, b);
    return -1;
}

/* ---- arguments: an object of known keys, each at most once ---- */

static int get_args(const JrVal *args, const char *const *names, int n, const JrVal **out, Buf *err) {
    for (int i = 0; i < n; i++) out[i] = NULL;
    if (!args || args->type == JR_NULL) return 0;
    if (args->type != JR_OBJ) return fail(err, "bad arguments: expected an object");
    for (size_t j = 0; j < args->n; j++) {
        const JrMember *mb = &args->members[j];
        int k = -1;
        for (int i = 0; i < n && k < 0; i++)
            if (jr_key_is(mb, names[i])) k = i;
        if (k < 0) {
            buf_clear(err);
            buf_puts(err, "bad arguments: unknown field `");
            buf_append(err, mb->key, mb->key_n < 64 ? mb->key_n : 64);
            buf_puts(err, "`");
            return -1;
        }
        if (out[k]) return fail2(err, "bad arguments: duplicate field ", names[k]);
        out[k] = mb->val.type == JR_NULL ? NULL : &mb->val;
    }
    return 0;
}

/* A size within [10, 100000] mm (verbs::size), or the default when absent. */
static int size_arg(const JrVal *v, const char *what, double def, double *out, Buf *err) {
    if (!v) {
        *out = def;
        return 0;
    }
    if (v->type != JR_NUM || !(v->num >= 10.0 && v->num <= 100000.0)) {
        buf_clear(err);
        buf_puts(err, what);
        buf_puts(err, ": ");
        if (v->type == JR_NUM) cc_buf_display_f64(err, v->num);
        else buf_puts(err, "not a number");
        buf_puts(err, " is not a size within [10, 100000] mm");
        return -1;
    }
    *out = v->num + 0.0;
    return 0;
}
/* A length >= 0 within the model's range (a sill), or the default. */
static int len_arg(const JrVal *v, const char *what, double def, double *out, Buf *err) {
    if (!v) {
        *out = def;
        return 0;
    }
    if (v->type != JR_NUM || !(v->num >= 0.0 && v->num <= CC_MAX_MM)) {
        buf_clear(err);
        buf_puts(err, what);
        buf_puts(err, ": not a length within [0, 10000000] mm");
        return -1;
    }
    *out = v->num + 0.0;
    return 0;
}
static int pt_arg(const JrVal *v, const char *what, double out[2], Buf *err) {
    if (!v || v->type != JR_ARR || v->n != 2 || v->items[0].type != JR_NUM || v->items[1].type != JR_NUM ||
        !finite_(v->items[0].num) || !finite_(v->items[1].num) || abs_(v->items[0].num) > CC_MAX_MM ||
        abs_(v->items[1].num) > CC_MAX_MM)
        return fail2(err, what, ": a point [x, y] in mm within \xc2\xb1" "10000000 is required");
    out[0] = v->items[0].num + 0.0, out[1] = v->items[1].num + 0.0;
    return 0;
}
static int gid_arg(const JrVal *v, const char *what, cc_gid *out, Buf *err) {
    if (!v || v->type != JR_STR || !cc_gid_valid(v->s, v->n)) return fail2(err, what, ": a GlobalId is required");
    memcpy(out->c, v->s, 22);
    return 0;
}
static int str_is(const JrVal *v, const char *s) { return v && v->type == JR_STR && v->n == strlen(s) && !memcmp(v->s, s, v->n); }

/* ---- model helpers ---- */

static int str_eq(cc_str a, const char *p, size_t n) { return a.n == n && (n == 0 || !memcmp(a.p, p, n)); }

/* verbs::material: the named one (model, then library), else the first concrete, else C30/37. */
typedef struct {
    cc_gid id;
    int add;
    cc_material mat;
} Mat;
static int material(const cc_model *m, const JrVal *name, Mat *out, Buf *err) {
    memset(out, 0, sizeof *out);
    if (name && name->type != JR_STR) return fail(err, "material: a name is required");
    if (name) {
        for (size_t i = 0; i < m->nmaterials; i++)
            if (str_eq(m->materials[i].name, name->s, name->n)) {
                out->id = m->materials[i].id;
                return 0;
            }
        const cc_library_material *lm = cc_library_lookup(name->s, name->n);
        if (!lm) {
            buf_clear(err);
            buf_puts(err, "unknown material: ");
            buf_append(err, name->s, name->n < 200 ? name->n : 200);
            return -1;
        }
        for (size_t i = 0; i < m->nmaterials; i++)
            if (str_eq(m->materials[i].name, lm->name, strlen(lm->name)) && m->materials[i].category == lm->category) {
                out->id = m->materials[i].id;
                return 0;
            }
        out->mat = cc_library_to_material(lm);
        out->id = out->mat.id, out->add = 1;
        return 0;
    }
    for (size_t i = 0; i < m->nmaterials; i++)
        if (m->materials[i].category == CC_CAT_CONCRETE) {
            out->id = m->materials[i].id;
            return 0;
        }
    out->mat = cc_library_to_material(cc_library_lookup("C30/37", 6));
    out->id = out->mat.id, out->add = 1;
    return 0;
}

/* The first name `prefix`<k> no element has. */
static cc_str next_name(const cc_model *m, const char *prefix, Arena *a) {
    char buf[32];
    size_t pn = strlen(prefix);
    memcpy(buf, prefix, pn);
    for (uint64_t k = 1;; k++) {
        char d[24];
        int nd = 0;
        for (uint64_t v = k; v; v /= 10) d[nd++] = (char)('0' + v % 10);
        size_t n = pn;
        while (nd) buf[n++] = d[--nd];
        int used = 0;
        for (size_t i = 0; i < m->nelements && !used; i++) used = str_eq(m->elements[i].name, buf, n);
        if (!used) return (cc_str){arena_dup(a, buf, n), n};
    }
}

static int vertical(const cc_placement *p) {
    return abs_(p->z_axis[0]) < EPS && abs_(p->z_axis[1]) < EPS && abs_(p->z_axis[2] - 1.0) < EPS;
}
/* A plan point in an element's plan frame (x along x_axis, y = z x x). */
static void plan_local(const cc_placement *p, const double q[2], double out[2]) {
    double dx = q[0] - p->origin[0], dy = q[1] - p->origin[1];
    out[0] = dx * p->x_axis[0] + dy * p->x_axis[1];
    out[1] = -dx * p->x_axis[1] + dy * p->x_axis[0];
}

static const cc_element *host_under(const cc_model *m, const cc_gid *storey, const double q[2], cc_gen_kind kind) {
    const cc_element *best = NULL;
    for (size_t i = 0; i < m->nelements; i++) {
        const cc_element *e = &m->elements[i];
        if (e->generator.kind != kind || !vertical(&e->placement)) continue;
        if (e->cls != (kind == CC_GEN_RECT_COLUMN ? CC_IFC_COLUMN : CC_IFC_WALL)) continue; /* not a footing's wall generator */
        if (storey && cc_gid_cmp(&e->storey, storey)) continue;
        double l[2];
        plan_local(&e->placement, q, l);
        int in = kind == CC_GEN_RECT_COLUMN
                     ? abs_(l[0]) <= e->generator.u.col.width / 2.0 + EPS && abs_(l[1]) <= e->generator.u.col.depth / 2.0 + EPS
                     : l[0] >= -EPS && l[0] <= e->generator.u.wall.length + EPS &&
                           abs_(l[1]) <= e->generator.u.wall.thickness / 2.0 + EPS;
        if (in && (!best || e->placement.origin[2] < best->placement.origin[2])) best = e; /* the lowest */
    }
    return best;
}

static int push_element(const cc_model *m, cc_element *e, const Mat *mat, const char *predefined, cc_cmds *l, Buf *err) {
    cc_guid id = cc_guid_new();
    memcpy(e->id.c, id.c, 22);
    e->material = mat->id;
    if (predefined) {
        cc_kv *kv = arena_alloc(&l->arena, sizeof *kv, 8);
        if (!kv) return fail(err, "out of memory");
        kv->key = (cc_str){CC_PREDEFINED_KEY, strlen(CC_PREDEFINED_KEY)};
        kv->value = (cc_str){predefined, strlen(predefined)};
        e->properties = kv, e->nproperties = 1;
    }
    (void)m;
    if (mat->add) {
        cc_command *c = cc_cmds_push(l, CC_CMD_ADD_MATERIAL);
        if (!c) return fail(err, "out of memory");
        c->material = mat->mat;
    }
    cc_command *c = cc_cmds_push(l, CC_CMD_ADD_ELEMENT);
    if (!c) return fail(err, "out of memory");
    c->element = *e;
    return 0;
}

/* ---- footing_add ---- */

int cc_vh_footing_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"storey", "kind", "a", "b", "width", "length", "depth", "material"};
    const JrVal *f[8];
    if (get_args(args, F, 8, f, err)) return -1;
    int strip = 0;
    if (f[1] && !str_is(f[1], "pad") && !(strip = str_is(f[1], "strip")))
        return fail(err, "kind: \"pad\" or \"strip\"");
    cc_gid storey;
    if (gid_arg(f[0], "storey", &storey, err)) return -1;
    if (!cc_model_storey(m, &storey)) return fail(err, "choose a storey");
    double a[2], b[2], width, length, depth;
    if (pt_arg(f[2], "a", a, err)) return -1;
    if (f[3] && pt_arg(f[3], "b", b, err)) return -1;
    if (size_arg(f[4], "width", strip ? 600.0 : 1200.0, &width, err) || size_arg(f[5], "length", 1200.0, &length, err) ||
        size_arg(f[6], "depth", strip ? 400.0 : 500.0, &depth, err))
        return -1;
    Mat mat;
    if (material(m, f[7], &mat, err)) return -1;
    cc_element e;
    memset(&e, 0, sizeof e);
    e.cls = CC_IFC_FOOTING;
    e.storey = storey;
    e.name = next_name(m, "F", &l->arena);
    if (!e.name.p) return fail(err, "out of memory");
    cc_placement p = {{a[0], a[1], -depth}, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}};
    if (!strip) {
        const cc_element *col = host_under(m, &storey, a, CC_GEN_RECT_COLUMN);
        if (col) { /* centred under the column, top at its base */
            p = col->placement;
            p.origin[2] = col->placement.origin[2] - depth;
            e.storey = col->storey;
        }
        e.placement = p;
        e.generator.kind = CC_GEN_PAD_FOOTING;
        e.generator.u.col.width = width, e.generator.u.col.depth = length, e.generator.u.col.height = depth;
        return push_element(m, &e, &mat, NULL, l, err);
    }
    const cc_element *wall = host_under(m, &storey, a, CC_GEN_WALL);
    if (wall && f[3]) { /* both points on the wall, else free */
        double lb[2];
        plan_local(&wall->placement, b, lb);
        if (!(lb[0] >= -EPS && lb[0] <= wall->generator.u.wall.length + EPS &&
              abs_(lb[1]) <= wall->generator.u.wall.thickness / 2.0 + EPS))
            wall = NULL;
    }
    e.generator.kind = CC_GEN_WALL;
    e.generator.u.wall.thickness = width, e.generator.u.wall.height = depth;
    if (wall) { /* along the whole wall, top at its base */
        p = wall->placement;
        p.origin[2] = wall->placement.origin[2] - depth;
        e.storey = wall->storey;
        e.generator.u.wall.length = wall->generator.u.wall.length;
    } else {
        if (!f[3]) return fail(err, "Strip footing: b is required (or a point on a wall)");
        double dx = b[0] - a[0], dy = b[1] - a[1], len = cc_hypot(dx, dy);
        if (len < 10.0) return fail(err, "the two points are too close");
        p.x_axis[0] = dx / len, p.x_axis[1] = dy / len;
        e.generator.u.wall.length = len;
    }
    e.placement = p;
    return push_element(m, &e, &mat, "STRIP_FOOTING", l, err);
}

/* ---- raft_add ---- */

int cc_vh_raft_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"storey", "outline", "thickness", "material"};
    const JrVal *f[4];
    if (get_args(args, F, 4, f, err)) return -1;
    double t;
    if (size_arg(f[2], "thickness", 400.0, &t, err)) return -1;
    cc_gid storey;
    if (f[0]) {
        if (gid_arg(f[0], "storey", &storey, err)) return -1;
    } else { /* the lowest storey */
        const cc_storey *lo = NULL;
        for (size_t i = 0; i < m->nstoreys; i++)
            if (!m->storeys[i].not_storey && (!lo || m->storeys[i].elevation < lo->elevation)) lo = &m->storeys[i];
        if (!lo) return fail(err, "choose a storey");
        storey = lo->id;
    }
    const JrVal *o = f[1];
    if (!o || o->type != JR_ARR) return fail(err, "outline: a list of points [x, y] is required");
    if (o->n > 1000) return fail(err, "outline: at most 1000 points");
    double(*pts)[2] = arena_alloc(&l->arena, (o->n + 1) * sizeof *pts, 8);
    if (!pts) return fail(err, "out of memory");
    for (size_t i = 0; i < o->n; i++)
        if (pt_arg(&o->items[i], "outline", pts[i], err)) return -1;
    char name[256];
    if (f[3] && (f[3]->type != JR_STR || f[3]->n >= sizeof name)) return fail(err, "material: a name is required");
    if (f[3]) memcpy(name, f[3]->s, f[3]->n), name[f[3]->n] = 0;
    double sizes[5] = {300.0, 250.0, 500.0, 200.0, t};
    size_t first = l->n;
    if (cc_verb_new_slab(m, &storey, (const double(*)[2])pts, o->n, sizes, f[3] ? name : NULL, l, err)) return -1;
    for (size_t i = first; i < l->n; i++) {
        if (l->v[i].op != CC_CMD_ADD_ELEMENT) continue;
        cc_element *e = &l->v[i].element;
        e->placement.origin[2] = -t; /* top at the storey's elevation */
        e->name = next_name(m, "R", &l->arena);
        cc_kv *kv = arena_alloc(&l->arena, sizeof *kv, 8);
        if (!kv || !e->name.p) return fail(err, "out of memory");
        kv->key = (cc_str){CC_PREDEFINED_KEY, strlen(CC_PREDEFINED_KEY)};
        kv->value = (cc_str){"BASESLAB", 8};
        e->properties = kv, e->nproperties = 1;
    }
    return 0;
}

/* ---- openings ---- */

typedef struct {
    const cc_element *e;
    int wall;
    double len, thick, height; /* wall: length, thickness, height; slab: thickness */
} Host;

static int host_of(const cc_model *m, const JrVal *v, Host *h, Buf *err) {
    cc_gid id;
    if (gid_arg(v, "host", &id, err)) return -1;
    memset(h, 0, sizeof *h);
    h->e = cc_model_element(m, &id);
    if (!h->e) {
        buf_clear(err);
        buf_puts(err, "no element GlobalId(");
        cc_buf_gid(err, &id);
        buf_putc(err, ')');
        return -1;
    }
    const cc_generator *g = &h->e->generator;
    if (vertical(&h->e->placement) && g->kind == CC_GEN_WALL && h->e->cls == CC_IFC_WALL) {
        h->wall = 1, h->len = g->u.wall.length, h->thick = g->u.wall.thickness, h->height = g->u.wall.height;
        return 0;
    }
    if (vertical(&h->e->placement) && g->kind == CC_GEN_SLAB && h->e->cls == CC_IFC_SLAB) {
        h->thick = g->u.slab.thickness;
        return 0;
    }
    return fail(err, "host: an opening goes in an IfcWall or an IfcSlab made by Bimble (a wall or slab generator)");
}

/* The opening's own frame mapped into the host's: host = o + x*px + y*py + z*pz. */
static void to_host(const cc_placement *pl, const double q[3], double out[3]) {
    const double *x = pl->x_axis, *z = pl->z_axis;
    double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    for (int k = 0; k < 3; k++) out[k] = pl->origin[k] + x[k] * q[0] + y[k] * q[1] + z[k] * q[2];
}
static void grow(double box[6], const double p[3]) {
    for (int k = 0; k < 3; k++) box[k] = min_(box[k], p[k]), box[3 + k] = max_(box[3 + k], p[k]);
}
/* The host-local box of an opening's solid (its generator's corners through its placement). */
static void opening_box(const cc_opening *o, double box[6]) {
    for (int k = 0; k < 3; k++) box[k] = 1e300, box[3 + k] = -1e300;
    const cc_generator *g = &o->generator;
    double q[3], r[3];
    const cc_loop *ol = NULL;
    double h = 0, w = 0, d = 0;
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING: w = g->u.col.width, d = g->u.col.depth, h = g->u.col.height; break;
    case CC_GEN_RECT_BEAM: w = g->u.beam.width, d = g->u.beam.height, h = g->u.beam.length; break;
    case CC_GEN_WALL:
        for (int i = 0; i < 8; i++) {
            q[0] = i & 1 ? g->u.wall.length : 0.0, q[1] = (i & 2 ? 0.5 : -0.5) * g->u.wall.thickness;
            q[2] = i & 4 ? g->u.wall.height : 0.0;
            to_host(&o->placement, q, r), grow(box, r);
        }
        return;
    case CC_GEN_SLAB: ol = &g->u.slab.outline, h = g->u.slab.thickness; break;
    case CC_GEN_EXTRUDED_POLYGON: ol = &g->u.poly.outline, h = g->u.poly.height; break;
    case CC_GEN_MESH:
        for (size_t i = 0; i < g->u.mesh.npositions; i++) to_host(&o->placement, g->u.mesh.positions[i], r), grow(box, r);
        return;
    default: return;
    }
    if (ol) {
        for (size_t i = 0; i < ol->n; i++)
            for (int k = 0; k < 2; k++) {
                q[0] = ol->pts[i][0], q[1] = ol->pts[i][1], q[2] = k ? h : 0.0;
                to_host(&o->placement, q, r), grow(box, r);
            }
        return;
    }
    for (int i = 0; i < 8; i++) {
        q[0] = (i & 1 ? 0.5 : -0.5) * w, q[1] = (i & 2 ? 0.5 : -0.5) * d, q[2] = i & 4 ? h : 0.0;
        to_host(&o->placement, q, r), grow(box, r);
    }
}
static int boxes_overlap(const double a[6], const double b[6]) {
    for (int k = 0; k < 3; k++)
        if (!(a[k] < b[3 + k] - EPS && b[k] < a[3 + k] - EPS)) return 0;
    return 1;
}

/* Point in the outline (boundary counts as in). */
static double cross3(const double *o, const double *a, const double *b) {
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}
static double seg_dist(const double *a, const double *b, const double *p) {
    double dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2 : 0.0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return cc_hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dy);
}
static int in_poly(const cc_loop *ol, const double p[2]) {
    int in = 0;
    for (size_t i = 0, j = ol->n - 1; i < ol->n; j = i++) {
        const double *a = ol->pts[i], *b = ol->pts[j];
        if (seg_dist(a, b, p) <= EPS) return 1;
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) in = !in;
    }
    return in;
}
static int proper_cross(const double *a, const double *b, const double *c, const double *d) {
    double d1 = cross3(c, d, a), d2 = cross3(c, d, b), d3 = cross3(a, b, c), d4 = cross3(a, b, d);
    return ((d1 > EPS && d2 < -EPS) || (d1 < -EPS && d2 > EPS)) && ((d3 > EPS && d4 < -EPS) || (d3 < -EPS && d4 > EPS));
}
static int rect_in_poly(const cc_loop *ol, double x0, double y0, double x1, double y1) {
    double c[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; i++)
        if (!in_poly(ol, c[i])) return 0;
    for (size_t i = 0; i < ol->n; i++) {
        const double *p = ol->pts[i];
        if (p[0] > x0 + EPS && p[0] < x1 - EPS && p[1] > y0 + EPS && p[1] < y1 - EPS) return 0;
        const double *q = ol->pts[(i + 1) % ol->n];
        for (int k = 0; k < 4; k++)
            if (proper_cross(p, q, c[k], c[(k + 1) % 4])) return 0;
    }
    return 1;
}
static int disc_in_poly(const cc_loop *ol, const double c[2], double r) {
    if (!in_poly(ol, c)) return 0;
    for (size_t i = 0; i < ol->n; i++)
        if (seg_dist(ol->pts[i], ol->pts[(i + 1) % ol->n], c) < r - EPS) return 0;
    return 1;
}

/* What one opening is, in the host's terms. wall: cx along the wall, sill its bottom; slab: cx, cy
 * its centre in the slab's plan frame, w x len its sizes. round: a sleeve of diameter `dia`. */
typedef struct {
    int round;
    double cx, cy, w, len, h, sill, dia;
} Spec;

static int build(const Host *h, const Spec *s, Arena *a, cc_opening *o, Buf *err) {
    memset(o, 0, sizeof *o);
    double os = CC_OPENING_OVERSHOOT, r = s->dia / 2.0;
    cc_placement p = {{s->cx, s->cy, -os}, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}};
    double depth = h->thick + 2.0 * os;
    if (h->wall) {
        double lo = s->round ? s->cx - r : s->cx - s->w / 2.0, hi = s->round ? s->cx + r : s->cx + s->w / 2.0;
        double top = s->sill + (s->round ? s->dia : s->h);
        if (lo < -EPS || hi > h->len + EPS || s->sill < -EPS || top > h->height + EPS)
            return fail(err, "the opening leaves its host: it must stay inside the wall");
        if (s->round) { /* a horizontal prism across the wall */
            cc_placement q = {{s->cx, -(h->thick / 2.0 + os), s->sill + r}, {0.0, 1.0, 0.0}, {1.0, 0.0, 0.0}};
            p = q;
        } else {
            cc_placement q = {{s->cx, 0.0, s->sill}, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}};
            p = q;
        }
    } else {
        const cc_loop *ol = &h->e->generator.u.slab.outline;
        double c[2] = {s->cx, s->cy};
        int in = s->round ? disc_in_poly(ol, c, r)
                          : rect_in_poly(ol, s->cx - s->w / 2.0, s->cy - s->len / 2.0, s->cx + s->w / 2.0, s->cy + s->len / 2.0);
        if (!in) return fail(err, "the opening leaves its host: it must stay inside the slab's outline");
    }
    o->placement = p;
    if (s->round) {
        double(*pts)[2] = arena_alloc(a, CC_SLEEVE_SIDES * sizeof *pts, 8);
        if (!pts) return fail(err, "out of memory");
        for (int i = 0; i < CC_SLEEVE_SIDES; i++) {
            double t = 2.0 * CC_PI * i / CC_SLEEVE_SIDES;
            pts[i][0] = r * cc_cos(t) + 0.0, pts[i][1] = r * cc_sin(t) + 0.0;
        }
        o->generator.kind = CC_GEN_EXTRUDED_POLYGON;
        o->generator.u.poly.outline.pts = pts, o->generator.u.poly.outline.n = CC_SLEEVE_SIDES;
        o->generator.u.poly.height = depth;
    } else {
        o->generator.kind = CC_GEN_RECT_COLUMN;
        o->generator.u.col.width = s->w;
        o->generator.u.col.depth = h->wall ? depth : s->len;
        o->generator.u.col.height = h->wall ? s->h : depth;
    }
    return 0;
}

/* The spec of an opening the tool made (a box or a sleeve), from its data. -1: another shape. */
static int spec_of(const Host *h, const cc_opening *o, Spec *s) {
    memset(s, 0, sizeof *s);
    double r;
    if (cc_opening_is_sleeve(o, &r)) {
        s->round = 1, s->dia = 2.0 * r, s->cx = o->placement.origin[0];
        if (h->wall) s->sill = o->placement.origin[2] - r;
        else s->cy = o->placement.origin[1];
        return 0;
    }
    if (o->generator.kind != CC_GEN_RECT_COLUMN) return -1;
    s->cx = o->placement.origin[0], s->w = o->generator.u.col.width;
    if (h->wall) s->sill = o->placement.origin[2], s->h = o->generator.u.col.height;
    else s->cy = o->placement.origin[1], s->len = o->generator.u.col.depth;
    return 0;
}

void cc_opening_info_of(const cc_element *e, size_t k, cc_opening_info *out) { /* WAVE1B (B2), verbs_concrete.h */
    memset(out, 0, sizeof *out);
    Host h;
    memset(&h, 0, sizeof h);
    h.e = e;
    const cc_generator *g = &e->generator;
    if (g->kind == CC_GEN_WALL && e->cls == CC_IFC_WALL) h.wall = 1, h.len = g->u.wall.length, h.thick = g->u.wall.thickness, h.height = g->u.wall.height;
    else if (g->kind == CC_GEN_SLAB) h.thick = g->u.slab.thickness;
    out->wall = h.wall;
    if (k >= e->nopenings) {
        out->kind = 2;
        return;
    }
    opening_box(&e->openings[k], out->box);
    Spec s;
    if (spec_of(&h, &e->openings[k], &s)) {
        out->kind = 2;
        return;
    }
    out->kind = s.round ? 1 : 0;
    out->cx = s.cx, out->cy = s.cy, out->w = s.w, out->h = s.h, out->len = s.len, out->sill = s.sill, out->dia = s.dia;
}

/* The new list: the host's openings with `o` added (skip < 0) or in place of index `skip`;
 * refused when it overlaps another one. One set_openings Command. */
static int push_openings(const Host *h, const cc_opening *o, long skip, cc_cmds *l, Buf *err) {
    double nb[6];
    opening_box(o, nb);
    const cc_element *e = h->e;
    for (size_t i = 0; i < e->nopenings; i++) {
        if ((long)i == skip) continue;
        double ob[6];
        opening_box(&e->openings[i], ob);
        if (boxes_overlap(nb, ob)) return fail(err, "the opening overlaps another opening of its host");
    }
    size_t n = e->nopenings + (skip < 0);
    if (n > CC_MAX_OPENINGS) return fail(err, "too many openings");
    cc_opening *v = arena_alloc(&l->arena, n * sizeof *v, 8);
    if (!v) return fail(err, "out of memory");
    if (e->nopenings) memcpy(v, e->openings, e->nopenings * sizeof *v);
    v[skip < 0 ? (long)e->nopenings : skip] = *o;
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_OPENINGS);
    if (!c) return fail(err, "out of memory");
    c->id = e->id;
    c->element.openings = v, c->element.nopenings = n;
    return 0;
}

static const char *const OPENING_F[] = {"host", "a", "b", "width", "length", "height", "sill", "shape", "diameter", "id"};
enum { O_HOST, O_A, O_B, O_WIDTH, O_LENGTH, O_HEIGHT, O_SILL, O_SHAPE, O_DIA, O_ID, O_N };

/* The sizes given in `f` over the spec `s` (opening_add: the defaults; opening_set: the opening's). */
static int sizes_over(const Host *h, const JrVal *const *f, Spec *s, Buf *err) {
    if (s->round) {
        if (size_arg(f[O_DIA], "diameter", s->dia, &s->dia, err)) return -1;
        if (h->wall && len_arg(f[O_SILL], "sill", s->sill, &s->sill, err)) return -1;
        return 0;
    }
    if (size_arg(f[O_WIDTH], "width", s->w, &s->w, err)) return -1;
    if (h->wall) return size_arg(f[O_HEIGHT], "height", s->h, &s->h, err) || len_arg(f[O_SILL], "sill", s->sill, &s->sill, err);
    return size_arg(f[O_LENGTH], "length", s->len, &s->len, err);
}

int cc_vh_opening_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    const JrVal *f[O_N];
    if (get_args(args, OPENING_F, O_N - 1, f, err)) return -1; /* no "id": a new one is made */
    Host h;
    if (host_of(m, f[O_HOST], &h, err)) return -1;
    Spec s;
    memset(&s, 0, sizeof s);
    if (f[O_SHAPE] && !str_is(f[O_SHAPE], "rectangle") && !(s.round = str_is(f[O_SHAPE], "round")))
        return fail(err, "shape: \"rectangle\" or \"round\"");
    double a[2], la[2], b[2], lb[2];
    if (pt_arg(f[O_A], "a", a, err)) return -1;
    if (f[O_B] && pt_arg(f[O_B], "b", b, err)) return -1;
    plan_local(&h.e->placement, a, la);
    if (f[O_B]) plan_local(&h.e->placement, b, lb);
    s.dia = 150.0, s.w = h.wall ? 1000.0 : 600.0, s.len = 600.0, s.h = 1200.0, s.sill = 900.0;
    if (s.round && h.wall) s.sill = h.height / 2.0 - 75.0;
    if (sizes_over(&h, f, &s, err)) return -1;
    if (s.round && h.wall && !f[O_SILL]) s.sill = h.height / 2.0 - s.dia / 2.0; /* centred in the height */
    s.cx = la[0], s.cy = la[1];
    if (f[O_B] && !s.round) { /* two corners (a slab) or two ends along the wall */
        s.cx = (la[0] + lb[0]) / 2.0, s.w = abs_(lb[0] - la[0]);
        if (!h.wall) s.cy = (la[1] + lb[1]) / 2.0, s.len = abs_(lb[1] - la[1]);
        if (s.w < 10.0 || (!h.wall && s.len < 10.0)) return fail(err, "the opening is too small (10 mm at least)");
    }
    cc_opening o;
    if (build(&h, &s, &l->arena, &o, err)) return -1;
    cc_guid id = cc_guid_new();
    memcpy(o.id.c, id.c, 22);
    o.has_id = 1;
    return push_openings(&h, &o, -1, l, err);
}

static long find_opening(const Host *h, const JrVal *v, Buf *err) {
    cc_gid id;
    if (gid_arg(v, "id", &id, err)) return -1;
    for (size_t i = 0; i < h->e->nopenings; i++)
        if (h->e->openings[i].has_id && !cc_gid_cmp(&h->e->openings[i].id, &id)) return (long)i;
    fail(err, "id: no such opening in this host");
    return -1;
}

int cc_vh_opening_set(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    const JrVal *f[O_N];
    if (get_args(args, OPENING_F, O_N, f, err)) return -1;
    if (f[O_B] || f[O_SHAPE]) return fail(err, "opening_set: give the centre `a` and the sizes, not `b` or `shape`");
    Host h;
    if (host_of(m, f[O_HOST], &h, err)) return -1;
    long k = find_opening(&h, f[O_ID], err);
    if (k < 0) return -1;
    const cc_opening *old = &h.e->openings[k];
    Spec s;
    if (spec_of(&h, old, &s)) return fail(err, "this opening has another shape (imported): delete it and draw it again");
    if (sizes_over(&h, (const JrVal *const *)f, &s, err)) return -1;
    if (f[O_A]) {
        double a[2], la[2];
        if (pt_arg(f[O_A], "a", a, err)) return -1;
        plan_local(&h.e->placement, a, la);
        s.cx = la[0];
        if (!h.wall) s.cy = la[1];
    }
    cc_opening o;
    if (build(&h, &s, &l->arena, &o, err)) return -1;
    o.id = old->id, o.has_id = 1;
    return push_openings(&h, &o, k, l, err);
}

int cc_vh_opening_remove(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"host", "id"};
    const JrVal *f[2];
    if (get_args(args, F, 2, f, err)) return -1;
    cc_gid hid;
    if (gid_arg(f[0], "host", &hid, err)) return -1;
    const cc_element *e = cc_model_element(m, &hid);
    if (!e) return fail(err, "host: no such element");
    Host h = {e, 0, 0, 0, 0};
    long k = find_opening(&h, f[1], err);
    if (k < 0) return -1;
    cc_opening *v = e->nopenings > 1 ? arena_alloc(&l->arena, (e->nopenings - 1) * sizeof *v, 8) : NULL;
    if (e->nopenings > 1 && !v) return fail(err, "out of memory");
    for (size_t i = 0, j = 0; i < e->nopenings; i++)
        if ((long)i != k) v[j++] = e->openings[i];
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_OPENINGS);
    if (!c) return fail(err, "out of memory");
    c->id = e->id;
    c->element.openings = v, c->element.nopenings = e->nopenings - 1;
    return 0;
}
