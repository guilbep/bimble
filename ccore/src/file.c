/* .bimble JSON text form: decode the DOM like serde derive does for model.rs, encode like
 * serde_json::to_string_pretty. Rust behaviours copied (see file.h and the notes below):
 * - unknown fields are ignored (no deny_unknown_fields), except in MaterialProps which denies
 *   them; ignored fields are lost on save, as in Rust;
 * - a duplicate struct field is an error ("duplicate field `x`"); a duplicate map key keeps the
 *   last value (BTreeMap insert);
 * - sections Rust knows and the C core does not hold (mesh arrays by hash) are refused, never
 *   dropped. The georeference (ADR 0045, georef.h) is held: key "georef", left out when the
 *   default. Openings are kept (data only: the C scene does not cut them
 *   yet). Views, sheets and schedules are held (view.h). Relations (ADR 0070, relation.h) are the
 *   C core's own section "relations", left out when empty: Rust ignores it (no oracle). Grids and
 *   levels that are not storeys (ADR 0056, grid.h) likewise: section "grids" and the storey key
 *   "is_storey" (false only), each left out when unused.
 * The binary form (version 2) is the same serde model in CBOR, read into the same DOM
 * (cbor.h), with each mesh array once in the top-level tables `positions` / `indices` and
 * elements referring to them by index (shared again in memory, as Rust shares its Arcs). */
#include "file.h"
#include <stdlib.h>
#include <string.h>
#include "cbor.h"
#include "fmt.h"
#include "georef.h"
#include "grid.h"
#include "inflate.h"
#include "json_r.h"
#include "json_w.h"
#include "refplane.h" /* WAVE1B */
#include "relation.h"
#include "view_io.h"

typedef struct {
    Arena *a; /* the model's arena */
    cc_error *err;
    int failed;
    Buf msg;
    uint32_t version; /* the version this form must carry: 1 text, 2 binary */
    /* binary: the tables, decoded (pointer, count) */
    double (**tpos)[3];
    uint32_t **tidx;
    size_t *tpos_n, *tidx_n, ntpos, ntidx;
} D;

static int f_finite(double x) { return x - x == 0.0; }

static int fail_buf(D *d, size_t off) {
    if (!d->failed) {
        d->failed = 1;
        const char *pre = "parse: ";
        size_t pn = strlen(pre), n = d->msg.oom ? 0 : d->msg.len;
        if (off == CC_NO_OFFSET) pn = 0;
        size_t cap = sizeof d->err->msg - 1 - pn;
        if (n > cap) {
            n = cap;
            while (n && ((unsigned char)d->msg.p[n] & 0xC0) == 0x80) n--;
        }
        memcpy(d->err->msg, pre, pn);
        if (n) memcpy(d->err->msg + pn, d->msg.p, n);
        d->err->msg[pn + n] = 0;
        d->err->offset = off;
    }
    return -1;
}

static int dfail(D *d, size_t off, const char *a, const char *b, const char *c) {
    if (d->failed) return -1;
    buf_clear(&d->msg);
    if (a) buf_puts(&d->msg, a);
    if (b) buf_puts(&d->msg, b);
    if (c) buf_puts(&d->msg, c);
    return fail_buf(d, off);
}

/* A number as its token: the JSON text, or (CBOR has none) its value. */
static void num_token(Buf *b, const JrVal *v) {
    if (v->s) {
        buf_append(b, v->s, v->n < 64 ? v->n : 64);
    } else if (v->num_int && v->num >= -9.2e18 && v->num <= 1.8e19) {
        if (v->num_neg) buf_i64(b, (int64_t)v->num);
        else buf_u64(b, (uint64_t)v->num);
    } else if (fmt_f64_json(b, v->num)) {
        buf_puts(b, v->num != v->num ? "NaN" : v->num > 0 ? "inf" : "-inf");
    }
}

/* serde's "invalid type: <unexpected>, expected <what>". */
static int type_err(D *d, const JrVal *v, const char *expected) {
    if (d->failed) return -1;
    buf_clear(&d->msg);
    buf_puts(&d->msg, "invalid type: ");
    switch (v->type) {
    case JR_NULL: buf_puts(&d->msg, "null"); break;
    case JR_BOOL: buf_puts(&d->msg, v->b ? "boolean `true`" : "boolean `false`"); break;
    case JR_NUM:
        buf_puts(&d->msg, v->num_int ? "integer `" : "floating point `");
        num_token(&d->msg, v);
        buf_putc(&d->msg, '`');
        break;
    case JR_STR:
        buf_puts(&d->msg, "string ");
        cc_buf_debug_str(&d->msg, v->s, v->n < 200 ? v->n : 200);
        break;
    case JR_ARR: buf_puts(&d->msg, "sequence"); break;
    default: buf_puts(&d->msg, "map"); break;
    }
    buf_puts(&d->msg, ", expected ");
    buf_puts(&d->msg, expected);
    return fail_buf(d, v->off);
}

static int d_f64(D *d, const JrVal *v, double *out) {
    if (v->type != JR_NUM) return type_err(d, v, "f64");
    if (!f_finite(v->num)) return dfail(d, v->off, "number out of range", 0, 0);
    *out = v->num;
    return 0;
}

static int d_u32(D *d, const JrVal *v, uint32_t *out) {
    if (v->type != JR_NUM || !v->num_int) return type_err(d, v, "u32");
    if (v->num_neg || v->num > 4294967295.0) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "invalid value: integer `");
        num_token(&d->msg, v);
        buf_puts(&d->msg, "`, expected u32");
        return fail_buf(d, v->off);
    }
    *out = (uint32_t)v->num;
    return 0;
}

static int d_str(D *d, const JrVal *v, cc_str *out) {
    if (v->type != JR_STR) return type_err(d, v, "a string");
    char *p = v->n ? arena_dup(d->a, v->s, v->n) : NULL;
    if (v->n && !p) return dfail(d, v->off, "out of memory", 0, 0);
    out->p = p;
    out->n = v->n;
    return 0;
}

static int gid_from(D *d, const char *s, size_t n, size_t off, cc_gid *out) {
    if (!cc_gid_valid(s, n)) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "invalid IFC GlobalId: ");
        cc_buf_debug_str(&d->msg, s, n < 200 ? n : 200);
        return fail_buf(d, off);
    }
    memcpy(out->c, s, 22);
    return 0;
}

static int d_gid(D *d, const JrVal *v, cc_gid *out) {
    if (v->type != JR_STR) return type_err(d, v, "a string");
    return gid_from(d, v->s, v->n, v->off, out);
}

/* Struct fields by name: out[i] = the member's value or NULL. Duplicates are errors; unknown
 * members are ignored, or refused when `deny_unknown`. */
static int d_fields(D *d, const JrVal *obj, const char *const *names, int nn, const JrVal **out,
                    const char *what, int deny_unknown) {
    if (obj->type != JR_OBJ) return type_err(d, obj, what);
    for (int i = 0; i < nn; i++) out[i] = NULL;
    for (size_t j = 0; j < obj->n; j++) {
        const JrMember *m = &obj->members[j];
        int i = 0;
        while (i < nn && !jr_key_is(m, names[i])) i++;
        if (i == nn) {
            if (!deny_unknown) continue;
            buf_clear(&d->msg);
            buf_puts(&d->msg, "unknown field `");
            buf_append(&d->msg, m->key, m->key_n < 200 ? m->key_n : 200);
            buf_puts(&d->msg, "`, expected one of ");
            for (int k = 0; k < nn; k++) {
                if (k) buf_puts(&d->msg, ", ");
                buf_putc(&d->msg, '`');
                buf_puts(&d->msg, names[k]);
                buf_putc(&d->msg, '`');
            }
            return fail_buf(d, m->val.off);
        }
        if (out[i]) return dfail(d, m->val.off, "duplicate field `", names[i], "`");
        out[i] = &m->val;
    }
    return 0;
}

static int need(D *d, const JrVal *obj, const JrVal *f, const char *name) {
    if (f) return 0;
    return dfail(d, obj->off, "missing field `", name, "`");
}

static int d_vec(D *d, const JrVal *v, double *out, size_t n) {
    if (v->type != JR_ARR) return type_err(d, v, n == 2 ? "an array of length 2" : "an array of length 3");
    if (v->n != n) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "invalid length ");
        buf_u64(&d->msg, v->n);
        buf_puts(&d->msg, n == 2 ? ", expected an array of length 2" : ", expected an array of length 3");
        return fail_buf(d, v->off);
    }
    for (size_t i = 0; i < n; i++)
        if (d_f64(d, &v->items[i], &out[i])) return -1;
    return 0;
}

