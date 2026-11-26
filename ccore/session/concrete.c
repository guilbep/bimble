/* CONC1 (ADR 0077): the Concrete tab's place kinds (concrete.h). */
#include "concrete.h"
#include <string.h>
#include "fmt.h"
#include "math.h"
#include "verbs.h"
#include "verbs_concrete.h" /* WAVE1B: cc_opening_info_of */

/* The sizes, keys sorted (the answer's order). */
enum {
    P_FOOTING_DEPTH, P_FOOTING_LENGTH, P_FOOTING_WIDTH, P_OPENING_HEIGHT, P_OPENING_LENGTH, P_OPENING_SILL,
    P_OPENING_WIDTH, P_RAFT_THICKNESS, P_SLEEVE_DIAMETER, P_STRIP_DEPTH, P_STRIP_WIDTH, P_N
};
static const char *const KEY[P_N] = {"footing_depth", "footing_length", "footing_width", "opening_height",
                                     "opening_length", "opening_sill", "opening_width", "raft_thickness",
                                     "sleeve_diameter", "strip_depth", "strip_width"};
static const double DEF[P_N] = {500, 1200, 1200, 1200, 600, 900, 1000, 400, 150, 400, 600};
_Static_assert(P_N == CC_CONC_N, "session.h CC_CONC_N");

void cc_conc_defaults(double *p) { memcpy(p, DEF, sizeof DEF); }

int cc_conc_kind(cc_kind k) { return k >= CC_KIND_FOOTING && k < CC_KIND_COUNT; }

int cc_conc_clicks(cc_kind k) {
    switch (k) {
    case CC_KIND_FOOTING:
    case CC_KIND_SLEEVE: return 1;
    case CC_KIND_RAFT: return 0;
    default: return 2;
    }
}

const char *cc_conc_hint(const cc_session *s) {
    size_t n = s->tool.npoints;
    switch (s->placing_kind) {
    case CC_KIND_FOOTING: return "Click a column (the footing goes under it) or a point on the level. Esc stops.";
    case CC_KIND_STRIP_FOOTING: return n ? "Click the end point." : "Click a wall (the footing goes under it) or the start point.";
    case CC_KIND_RAFT: return n < 3 ? "Click the next corner of the raft." : "Click the next corner; click the first corner or Enter closes.";
    case CC_KIND_OPENING: return n ? "Click the other end (wall) or the opposite corner (slab)." : "Click on a wall or a slab: the opening's first end or corner.";
    case CC_KIND_SLEEVE: return "Click on a wall or a slab where the sleeve goes. Esc stops.";
    default: return "";
    }
}

static int vertical(const cc_placement *p) { return p->z_axis[2] > 1.0 - 1e-9; }
static void plan_local(const cc_placement *p, const double q[2], double out[2]) {
    double dx = q[0] - p->origin[0], dy = q[1] - p->origin[1];
    out[0] = dx * p->x_axis[0] + dy * p->x_axis[1];
    out[1] = -dx * p->x_axis[1] + dy * p->x_axis[0];
}
static int in_outline(const cc_loop *ol, const double p[2]) {
    int in = 0;
    for (size_t i = 0, j = ol->n - 1; i < ol->n; j = i++) {
        const double *a = ol->pts[i], *b = ol->pts[j];
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) in = !in;
    }
    return in;
}

/* The wall, else the slab, under a plan point: the active storey's first, then any. */
static const cc_element *host_at(const cc_session *s, const double q[2]) {
    const cc_model *m = &s->model;
    for (int pass = 0; pass < 4; pass++) {
        int wall = pass % 2 == 0, any = pass >= 2;
        for (size_t i = 0; i < m->nelements; i++) {
            const cc_element *e = &m->elements[i];
            if (!vertical(&e->placement) || (!any && s->has_storey && cc_gid_cmp(&e->storey, &s->storey))) continue;
            double l[2];
            plan_local(&e->placement, q, l);
            if (wall && e->generator.kind == CC_GEN_WALL && e->cls == CC_IFC_WALL && l[0] >= -1e-6 && l[0] <= e->generator.u.wall.length + 1e-6 &&
                (l[1] < 0 ? -l[1] : l[1]) <= e->generator.u.wall.thickness / 2.0 + 1e-6)
                return e;
            if (!wall && e->generator.kind == CC_GEN_SLAB && e->cls == CC_IFC_SLAB && in_outline(&e->generator.u.slab.outline, l)) return e;
        }
    }
    return NULL;
}

