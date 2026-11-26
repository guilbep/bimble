/* Levels and grids (ADR 0056). See grid.h. No Rust oracle: the texts and rules are this file's
 * own (ccore/golden/DEVIATIONS.md). */
#include "grid.h"
#include <stdlib.h>
#include <string.h>
#include "cbor.h"
#include "relation.h"

#ifndef __TINYC__ /* tcc never contracts and warns on the pragma */
#pragma STDC FP_CONTRACT OFF
#endif

static int finite_(double x) { return x - x == 0.0; }
static double fabs_(double x) { return x < 0 ? -x : x; }

/* ---- lookups ---- */

const cc_grid *cc_model_grid(const cc_model *m, const cc_gid *id) {
    size_t lo = 0, hi = m->ngrids;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cc_gid_cmp(&m->grids[mid].id, id);
        if (c == 0) return &m->grids[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

const cc_grid_axis *cc_grid_axis_find(const cc_grid *g, const cc_gid *axis) {
    for (size_t i = 0; i < g->naxes; i++)
        if (!cc_gid_cmp(&g->axes[i].id, axis)) return &g->axes[i];
    return NULL;
}

int cc_grid_uses_id(const cc_model *m, const cc_gid *id) {
    if (cc_model_grid(m, id)) return 1;
    for (size_t i = 0; i < m->ngrids; i++)
        if (cc_grid_axis_find(&m->grids[i], id)) return 1;
    return 0;
}

/* ---- validation ---- */

static size_t utf8_chars(cc_str s) {
    size_t k = 0;
    for (size_t i = 0; i < s.n; i++)
        if (((unsigned char)s.p[i] & 0xC0) != 0x80) k++;
    return k;
}

static int blank(cc_str s) {
    for (size_t i = 0; i < s.n; i++)
        if (s.p[i] != ' ' && s.p[i] != '\t') return 0;
    return 1;
}

static int has_control(cc_str s) {
    for (size_t i = 0; i < s.n; i++)
        if ((unsigned char)s.p[i] < 0x20 || s.p[i] == 0x7F) return 1;
    return 0;
}

static int str_eq(cc_str a, cc_str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }

/* "grid <id> (<name>): " */
static void ctx(Buf *why, const cc_grid *g) {
    buf_puts(why, "grid ");
    cc_buf_gid(why, &g->id);
    buf_puts(why, " (");
    buf_append(why, g->name.p, g->name.n < 200 ? g->name.n : 200);
    buf_puts(why, "): ");
}

/* "grid <id> (<name>), axis <tag>: " */
static void actx(Buf *why, const cc_grid *g, const cc_grid_axis *a) {
    buf_puts(why, "grid ");
    cc_buf_gid(why, &g->id);
    buf_puts(why, " (");
    buf_append(why, g->name.p, g->name.n < 200 ? g->name.n : 200);
    buf_puts(why, "), axis ");
    cc_buf_debug_str(why, a->tag.p, a->tag.n < 64 ? a->tag.n : 64);
    buf_puts(why, ": ");
}

static int cross_parallel(const cc_grid_axis *a, const cc_grid_axis *b) {
    double ax = a->end[0] - a->start[0], ay = a->end[1] - a->start[1];
    double bx = b->end[0] - b->start[0], by = b->end[1] - b->start[1];
    double cr = ax * by - ay * bx;
    double la = ax * ax + ay * ay, lb = bx * bx + by * by;
    /* |sin| <= 1e-6: cr^2 <= 1e-12 la lb */
    return cr * cr <= 1e-12 * la * lb;
}

int cc_grid_axis_is_u(const cc_grid *g, size_t k) { return k == 0 || cross_parallel(&g->axes[0], &g->axes[k]); }

static int one_check(const cc_grid *g, Buf *why) {
    if (blank(g->name) || utf8_chars(g->name) > CC_GRID_MAX_NAME || has_control(g->name)) {
        ctx(why, g);
        buf_puts(why, "a name of 1 to 200 characters, one line");
        return -1;
    }
    if (g->naxes == 0 || g->naxes > CC_GRID_MAX_AXES) {
        ctx(why, g);
        buf_puts(why, "1 to 1000 axes, got ");
        buf_u64(why, g->naxes);
        return -1;
    }
    for (size_t i = 0; i < g->naxes; i++) {
        const cc_grid_axis *a = &g->axes[i];
        if (blank(a->tag) || utf8_chars(a->tag) > CC_GRID_MAX_TAG || has_control(a->tag)) {
            ctx(why, g);
            buf_puts(why, "axis ");
            cc_buf_gid(why, &a->id);
            buf_puts(why, ": a tag of 1 to 32 characters, one line");
            return -1;
        }
        for (size_t j = 0; j < i; j++)
            if (str_eq(g->axes[j].tag, a->tag)) {
                actx(why, g, a);
                buf_puts(why, "the tag is used twice");
                return -1;
            }
        const double v[4] = {a->start[0], a->start[1], a->end[0], a->end[1]};
        for (int k = 0; k < 4; k++)
            if (!finite_(v[k]) || fabs_(v[k]) > CC_GRID_MAX_MM) {
                actx(why, g, a);
                buf_puts(why, "ends within 1e7 mm of the origin");
                return -1;
            }
        double dx = a->end[0] - a->start[0], dy = a->end[1] - a->start[1];
        if (dx * dx + dy * dy < CC_GRID_MIN_LENGTH * CC_GRID_MIN_LENGTH) {
            actx(why, g, a);
            buf_puts(why, "shorter than 1 mm");
            return -1;
        }
    }
    int two = 0;
    for (size_t i = 1; i < g->naxes && !two; i++) two = !cc_grid_axis_is_u(g, i);
    if (!two) {
        ctx(why, g);
        buf_puts(why, "its axes run in one direction; an IfcGrid needs axes in two");
        return -1;
    }
    return 0;
}

static int id_taken(const cc_model *m, const cc_gid *id) {
    return !cc_gid_cmp(&m->project_id, id) || cc_model_storey(m, id) || cc_model_material(m, id) ||
           cc_model_element(m, id) || cc_model_view(m, id) || cc_model_sheet(m, id) || cc_model_relation(m, id);
}

static int gid_qcmp(const void *a, const void *b) { return cc_gid_cmp(a, b); }

int cc_grids_validate(const cc_model *m, const cc_grid *g, size_t n, const cc_gid *keys, Buf *why) {
    size_t nids = 0;
    for (size_t i = 0; i < n; i++) {
        if (keys && cc_gid_cmp(&keys[i], &g[i].id)) {
            buf_puts(why, "grid ");
            cc_buf_gid(why, &keys[i]);
            buf_puts(why, ": key does not match id");
            return -1;
        }
        if (i && cc_gid_cmp(&g[i - 1].id, &g[i].id) >= 0) { /* a broken caller, not a file */
            buf_puts(why, "grids must be sorted by id, without duplicates");
            return -1;
        }
        if (one_check(&g[i], why)) return -1;
        nids += 1 + g[i].naxes; /* each naxes <= CC_GRID_MAX_AXES: no overflow */
    }
    if (!n) return 0;
    cc_gid *ids = malloc(nids * sizeof *ids);
    if (!ids) {
        buf_puts(why, "out of memory");
        return -1;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        ids[k++] = g[i].id;
        for (size_t j = 0; j < g[i].naxes; j++) ids[k++] = g[i].axes[j].id;
    }
    qsort(ids, nids, sizeof *ids, gid_qcmp);
    int rc = 0;
    for (size_t i = 0; !rc && i < nids; i++) {
        if (i && !cc_gid_cmp(&ids[i - 1], &ids[i])) {
            buf_puts(why, "the id ");
            cc_buf_gid(why, &ids[i]);
            buf_puts(why, " is used twice in the grids");
            rc = -1;
        } else if (id_taken(m, &ids[i])) {
            buf_puts(why, "the grid or axis id ");
            cc_buf_gid(why, &ids[i]);
            buf_puts(why, " is already used by another object");
            rc = -1;
        }
    }
    free(ids);
    return rc;
}

/* ---- copies ---- */

static void *dup_bytes(Arena *a, const void *src, size_t size) {
    if (!size) return NULL;
    void *p = arena_alloc(a, size, 16);
    if (p) memcpy(p, src, size);
    return p;
}

static int dup_str(Arena *a, cc_str *s) {
    if (!s->n) return 0;
    char *p = dup_bytes(a, s->p, s->n);
    if (!p) return -1;
    s->p = p;
    return 0;
}

void cc_grid_canonical(cc_grid *g) {
    for (size_t i = 0; i < g->naxes; i++)
        for (int k = 0; k < 2; k++) g->axes[i].start[k] += 0.0, g->axes[i].end[k] += 0.0;
}

int cc_grid_dup(Arena *a, cc_grid *g) {
    if (dup_str(a, &g->name)) return -1;
    if (g->naxes) {
        cc_grid_axis *ax = dup_bytes(a, g->axes, g->naxes * sizeof *ax);
        if (!ax) return -1;
        for (size_t i = 0; i < g->naxes; i++)
            if (dup_str(a, &ax[i].tag)) return -1;
        g->axes = ax;
    } else g->axes = NULL;
    cc_grid_canonical(g);
    return 0;
}

int cc_grid_eq(const cc_grid *a, const cc_grid *b) {
    if (cc_gid_cmp(&a->id, &b->id) || !str_eq(a->name, b->name) || a->naxes != b->naxes) return 0;
    for (size_t i = 0; i < a->naxes; i++) {
        const cc_grid_axis *x = &a->axes[i], *y = &b->axes[i];
        if (cc_gid_cmp(&x->id, &y->id) || !str_eq(x->tag, y->tag) || memcmp(x->start, y->start, sizeof x->start) ||
            memcmp(x->end, y->end, sizeof x->end))
            return 0;
    }
    return 1;
}

/* ---- decoding ---- */

static int vfail(cc_vd *d, size_t off) {
    if (!d->failed) d->failed = 1, d->off = off;
    return -1;
}

static int vfail3(cc_vd *d, size_t off, const char *a, const char *b, const char *c) {
    if (d->failed) return -1;
    buf_clear(&d->msg);
    if (a) buf_puts(&d->msg, a);
    if (b) buf_puts(&d->msg, b);
    if (c) buf_puts(&d->msg, c);
    return vfail(d, off);
}

static int type_err(cc_vd *d, const JrVal *v, const char *expected) {
    if (d->failed) return -1;
    buf_clear(&d->msg);
    buf_puts(&d->msg, "invalid type: ");
    switch (v->type) {
    case JR_NULL: buf_puts(&d->msg, "null"); break;
    case JR_BOOL: buf_puts(&d->msg, v->b ? "boolean `true`" : "boolean `false`"); break;
    case JR_NUM: buf_puts(&d->msg, v->num_int ? "integer" : "floating point"); break;
    case JR_STR:
        buf_puts(&d->msg, "string ");
        cc_buf_debug_str(&d->msg, v->s, v->n < 200 ? v->n : 200);
        break;
    case JR_ARR: buf_puts(&d->msg, "sequence"); break;
    default: buf_puts(&d->msg, "map"); break;
    }
    buf_puts(&d->msg, ", expected ");
    buf_puts(&d->msg, expected);
    return vfail(d, v->off);
}

static int d_str(cc_vd *d, const JrVal *v, cc_str *out) {
    if (v->type != JR_STR) return type_err(d, v, "a string");
    char *p = v->n ? arena_dup(d->a, v->s, v->n) : NULL;
    if (v->n && !p) return vfail3(d, v->off, "out of memory", 0, 0);
    out->p = p;
    out->n = v->n;
    return 0;
}

static int d_gid(cc_vd *d, const JrVal *v, cc_gid *out) {
    if (v->type != JR_STR) return type_err(d, v, "a string");
    if (!cc_gid_valid(v->s, v->n)) {
        if (d->failed) return -1;
        buf_clear(&d->msg);
        buf_puts(&d->msg, "invalid IFC GlobalId: ");
        cc_buf_debug_str(&d->msg, v->s, v->n < 200 ? v->n : 200);
        return vfail(d, v->off);
    }
    memcpy(out->c, v->s, 22);
    return 0;
}

static int d_pt(cc_vd *d, const JrVal *v, double out[2]) {
    if (v->type != JR_ARR) return type_err(d, v, "an array of length 2");
    if (v->n != 2) return vfail3(d, v->off, "invalid length, expected an array of length 2", 0, 0);
    for (int k = 0; k < 2; k++) {
        if (v->items[k].type != JR_NUM) return type_err(d, &v->items[k], "f64");
        if (!finite_(v->items[k].num)) return vfail3(d, v->items[k].off, "number out of range", 0, 0);
        out[k] = v->items[k].num;
    }
    return 0;
}

/* Members named in F (n of them) into f[]; unknown ones ignored, `skip` skipped, a repeat is an
 * error; then the first missing one of the first `nreq`. */
static int fields(cc_vd *d, const JrVal *v, const char *skip, const char *const *F, int n, int nreq,
                  const JrVal **f, const char *what) {
    for (int i = 0; i < n; i++) f[i] = NULL;
    if (v->type != JR_OBJ) return type_err(d, v, what);
    for (size_t j = 0; j < v->n; j++) {
        const JrMember *mb = &v->members[j];
        if (skip && jr_key_is(mb, skip)) continue;
        int i = 0;
        while (i < n && !jr_key_is(mb, F[i])) i++;
        if (i == n) continue;
        if (f[i]) return vfail3(d, mb->val.off, "duplicate field `", F[i], "`");
        f[i] = &mb->val;
    }
    for (int i = 0; i < nreq; i++)
        if (!f[i]) return vfail3(d, v->off, "missing field `", F[i], "`");
    return 0;
}

int cc_vd_grid(cc_vd *d, const JrVal *v, const char *skip, cc_grid *out) {
    static const char *const F[] = {"id", "name", "axes"};
    static const char *const A[] = {"id", "tag", "start_mm", "end_mm"};
    const JrVal *f[3], *fa[4];
    memset(out, 0, sizeof *out);
    if (fields(d, v, skip, F, 3, 3, f, "struct Grid")) return -1;
    if (d_gid(d, f[0], &out->id) || d_str(d, f[1], &out->name)) return -1;
    const JrVal *ax = f[2];
    if (ax->type != JR_ARR) return type_err(d, ax, "a sequence");
    if (ax->n > CC_GRID_MAX_AXES) return vfail3(d, ax->off, "a grid has at most 1000 axes", 0, 0);
    if (!ax->n) return 0; /* validation refuses it, with the grid's name */
    cc_grid_axis *a = arena_alloc(d->a, ax->n * sizeof *a, 8);
    if (!a) return vfail3(d, ax->off, "out of memory", 0, 0);
    memset(a, 0, ax->n * sizeof *a);
    for (size_t i = 0; i < ax->n; i++) {
        if (fields(d, &ax->items[i], NULL, A, 4, 4, fa, "struct GridAxis")) return -1;
        if (d_gid(d, fa[0], &a[i].id) || d_str(d, fa[1], &a[i].tag) || d_pt(d, fa[2], a[i].start) ||
            d_pt(d, fa[3], a[i].end))
            return -1;
    }
    out->axes = a;
    out->naxes = ax->n;
    return 0;
}

/* ---- writing ---- */

static void e_key(cc_em *e, const char *k) {
    if (e->j) jw_key(e->j, k);
    else cb_text(e->c, k, strlen(k));
}

static void e_str(cc_em *e, cc_str s) {
    if (e->j) jw_str_n(e->j, s.n ? s.p : "", s.n);
    else cb_text(e->c, s.n ? s.p : "", s.n);
}

static void e_gid(cc_em *e, const cc_gid *g) {
    if (e->j) jw_str_n(e->j, g->c, 22);
    else cb_text(e->c, g->c, 22);
}

static void e_pt(cc_em *e, const double p[2]) {
    if (e->j) {
        jw_arr_begin(e->j);
        jw_f64(e->j, p[0]), jw_f64(e->j, p[1]);
        jw_arr_end(e->j);
    } else {
        cb_head(e->c, CB_ARRAY, 2);
        cb_f64(e->c, p[0]), cb_f64(e->c, p[1]);
    }
}

void cc_em_grid(cc_em *e, const cc_grid *g, const char *op) {
    if (e->j) {
        jw_obj_begin(e->j);
        if (op) jw_key(e->j, "op"), jw_str(e->j, op);
    } else {
        cb_head(e->c, CB_MAP, 3);
    }
    e_key(e, "id"), e_gid(e, &g->id);
    e_key(e, "name"), e_str(e, g->name);
    e_key(e, "axes");
    if (e->j) jw_arr_begin(e->j);
    else cb_head(e->c, CB_ARRAY, g->naxes);
    for (size_t i = 0; i < g->naxes; i++) {
        const cc_grid_axis *a = &g->axes[i];
        if (e->j) jw_obj_begin(e->j);
        else cb_head(e->c, CB_MAP, 4);
        e_key(e, "id"), e_gid(e, &a->id);
        e_key(e, "tag"), e_str(e, a->tag);
        e_key(e, "start_mm"), e_pt(e, a->start);
        e_key(e, "end_mm"), e_pt(e, a->end);
        if (e->j) jw_obj_end(e->j);
    }
    if (e->j) jw_arr_end(e->j);
    if (e->j) jw_obj_end(e->j);
}