static void *alloc_n(D *d, size_t n, size_t sz, size_t off) {
    if (n == 0) return NULL;
    void *p = arena_alloc_n(d->a, n, sz, 8); /* NULL also when n * sz overflows */
    if (!p) dfail(d, off, "out of memory", 0, 0);
    return p;
}

static int d_loop(D *d, const JrVal *v, cc_loop *out) {
    if (v->type != JR_ARR) return type_err(d, v, "a sequence");
    out->n = v->n;
    out->pts = alloc_n(d, v->n, sizeof *out->pts, v->off);
    if (v->n && !out->pts) return -1;
    for (size_t i = 0; i < v->n; i++)
        if (d_vec(d, &v->items[i], out->pts[i], 2)) return -1;
    return 0;
}

static int d_placement(D *d, const JrVal *v, cc_placement *p) {
    static const char *const F[] = {"origin", "z_axis", "x_axis"};
    const JrVal *f[3];
    if (d_fields(d, v, F, 3, f, "struct Placement", 0)) return -1;
    for (int i = 0; i < 3; i++)
        if (need(d, v, f[i], F[i])) return -1;
    if (d_vec(d, f[0], p->origin, 3) || d_vec(d, f[1], p->z_axis, 3) || d_vec(d, f[2], p->x_axis, 3))
        return -1;
    return 0;
}

/* Three f64 fields in order (the rectangle generators and the wall). */
static int d_three(D *d, const JrVal *v, const char *a, const char *b, const char *c, double *out) {
    const char *names[3] = {a, b, c};
    const JrVal *f[3];
    if (d_fields(d, v, names, 3, f, "struct variant", 0)) return -1;
    for (int i = 0; i < 3; i++)
        if (need(d, v, f[i], names[i]) || d_f64(d, f[i], &out[i])) return -1;
    return 0;
}

static int d_generator(D *d, const JrVal *v, cc_generator *g) {
    if (v->type != JR_OBJ) return type_err(d, v, "internally tagged enum Generator");
    const JrVal *kind = NULL;
    for (size_t j = 0; j < v->n; j++)
        if (jr_key_is(&v->members[j], "kind")) {
            if (kind) return dfail(d, v->members[j].val.off, "duplicate field `kind`", 0, 0);
            kind = &v->members[j].val;
        }
    if (!kind) return dfail(d, v->off, "missing field `kind`", 0, 0);
    if (kind->type != JR_STR) return type_err(d, kind, "variant identifier");
    int k = 0;
    while (k < CC_GEN_COUNT && !(strlen(cc_gen_kind_name((cc_gen_kind)k)) == kind->n &&
                                 memcmp(cc_gen_kind_name((cc_gen_kind)k), kind->s, kind->n) == 0))
        k++;
    if (k == CC_GEN_COUNT) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "unknown variant ");
        buf_putc(&d->msg, '`');
        buf_append(&d->msg, kind->s, kind->n < 200 ? kind->n : 200);
        buf_puts(&d->msg, "`, expected one of `rect_column`, `rect_beam`, `slab`, `wall`, "
                          "`pad_footing`, `extruded_polygon`, `mesh`");
        return fail_buf(d, kind->off);
    }
    g->kind = (cc_gen_kind)k;
    double t[3];
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
        if (d_three(d, v, "width", "depth", "height", t)) return -1;
        g->u.col.width = t[0], g->u.col.depth = t[1], g->u.col.height = t[2];
        return 0;
    case CC_GEN_RECT_BEAM:
        if (d_three(d, v, "width", "height", "length", t)) return -1;
        g->u.beam.width = t[0], g->u.beam.height = t[1], g->u.beam.length = t[2];
        return 0;
    case CC_GEN_WALL:
        if (d_three(d, v, "length", "thickness", "height", t)) return -1;
        g->u.wall.length = t[0], g->u.wall.thickness = t[1], g->u.wall.height = t[2];
        return 0;
    case CC_GEN_SLAB: {
        static const char *const F[] = {"outline", "thickness"};
        const JrVal *f[2];
        if (d_fields(d, v, F, 2, f, "struct variant Generator::Slab", 0)) return -1;
        if (need(d, v, f[0], F[0]) || need(d, v, f[1], F[1])) return -1;
        if (d_loop(d, f[0], &g->u.slab.outline) || d_f64(d, f[1], &g->u.slab.thickness)) return -1;
        return 0;
    }
    case CC_GEN_EXTRUDED_POLYGON: {
        static const char *const F[] = {"outline", "voids", "height"};
        const JrVal *f[3];
        if (d_fields(d, v, F, 3, f, "struct variant Generator::ExtrudedPolygon", 0)) return -1;
        if (need(d, v, f[0], F[0]) || need(d, v, f[2], F[2])) return -1;
        if (d_loop(d, f[0], &g->u.poly.outline)) return -1;
        g->u.poly.voids = NULL;
        g->u.poly.nvoids = 0;
        if (f[1]) {
            if (f[1]->type != JR_ARR) return type_err(d, f[1], "a sequence");
            g->u.poly.nvoids = f[1]->n;
            g->u.poly.voids = alloc_n(d, f[1]->n, sizeof(cc_loop), f[1]->off);
            if (f[1]->n && !g->u.poly.voids) return -1;
            for (size_t i = 0; i < f[1]->n; i++)
                if (d_loop(d, &f[1]->items[i], &g->u.poly.voids[i])) return -1;
        }
        return d_f64(d, f[2], &g->u.poly.height);
    }
    case CC_GEN_MESH: {
        static const char *const F[] = {"positions", "indices"};
        const JrVal *f[2];
        if (d_fields(d, v, F, 2, f, "struct variant Generator::Mesh", 0)) return -1;
        if (need(d, v, f[0], F[0]) || need(d, v, f[1], F[1])) return -1;
        int by_index = 0;
        for (int i = 0; i < 2; i++)
            if (d->version != CC_BINARY_VERSION && (f[i]->type == JR_NUM || f[i]->type == JR_STR))
                return dfail(d, f[i]->off, "unsupported by the C core: mesh arrays by index or hash", 0, 0);
            else if (f[i]->type == JR_NUM && f[i]->num_int && !f[i]->num_neg) {
                size_t nt = i ? d->ntidx : d->ntpos;
                if (f[i]->num >= (double)nt) {
                    buf_clear(&d->msg);
                    buf_puts(&d->msg, "mesh ");
                    num_token(&d->msg, f[i]);
                    buf_puts(&d->msg, " not in the file's table");
                    return fail_buf(d, f[i]->off);
                }
                by_index |= 1 << i;
            } else if (f[i]->type == JR_STR)
                return dfail(d, f[i]->off, "unsupported by the C core: mesh arrays by hash", 0, 0);
            else if (f[i]->type != JR_ARR)
                return type_err(d, f[i], "a mesh array, its index in the file or its hash");
        if (by_index & 1) {
            size_t k = (size_t)f[0]->num;
            g->u.mesh.positions = d->tpos[k];
            g->u.mesh.npositions = d->tpos_n[k];
        }
        if (by_index & 2) {
            size_t k = (size_t)f[1]->num;
            g->u.mesh.indices = d->tidx[k];
            g->u.mesh.nindices = d->tidx_n[k];
        }
        if (by_index == 3) return 0;
        if (!(by_index & 1)) {
            g->u.mesh.npositions = f[0]->n;
            g->u.mesh.positions = alloc_n(d, f[0]->n, sizeof *g->u.mesh.positions, f[0]->off);
            if (f[0]->n && !g->u.mesh.positions) return -1;
            for (size_t i = 0; i < f[0]->n; i++)
                if (d_vec(d, &f[0]->items[i], g->u.mesh.positions[i], 3)) return -1;
        }
        if (!(by_index & 2)) {
            g->u.mesh.nindices = f[1]->n;
            g->u.mesh.indices = alloc_n(d, f[1]->n, sizeof(uint32_t), f[1]->off);
            if (f[1]->n && !g->u.mesh.indices) return -1;
            for (size_t i = 0; i < f[1]->n; i++)
                if (d_u32(d, &f[1]->items[i], &g->u.mesh.indices[i])) return -1;
        }
        return 0;
    }
    default: return -1;
    }
}

static int d_enum(D *d, const JrVal *v, const char *(*name)(int), int count, const char *expected,
                  int *out) {
    if (v->type != JR_STR) return type_err(d, v, "variant identifier");
    for (int k = 0; k < count; k++) {
        const char *s = name(k);
        if (strlen(s) == v->n && memcmp(s, v->s, v->n) == 0) {
            *out = k;
            return 0;
        }
    }
    buf_clear(&d->msg);
    buf_puts(&d->msg, "unknown variant `");
    buf_append(&d->msg, v->s, v->n < 200 ? v->n : 200);
    buf_puts(&d->msg, "`, expected one of ");
    buf_puts(&d->msg, expected);
    return fail_buf(d, v->off);
}