static void pt(JsonW *w, const char *k, const double *p) {
    jw_key(w, k), jw_arr_begin(w), jw_f64(w, p[0]), jw_f64(w, p[1]), jw_arr_end(w);
}

int cc_conc_cmds(cc_session *s, const double *a, const double *b, int dry_run, cc_cmds *l, Buf *err) {
    if (!s->has_storey) {
        buf_puts(err, "choose a storey");
        return -1;
    }
    const double *p = s->conc;
    Buf args;
    buf_init(&args);
    JsonW w;
    jw_init(&w, &args, 0);
    jw_obj_begin(&w);
    const char *verb = "footing_add";
    switch (s->placing_kind) {
    case CC_KIND_FOOTING:
    case CC_KIND_STRIP_FOOTING: {
        int strip = s->placing_kind == CC_KIND_STRIP_FOOTING;
        pt(&w, "a", a);
        if (b) pt(&w, "b", b);
        jw_key(&w, "depth"), jw_f64(&w, p[strip ? P_STRIP_DEPTH : P_FOOTING_DEPTH]);
        jw_key(&w, "kind"), jw_str(&w, strip ? "strip" : "pad");
        if (!strip) jw_key(&w, "length"), jw_f64(&w, p[P_FOOTING_LENGTH]);
        if (s->material) jw_key(&w, "material"), jw_str(&w, s->material);
        jw_key(&w, "storey"), jw_str_n(&w, s->storey.c, 22);
        jw_key(&w, "width"), jw_f64(&w, p[strip ? P_STRIP_WIDTH : P_FOOTING_WIDTH]);
        break;
    }
    default: { /* opening, sleeve */
        int round = s->placing_kind == CC_KIND_SLEEVE;
        const cc_element *h = host_at(s, a);
        if (!h) {
            buf_free(&args);
            buf_puts(err, round ? "click on a wall or a slab: a sleeve goes through one" : "click on a wall or a slab: an opening goes through one");
            return -1;
        }
        verb = "opening_add";
        pt(&w, "a", a);
        if (b && !round) pt(&w, "b", b);
        if (round) jw_key(&w, "diameter"), jw_f64(&w, p[P_SLEEVE_DIAMETER]);
        int wall = h->generator.kind == CC_GEN_WALL;
        if (!round && wall) jw_key(&w, "height"), jw_f64(&w, p[P_OPENING_HEIGHT]);
        jw_key(&w, "host"), jw_str_n(&w, h->id.c, 22);
        if (round) jw_key(&w, "shape"), jw_str(&w, "round");
        if (!round && wall) jw_key(&w, "sill"), jw_f64(&w, p[P_OPENING_SILL]);
        break;
    }
    }
    jw_obj_end(&w);
    jw_finish(&w);
    int rc = args.oom ? (buf_puts(err, "out of memory"), -1)
                      : cc_verb_run_ex(&s->model, verb, args.p, args.len, dry_run, -1, s->rev, l, err) ? -1 : 0;
    buf_free(&args);
    return rc;
}

