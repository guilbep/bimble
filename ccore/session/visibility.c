/* Visibility and the section box (visibility.h). */
#include <stdlib.h>
#include <string.h>

#include "fmt.h"
#include "json_r.h"
#include "math.h"
#include "strtod.h"
#include "visibility.h"

#define MIN_SIZE 10.0f /* section.rs:12 */
#define MAX_LIST 50    /* calls.rs MAX_LIST, `capped` */

/* ---- GlobalId sets ---- */

static size_t lower(const cc_gidset *s, const cc_gid *g) {
    size_t lo = 0, hi = s->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cc_gid_cmp(&s->v[mid], g) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

int cc_gidset_has(const cc_gidset *s, const cc_gid *g) {
    size_t i = lower(s, g);
    return i < s->n && !cc_gid_cmp(&s->v[i], g);
}

int cc_gidset_add(cc_gidset *s, const cc_gid *g) {
    size_t i = lower(s, g);
    if (i < s->n && !cc_gid_cmp(&s->v[i], g)) return 0;
    if (s->n == s->cap) {
        size_t c = s->cap ? 2 * s->cap : 16;
        cc_gid *p = realloc(s->v, c * sizeof *p);
        if (!p) return -1;
        s->v = p, s->cap = c;
    }
    memmove(s->v + i + 1, s->v + i, (s->n - i) * sizeof *s->v);
    s->v[i] = *g;
    s->n++;
    return 0;
}

void cc_gidset_remove(cc_gidset *s, const cc_gid *g) {
    size_t i = lower(s, g);
    if (i < s->n && !cc_gid_cmp(&s->v[i], g)) memmove(s->v + i, s->v + i + 1, (--s->n - i) * sizeof *s->v);
}

static int set_copy(cc_gidset *s, const cc_gid *g, size_t n) {
    s->n = 0;
    for (size_t i = 0; i < n; i++)
        if (cc_gidset_add(s, &g[i])) return -1;
    return 0;
}

/* ---- state ---- */

void cc_vis_init(cc_vis *v) {
    memset(v, 0, sizeof *v);
    v->drag_face = -1;
}

void cc_vis_free(cc_vis *v) {
    free(v->hidden.v), free(v->isolated.v), free(v->temp.v);
    cc_vis_init(v);
}

void cc_vis_reset(cc_vis *v) {
    v->hidden.n = v->isolated.n = v->temp.n = 0;
    v->has_temp = v->temp_isolate = v->reveal = 0;
    v->section_on = v->has_box = 0;
    v->drag_face = -1;
}

int cc_vis_hidden(const cc_vis *v, const cc_gid *g) {
    return cc_gidset_has(&v->hidden, g) || (v->isolated.n && !cc_gidset_has(&v->isolated, g)) ||
           (v->has_temp && cc_gidset_has(&v->temp, g) != v->temp_isolate);
}

int cc_vis_hidden_in_view(const cc_vis *v) { return v->hidden.n || v->isolated.n; }

int cc_vis_hide_or_isolate(cc_vis *v, int a, const cc_gid *sel, size_t n) {
    if (!n) return 0;
    int rc = 0;
    switch (a) {
    case CC_VIS_HIDE_NOW:
    case CC_VIS_ISOLATE_NOW: {
        int isolate = a == CC_VIS_ISOLATE_NOW;
        /* Hiding more adds to what is hidden for now; isolating replaces it. */
        if (!(v->has_temp && !v->temp_isolate && !isolate)) v->temp.n = 0;
        for (size_t i = 0; i < n && !rc; i++) rc = cc_gidset_add(&v->temp, &sel[i]);
        v->has_temp = 1, v->temp_isolate = isolate;
        break;
    }
    case CC_VIS_HIDE_IN_VIEW:
        for (size_t i = 0; i < n && !rc; i++) rc = cc_gidset_add(&v->hidden, &sel[i]);
        break;
    case CC_VIS_ISOLATE_IN_VIEW:
        rc = set_copy(&v->isolated, sel, n);
        v->hidden.n = 0;
        break;
    default: /* Unhide: shown again in the view, and for now. */
        for (size_t i = 0; i < n; i++) cc_gidset_remove(&v->hidden, &sel[i]);
        if (v->isolated.n)
            for (size_t i = 0; i < n && !rc; i++) rc = cc_gidset_add(&v->isolated, &sel[i]);
        if (v->has_temp) {
            for (size_t i = 0; i < n && !rc; i++) {
                if (v->temp_isolate) rc = cc_gidset_add(&v->temp, &sel[i]);
                else cc_gidset_remove(&v->temp, &sel[i]);
            }
        }
    }
    return rc;
}

void cc_vis_show_all_in_view(cc_vis *v) { v->hidden.n = v->isolated.n = 0; }
void cc_vis_clear_temporary(cc_vis *v) { v->has_temp = 0, v->temp.n = 0; }

/* ---- the box ---- */

static void corner_union(float lo[3], float hi[3], const float p[3]) {
    for (int k = 0; k < 3; k++) {
        if (p[k] < lo[k]) lo[k] = p[k];
        if (p[k] > hi[k]) hi[k] = p[k];
    }
}

/* SceneElement::bounds. 0: empty. */
static int element_bounds(const cc_scene *sc, size_t k, float lo[3], float hi[3]) {
    const cc_scene_element *e = &sc->elements[k];
    const cc_mesh *m = cc_scene_local_mesh(sc, k);
    float a[3] = {1e30f * 1e30f, 1e30f * 1e30f, 1e30f * 1e30f}, b[3] = {-a[0], -a[0], -a[0]};
    for (uint32_t i = 0; i < m->nv; i++) corner_union(a, b, m->pos[i]);
    if (a[0] > b[0] || a[1] > b[1] || a[2] > b[2]) return 0;
    if (e->shape < 0) {
        memcpy(lo, a, sizeof a), memcpy(hi, b, sizeof b);
        return 1;
    }
    const cc_placement32 *t = &e->to_scene;
    for (int k2 = 0; k2 < 3; k2++) lo[k2] = 1e30f * 1e30f, hi[k2] = -lo[k2];
    for (int i = 0; i < 8; i++) {
        float c[3] = {(i & 1) ? b[0] : a[0], (i & 2) ? b[1] : a[1], (i & 4) ? b[2] : a[2]}, p[3];
        for (int q = 0; q < 3; q++) {
            float s = t->x[q] * c[0];
            s = s + t->y[q] * c[1];
            s = s + t->z[q] * c[2];
            p[q] = s + t->t[q];
        }
        corner_union(lo, hi, p);
    }
    return 1;
}

void cc_vis_fit(cc_vis *v, const cc_scene *sc, const cc_gid *primary) {
    if (!sc) return;
    float inf = 1e30f * 1e30f, lo[3] = {inf, inf, inf}, hi[3] = {-inf, -inf, -inf};
    for (size_t k = 0; k < sc->nelements; k++) {
        const cc_gid *g = &sc->elements[k].id;
        if (!v->reveal && cc_vis_hidden(v, g)) continue; /* lib.rs `visible` */
        if (primary && cc_gid_cmp(primary, g)) continue;
        float a[3], b[3];
        if (element_bounds(sc, k, a, b)) corner_union(lo, hi, a), corner_union(lo, hi, b);
    }
    if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) return;
    for (int k = 0; k < 3; k++) { /* a small margin: faces on a clip plane would flicker */
        float d = hi[k] - lo[k];
        float m = 1.0f + d * 1e-5f;
        v->min[k] = lo[k] - m, v->max[k] = hi[k] + m;
    }
    v->has_box = 1;
}