static const char *class_name_i(int k) { return cc_ifc_class_name((cc_ifc_class)k); }
static const char *cat_name_i(int k) { return cc_category_name((cc_category)k); }

/* ---- sorted maps ---- */

typedef struct {
    const char *key;
    size_t key_n;
    size_t idx; /* member index in the object */
} Ent;

static int ent_cmp(const Ent *a, const Ent *b) {
    size_t n = a->key_n < b->key_n ? a->key_n : b->key_n;
    int c = memcmp(a->key, b->key, n);
    if (c) return c;
    return a->key_n < b->key_n ? -1 : a->key_n > b->key_n;
}

/* Stable merge sort, then keep the last of equal keys (BTreeMap insert). Returns the count. */
static size_t sort_dedupe(Ent *e, size_t n, Ent *tmp) {
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = ent_cmp(&e[j], &e[i]) < 0 ? e[j++] : e[i++];
            while (i < mid) tmp[k++] = e[i++];
            while (j < hi) tmp[k++] = e[j++];
        }
        memcpy(e, tmp, n * sizeof *e);
    }
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        if (out && ent_cmp(&e[out - 1], &e[i]) == 0) e[out - 1] = e[i];
        else e[out++] = e[i];
    }
    return out;
}

/* The object's members sorted by key, duplicates resolved. *ents is malloc'd (caller frees). */
static int map_entries(D *d, const JrVal *v, const char *expected, Ent **ents, size_t *count) {
    *ents = NULL;
    *count = 0;
    if (v->type != JR_OBJ) return type_err(d, v, expected);
    if (!v->n) return 0;
    Ent *e = v->n <= (size_t)-1 / (2 * sizeof *e) ? malloc(2 * v->n * sizeof *e) : NULL;
    if (!e) return dfail(d, v->off, "out of memory", 0, 0);
    for (size_t i = 0; i < v->n; i++) {
        e[i].key = v->members[i].key;
        e[i].key_n = v->members[i].key_n;
        e[i].idx = i;
    }
    *count = sort_dedupe(e, v->n, e + v->n);
    *ents = e;
    return 0;
}

/* BTreeMap<String, String>. */
static int d_strmap(D *d, const JrVal *v, cc_kv **out, size_t *n) {
    Ent *e;
    size_t c;
    *out = NULL;
    *n = 0;
    if (v->type == JR_OBJ)
        for (size_t i = 0; i < v->n; i++)
            if (v->members[i].val.type != JR_STR) return type_err(d, &v->members[i].val, "a string");
    if (map_entries(d, v, "a map", &e, &c)) return -1;
    cc_kv *kv = alloc_n(d, c, sizeof *kv, v->off);
    int rc = c && !kv ? -1 : 0;
    for (size_t i = 0; !rc && i < c; i++) {
        const JrMember *m = &v->members[e[i].idx];
        kv[i].key.n = m->key_n;
        kv[i].key.p = m->key_n ? arena_dup(d->a, m->key, m->key_n) : NULL;
        if (m->key_n && !kv[i].key.p) rc = dfail(d, v->off, "out of memory", 0, 0);
        else rc = d_str(d, &m->val, &kv[i].value);
    }
    free(e);
    if (rc) return -1;
    *out = kv;
    *n = c;
    return 0;
}

static int d_storey(D *d, const JrVal *v, cc_storey *s) {
    static const char *const F[] = {"id", "name", "elevation", "is_storey"};
    const JrVal *f[4];
    if (d_fields(d, v, F, 4, f, "struct Storey", 0)) return -1;
    for (int i = 0; i < 3; i++)
        if (need(d, v, f[i], F[i])) return -1;
    if (d_gid(d, f[0], &s->id) || d_str(d, f[1], &s->name) || d_f64(d, f[2], &s->elevation)) return -1;
    /* ADR 0056: "is_storey" is the C core's own, false only (true is the default, left out). */
    if (f[3] && f[3]->type != JR_BOOL) return type_err(d, f[3], "a boolean");
    s->not_storey = f[3] && !f[3]->b;
    return 0;
}

static int d_props(D *d, const JrVal *v, cc_material_props *p) {
    static const char *const F[] = {"density_kg_m3", "e_mpa", "strength_mpa", "source"};
    const JrVal *f[4];
    if (d_fields(d, v, F, 4, f, "struct MaterialProps", 1)) return -1;
    int *has[3] = {&p->has_density, &p->has_e, &p->has_strength};
    double *val[3] = {&p->density_kg_m3, &p->e_mpa, &p->strength_mpa};
    for (int i = 0; i < 3; i++)
        if (f[i] && f[i]->type != JR_NULL) {
            if (d_f64(d, f[i], val[i])) return -1;
            *has[i] = 1;
        }
    if (f[3] && d_str(d, f[3], &p->source)) return -1;
    return 0;
}

static int d_material(D *d, const JrVal *v, cc_material *m) {
    static const char *const F[] = {"id", "name", "category", "props"};
    const JrVal *f[4];
    if (d_fields(d, v, F, 4, f, "struct Material", 0)) return -1;
    for (int i = 0; i < 3; i++)
        if (need(d, v, f[i], F[i])) return -1;
    int cat = 0;
    if (d_gid(d, f[0], &m->id) || d_str(d, f[1], &m->name) ||
        d_enum(d, f[2], cat_name_i, CC_CAT_COUNT,
               "`concrete`, `steel`, `timber`, `masonry`, `insulation`, `earth`, `glass`, `other`", &cat))
        return -1;
    m->category = (cc_category)cat;
    if (f[3] && d_props(d, f[3], &m->props)) return -1;
    return 0;
}

/* model.rs Opening: { id?: GlobalId, placement, generator }. */
static int d_opening(D *d, const JrVal *v, cc_opening *o) {
    static const char *const F[] = {"id", "placement", "generator"};
    const JrVal *f[3];
    memset(o, 0, sizeof *o);
    if (d_fields(d, v, F, 3, f, "struct Opening", 0)) return -1;
    for (int i = 1; i < 3; i++)
        if (need(d, v, f[i], F[i])) return -1;
    if (f[0] && f[0]->type != JR_NULL) {
        if (d_gid(d, f[0], &o->id)) return -1;
        o->has_id = 1;
    }
    return d_placement(d, f[1], &o->placement) || d_generator(d, f[2], &o->generator) ? -1 : 0;
}

static int d_element(D *d, const JrVal *v, cc_element *e) {
    static const char *const F[] = {"id", "class", "name", "storey", "material", "placement",
                                    "generator", "openings", "properties", "ifc_entity"};
    const JrVal *f[10];
    if (d_fields(d, v, F, 10, f, "struct Element", 0)) return -1;
    for (int i = 0; i < 6; i++)
        if (need(d, v, f[i], F[i])) return -1;
    int cls = 0;
    if (d_gid(d, f[0], &e->id) ||
        d_enum(d, f[1], class_name_i, CC_IFC_CLASS_COUNT,
               "`IfcColumn`, `IfcBeam`, `IfcSlab`, `IfcWall`, `IfcFooting`, `IfcBuildingElementProxy`, "
               "`IfcElementAssembly`, `IfcVirtualElement`",
               &cls) ||
        d_str(d, f[2], &e->name) || d_gid(d, f[3], &e->storey) || d_gid(d, f[4], &e->material) ||
        d_placement(d, f[5], &e->placement))
        return -1;
    e->cls = (cc_ifc_class)cls;
    /* An IfcElementAssembly has no generator (ADR 0070): the key is left out. */
    if (!f[6] && cc_class_shapeless(e->cls)) e->generator.kind = CC_GEN_NONE; /* WAVE1B: a reference plane too */
    else if (need(d, v, f[6], F[6]) || d_generator(d, f[6], &e->generator)) return -1;
    if (f[7]) {
        if (f[7]->type != JR_ARR) return type_err(d, f[7], "a sequence");
        e->nopenings = f[7]->n;
        e->openings = alloc_n(d, f[7]->n, sizeof *e->openings, f[7]->off);
        if (f[7]->n && !e->openings) return -1;
        for (size_t i = 0; i < f[7]->n; i++)
            if (d_opening(d, &f[7]->items[i], &e->openings[i])) return -1;
    }
    if (f[8] && d_strmap(d, f[8], &e->properties, &e->nproperties)) return -1;
    if (f[9] && f[9]->type != JR_NULL) {
        if (d_str(d, f[9], &e->ifc_entity)) return -1;
        e->has_ifc_entity = 1;
    }
    return 0;
}