int cc_conc_raft_cmds(cc_session *s, const double (*pts)[2], size_t n, cc_cmds *l, Buf *err) {
    Buf args;
    buf_init(&args);
    JsonW w;
    jw_init(&w, &args, 0);
    jw_obj_begin(&w);
    if (s->material) jw_key(&w, "material"), jw_str(&w, s->material);
    jw_key(&w, "outline"), jw_arr_begin(&w);
    for (size_t i = 0; i < n; i++) jw_arr_begin(&w), jw_f64(&w, pts[i][0]), jw_f64(&w, pts[i][1]), jw_arr_end(&w);
    jw_arr_end(&w);
    jw_key(&w, "thickness"), jw_f64(&w, s->conc[P_RAFT_THICKNESS]);
    jw_obj_end(&w);
    jw_finish(&w);
    int rc = args.oom ? (buf_puts(err, "out of memory"), -1)
                      : cc_verb_run_ex(&s->model, "raft_add", args.p, args.len, 1, -1, s->rev, l, err) ? -1 : 0;
    buf_free(&args);
    return rc;
}

int cc_conc_set(cc_session *s, const JrVal *v, Buf *err) {
    if (v->type != JR_OBJ) {
        buf_puts(err, "concrete: expected a map of sizes");
        return -1;
    }
    double np[P_N];
    memcpy(np, s->conc, sizeof np);
    for (size_t i = 0; i < v->n; i++) {
        const JrMember *m = &v->members[i];
        int k = -1;
        for (int j = 0; j < P_N && k < 0; j++)
            if (jr_key_is(m, KEY[j])) k = j;
        if (k < 0) {
            buf_puts(err, "concrete: unknown size: ");
            buf_append(err, m->key, m->key_n < 64 ? m->key_n : 64);
            return -1;
        }
        double x = m->val.num, lo = k == P_OPENING_SILL ? 0.0 : 10.0, hi = k == P_OPENING_SILL ? 1e7 : 100000.0;
        if (m->val.type != JR_NUM || !(x >= lo && x <= hi)) {
            buf_puts(err, KEY[k]);
            buf_puts(err, k == P_OPENING_SILL ? ": not a length within [0, 10000000] mm" : ": not a size within [10, 100000] mm");
            return -1;
        }
        np[k] = x + 0.0;
    }
    memcpy(s->conc, np, sizeof np);
    return 0;
}

void cc_conc_json(const cc_session *s, JsonW *w) {
    jw_obj_begin(w);
    for (int k = 0; k < P_N; k++) jw_key(w, KEY[k]), jw_f64(w, s->conc[k]);
    jw_obj_end(w);
}

/* ---- WAVE1B (B2): openings selected and previewed (concrete.h) ---- */

static double elev_of(const cc_model *m, const cc_gid *storey) {
    const cc_storey *st = cc_model_storey(m, storey);
    return st ? st->elevation : 0.0;
}
/* A host-local point to world mm (the host's placement, its storey's elevation). */
static void host_world(const cc_element *e, double elev, const double q[3], double out[3]) {
    const double *x = e->placement.x_axis, *z = e->placement.z_axis, *o = e->placement.origin;
    double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    for (int k = 0; k < 3; k++) out[k] = o[k] + x[k] * q[0] + y[k] * q[1] + z[k] * q[2] + 0.0;
    out[2] += elev;
}
static void wv3(JsonW *w, const double v[3]) {
    jw_arr_begin(w);
    for (int k = 0; k < 3; k++) jw_f64(w, v[k] + 0.0);
    jw_arr_end(w);
}