void cc_vis_toggle(cc_vis *v, const cc_scene *sc, const cc_gid *primary) {
    v->section_on = !v->section_on;
    if (v->section_on && !v->has_box) cc_vis_fit(v, sc, primary);
    v->drag_face = -1;
}

/* ---- JSON ---- */

/* An f32 as serde_json writes it (its shortest round-trip digits): the double whose f64 shortest
 * text is that. The candidates are the f64 shortest digits of the value cut to 1..9 digits. */
static double f32_json(float f) {
    if (f == 0.0f) return 0.0;
    Buf b;
    buf_init(&b);
    double out = f;
    if (fmt_f64_json(&b, (double)f) || b.oom) goto end;
    char dig[24];
    int nd = 0, point = -1, neg = 0, ex = 0;
    for (size_t i = 0; i < b.len; i++) {
        char c = b.p[i];
        if (c == '-') neg = 1;
        else if (c == '.') point = nd;
        else if (c == 'e') {
            ex = (int)cc_strtod(b.p + i + 1, b.len - i - 1);
            break;
        } else if (nd < 20) dig[nd++] = c;
    }
    if (point < 0) point = nd;
    int lead = 0;
    while (lead < nd - 1 && dig[lead] == '0') lead++; /* 0.00123: the digits start later */
    for (int p = 1; p <= 9 && lead + p <= nd; p++) {
        char d[24];
        memcpy(d, dig + lead, (size_t)p);
        int carry = lead + p < nd && dig[lead + p] >= '5';
        for (int i = p - 1; i >= 0 && carry; i--) {
            if (d[i] == '9') d[i] = '0';
            else d[i]++, carry = 0;
        }
        /* text: [-]0.<digits>e<exp>, exp = point - lead (+1 with a carry out) + ex */
        char t[48];
        size_t n = 0;
        if (neg) t[n++] = '-';
        t[n++] = '0', t[n++] = '.';
        if (carry) t[n++] = '1';
        memcpy(t + n, d, (size_t)p), n += (size_t)p;
        int e = point - lead + carry + ex;
        t[n++] = 'e';
        if (e < 0) t[n++] = '-', e = -e;
        char r[8];
        int nr = 0;
        do r[nr++] = (char)('0' + e % 10), e /= 10;
        while (e);
        while (nr) t[n++] = r[--nr];
        double c = cc_strtod(t, n);
        if ((float)c == f) {
            out = c;
            break;
        }
    }
end:
    buf_free(&b);
    return out;
}

