/* CONC1 (ADR 0077): the Concrete tab's place kinds (concrete.h). */
#include "concrete.h"
#include <string.h>
#include "fmt.h"
#include "math.h"
#include "verbs.h"

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