/* BTreeMap<GlobalId, T>: keys parsed, sorted, last duplicate kept. `keys` gets the map keys. */
typedef int (*DecodeFn)(D *, const JrVal *, void *);
static int d_idmap(D *d, const JrVal *v, const char *expected, size_t sz, DecodeFn fn, void **out,
                   size_t *n, cc_gid **keys) {
    Ent *e;
    size_t c;
    *out = NULL;
    *n = 0;
    *keys = NULL;
    if (v->type == JR_OBJ)
        for (size_t i = 0; i < v->n; i++) {
            cc_gid g;
            if (gid_from(d, v->members[i].key, v->members[i].key_n, v->members[i].val.off, &g)) return -1;
        }
    if (map_entries(d, v, expected, &e, &c)) return -1;
    /* Only the kept values are decoded: an error inside a value shadowed by a later duplicate key
     * goes unreported here, where serde reports it. */
    char *arr = alloc_n(d, c, sz, v->off);
    cc_gid *k = alloc_n(d, c, sizeof *k, v->off);
    int rc = c && (!arr || !k) ? -1 : 0;
    for (size_t i = 0; !rc && i < c; i++) {
        const JrMember *m = &v->members[e[i].idx];
        memcpy(k[i].c, m->key, 22);
        rc = fn(d, &m->val, arr + i * sz);
    }
    free(e);
    if (rc) return -1;
    *out = arr;
    *n = c;
    *keys = k;
    return 0;
}

static int fn_storey(D *d, const JrVal *v, void *o) { return d_storey(d, v, o); }
static int fn_material(D *d, const JrVal *v, void *o) { return d_material(d, v, o); }
static int fn_element(D *d, const JrVal *v, void *o) { return d_element(d, v, o); }

/* Views, sheets, schedules: view.c's decoders, their error made this one's. */
static int vd_done(D *d, cc_vd *vd, int rc) {
    if (rc && !d->failed) {
        buf_clear(&d->msg);
        buf_append(&d->msg, vd->msg.p, vd->msg.oom ? 0 : vd->msg.len);
        fail_buf(d, vd->off);
    }
    cc_vd_free(vd);
    return rc ? -1 : 0;
}
static int fn_view(D *d, const JrVal *v, void *o) {
    cc_vd vd;
    cc_vd_init(&vd, d->a);
    return vd_done(d, &vd, cc_vd_view(&vd, v, NULL, o));
}
static int fn_sheet(D *d, const JrVal *v, void *o) {
    cc_vd vd;
    cc_vd_init(&vd, d->a);
    return vd_done(d, &vd, cc_vd_sheet(&vd, v, NULL, o));
}
static int fn_relation(D *d, const JrVal *v, void *o) {
    cc_vd vd;
    cc_vd_init(&vd, d->a);
    return vd_done(d, &vd, cc_vd_relation(&vd, v, NULL, o));
}
static int fn_grid(D *d, const JrVal *v, void *o) {
    cc_vd vd;
    cc_vd_init(&vd, d->a);
    return vd_done(d, &vd, cc_vd_grid(&vd, v, NULL, o));
}

typedef struct {
    cc_gid *skeys, *mkeys, *ekeys, *vkeys, *shkeys, *rkeys, *gkeys;
} Keys;

/* ---- LNK1: "links" (model.h cc_model_link): at most CC_MODEL_LINKS_MAX entries, each a name
 * and a path (4096 bytes at most, no NUL), maybe `relative` and `hidden` ---- */
static int d_links(D *d, const JrVal *v, cc_model *m) {
    static const char *const F[] = {"hidden", "name", "path", "relative"};
    if (v->type != JR_ARR) return type_err(d, v, "a sequence");
    if (v->n > CC_MODEL_LINKS_MAX) return dfail(d, v->off, "links: too many linked models", 0, 0);
    if (!v->n) return 0;
    m->links = arena_alloc_n(d->a, v->n, sizeof *m->links, sizeof(void *));
    if (!m->links) return dfail(d, v->off, "out of memory", 0, 0);
    for (size_t i = 0; i < v->n; i++) {
        const JrVal *x = &v->items[i], *f[4];
        cc_model_link *k = &m->links[i];
        memset(k, 0, sizeof *k);
        if (d_fields(d, x, F, 4, f, "struct Link", 1) || need(d, x, f[1], F[1]) || need(d, x, f[2], F[2])) return -1;
        if (d_str(d, f[1], &k->name) || d_str(d, f[2], &k->path) || (f[3] && d_str(d, f[3], &k->relative))) return -1;
        if (f[0] && f[0]->type != JR_BOOL) return type_err(d, f[0], "a boolean");
        k->hidden = f[0] && f[0]->b;
        const cc_str t[3] = {k->name, k->path, k->relative};
        for (int j = 0; j < 3; j++) {
            int nul = 0;
            for (size_t c = 0; c < t[j].n && t[j].n <= 4096; c++) nul |= t[j].p[c] == 0;
            if (t[j].n > 4096 || nul || (j < 2 && !t[j].n))
                return dfail(d, x->off, "links: a name and a path (4096 bytes at most, no NUL)", 0, 0);
        }
    }
    m->nlinks = v->n;
    return 0;
}
/* ---- end LNK1 ---- */

static int d_model(D *d, const JrVal *v, cc_model *m, Keys *keys) {
    static const char *const F[] = {"project_id", "project_name", "storeys", "materials", "elements",
                                    "views", "sheets", "project", "schedules", "georef", "relations", "grids",
                                    "links" /* LNK1 */};
    const JrVal *f[13];
    if (d_fields(d, v, F, 13, f, "struct Model", 0)) return -1;
    for (int i = 0; i < 5; i++)
        if (need(d, v, f[i], F[i])) return -1;
    void *p;
    if (d_gid(d, f[0], &m->project_id) || d_str(d, f[1], &m->project_name)) return -1;
    if (d_idmap(d, f[2], "a map", sizeof(cc_storey), fn_storey, &p, &m->nstoreys, &keys->skeys)) return -1;
    m->storeys = p;
    if (d_idmap(d, f[3], "a map", sizeof(cc_material), fn_material, &p, &m->nmaterials, &keys->mkeys)) return -1;
    m->materials = p;
    if (d_idmap(d, f[4], "a map", sizeof(cc_element), fn_element, &p, &m->nelements, &keys->ekeys)) return -1;
    m->elements = p, m->cap_elements = 0;
    if (f[5]) {
        if (d_idmap(d, f[5], "a map", sizeof(cc_view), fn_view, &p, &m->nviews, &keys->vkeys)) return -1;
        m->views = p;
    }
    if (f[6]) {
        if (d_idmap(d, f[6], "a map", sizeof(cc_sheet), fn_sheet, &p, &m->nsheets, &keys->shkeys)) return -1;
        m->sheets = p;
    }
    if (f[7] && d_strmap(d, f[7], &m->project, &m->nproject)) return -1;
    if (f[8]) {
        cc_vd vd;
        cc_vd_init(&vd, d->a);
        if (vd_done(d, &vd, cc_vd_schedules(&vd, f[8], &m->schedules, &m->nschedules))) return -1;
    }
    if (f[9]) {
        cc_vd vd;
        cc_vd_init(&vd, d->a);
        if (vd_done(d, &vd, cc_vd_georef(&vd, f[9], NULL, &m->georef))) return -1;
    }
    if (f[10]) {
        if (d_idmap(d, f[10], "a map", sizeof(cc_relation), fn_relation, &p, &m->nrelations, &keys->rkeys)) return -1;
        m->relations = p;
    }
    if (f[11]) {
        if (d_idmap(d, f[11], "a map", sizeof(cc_grid), fn_grid, &p, &m->ngrids, &keys->gkeys)) return -1;
        m->grids = p;
    }
    if (f[12] && d_links(d, f[12], m)) return -1; /* LNK1 */
    return 0;
}

/* The binary tables `positions` / `indices` of the top level: every entry decoded (as Rust's
 * read_table does), kept for the elements' indices in a binary read; in a text read only
 * checked and dropped, as Rust does outside a binary read. */