static void vec3_f32(JsonW *w, const float p[3]) {
    jw_arr_begin(w);
    for (int k = 0; k < 3; k++) jw_f64(w, f32_json(p[k]));
    jw_arr_end(w);
}

static void box_json(JsonW *w, const float mn[3], const float mx[3]) { /* json!({"min","max","yaw"}) */
    jw_obj_begin(w);
    jw_key(w, "max"), vec3_f32(w, mx);
    jw_key(w, "min"), vec3_f32(w, mn);
    jw_key(w, "yaw"), jw_f64(w, 0.0);
    jw_obj_end(w);
}

static void capped(JsonW *w, const cc_gidset *s, int on) { /* calls.rs:57-63 */
    size_t n = on ? s->n : 0;
    jw_obj_begin(w);
    jw_key(w, "count"), jw_u64(w, n);
    jw_key(w, "ids"), jw_arr_begin(w);
    for (size_t i = 0; i < n && i < MAX_LIST; i++) jw_str_n(w, s->v[i].c, 22);
    jw_arr_end(w);
    jw_obj_end(w);
}

void cc_vis_json(const cc_vis *v, JsonW *w, const double *origin) {
    jw_obj_begin(w);
    jw_key(w, "hidden_in_view"), capped(w, &v->hidden, 1);
    jw_key(w, "hidden_now"), capped(w, &v->temp, v->has_temp && !v->temp_isolate);
    jw_key(w, "isolated_in_view"), capped(w, &v->isolated, 1);
    jw_key(w, "isolated_now"), capped(w, &v->temp, v->has_temp && v->temp_isolate);
    jw_key(w, "section_box"), jw_obj_begin(w);
    jw_key(w, "box");
    if (v->has_box) box_json(w, v->min, v->max);
    else jw_null(w);
    jw_key(w, "on"), jw_bool(w, v->section_on);
    jw_key(w, "origin_mm");
    if (origin) {
        jw_arr_begin(w);
        for (int i = 0; i < 3; i++) jw_f64(w, origin[i]);
        jw_arr_end(w);
    } else jw_null(w);
    jw_obj_end(w);
    jw_obj_end(w);
}

static void ids(JsonW *w, const cc_gidset *s) {
    jw_arr_begin(w);
    for (size_t i = 0; i < s->n; i++) jw_str_n(w, s->v[i].c, 22);
    jw_arr_end(w);
}

void cc_vis_sets_json(const cc_vis *v, JsonW *w) {
    jw_obj_begin(w);
    jw_key(w, "hidden_in_view"), ids(w, &v->hidden);
    jw_key(w, "isolated_in_view"), ids(w, &v->isolated);
    jw_key(w, "now");
    if (v->has_temp) {
        jw_obj_begin(w);
        jw_key(w, "ids"), ids(w, &v->temp);
        jw_key(w, "isolate"), jw_bool(w, v->temp_isolate);
        jw_obj_end(w);
    } else jw_null(w);
    jw_key(w, "reveal"), jw_bool(w, v->reveal);
    jw_obj_end(w);
}

/* ---- the box tool ---- */

static const char *const TOOL_NAMES[] = {"box_press", "box_move", "box_release", "box_cancel", "box_set"};

int cc_vis_tool_name(const char *name) {
    for (size_t i = 0; i < sizeof TOOL_NAMES / sizeof TOOL_NAMES[0]; i++)
        if (!strcmp(name, TOOL_NAMES[i])) return 1;
    return 0;
}