void cc_conc_openings_json(const cc_model *m, const cc_element *e, JsonW *w) {
    double elev = elev_of(m, &e->storey);
    jw_arr_begin(w);
    for (size_t k = 0; k < e->nopenings; k++) {
        cc_opening_info in;
        cc_opening_info_of(e, k, &in);
        const cc_opening *o = &e->openings[k];
        int kind = o->has_id ? in.kind : 2;
        const double *b = in.box;
        double mid[3] = {(b[0] + b[3]) / 2.0, (b[1] + b[4]) / 2.0, (b[2] + b[5]) / 2.0}, c[3];
        jw_obj_begin(w);
        jw_key(w, "centre"), host_world(e, elev, mid, c), wv3(w, c);
        jw_key(w, "corners"), jw_arr_begin(w);
        for (int i = 0; i < 8; i++) {
            double q[3] = {b[i & 1 ? 3 : 0], b[i & 2 ? 4 : 1], b[i & 4 ? 5 : 2]};
            host_world(e, elev, q, c), wv3(w, c);
        }
        jw_arr_end(w);
        if (kind == 1) jw_key(w, "diameter"), jw_f64(w, in.dia);
        if (kind == 0 && in.wall) jw_key(w, "height"), jw_f64(w, in.h);
        jw_key(w, "id");
        if (o->has_id) jw_str_n(w, o->id.c, 22);
        else jw_str(w, "");
        jw_key(w, "kind"), jw_str(w, kind == 0 ? "opening" : kind == 1 ? "sleeve" : "other");
        if (kind == 0 && !in.wall) jw_key(w, "length"), jw_f64(w, in.len);
        if (kind != 2 && in.wall) jw_key(w, "sill"), jw_f64(w, in.sill);
        jw_key(w, "wall"), jw_bool(w, in.wall);
        if (kind == 0) jw_key(w, "width"), jw_f64(w, in.w);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

/* Slab test of the ray (local: origin o, direction d) against the box b grown by tol: the entry t
 * (0 when the origin is inside), -1 when missed. */
static double ray_box(const double o[3], const double d[3], const double b[6], double tol) {
    double t0 = 0.0, t1 = 1e300;
    for (int k = 0; k < 3; k++) {
        double lo = b[k] - tol, hi = b[3 + k] + tol;
        if (d[k] > -1e-12 && d[k] < 1e-12) {
            if (o[k] < lo || o[k] > hi) return -1.0;
            continue;
        }
        double a = (lo - o[k]) / d[k], c = (hi - o[k]) / d[k];
        if (a > c) {
            double t = a;
            a = c, c = t;
        }
        if (a > t0) t0 = a;
        if (c < t1) t1 = c;
        if (t0 > t1) return -1.0;
    }
    return t0;
}

int cc_conc_opening_at(const cc_model *m, const double ray[6], double t_max, double tol, cc_gid *host, cc_gid *id) {
    double d[3] = {ray[3], ray[4], ray[5]}, l = cc_sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 0.0) || l != l) return 0;
    for (int k = 0; k < 3; k++) d[k] /= l;
    double best = 1e300;
    int found = 0;
    for (size_t i = 0; i < m->nelements; i++) {
        const cc_element *e = &m->elements[i];
        if (!e->nopenings) continue;
        double elev = elev_of(m, &e->storey);
        const double *x = e->placement.x_axis, *z = e->placement.z_axis, *po = e->placement.origin;
        double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
        double r[3] = {ray[0] - po[0], ray[1] - po[1], ray[2] - po[2] - elev};
        double lo[3] = {r[0] * x[0] + r[1] * x[1] + r[2] * x[2], r[0] * y[0] + r[1] * y[1] + r[2] * y[2], r[0] * z[0] + r[1] * z[1] + r[2] * z[2]};
        double ld[3] = {d[0] * x[0] + d[1] * x[1] + d[2] * x[2], d[0] * y[0] + d[1] * y[1] + d[2] * y[2], d[0] * z[0] + d[1] * z[1] + d[2] * z[2]};
        for (size_t k = 0; k < e->nopenings; k++) {
            if (!e->openings[k].has_id) continue;
            cc_opening_info in;
            cc_opening_info_of(e, k, &in);
            double t = ray_box(lo, ld, in.box, tol);
            if (t < 0.0 || (t_max >= 0.0 && t > t_max + tol) || t >= best) continue;
            best = t, found = 1, *host = e->id, *id = e->openings[k].id;
        }
    }
    return found;
}