static int d_table(D *d, const JrVal *v, int positions) {
    if (v->type != JR_ARR) return type_err(d, v, "a sequence");
    int keep = d->version == CC_BINARY_VERSION;
    size_t *cnt = NULL;
    if (keep && v->n) {
        cnt = alloc_n(d, v->n, sizeof(size_t), v->off);
        if (!cnt) return -1;
        if (positions) {
            if (!(d->tpos = alloc_n(d, v->n, sizeof *d->tpos, v->off))) return -1;
            d->tpos_n = cnt, d->ntpos = v->n;
        } else {
            if (!(d->tidx = alloc_n(d, v->n, sizeof *d->tidx, v->off))) return -1;
            d->tidx_n = cnt, d->ntidx = v->n;
        }
    }
    for (size_t i = 0; i < v->n; i++) {
        const JrVal *a = &v->items[i];
        if (a->type != JR_ARR) return type_err(d, a, "a sequence");
        double(*pos)[3] = NULL;
        uint32_t *idx = NULL;
        if (keep && a->n) {
            if (positions) pos = alloc_n(d, a->n, sizeof *pos, a->off);
            else idx = alloc_n(d, a->n, sizeof *idx, a->off);
            if (!pos && !idx) return -1;
        }
        for (size_t j = 0; j < a->n; j++) {
            double t[3];
            uint32_t u;
            if (positions ? d_vec(d, &a->items[j], pos ? pos[j] : t, 3)
                          : d_u32(d, &a->items[j], idx ? &idx[j] : &u))
                return -1;
        }
        if (keep) {
            if (positions) d->tpos[i] = pos;
            else d->tidx[i] = idx;
            cnt[i] = a->n;
        }
    }
    return 0;
}

static int d_file(D *d, const JrVal *root, cc_model *m, Keys *keys) {
    static const char *const F[] = {"format", "positions", "indices", "version", "model"};
    const JrVal *f[5];
    cc_str format;
    uint32_t version = 0;
    if (d_fields(d, root, F, 5, f, "struct In", 0)) return -1;
    if (need(d, root, f[0], F[0]) || d_str(d, f[0], &format)) return -1;
    if (f[1] && d_table(d, f[1], 1)) return -1;
    if (f[2] && d_table(d, f[2], 0)) return -1;
    if (need(d, root, f[3], F[3]) || d_u32(d, f[3], &version)) return -1;
    if (need(d, root, f[4], F[4]) || d_model(d, f[4], m, keys)) return -1;
    if (format.n != 6 || memcmp(format.p, "bimble", 6) != 0) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "not a bimble file (format = ");
        cc_buf_debug_str(&d->msg, format.p, format.n < 200 ? format.n : 200);
        buf_putc(&d->msg, ')');
        return fail_buf(d, CC_NO_OFFSET);
    }
    if (version != d->version) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "unsupported bimble format version ");
        buf_u64(&d->msg, version);
        buf_puts(&d->msg, " (this build reads 1 and 2)");
        return fail_buf(d, CC_NO_OFFSET);
    }
    return 0;
}

static int load(cc_model *m, const char *text, size_t n, cc_error *err, int validate) {
    cc_error dummy;
    if (!err) err = &dummy;
    cc_model_init(m);
    D d = {&m->arena, err, 0, {0}, CC_FORMAT_VERSION, NULL, NULL, NULL, NULL, 0, 0};
    buf_init(&d.msg);
    err->offset = CC_NO_OFFSET;
    err->msg[0] = 0;
    int binary = n >= CC_MAGIC_LEN && memcmp(text, CC_MAGIC, CC_MAGIC_LEN) == 0;
    if (n > (binary ? CC_MAX_BINARY_FILE_BYTES : CC_MAX_FILE_BYTES)) {
        buf_puts(&d.msg, "file too large: ");
        buf_u64(&d.msg, n);
        buf_puts(&d.msg, " bytes");
        fail_buf(&d, CC_NO_OFFSET);
        buf_free(&d.msg);
        cc_model_free(m);
        cc_model_init(m);
        return -1;
    }
    Arena dom;
    arena_init(&dom, 0);
    JrError je;
    Keys keys = {0};
    int rc = 0;
    const JrVal *root = NULL;
    if (binary) {
        /* Untrusted: what decompresses is capped too, not only the file. */
        Buf body;
        const char *why;
        buf_init(&body);
        d.version = CC_BINARY_VERSION;
        if (cc_gunzip((const unsigned char *)text + CC_MAGIC_LEN, n - CC_MAGIC_LEN, CC_MAX_INFLATED_BYTES,
                      &body, &why)) {
            rc = dfail(&d, CC_NO_OFFSET, "parse: ", why, 0);
        } else if (!(root = cbor_parse(&dom, (const unsigned char *)body.p, body.len, &je))) {
            rc = dfail(&d, je.offset, je.msg, 0, 0);
        }
        buf_free(&body);
    } else if (!(root = jr_parse(&dom, text, n, &je))) {
        rc = dfail(&d, je.offset, je.msg, 0, 0);
    }
    if (rc) {
    } else if (d_file(&d, root, m, &keys)) {
        rc = -1;
    } else {
        cc_error verr;
        if (validate && cc_model_validate_keys(m, keys.skeys, keys.mkeys, keys.ekeys, keys.vkeys, keys.shkeys, keys.rkeys,
                                   keys.gkeys, &verr)) {
            rc = dfail(&d, CC_NO_OFFSET, "invalid model: ", verr.msg, 0);
        } else {
            cc_model_canonical(m);
        }
    }
    arena_free(&dom);
    buf_free(&d.msg);
    if (rc) {
        cc_model_free(m);
        cc_model_init(m);
    }
    return rc;
}

int cc_model_load(cc_model *m, const char *text, size_t n, cc_error *err) { return load(m, text, n, err, 1); }

/* ---- H2b: a merged model, decoded but not validated (merge.c) ---- */
int cc_model_load_unchecked(cc_model *m, const char *text, size_t n, cc_error *err) {
    return load(m, text, n, err, 0);
}
/* ---- end H2b ---- */

/* ---- save ---- */

static void w_str(JsonW *w, cc_str s) { jw_str_n(w, s.n ? s.p : "", s.n); }
static void w_gid(JsonW *w, const cc_gid *g) { jw_str_n(w, g->c, 22); }

static void w_vec(JsonW *w, const double *v, int n) {
    jw_arr_begin(w);
    for (int i = 0; i < n; i++) jw_f64(w, v[i]);
    jw_arr_end(w);
}

static void w_loop(JsonW *w, const cc_loop *l) {
    jw_arr_begin(w);
    for (size_t i = 0; i < l->n; i++) w_vec(w, l->pts[i], 2);
    jw_arr_end(w);
}

static void w_kvs(JsonW *w, const cc_kv *kv, size_t n) {
    jw_obj_begin(w);
    for (size_t i = 0; i < n; i++) {
        jw_key_n(w, kv[i].key.n ? kv[i].key.p : "", kv[i].key.n);
        w_str(w, kv[i].value);
    }
    jw_obj_end(w);
}

/* H2a: mesh arrays as references (cc_model_object_json): NULL writes them in full. */
static void w_generator(JsonW *w, const cc_generator *g, const cc_mesh_refs *refs);

/* One mesh array written compactly into a scratch buffer, handed to refs->mesh_ref, its hash
 * written in its place (repo.rs put: file::shared::Refs). A failure sticks in the writer. */
static void w_mesh_ref(JsonW *w, const cc_generator *g, int indices, const cc_mesh_refs *refs) {
    Buf a;
    buf_init(&a);
    JsonW aw;
    jw_init(&aw, &a, 0);
    jw_arr_begin(&aw);
    if (indices)
        for (size_t i = 0; i < g->u.mesh.nindices; i++) jw_u64(&aw, g->u.mesh.indices[i]);
    else
        for (size_t i = 0; i < g->u.mesh.npositions; i++) {
            jw_arr_begin(&aw);
            for (int k = 0; k < 3; k++) jw_f64(&aw, g->u.mesh.positions[i][k]);
            jw_arr_end(&aw);
        }
    jw_arr_end(&aw);
    char hash[65];
    if (jw_finish(&aw) != JW_OK || a.oom || refs->mesh_ref(refs->ctx, a.p ? a.p : "", a.len, hash)) {
        if (!w->err) w->err = JW_ERR_OOM;
    } else
        jw_str_n(w, hash, 64);
    buf_free(&a);
}