typedef struct {
    int has_ray, has_ray_tol, has_min, has_max, has_on, on;
    double ray[6], ray_tol[6], min[3], max[3];
} BoxArgs;

static int nums(const JrVal *x, double *out, size_t n, double lim) {
    if (x->type != JR_ARR || x->n != n) return -1;
    for (size_t i = 0; i < n; i++) {
        if (x->items[i].type != JR_NUM || !(x->items[i].num >= -lim && x->items[i].num <= lim)) return -1;
        out[i] = x->items[i].num;
    }
    return 0;
}

static int box_args(const char *body, size_t n, BoxArgs *t, Buf *out) {
    memset(t, 0, sizeof *t);
    size_t k = 0;
    while (k < n && (body[k] == ' ' || body[k] == '\n' || body[k] == '\t' || body[k] == '\r')) k++;
    if (k == n || (n - k == 4 && !memcmp(body + k, "null", 4))) return 0;
    Arena a;
    arena_init(&a, 0);
    JrError je;
    int rc = -1;
    const JrVal *v = jr_parse(&a, body, n, &je);
    if (!v) {
        buf_puts(out, je.msg);
        goto end;
    }
    if (v->type != JR_OBJ) {
        buf_puts(out, "the box tool's body is an object");
        goto end;
    }
    for (size_t i = 0; i < v->n; i++) {
        const JrMember *m = &v->members[i];
        const JrVal *x = &m->val;
        const char *bad = NULL;
        if (jr_key_is(m, "ray")) bad = nums(x, t->ray, 6, 1e12) ? "ray: expected [ox, oy, oz, dx, dy, dz]" : NULL, t->has_ray = 1;
        else if (jr_key_is(m, "ray_tol")) bad = nums(x, t->ray_tol, 6, 1e12) ? "ray_tol: expected [ox, oy, oz, dx, dy, dz]" : NULL, t->has_ray_tol = 1;
        else if (jr_key_is(m, "min")) bad = nums(x, t->min, 3, 1e7) ? "min: expected [x, y, z] within ±10000000 mm" : NULL, t->has_min = 1;
        else if (jr_key_is(m, "max")) bad = nums(x, t->max, 3, 1e7) ? "max: expected [x, y, z] within ±10000000 mm" : NULL, t->has_max = 1;
        else if (jr_key_is(m, "on")) {
            if (x->type != JR_BOOL) bad = "on: expected a boolean";
            else t->has_on = 1, t->on = x->b;
        } else {
            buf_puts(out, "unknown field: "), buf_append(out, m->key, m->key_n);
            goto end;
        }
        if (bad) {
            buf_puts(out, bad);
            goto end;
        }
    }
    rc = 0;
end:
    arena_free(&a);
    return rc;
}

static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* A ray with a unit direction; 0 when the direction is zero. */
static int unit_ray(const double in[6], double o[3], double d[3]) {
    double l = cc_sqrt(dot3(in + 3, in + 3));
    if (!(l > 1e-12)) return 0;
    for (int k = 0; k < 3; k++) o[k] = in[k], d[k] = in[3 + k] / l;
    return 1;
}

static void face_center(const float mn[3], const float mx[3], int f, double c[3]) { /* section.rs:36 */
    for (int k = 0; k < 3; k++) c[k] = 0.5 * ((double)mn[k] + (double)mx[k]);
    c[f / 2] = (f & 1) ? mx[f / 2] : mn[f / 2];
}

/* Where along the axis `a` through `c` the ray passes nearest: the axis parameter t; 0 parallel. */
static int axis_param(const double c[3], int a, const double o[3], const double d[3], double *t) {
    double w0[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    double b = d[a], dd = w0[a], e = dot3(d, w0), den = 1.0 - b * b;
    if (den < 1e-6) return 0;
    *t = (b * e - dd) / den;
    return 1;
}

static void tool_json(const cc_vis *v, Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "active"), jw_bool(&w, v->drag_face >= 0);
    jw_key(&w, "box");
    if (v->drag_face >= 0) box_json(&w, v->prev_min, v->prev_max);
    else if (v->has_box) box_json(&w, v->min, v->max);
    else jw_null(&w);
    jw_key(&w, "face");
    if (v->drag_face >= 0) jw_u64(&w, (uint64_t)v->drag_face);
    else jw_null(&w);
    jw_key(&w, "on"), jw_bool(&w, v->section_on);
    jw_obj_end(&w);
    jw_finish(&w);
}