int cc_conc_opening_ghost(const cc_model *m, const cc_cmds *in, cc_cmds *out) {
    for (size_t i = 0; i < in->n; i++) {
        const cc_command *c = &in->v[i];
        if (c->op != CC_CMD_SET_OPENINGS) continue;
        const cc_element *h = cc_model_element(m, &c->id);
        if (!h) return -1;
        const cc_opening *o = NULL; /* the one the host has not got */
        for (size_t k = 0; k < c->element.nopenings && !o; k++) {
            const cc_opening *q = &c->element.openings[k];
            int old = 0;
            for (size_t j = 0; j < h->nopenings && !old; j++) old = q->has_id && h->openings[j].has_id && !cc_gid_cmp(&q->id, &h->openings[j].id);
            if (!old) o = q;
        }
        if (!o) return -1;
        cc_element g;
        memset(&g, 0, sizeof g);
        g.id = o->has_id ? o->id : h->id;
        g.cls = CC_IFC_PROXY, g.storey = h->storey, g.material = h->material, g.generator = o->generator;
        g.name = (cc_str){"opening", 7};
        const double *x = h->placement.x_axis, *z = h->placement.z_axis;
        double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
        for (int k = 0; k < 3; k++) {
            const double *q = o->placement.origin, *qz = o->placement.z_axis, *qx = o->placement.x_axis;
            g.placement.origin[k] = h->placement.origin[k] + x[k] * q[0] + y[k] * q[1] + z[k] * q[2];
            g.placement.z_axis[k] = x[k] * qz[0] + y[k] * qz[1] + z[k] * qz[2];
            g.placement.x_axis[k] = x[k] * qx[0] + y[k] * qx[1] + z[k] * qx[2];
        }
        cc_command *a = cc_cmds_push(out, CC_CMD_ADD_ELEMENT);
        if (!a) return -1;
        a->element = g;
        return 0;
    }
    return -1;
}

void cc_conc_opening_rows_json(const cc_model *m, JsonW *w) { /* WAVE1B (B1), concrete.h */
    jw_arr_begin(w);
    for (size_t i = 0; i < m->nelements; i++) {
        const cc_element *e = &m->elements[i];
        if (!e->nopenings) continue;
        const cc_storey *st = cc_model_storey(m, &e->storey);
        double t = e->generator.kind == CC_GEN_WALL ? e->generator.u.wall.thickness
                   : e->generator.kind == CC_GEN_SLAB ? e->generator.u.slab.thickness
                                                       : 0.0;
        for (size_t k = 0; k < e->nopenings; k++) {
            cc_opening_info in;
            cc_opening_info_of(e, k, &in);
            const cc_opening *o = &e->openings[k];
            jw_obj_begin(w);
            if (in.kind == 1) jw_key(w, "diameter"), jw_f64(w, in.dia);
            if (in.kind == 0 && in.wall) jw_key(w, "height"), jw_f64(w, in.h);
            jw_key(w, "host"), jw_str_n(w, e->id.c, 22);
            jw_key(w, "host_name"), jw_str_n(w, e->name.n ? e->name.p : "", e->name.n);
            jw_key(w, "id");
            if (o->has_id) jw_str_n(w, o->id.c, 22);
            else jw_str(w, "");
            jw_key(w, "kind"), jw_str(w, in.kind == 0 ? "opening" : in.kind == 1 ? "sleeve" : "other");
            if (in.kind == 0 && !in.wall) jw_key(w, "length"), jw_f64(w, in.len);
            jw_key(w, "level"), jw_str_n(w, st && st->name.n ? st->name.p : "", st ? st->name.n : 0);
            jw_key(w, "volume_m3");
            if (in.kind == 1 && t > 0) jw_f64(w, 3.141592653589793 * (in.dia / 2.0) * (in.dia / 2.0) * t / 1e9);
            else if (in.kind == 0 && t > 0) jw_f64(w, in.w * (in.wall ? in.h : in.len) * t / 1e9);
            else jw_null(w);
            if (in.kind == 0) jw_key(w, "width"), jw_f64(w, in.w);
            jw_obj_end(w);
        }
    }
    jw_arr_end(w);
}