static void w_generator(JsonW *w, const cc_generator *g, const cc_mesh_refs *refs) {
    jw_obj_begin(w);
    jw_key(w, "kind");
    jw_str(w, cc_gen_kind_name(g->kind));
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
        jw_key(w, "width"), jw_f64(w, g->u.col.width);
        jw_key(w, "depth"), jw_f64(w, g->u.col.depth);
        jw_key(w, "height"), jw_f64(w, g->u.col.height);
        break;
    case CC_GEN_RECT_BEAM:
        jw_key(w, "width"), jw_f64(w, g->u.beam.width);
        jw_key(w, "height"), jw_f64(w, g->u.beam.height);
        jw_key(w, "length"), jw_f64(w, g->u.beam.length);
        break;
    case CC_GEN_WALL:
        jw_key(w, "length"), jw_f64(w, g->u.wall.length);
        jw_key(w, "thickness"), jw_f64(w, g->u.wall.thickness);
        jw_key(w, "height"), jw_f64(w, g->u.wall.height);
        break;
    case CC_GEN_SLAB:
        jw_key(w, "outline"), w_loop(w, &g->u.slab.outline);
        jw_key(w, "thickness"), jw_f64(w, g->u.slab.thickness);
        break;
    case CC_GEN_EXTRUDED_POLYGON:
        jw_key(w, "outline"), w_loop(w, &g->u.poly.outline);
        if (g->u.poly.nvoids) {
            jw_key(w, "voids");
            jw_arr_begin(w);
            for (size_t i = 0; i < g->u.poly.nvoids; i++) w_loop(w, &g->u.poly.voids[i]);
            jw_arr_end(w);
        }
        jw_key(w, "height"), jw_f64(w, g->u.poly.height);
        break;
    case CC_GEN_MESH:
        if (refs && refs->mesh_ref) {
            jw_key(w, "positions"), w_mesh_ref(w, g, 0, refs);
            jw_key(w, "indices"), w_mesh_ref(w, g, 1, refs);
            break;
        }
        jw_key(w, "positions");
        jw_arr_begin(w);
        for (size_t i = 0; i < g->u.mesh.npositions; i++) w_vec(w, g->u.mesh.positions[i], 3);
        jw_arr_end(w);
        jw_key(w, "indices");
        jw_arr_begin(w);
        for (size_t i = 0; i < g->u.mesh.nindices; i++) jw_u64(w, g->u.mesh.indices[i]);
        jw_arr_end(w);
        break;
    default: break;
    }
    jw_obj_end(w);
}

static void w_placement(JsonW *w, const cc_placement *p) {
    jw_obj_begin(w);
    jw_key(w, "origin"), w_vec(w, p->origin, 3);
    jw_key(w, "z_axis"), w_vec(w, p->z_axis, 3);
    jw_key(w, "x_axis"), w_vec(w, p->x_axis, 3);
    jw_obj_end(w);
}

/* One storey, material, element as serde writes it (cc_model_save, cc_model_object_json). */
static void w_storey(JsonW *w, const cc_storey *s) {
    jw_obj_begin(w);
    jw_key(w, "id"), w_gid(w, &s->id);
    jw_key(w, "name"), w_str(w, s->name);
    jw_key(w, "elevation"), jw_f64(w, s->elevation);
    if (s->not_storey) jw_key(w, "is_storey"), jw_bool(w, 0);
    jw_obj_end(w);
}

static void w_material(JsonW *w, const cc_material *mt) {
    const cc_material_props *p = &mt->props;
    jw_obj_begin(w);
    jw_key(w, "id"), w_gid(w, &mt->id);
    jw_key(w, "name"), w_str(w, mt->name);
    jw_key(w, "category"), jw_str(w, cc_category_name(mt->category));
    if (p->has_density || p->has_e || p->has_strength || p->source.n) {
        jw_key(w, "props");
        jw_obj_begin(w);
        if (p->has_density) jw_key(w, "density_kg_m3"), jw_f64(w, p->density_kg_m3);
        if (p->has_e) jw_key(w, "e_mpa"), jw_f64(w, p->e_mpa);
        if (p->has_strength) jw_key(w, "strength_mpa"), jw_f64(w, p->strength_mpa);
        if (p->source.n) jw_key(w, "source"), w_str(w, p->source);
        jw_obj_end(w);
    }
    jw_obj_end(w);
}

static void w_element(JsonW *w, const cc_element *e, const cc_mesh_refs *refs) {
    jw_obj_begin(w);
    jw_key(w, "id"), w_gid(w, &e->id);
    jw_key(w, "class"), jw_str(w, cc_ifc_class_name(e->cls));
    jw_key(w, "name"), w_str(w, e->name);
    jw_key(w, "storey"), w_gid(w, &e->storey);
    jw_key(w, "material"), w_gid(w, &e->material);
    jw_key(w, "placement"), w_placement(w, &e->placement);
    if (e->generator.kind != CC_GEN_NONE) jw_key(w, "generator"), w_generator(w, &e->generator, refs);
    if (e->nopenings) {
        jw_key(w, "openings");
        jw_arr_begin(w);
        for (size_t j = 0; j < e->nopenings; j++) {
            const cc_opening *o = &e->openings[j];
            jw_obj_begin(w);
            if (o->has_id) jw_key(w, "id"), w_gid(w, &o->id);
            jw_key(w, "placement"), w_placement(w, &o->placement);
            jw_key(w, "generator"), w_generator(w, &o->generator, refs);
            jw_obj_end(w);
        }
        jw_arr_end(w);
    }
    if (e->nproperties) jw_key(w, "properties"), w_kvs(w, e->properties, e->nproperties);
    if (e->has_ifc_entity) jw_key(w, "ifc_entity"), w_str(w, e->ifc_entity);
    jw_obj_end(w);
}