/* The face drag moves the preview box to the pointer ray. */
static void drag_to(cc_vis *v, const double ray[6]) {
    double o[3], d[3], c[3], t;
    int f = v->drag_face, a = f / 2;
    if (!unit_ray(ray, o, d)) return;
    face_center(v->drag_min, v->drag_max, f, c);
    if (!axis_param(c, a, o, d, &t)) return;
    double delta = t - v->drag_t0;
    memcpy(v->prev_min, v->drag_min, sizeof v->prev_min), memcpy(v->prev_max, v->drag_max, sizeof v->prev_max);
    if (f & 1) { /* section.rs:113-115: at least MIN_SIZE thick */
        float x = (float)((double)v->drag_max[a] + delta), lim = v->drag_min[a] + MIN_SIZE;
        v->prev_max[a] = x > lim ? x : lim;
    } else {
        float x = (float)((double)v->drag_min[a] + delta), lim = v->drag_max[a] - MIN_SIZE;
        v->prev_min[a] = x < lim ? x : lim;
    }
}

int cc_vis_tool(cc_vis *v, const char *name, const char *body, size_t n, Buf *out) {
    BoxArgs a;
    if (box_args(body, n, &a, out)) return 400;
    if (!strcmp(name, "box_set")) {
        if (!a.has_min || !a.has_max) {
            buf_puts(out, "box_set needs \"min\" and \"max\" ([x, y, z], mm relative to the scene origin)");
            return 400;
        }
        float mn[3], mx[3];
        for (int k = 0; k < 3; k++) {
            mn[k] = (float)a.min[k], mx[k] = (float)a.max[k];
            if (!(mx[k] - mn[k] >= MIN_SIZE)) {
                buf_puts(out, "the box is at least 10 mm on each axis (max - min)");
                return 400;
            }
        }
        memcpy(v->min, mn, sizeof mn), memcpy(v->max, mx, sizeof mx);
        v->has_box = 1;
        if (a.has_on) v->section_on = a.on;
        v->drag_face = -1;
    } else if (!strcmp(name, "box_press")) {
        v->drag_face = -1;
        double o[3], d[3];
        if (!v->section_on || !v->has_box || !a.has_ray || !unit_ray(a.ray, o, d)) goto answer;
        /* The handle under the pointer: the face centre nearest to the ray, within the distance
         * between the ray and `ray_tol` (the ray 12 px beside it) at that depth; else 2 % of the
         * box's diagonal. */
        double o2[3], d2[3], best = 0.0;
        int has_tol = a.has_ray_tol && unit_ray(a.ray_tol, o2, d2), face = -1;
        double diag[3] = {(double)v->max[0] - v->min[0], (double)v->max[1] - v->min[1], (double)v->max[2] - v->min[2]};
        for (int f = 0; f < 6; f++) {
            double c[3], w0[3];
            face_center(v->min, v->max, f, c);
            for (int k = 0; k < 3; k++) w0[k] = c[k] - o[k];
            double s = dot3(w0, d);
            if (!(s > 0)) continue;
            double q[3];
            for (int k = 0; k < 3; k++) q[k] = w0[k] - s * d[k];
            double dist = cc_sqrt(dot3(q, q)), tol = 0.02 * cc_sqrt(dot3(diag, diag));
            if (has_tol) {
                double r[3];
                for (int k = 0; k < 3; k++) r[k] = (o2[k] + s * d2[k]) - (o[k] + s * d[k]);
                tol = cc_sqrt(dot3(r, r));
            }
            if (dist <= tol && (face < 0 || dist < best)) face = f, best = dist;
        }
        double c[3], t;
        if (face < 0) goto answer;
        face_center(v->min, v->max, face, c);
        if (!axis_param(c, face / 2, o, d, &t)) goto answer;
        v->drag_face = face, v->drag_t0 = t;
        memcpy(v->drag_min, v->min, sizeof v->min), memcpy(v->drag_max, v->max, sizeof v->max);
        memcpy(v->prev_min, v->min, sizeof v->min), memcpy(v->prev_max, v->max, sizeof v->max);
    } else if (!strcmp(name, "box_move")) {
        if (v->drag_face >= 0 && a.has_ray) drag_to(v, a.ray);
    } else if (!strcmp(name, "box_release")) {
        if (v->drag_face >= 0) {
            if (a.has_ray) drag_to(v, a.ray);
            memcpy(v->min, v->prev_min, sizeof v->min), memcpy(v->max, v->prev_max, sizeof v->max);
            v->drag_face = -1;
        }
    } else v->drag_face = -1; /* box_cancel: the box as it was */
answer:
    tool_json(v, out);
    return out->oom ? 400 : 200;
}