int cc_model_save(const cc_model *m, Buf *b) {
    JsonW w;
    jw_init(&w, b, 1);
    jw_obj_begin(&w);
    jw_key(&w, "format"), jw_str(&w, "bimble");
    jw_key(&w, "version"), jw_u64(&w, CC_FORMAT_VERSION);
    jw_key(&w, "model");
    jw_obj_begin(&w);
    jw_key(&w, "project_id"), w_gid(&w, &m->project_id);
    jw_key(&w, "project_name"), w_str(&w, m->project_name);
    jw_key(&w, "storeys");
    jw_obj_begin(&w);
    for (size_t i = 0; i < m->nstoreys; i++) jw_key_n(&w, m->storeys[i].id.c, 22), w_storey(&w, &m->storeys[i]);
    jw_obj_end(&w);
    jw_key(&w, "materials");
    jw_obj_begin(&w);
    for (size_t i = 0; i < m->nmaterials; i++) jw_key_n(&w, m->materials[i].id.c, 22), w_material(&w, &m->materials[i]);
    jw_obj_end(&w);
    jw_key(&w, "elements");
    jw_obj_begin(&w);
    for (size_t i = 0; i < m->nelements; i++) jw_key_n(&w, m->elements[i].id.c, 22), w_element(&w, &m->elements[i], NULL);
    jw_obj_end(&w);
    cc_em em = {&w, NULL};
    if (m->nviews) {
        jw_key(&w, "views");
        jw_obj_begin(&w);
        for (size_t i = 0; i < m->nviews; i++) jw_key_n(&w, m->views[i].id.c, 22), cc_em_view(&em, &m->views[i], NULL);
        jw_obj_end(&w);
    }
    if (m->nsheets) {
        jw_key(&w, "sheets");
        jw_obj_begin(&w);
        for (size_t i = 0; i < m->nsheets; i++) jw_key_n(&w, m->sheets[i].id.c, 22), cc_em_sheet(&em, &m->sheets[i], NULL);
        jw_obj_end(&w);
    }
    if (m->nproject) jw_key(&w, "project"), w_kvs(&w, m->project, m->nproject);
    if (m->nschedules) jw_key(&w, "schedules"), cc_em_schedules(&em, m->schedules, m->nschedules);
    if (!cc_georef_is_default(&m->georef)) jw_key(&w, "georef"), cc_em_georef(&em, &m->georef);
    if (m->nrelations) {
        jw_key(&w, "relations");
        jw_obj_begin(&w);
        for (size_t i = 0; i < m->nrelations; i++)
            jw_key_n(&w, m->relations[i].id.c, 22), cc_em_relation(&em, &m->relations[i], NULL);
        jw_obj_end(&w);
    }
    if (m->ngrids) {
        jw_key(&w, "grids");
        jw_obj_begin(&w);
        for (size_t i = 0; i < m->ngrids; i++) jw_key_n(&w, m->grids[i].id.c, 22), cc_em_grid(&em, &m->grids[i], NULL);
        jw_obj_end(&w);
    }
    if (m->nlinks) { /* LNK1: model.h cc_model_link, keys sorted */
        jw_key(&w, "links");
        jw_arr_begin(&w);
        for (size_t i = 0; i < m->nlinks; i++) {
            const cc_model_link *k = &m->links[i];
            jw_obj_begin(&w);
            if (k->hidden) jw_key(&w, "hidden"), jw_bool(&w, 1);
            jw_key(&w, "name"), jw_str_n(&w, k->name.p ? k->name.p : "", k->name.n);
            jw_key(&w, "path"), jw_str_n(&w, k->path.p ? k->path.p : "", k->path.n);
            if (k->relative.n) jw_key(&w, "relative"), jw_str_n(&w, k->relative.p, k->relative.n);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
    }
    jw_obj_end(&w);
    jw_obj_end(&w);
    return jw_finish(&w) == JW_OK && !b->oom ? 0 : -1;
}

/* ---- H2a: one object's compact JSON (the history store, repo_write.c) ---- */
int cc_model_object_json(const cc_model *m, int kind, size_t i, const cc_mesh_refs *refs, Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    cc_em em = {&w, NULL};
    switch (kind) {
    case CC_OBJ_STOREY: if (i >= m->nstoreys) return -1; w_storey(&w, &m->storeys[i]); break;
    case CC_OBJ_MATERIAL: if (i >= m->nmaterials) return -1; w_material(&w, &m->materials[i]); break;
    case CC_OBJ_ELEMENT: if (i >= m->nelements) return -1; w_element(&w, &m->elements[i], refs); break;
    case CC_OBJ_VIEW: if (i >= m->nviews) return -1; cc_em_view(&em, &m->views[i], NULL); break;
    case CC_OBJ_SHEET: if (i >= m->nsheets) return -1; cc_em_sheet(&em, &m->sheets[i], NULL); break;
    case CC_OBJ_RELATION: if (i >= m->nrelations) return -1; cc_em_relation(&em, &m->relations[i], NULL); break;
    case CC_OBJ_GRID: if (i >= m->ngrids) return -1; cc_em_grid(&em, &m->grids[i], NULL); break;
    case CC_OBJ_PROJECT: w_kvs(&w, m->project, m->nproject); break;
    case CC_OBJ_SCHEDULES: cc_em_schedules(&em, m->schedules, m->nschedules); break;
    case CC_OBJ_GEOREF: cc_em_georef(&em, &m->georef); break;
    default: return -1;
    }
    return jw_finish(&w) == JW_OK && !out->oom ? 0 : -1;
}
/* ---- end H2a ---- */

/* ---- save, binary (version 2) ---- */

/* Distinct mesh arrays by address, in the order first met (Rust's shared::Pool). */
typedef struct {
    const void **items;
    size_t *counts;
    size_t n, cap;
    const void **slot; /* open addressing: address -> index + 1 kept in `at` */
    size_t *at;
    size_t mask;
} Pool;

static size_t ptr_hash(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return (size_t)x;
}

/* Index of `p` in the pool, added if new. An empty array (NULL) is never shared. -1 on out of
 * memory. */
static long long pool_add(Pool *pl, const void *p, size_t count) {
    if (p) {
        for (size_t h = ptr_hash(p) & pl->mask;; h = (h + 1) & pl->mask) {
            if (!pl->slot[h]) {
                pl->slot[h] = p;
                pl->at[h] = pl->n;
                break;
            }
            if (pl->slot[h] == p) return (long long)pl->at[h];
        }
    }
    if (pl->n == pl->cap) return -1; /* sized for every mesh up front */
    pl->items[pl->n] = p;
    pl->counts[pl->n] = count;
    return (long long)pl->n++;
}

static int pool_init(Pool *pl, size_t meshes) {
    memset(pl, 0, sizeof *pl);
    size_t tab = 16;
    while (tab < 2 * meshes) tab *= 2;
    pl->cap = meshes;
    pl->mask = tab - 1;
    pl->items = calloc(meshes ? meshes : 1, sizeof *pl->items);
    pl->counts = calloc(meshes ? meshes : 1, sizeof *pl->counts);
    pl->slot = calloc(tab, sizeof *pl->slot);
    pl->at = calloc(tab, sizeof *pl->at);
    return pl->items && pl->counts && pl->slot && pl->at ? 0 : -1;
}

static void pool_free(Pool *pl) {
    free(pl->items);
    free(pl->counts);
    free(pl->slot);
    free(pl->at);
}

static void c_str(Buf *b, cc_str s) { cb_text(b, s.n ? s.p : "", s.n); }
static void c_key(Buf *b, const char *k) { cb_text(b, k, strlen(k)); }
static void c_gid(Buf *b, const cc_gid *g) { cb_text(b, g->c, 22); }

static void c_vec(Buf *b, const double *v, int n) {
    cb_head(b, CB_ARRAY, (uint64_t)n);
    for (int i = 0; i < n; i++) cb_f64(b, v[i]);
}

static void c_loop(Buf *b, const cc_loop *l) {
    cb_head(b, CB_ARRAY, l->n);
    for (size_t i = 0; i < l->n; i++) c_vec(b, l->pts[i], 2);
}

static void c_kvs(Buf *b, const cc_kv *kv, size_t n) {
    cb_head(b, CB_MAP, n);
    for (size_t i = 0; i < n; i++) c_str(b, kv[i].key), c_str(b, kv[i].value);
}

static void c_generator(Buf *b, const cc_generator *g, uint64_t pos_i, uint64_t idx_i) {
    int fields = 4;
    if (g->kind == CC_GEN_SLAB || g->kind == CC_GEN_MESH) fields = 3;
    if (g->kind == CC_GEN_EXTRUDED_POLYGON) fields = g->u.poly.nvoids ? 4 : 3;
    cb_head(b, CB_MAP, (uint64_t)fields);
    c_key(b, "kind"), c_key(b, cc_gen_kind_name(g->kind));
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
        c_key(b, "width"), cb_f64(b, g->u.col.width);
        c_key(b, "depth"), cb_f64(b, g->u.col.depth);
        c_key(b, "height"), cb_f64(b, g->u.col.height);
        break;
    case CC_GEN_RECT_BEAM:
        c_key(b, "width"), cb_f64(b, g->u.beam.width);
        c_key(b, "height"), cb_f64(b, g->u.beam.height);
        c_key(b, "length"), cb_f64(b, g->u.beam.length);
        break;
    case CC_GEN_WALL:
        c_key(b, "length"), cb_f64(b, g->u.wall.length);
        c_key(b, "thickness"), cb_f64(b, g->u.wall.thickness);
        c_key(b, "height"), cb_f64(b, g->u.wall.height);
        break;
    case CC_GEN_SLAB:
        c_key(b, "outline"), c_loop(b, &g->u.slab.outline);
        c_key(b, "thickness"), cb_f64(b, g->u.slab.thickness);
        break;
    case CC_GEN_EXTRUDED_POLYGON:
        c_key(b, "outline"), c_loop(b, &g->u.poly.outline);
        if (g->u.poly.nvoids) {
            c_key(b, "voids");
            cb_head(b, CB_ARRAY, g->u.poly.nvoids);
            for (size_t i = 0; i < g->u.poly.nvoids; i++) c_loop(b, &g->u.poly.voids[i]);
        }
        c_key(b, "height"), cb_f64(b, g->u.poly.height);
        break;
    case CC_GEN_MESH:
        c_key(b, "positions"), cb_head(b, CB_UINT, pos_i);
        c_key(b, "indices"), cb_head(b, CB_UINT, idx_i);
        break;
    default: break;
    }
}

int cc_model_to_cbor(const cc_model *m, Buf *b) {
    size_t meshes = 0;
    for (size_t i = 0; i < m->nelements; i++) {
        meshes += m->elements[i].generator.kind == CC_GEN_MESH;
        for (size_t j = 0; j < m->elements[i].nopenings; j++) meshes += m->elements[i].openings[j].generator.kind == CC_GEN_MESH;
    }
    Pool pp, pi;
    long long *refs = calloc(2 * (meshes ? meshes : 1), sizeof *refs);
    int rc = pool_init(&pp, meshes) | pool_init(&pi, meshes) | (refs ? 0 : -1);
    /* Every mesh array once, in model order (an element's generator, then its openings'); then
     * the model, its meshes as indices. */
    for (size_t i = 0, k = 0; !rc && i < m->nelements; i++)
        for (size_t j = 0; !rc && j <= m->elements[i].nopenings; j++) {
            const cc_generator *g = j ? &m->elements[i].openings[j - 1].generator : &m->elements[i].generator;
            if (g->kind != CC_GEN_MESH) continue;
            refs[2 * k] = pool_add(&pp, g->u.mesh.positions, g->u.mesh.npositions);
            refs[2 * k + 1] = pool_add(&pi, g->u.mesh.indices, g->u.mesh.nindices);
            if (refs[2 * k] < 0 || refs[2 * k + 1] < 0) rc = -1;
            k++;
        }
    if (!rc) {
        cb_head(b, CB_MAP, 5);
        c_key(b, "format"), c_key(b, "bimble");
        c_key(b, "version"), cb_head(b, CB_UINT, CC_BINARY_VERSION);
        c_key(b, "positions");
        cb_head(b, CB_ARRAY, pp.n);
        for (size_t i = 0; i < pp.n; i++) {
            const double(*p)[3] = pp.items[i];
            cb_head(b, CB_ARRAY, pp.counts[i]);
            for (size_t j = 0; j < pp.counts[i]; j++) c_vec(b, p[j], 3);
        }
        c_key(b, "indices");
        cb_head(b, CB_ARRAY, pi.n);
        for (size_t i = 0; i < pi.n; i++) {
            const uint32_t *x = pi.items[i];
            cb_head(b, CB_ARRAY, pi.counts[i]);
            for (size_t j = 0; j < pi.counts[i]; j++) cb_head(b, CB_UINT, x[j]);
        }
        c_key(b, "model");
        cb_head(b, CB_MAP, 5 + (m->nviews ? 1 : 0) + (m->nsheets ? 1 : 0) + (m->nproject ? 1 : 0) +
                               (m->nschedules ? 1 : 0) + (cc_georef_is_default(&m->georef) ? 0 : 1) +
                               (m->nrelations ? 1 : 0) + (m->ngrids ? 1 : 0) + (m->nlinks ? 1 : 0) /* LNK1 */);
        c_key(b, "project_id"), c_gid(b, &m->project_id);
        c_key(b, "project_name"), c_str(b, m->project_name);
        c_key(b, "storeys");
        cb_head(b, CB_MAP, m->nstoreys);
        for (size_t i = 0; i < m->nstoreys; i++) {
            const cc_storey *s = &m->storeys[i];
            c_gid(b, &s->id);
            cb_head(b, CB_MAP, 3 + (s->not_storey ? 1 : 0));
            c_key(b, "id"), c_gid(b, &s->id);
            c_key(b, "name"), c_str(b, s->name);
            c_key(b, "elevation"), cb_f64(b, s->elevation);
            if (s->not_storey) c_key(b, "is_storey"), buf_putc(b, (char)0xF4); /* CBOR false */
        }
        c_key(b, "materials");
        cb_head(b, CB_MAP, m->nmaterials);
        for (size_t i = 0; i < m->nmaterials; i++) {
            const cc_material *mt = &m->materials[i];
            const cc_material_props *p = &mt->props;
            int np = p->has_density + p->has_e + p->has_strength + (p->source.n ? 1 : 0);
            c_gid(b, &mt->id);
            cb_head(b, CB_MAP, 3 + (np ? 1 : 0));
            c_key(b, "id"), c_gid(b, &mt->id);
            c_key(b, "name"), c_str(b, mt->name);
            c_key(b, "category"), c_key(b, cc_category_name(mt->category));
            if (np) {
                c_key(b, "props");
                cb_head(b, CB_MAP, (uint64_t)np);
                if (p->has_density) c_key(b, "density_kg_m3"), cb_f64(b, p->density_kg_m3);
                if (p->has_e) c_key(b, "e_mpa"), cb_f64(b, p->e_mpa);
                if (p->has_strength) c_key(b, "strength_mpa"), cb_f64(b, p->strength_mpa);
                if (p->source.n) c_key(b, "source"), c_str(b, p->source);
            }
        }
        c_key(b, "elements");
        cb_head(b, CB_MAP, m->nelements);
        for (size_t i = 0, k = 0; i < m->nelements; i++) {
            const cc_element *e = &m->elements[i];
            c_gid(b, &e->id);
            int has_gen = e->generator.kind != CC_GEN_NONE;
            cb_head(b, CB_MAP, 6 + (has_gen ? 1 : 0) + (e->nopenings ? 1 : 0) + (e->nproperties ? 1 : 0) +
                                   (e->has_ifc_entity ? 1 : 0));
            c_key(b, "id"), c_gid(b, &e->id);
            c_key(b, "class"), c_key(b, cc_ifc_class_name(e->cls));
            c_key(b, "name"), c_str(b, e->name);
            c_key(b, "storey"), c_gid(b, &e->storey);
            c_key(b, "material"), c_gid(b, &e->material);
            c_key(b, "placement");
            cb_head(b, CB_MAP, 3);
            c_key(b, "origin"), c_vec(b, e->placement.origin, 3);
            c_key(b, "z_axis"), c_vec(b, e->placement.z_axis, 3);
            c_key(b, "x_axis"), c_vec(b, e->placement.x_axis, 3);
            if (has_gen) c_key(b, "generator");
            if (!has_gen) {
            } else if (e->generator.kind == CC_GEN_MESH) {
                c_generator(b, &e->generator, (uint64_t)refs[2 * k], (uint64_t)refs[2 * k + 1]);
                k++;
            } else c_generator(b, &e->generator, 0, 0);
            if (e->nopenings) {
                c_key(b, "openings");
                cb_head(b, CB_ARRAY, e->nopenings);
                for (size_t j = 0; j < e->nopenings; j++) {
                    const cc_opening *o = &e->openings[j];
                    cb_head(b, CB_MAP, 2 + (o->has_id ? 1 : 0));
                    if (o->has_id) c_key(b, "id"), c_gid(b, &o->id);
                    c_key(b, "placement");
                    cb_head(b, CB_MAP, 3);
                    c_key(b, "origin"), c_vec(b, o->placement.origin, 3);
                    c_key(b, "z_axis"), c_vec(b, o->placement.z_axis, 3);
                    c_key(b, "x_axis"), c_vec(b, o->placement.x_axis, 3);
                    c_key(b, "generator");
                    if (o->generator.kind == CC_GEN_MESH) {
                        c_generator(b, &o->generator, (uint64_t)refs[2 * k], (uint64_t)refs[2 * k + 1]);
                        k++;
                    } else c_generator(b, &o->generator, 0, 0);
                }
            }
            if (e->nproperties) c_key(b, "properties"), c_kvs(b, e->properties, e->nproperties);
            if (e->has_ifc_entity) c_key(b, "ifc_entity"), c_str(b, e->ifc_entity);
        }
        cc_em em = {NULL, b};
        if (m->nviews) {
            c_key(b, "views");
            cb_head(b, CB_MAP, m->nviews);
            for (size_t i = 0; i < m->nviews; i++) c_gid(b, &m->views[i].id), cc_em_view(&em, &m->views[i], NULL);
        }
        if (m->nsheets) {
            c_key(b, "sheets");
            cb_head(b, CB_MAP, m->nsheets);
            for (size_t i = 0; i < m->nsheets; i++) c_gid(b, &m->sheets[i].id), cc_em_sheet(&em, &m->sheets[i], NULL);
        }
        if (m->nproject) c_key(b, "project"), c_kvs(b, m->project, m->nproject);
        if (m->nschedules) c_key(b, "schedules"), cc_em_schedules(&em, m->schedules, m->nschedules);
        if (!cc_georef_is_default(&m->georef)) c_key(b, "georef"), cc_em_georef(&em, &m->georef);
        if (m->nrelations) {
            c_key(b, "relations");
            cb_head(b, CB_MAP, m->nrelations);
            for (size_t i = 0; i < m->nrelations; i++) c_gid(b, &m->relations[i].id), cc_em_relation(&em, &m->relations[i], NULL);
        }
        if (m->ngrids) {
            c_key(b, "grids");
            cb_head(b, CB_MAP, m->ngrids);
            for (size_t i = 0; i < m->ngrids; i++) c_gid(b, &m->grids[i].id), cc_em_grid(&em, &m->grids[i], NULL);
        }
        if (m->nlinks) { /* LNK1: as the text form */
            c_key(b, "links");
            cb_head(b, CB_ARRAY, m->nlinks);
            for (size_t i = 0; i < m->nlinks; i++) {
                const cc_model_link *k = &m->links[i];
                cb_head(b, CB_MAP, 2 + (k->hidden ? 1 : 0) + (k->relative.n ? 1 : 0));
                if (k->hidden) c_key(b, "hidden"), cb_head(b, 7, 21); /* true */
                c_key(b, "name"), c_str(b, k->name);
                c_key(b, "path"), c_str(b, k->path);
                if (k->relative.n) c_key(b, "relative"), c_str(b, k->relative);
            }
        }
    }
    pool_free(&pp);
    pool_free(&pi);
    free(refs);
    return !rc && !b->oom ? 0 : -1;
}

int cc_model_save_v2(const cc_model *m, Buf *b) {
    Buf cbor;
    buf_init(&cbor);
    int rc = cc_model_to_cbor(m, &cbor);
    if (!rc) {
        buf_append(b, CC_MAGIC, CC_MAGIC_LEN);
        rc = cc_gzip((const unsigned char *)cbor.p, cbor.len, b);
    }
    buf_free(&cbor);
    return !rc && !b->oom ? 0 : -1;
}

int cc_model_save_v2_stored(const cc_model *m, Buf *b) {
    Buf cbor;
    buf_init(&cbor);
    int rc = cc_model_to_cbor(m, &cbor);
    if (!rc) {
        buf_append(b, CC_MAGIC, CC_MAGIC_LEN);
        rc = cc_gzip_stored((const unsigned char *)cbor.p, cbor.len, b);
    }
    buf_free(&cbor);
    return !rc && !b->oom ? 0 : -1;
}
