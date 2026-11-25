/* Views, sheets and schedules: see view.h and view_io.h. */
#include "view_io.h"
#include <stdlib.h>
#include <string.h>
#include "cbor.h"
#include "command.h"
#include "guid.h"

static const char *const VKIND[] = {"three_d", "plan", "section"};
static const char *const LOOK[] = {"down", "up"};
static const char *const FACING[] = {"north", "south", "east", "west"};
static const char *const LEVEL[] = {"associated", "above", "below", "unlimited"};
static const char *const AXIS[] = {"aligned", "horizontal", "vertical"};
static const char *const ANN[] = {"dimension", "text"};
static const char *const FIELD[] = {"level",  "location", "name",   "class", "material", "section",  "length",
                                    "width",  "height",   "area",   "volume", "mass",    "global_id"};
static const char *const RANGE[] = {"top", "cut", "bottom", "depth"};
static const char *const PAPERS[] = {"A4", "A3", "A2", "A1", "A0"};
static const double PAPER_W[] = {297.0, 420.0, 594.0, 841.0, 1189.0};
static const double PAPER_H[] = {210.0, 297.0, 420.0, 594.0, 841.0};

const char *cc_view_kind_name(int k) { return (unsigned)k < 3 ? VKIND[k] : "?"; }
const char *cc_look_name(int k) { return (unsigned)k < 2 ? LOOK[k] : "?"; }
const char *cc_facing_name(int k) { return (unsigned)k < 4 ? FACING[k] : "?"; }
const char *cc_level_ref_name(int k) { return (unsigned)k < 4 ? LEVEL[k] : "?"; }
const char *cc_sched_field_name(int k) { return (unsigned)k < CC_FIELD_COUNT ? FIELD[k] : "?"; }

static int finite(double x) { return x - x == 0.0; }
static double fabs_(double x) { return x < 0 ? -x : x; }

static size_t chars(cc_str s) {
    size_t k = 0;
    for (size_t i = 0; i < s.n; i++)
        if (((unsigned char)s.p[i] & 0xC0) != 0x80) k++;
    return k;
}

static int str_eq(cc_str a, cc_str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
static void put(Buf *b, cc_str s) { buf_append(b, s.p, s.n); }

/* ================= decoding ================= */

void cc_vd_init(cc_vd *d, Arena *a) {
    d->a = a;
    buf_init(&d->msg);
    d->off = 0;
    d->failed = 0;
}

void cc_vd_free(cc_vd *d) { buf_free(&d->msg); }

static int vfail(cc_vd *d, size_t off) {
    d->failed = 1;
    d->off = off;
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
    case JR_NUM:
        buf_puts(&d->msg, v->num_int ? "integer `" : "floating point `");
        if (v->s) buf_append(&d->msg, v->s, v->n < 64 ? v->n : 64);
        else cc_buf_display_f64(&d->msg, v->num);
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
    return vfail(d, v->off);
}

static void *alloc_n(cc_vd *d, size_t n, size_t sz, size_t off) {
    if (n == 0) return NULL;
    if (n > ((size_t)-1) / sz) {
        vfail3(d, off, "out of memory", 0, 0);
        return NULL;
    }
    void *p = arena_alloc(d->a, n * sz, 16);
    if (!p) vfail3(d, off, "out of memory", 0, 0);
    return p;
}

/* Struct fields by name (deny_unknown_fields: every struct here), `skip` ignored. */
static int fields(cc_vd *d, const JrVal *obj, const char *const *names, int nn, const JrVal **out,
                  const char *what, const char *skip) {
    if (obj->type != JR_OBJ) return type_err(d, obj, what);
    for (int i = 0; i < nn; i++) out[i] = NULL;
    for (size_t j = 0; j < obj->n; j++) {
        const JrMember *m = &obj->members[j];
        if (skip && jr_key_is(m, skip)) continue;
        int i = 0;
        while (i < nn && !jr_key_is(m, names[i])) i++;
        if (i == nn) {
            buf_clear(&d->msg);
            buf_puts(&d->msg, "unknown field `");
            buf_append(&d->msg, m->key, m->key_n < 200 ? m->key_n : 200);
            if (nn == 0) buf_puts(&d->msg, "`, there are no fields");
            else buf_puts(&d->msg, nn == 1 ? "`, expected " : "`, expected one of ");
            for (int k = 0; k < nn; k++) {
                if (k) buf_puts(&d->msg, ", ");
                buf_putc(&d->msg, '`');
                buf_puts(&d->msg, names[k]);
                buf_putc(&d->msg, '`');
            }
            return vfail(d, m->val.off);
        }
        if (out[i]) return vfail3(d, m->val.off, "duplicate field `", names[i], "`");
        out[i] = &m->val;
    }
    return 0;
}

static int need(cc_vd *d, const JrVal *obj, const JrVal *f, const char *name) {
    return f ? 0 : vfail3(d, obj->off, "missing field `", name, "`");
}

static int d_f64(cc_vd *d, const JrVal *v, double *out) {
    if (v->type != JR_NUM) return type_err(d, v, "f64");
    if (!finite(v->num)) return vfail3(d, v->off, "number out of range", 0, 0);
    *out = v->num;
    return 0;
}

static int d_u32(cc_vd *d, const JrVal *v, uint32_t *out) {
    if (v->type != JR_NUM || !v->num_int) return type_err(d, v, "u32");
    if (v->num_neg || v->num > 4294967295.0) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "invalid value: integer `");
        if (v->s) buf_append(&d->msg, v->s, v->n < 64 ? v->n : 64);
        else cc_buf_display_f64(&d->msg, v->num);
        buf_puts(&d->msg, "`, expected u32");
        return vfail(d, v->off);
    }
    *out = (uint32_t)v->num;
    return 0;
}

static int d_bool(cc_vd *d, const JrVal *v, int *out) {
    if (v->type != JR_BOOL) return type_err(d, v, "a boolean");
    *out = v->b ? 1 : 0;
    return 0;
}

static int d_str(cc_vd *d, const JrVal *v, cc_str *out) {
    if (v->type != JR_STR) return type_err(d, v, "a string");
    char *p = v->n ? arena_dup(d->a, v->s, v->n) : NULL;
    if (v->n && !p) return vfail3(d, v->off, "out of memory", 0, 0);
    out->p = p;
    out->n = v->n;
    return 0;
}

/* Option<String>: absent or null is None. */
static int d_opt_str(cc_vd *d, const JrVal *v, int *has, cc_str *out) {
    *has = 0;
    if (!v || v->type == JR_NULL) return 0;
    if (d_str(d, v, out)) return -1;
    *has = 1;
    return 0;
}

static int d_gid(cc_vd *d, const JrVal *v, cc_gid *out) {
    if (v->type != JR_STR) return type_err(d, v, "a string");
    if (!cc_gid_valid(v->s, v->n)) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "invalid IFC GlobalId: ");
        cc_buf_debug_str(&d->msg, v->s, v->n < 200 ? v->n : 200);
        return vfail(d, v->off);
    }
    memcpy(out->c, v->s, 22);
    return 0;
}

static int d_vec(cc_vd *d, const JrVal *v, double *out, size_t n) {
    if (v->type != JR_ARR) return type_err(d, v, n == 2 ? "an array of length 2" : "an array of length 3");
    if (v->n != n) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "invalid length ");
        buf_u64(&d->msg, v->n);
        buf_puts(&d->msg, n == 2 ? ", expected an array of length 2" : ", expected an array of length 3");
        return vfail(d, v->off);
    }
    for (size_t i = 0; i < n; i++)
        if (d_f64(d, &v->items[i], &out[i])) return -1;
    return 0;
}

static int d_enum(cc_vd *d, const JrVal *v, const char *const *names, int count, int *out) {
    if (v->type != JR_STR) return type_err(d, v, "variant identifier");
    for (int k = 0; k < count; k++)
        if (strlen(names[k]) == v->n && memcmp(names[k], v->s, v->n) == 0) {
            *out = k;
            return 0;
        }
    buf_clear(&d->msg);
    buf_puts(&d->msg, "unknown variant `");
    buf_append(&d->msg, v->s, v->n < 200 ? v->n : 200);
    buf_puts(&d->msg, count == 1 ? "`, expected " : "`, expected one of ");
    for (int k = 0; k < count; k++) {
        if (k) buf_puts(&d->msg, ", ");
        buf_putc(&d->msg, '`');
        buf_puts(&d->msg, names[k]);
        buf_putc(&d->msg, '`');
    }
    return vfail(d, v->off);
}

static int d_strs(cc_vd *d, const JrVal *v, cc_strs *out) {
    out->v = NULL;
    out->n = 0;
    if (v->type != JR_ARR) return type_err(d, v, "a sequence");
    out->v = alloc_n(d, v->n, sizeof(cc_str), v->off);
    if (v->n && !out->v) return -1;
    out->n = v->n;
    for (size_t i = 0; i < v->n; i++)
        if (d_str(d, &v->items[i], &out->v[i])) return -1;
    return 0;
}

static int d_points(cc_vd *d, const JrVal *v, cc_loop *out) {
    if (v->type != JR_ARR) return type_err(d, v, "a sequence");
    out->n = v->n;
    out->pts = alloc_n(d, v->n, sizeof *out->pts, v->off);
    if (v->n && !out->pts) return -1;
    for (size_t i = 0; i < v->n; i++)
        if (d_vec(d, &v->items[i], out->pts[i], 2)) return -1;
    return 0;
}

/* The tag of an internally tagged enum, by name. */
static const JrVal *tag_of(cc_vd *d, const JrVal *v, const char *tag, const char *what) {
    if (v->type != JR_OBJ) {
        type_err(d, v, what);
        return NULL;
    }
    const JrVal *t = NULL;
    for (size_t j = 0; j < v->n; j++)
        if (jr_key_is(&v->members[j], tag)) {
            if (t) {
                vfail3(d, v->members[j].val.off, "duplicate field `", tag, "`");
                return NULL;
            }
            t = &v->members[j].val;
        }
    if (!t) vfail3(d, v->off, "missing field `", tag, "`");
    return t;
}

/* BTreeMap<String, String>: sorted by key bytes, the last of equal keys kept. */
typedef struct {
    const JrMember *m;
    size_t i;
} KEnt;

static int kent_cmp(const void *x, const void *y) {
    const KEnt *a = x, *b = y;
    size_t n = a->m->key_n < b->m->key_n ? a->m->key_n : b->m->key_n;
    int c = n ? memcmp(a->m->key, b->m->key, n) : 0;
    if (c) return c;
    if (a->m->key_n != b->m->key_n) return a->m->key_n < b->m->key_n ? -1 : 1;
    return a->i < b->i ? -1 : a->i > b->i;
}

static int d_strmap(cc_vd *d, const JrVal *v, cc_kv **out, size_t *n) {
    *out = NULL;
    *n = 0;
    if (v->type != JR_OBJ) return type_err(d, v, "a map");
    if (!v->n) return 0;
    for (size_t i = 0; i < v->n; i++)
        if (v->members[i].val.type != JR_STR) return type_err(d, &v->members[i].val, "a string");
    KEnt *e = malloc(v->n * sizeof *e);
    if (!e) return vfail3(d, v->off, "out of memory", 0, 0);
    for (size_t i = 0; i < v->n; i++) e[i].m = &v->members[i], e[i].i = i;
    qsort(e, v->n, sizeof *e, kent_cmp);
    size_t c = 0;
    for (size_t i = 0; i < v->n; i++) {
        if (c && e[c - 1].m->key_n == e[i].m->key_n && !memcmp(e[c - 1].m->key, e[i].m->key, e[i].m->key_n))
            e[c - 1] = e[i];
        else e[c++] = e[i];
    }
    cc_kv *kv = alloc_n(d, c, sizeof *kv, v->off);
    int rc = kv ? 0 : -1;
    for (size_t i = 0; !rc && i < c; i++) {
        const JrMember *m = e[i].m;
        kv[i].key.n = m->key_n;
        kv[i].key.p = m->key_n ? arena_dup(d->a, m->key, m->key_n) : NULL;
        if (m->key_n && !kv[i].key.p) rc = vfail3(d, v->off, "out of memory", 0, 0);
        else rc = d_str(d, &m->val, &kv[i].value);
    }
    free(e);
    if (rc) return -1;
    *out = kv;
    *n = c;
    return 0;
}

static int d_range_plane(cc_vd *d, const JrVal *v, cc_range_plane *p) {
    static const char *const F[] = {"level", "offset_mm"};
    const JrVal *f[2];
    int lv = 0;
    if (fields(d, v, F, 2, f, "struct RangePlane", NULL) || need(d, v, f[0], F[0]) || need(d, v, f[1], F[1]) ||
        d_enum(d, f[0], LEVEL, 4, &lv) || d_f64(d, f[1], &p->offset_mm))
        return -1;
    p->level = (cc_level_ref)lv;
    return 0;
}

static int d_kind(cc_vd *d, const JrVal *v, cc_view *out) {
    const JrVal *t = tag_of(d, v, "type", "internally tagged enum ViewKind");
    int k = 0;
    if (!t || d_enum(d, t, VKIND, 3, &k)) return -1;
    out->kind = (cc_view_kind)k;
    if (k == CC_VIEW_THREE_D) return fields(d, v, NULL, 0, NULL, "unit variant", "type");
    if (k == CC_VIEW_PLAN) {
        static const char *const F[] = {"storey", "look", "range"};
        const JrVal *f[3], *r[4];
        int look = 0;
        if (fields(d, v, F, 3, f, "struct variant ViewKind::Plan", "type")) return -1;
        for (int i = 0; i < 3; i++)
            if (need(d, v, f[i], F[i])) return -1;
        if (d_str(d, f[0], &out->storey) || d_enum(d, f[1], LOOK, 2, &look)) return -1;
        out->look = (cc_look)look;
        if (fields(d, f[2], RANGE, 4, r, "struct ViewRange", NULL)) return -1;
        for (int i = 0; i < 4; i++)
            if (need(d, f[2], r[i], RANGE[i]) || d_range_plane(d, r[i], &out->range[i])) return -1;
        return 0;
    }
    static const char *const F[] = {"origin", "facing", "depth_mm"};
    const JrVal *f[3];
    int facing = 0;
    if (fields(d, v, F, 3, f, "struct variant ViewKind::Section", "type")) return -1;
    for (int i = 0; i < 3; i++)
        if (need(d, v, f[i], F[i])) return -1;
    if (d_vec(d, f[0], out->origin, 2) || d_enum(d, f[1], FACING, 4, &facing) || d_f64(d, f[2], &out->depth_mm))
        return -1;
    out->facing = (cc_facing)facing;
    return 0;
}

static int d_visibility(cc_vd *d, const JrVal *v, cc_visibility *out) {
    static const char *const F[] = {"hidden_families", "hidden_classes",  "hidden_storeys",   "hidden_links",
                                    "hidden_materials", "hidden_elements", "isolated_elements"};
    const JrVal *f[7];
    cc_strs *dst[7] = {&out->hidden_families,  &out->hidden_classes,  &out->hidden_storeys,  &out->hidden_links,
                       &out->hidden_materials, &out->hidden_elements, &out->isolated_elements};
    if (fields(d, v, F, 7, f, "struct Visibility", NULL)) return -1;
    for (int i = 0; i < 7; i++)
        if (f[i] && d_strs(d, f[i], dst[i])) return -1;
    return 0;
}

static int d_anchor(cc_vd *d, const JrVal *v, cc_anchor *a) {
    static const char *const F[] = {"at", "element"};
    const JrVal *f[2];
    if (fields(d, v, F, 2, f, "struct Anchor", NULL) || need(d, v, f[0], F[0]) || d_vec(d, f[0], a->at, 3))
        return -1;
    a->has_element = 0;
    if (f[1] && f[1]->type != JR_NULL) {
        if (d_gid(d, f[1], &a->element)) return -1;
        a->has_element = 1;
    }
    return 0;
}

static int d_annotation(cc_vd *d, const JrVal *v, cc_annotation *a) {
    const JrVal *t = tag_of(d, v, "type", "internally tagged enum Annotation");
    int k = 0;
    if (!t || d_enum(d, t, ANN, 2, &k)) return -1;
    a->type = (cc_ann_type)k;
    if (k == CC_ANN_DIMENSION) {
        static const char *const F[] = {"id", "a", "b", "offset_mm", "axis", "kind"};
        const JrVal *f[6];
        int axis = 0;
        if (fields(d, v, F, 6, f, "struct variant Annotation::Dimension", "type")) return -1;
        for (int i = 0; i < 4; i++)
            if (need(d, v, f[i], F[i])) return -1;
        if (d_gid(d, f[0], &a->id) || d_anchor(d, f[1], &a->a) || d_anchor(d, f[2], &a->b) ||
            d_f64(d, f[3], &a->offset_mm) || (f[4] && d_enum(d, f[4], AXIS, 3, &axis)))
            return -1;
        a->axis = (cc_dim_axis)axis;
        return d_opt_str(d, f[5], &a->has_kind, &a->kind);
    }
    static const char *const F[] = {"id", "text", "at", "size_mm", "angle_deg", "kind"};
    const JrVal *f[6];
    if (fields(d, v, F, 6, f, "struct variant Annotation::Text", "type")) return -1;
    for (int i = 0; i < 4; i++)
        if (need(d, v, f[i], F[i])) return -1;
    if (d_gid(d, f[0], &a->id) || d_str(d, f[1], &a->text) || d_vec(d, f[2], a->at, 3) ||
        d_f64(d, f[3], &a->size_mm) || (f[4] && d_f64(d, f[4], &a->angle_deg)))
        return -1;
    return d_opt_str(d, f[5], &a->has_kind, &a->kind);
}

static int d_reference(cc_vd *d, const JrVal *v, cc_reference *r) {
    static const char *const F[] = {"id", "name", "format", "source", "offset_mm", "rotation_deg", "visible", "polylines"};
    const JrVal *f[8];
    if (fields(d, v, F, 8, f, "struct Reference", NULL)) return -1;
    for (int i = 0; i < 8; i++)
        if (i != 3 && i != 6 && need(d, v, f[i], F[i])) return -1;
    if (d_gid(d, f[0], &r->id) || d_str(d, f[1], &r->name) || d_str(d, f[2], &r->format) ||
        d_opt_str(d, f[3], &r->has_source, &r->source) || d_vec(d, f[4], r->offset_mm, 2) ||
        d_f64(d, f[5], &r->rotation_deg))
        return -1;
    r->visible = 1;
    if (f[6] && d_bool(d, f[6], &r->visible)) return -1;
    const JrVal *p = f[7];
    if (p->type != JR_ARR) return type_err(d, p, "a sequence");
    r->polylines = alloc_n(d, p->n, sizeof(cc_loop), p->off);
    if (p->n && !r->polylines) return -1;
    r->npolylines = p->n;
    for (size_t i = 0; i < p->n; i++)
        if (d_points(d, &p->items[i], &r->polylines[i])) return -1;
    return 0;
}

int cc_vd_view(cc_vd *d, const JrVal *v, const char *skip, cc_view *out) {
    static const char *const F[] = {"id", "name", "kind", "visibility", "references", "annotations",
                                    "true_north", "scale", "crop"};
    const JrVal *f[9];
    memset(out, 0, sizeof *out);
    out->scale = 100;
    if (fields(d, v, F, 9, f, "struct View", skip)) return -1;
    for (int i = 0; i < 3; i++)
        if (need(d, v, f[i], F[i])) return -1;
    if (d_gid(d, f[0], &out->id) || d_str(d, f[1], &out->name) || d_kind(d, f[2], out)) return -1;
    if (f[3] && d_visibility(d, f[3], &out->visibility)) return -1;
    if (f[4]) {
        if (f[4]->type != JR_ARR) return type_err(d, f[4], "a sequence");
        out->references = alloc_n(d, f[4]->n, sizeof(cc_reference), f[4]->off);
        if (f[4]->n && !out->references) return -1;
        out->nreferences = f[4]->n;
        for (size_t i = 0; i < f[4]->n; i++)
            if (d_reference(d, &f[4]->items[i], &out->references[i])) return -1;
    }
    if (f[5]) {
        if (f[5]->type != JR_ARR) return type_err(d, f[5], "a sequence");
        out->annotations = alloc_n(d, f[5]->n, sizeof(cc_annotation), f[5]->off);
        if (f[5]->n && !out->annotations) return -1;
        out->nannotations = f[5]->n;
        for (size_t i = 0; i < f[5]->n; i++)
            if (d_annotation(d, &f[5]->items[i], &out->annotations[i])) return -1;
    }
    if (f[6] && d_bool(d, f[6], &out->true_north)) return -1;
    if (f[7] && d_u32(d, f[7], &out->scale)) return -1;
    if (f[8] && f[8]->type != JR_NULL) {
        static const char *const C[] = {"min", "max"};
        const JrVal *c[2];
        if (fields(d, f[8], C, 2, c, "struct Crop", NULL) || need(d, f[8], c[0], C[0]) ||
            need(d, f[8], c[1], C[1]) || d_vec(d, c[0], out->crop_min, 2) || d_vec(d, c[1], out->crop_max, 2))
            return -1;
        out->has_crop = 1;
    }
    return 0;
}

int cc_vd_sheet(cc_vd *d, const JrVal *v, const char *skip, cc_sheet *out) {
    static const char *const F[] = {"id", "number", "name", "paper", "portrait", "fields", "placed", "texts"};
    const JrVal *f[8];
    memset(out, 0, sizeof *out);
    if (fields(d, v, F, 8, f, "struct Sheet", skip)) return -1;
    for (int i = 0; i < 4; i++)
        if (need(d, v, f[i], F[i])) return -1;
    if (d_gid(d, f[0], &out->id) || d_str(d, f[1], &out->number) || d_str(d, f[2], &out->name) ||
        d_str(d, f[3], &out->paper))
        return -1;
    if (f[4] && d_bool(d, f[4], &out->portrait)) return -1;
    if (f[5] && d_strmap(d, f[5], &out->fields, &out->nfields)) return -1;
    if (f[6]) {
        static const char *const P[] = {"id", "view", "scale", "at_mm"};
        if (f[6]->type != JR_ARR) return type_err(d, f[6], "a sequence");
        out->placed = alloc_n(d, f[6]->n, sizeof(cc_placed), f[6]->off);
        if (f[6]->n && !out->placed) return -1;
        out->nplaced = f[6]->n;
        for (size_t i = 0; i < f[6]->n; i++) {
            const JrVal *x = &f[6]->items[i], *p[4];
            cc_placed *pl = &out->placed[i];
            pl->scale = 100;
            if (fields(d, x, P, 4, p, "struct Placed", NULL) || need(d, x, p[0], P[0]) || need(d, x, p[1], P[1]) ||
                need(d, x, p[3], P[3]) || d_gid(d, p[0], &pl->id) || d_gid(d, p[1], &pl->view) ||
                (p[2] && d_u32(d, p[2], &pl->scale)) || d_vec(d, p[3], pl->at_mm, 2))
                return -1;
        }
    }
    if (f[7]) {
        static const char *const T[] = {"id", "text", "at_mm", "size_mm", "angle_deg", "kind"};
        if (f[7]->type != JR_ARR) return type_err(d, f[7], "a sequence");
        out->texts = alloc_n(d, f[7]->n, sizeof(cc_sheet_text), f[7]->off);
        if (f[7]->n && !out->texts) return -1;
        out->ntexts = f[7]->n;
        for (size_t i = 0; i < f[7]->n; i++) {
            const JrVal *x = &f[7]->items[i], *t[6];
            cc_sheet_text *st = &out->texts[i];
            if (fields(d, x, T, 6, t, "struct SheetText", NULL)) return -1;
            for (int k = 0; k < 4; k++)
                if (need(d, x, t[k], T[k])) return -1;
            if (d_gid(d, t[0], &st->id) || d_str(d, t[1], &st->text) || d_vec(d, t[2], st->at_mm, 2) ||
                d_f64(d, t[3], &st->size_mm) || (t[4] && d_f64(d, t[4], &st->angle_deg)) ||
                d_opt_str(d, t[5], &st->has_kind, &st->kind))
                return -1;
        }
    }
    return 0;
}

static const char *const CATS[] = {"concrete", "steel", "timber", "masonry", "insulation", "earth", "glass", "other"};

static int d_schedule(cc_vd *d, const JrVal *v, cc_schedule *s) {
    static const char *const F[] = {"name", "classes", "family", "fields", "group_by_level", "totals"};
    const JrVal *f[6];
    memset(s, 0, sizeof *s);
    if (fields(d, v, F, 6, f, "struct ScheduleDef", NULL)) return -1;
    if (need(d, v, f[0], F[0]) || need(d, v, f[1], F[1]) || need(d, v, f[3], F[3])) return -1;
    if (d_str(d, f[0], &s->name) || d_strs(d, f[1], &s->classes)) return -1;
    if (f[2] && f[2]->type != JR_NULL) {
        if (d_enum(d, f[2], CATS, 8, &s->family)) return -1;
        s->has_family = 1;
    }
    const JrVal *fl = f[3];
    if (fl->type != JR_ARR) return type_err(d, fl, "a sequence");
    s->fields = alloc_n(d, fl->n, 1, fl->off);
    if (fl->n && !s->fields) return -1;
    s->nfields = fl->n;
    for (size_t i = 0; i < fl->n; i++) {
        int k = 0;
        if (d_enum(d, &fl->items[i], FIELD, CC_FIELD_COUNT, &k)) return -1;
        s->fields[i] = (uint8_t)k;
    }
    if (f[4] && d_bool(d, f[4], &s->group_by_level)) return -1;
    if (f[5] && d_bool(d, f[5], &s->totals)) return -1;
    return 0;
}

int cc_vd_schedules(cc_vd *d, const JrVal *v, cc_schedule **out, size_t *n) {
    *out = NULL;
    *n = 0;
    if (v->type != JR_ARR) return type_err(d, v, "a sequence");
    cc_schedule *s = alloc_n(d, v->n, sizeof *s, v->off);
    if (v->n && !s) return -1;
    for (size_t i = 0; i < v->n; i++)
        if (d_schedule(d, &v->items[i], &s[i])) return -1;
    *out = s;
    *n = v->n;
    return 0;
}

/* ================= writing ================= */

static void e_map(cc_em *e, size_t n) {
    if (e->j) jw_obj_begin(e->j);
    else cb_head(e->c, CB_MAP, n);
}
static void e_map_end(cc_em *e) {
    if (e->j) jw_obj_end(e->j);
}
static void e_arr(cc_em *e, size_t n) {
    if (e->j) jw_arr_begin(e->j);
    else cb_head(e->c, CB_ARRAY, n);
}
static void e_arr_end(cc_em *e) {
    if (e->j) jw_arr_end(e->j);
}
static void e_key(cc_em *e, const char *k) {
    if (e->j) jw_key(e->j, k);
    else cb_text(e->c, k, strlen(k));
}
static void e_cstr(cc_em *e, const char *s) {
    if (e->j) jw_str(e->j, s);
    else cb_text(e->c, s, strlen(s));
}
static void e_str(cc_em *e, cc_str s) {
    if (e->j) jw_str_n(e->j, s.n ? s.p : "", s.n);
    else cb_text(e->c, s.n ? s.p : "", s.n);
}
static void e_gid(cc_em *e, const cc_gid *g) {
    if (e->j) jw_str_n(e->j, g->c, 22);
    else cb_text(e->c, g->c, 22);
}
static void e_f64(cc_em *e, double x) {
    if (e->j) jw_f64(e->j, x);
    else cb_f64(e->c, x);
}
static void e_u64(cc_em *e, uint64_t x) {
    if (e->j) jw_u64(e->j, x);
    else cb_head(e->c, CB_UINT, x);
}
static void e_bool(cc_em *e, int b) {
    if (e->j) jw_bool(e->j, b);
    else buf_putc(e->c, (char)(b ? 0xF5 : 0xF4));
}
static void e_vec(cc_em *e, const double *v, int n) {
    e_arr(e, (size_t)n);
    for (int i = 0; i < n; i++) e_f64(e, v[i]);
    e_arr_end(e);
}
static void e_strs(cc_em *e, const cc_strs *s) {
    e_arr(e, s->n);
    for (size_t i = 0; i < s->n; i++) e_str(e, s->v[i]);
    e_arr_end(e);
}
static void e_kvs(cc_em *e, const cc_kv *kv, size_t n) {
    e_map(e, n);
    for (size_t i = 0; i < n; i++) {
        if (e->j) jw_key_n(e->j, kv[i].key.n ? kv[i].key.p : "", kv[i].key.n);
        else cb_text(e->c, kv[i].key.n ? kv[i].key.p : "", kv[i].key.n);
        e_str(e, kv[i].value);
    }
    e_map_end(e);
}

static int vis_empty(const cc_visibility *v) {
    return !(v->hidden_families.n || v->hidden_classes.n || v->hidden_storeys.n || v->hidden_links.n ||
             v->hidden_materials.n || v->hidden_elements.n || v->isolated_elements.n);
}

static void e_visibility(cc_em *e, const cc_visibility *v) {
    static const char *const F[] = {"hidden_families", "hidden_classes",  "hidden_storeys",   "hidden_links",
                                    "hidden_materials", "hidden_elements", "isolated_elements"};
    const cc_strs *s[7] = {&v->hidden_families,  &v->hidden_classes,  &v->hidden_storeys,  &v->hidden_links,
                           &v->hidden_materials, &v->hidden_elements, &v->isolated_elements};
    size_t n = 0;
    for (int i = 0; i < 7; i++) n += s[i]->n != 0;
    e_map(e, n);
    for (int i = 0; i < 7; i++)
        if (s[i]->n) e_key(e, F[i]), e_strs(e, s[i]);
    e_map_end(e);
}

static void e_anchor(cc_em *e, const cc_anchor *a) {
    e_map(e, 1 + (size_t)(a->has_element != 0));
    e_key(e, "at"), e_vec(e, a->at, 3);
    if (a->has_element) e_key(e, "element"), e_gid(e, &a->element);
    e_map_end(e);
}

static void e_annotation(cc_em *e, const cc_annotation *a) {
    if (a->type == CC_ANN_DIMENSION) {
        e_map(e, 5 + (size_t)(a->axis != CC_AXIS_ALIGNED) + (size_t)(a->has_kind != 0));
        e_key(e, "type"), e_cstr(e, "dimension");
        e_key(e, "id"), e_gid(e, &a->id);
        e_key(e, "a"), e_anchor(e, &a->a);
        e_key(e, "b"), e_anchor(e, &a->b);
        e_key(e, "offset_mm"), e_f64(e, a->offset_mm);
        if (a->axis != CC_AXIS_ALIGNED) e_key(e, "axis"), e_cstr(e, AXIS[a->axis]);
    } else {
        e_map(e, 5 + (size_t)(a->angle_deg != 0.0) + (size_t)(a->has_kind != 0));
        e_key(e, "type"), e_cstr(e, "text");
        e_key(e, "id"), e_gid(e, &a->id);
        e_key(e, "text"), e_str(e, a->text);
        e_key(e, "at"), e_vec(e, a->at, 3);
        e_key(e, "size_mm"), e_f64(e, a->size_mm);
        if (a->angle_deg != 0.0) e_key(e, "angle_deg"), e_f64(e, a->angle_deg);
    }
    if (a->has_kind) e_key(e, "kind"), e_str(e, a->kind);
    e_map_end(e);
}

static void e_reference(cc_em *e, const cc_reference *r) {
    e_map(e, 7 + (size_t)(r->has_source != 0));
    e_key(e, "id"), e_gid(e, &r->id);
    e_key(e, "name"), e_str(e, r->name);
    e_key(e, "format"), e_str(e, r->format);
    if (r->has_source) e_key(e, "source"), e_str(e, r->source);
    e_key(e, "offset_mm"), e_vec(e, r->offset_mm, 2);
    e_key(e, "rotation_deg"), e_f64(e, r->rotation_deg);
    e_key(e, "visible"), e_bool(e, r->visible);
    e_key(e, "polylines");
    e_arr(e, r->npolylines);
    for (size_t i = 0; i < r->npolylines; i++) {
        const cc_loop *l = &r->polylines[i];
        e_arr(e, l->n);
        for (size_t k = 0; k < l->n; k++) e_vec(e, l->pts[k], 2);
        e_arr_end(e);
    }
    e_arr_end(e);
    e_map_end(e);
}

static void e_kind(cc_em *e, const cc_view *v) {
    switch (v->kind) {
    case CC_VIEW_PLAN:
        e_map(e, 4);
        e_key(e, "type"), e_cstr(e, "plan");
        e_key(e, "storey"), e_str(e, v->storey);
        e_key(e, "look"), e_cstr(e, LOOK[v->look]);
        e_key(e, "range");
        e_map(e, 4);
        for (int i = 0; i < 4; i++) {
            e_key(e, RANGE[i]);
            e_map(e, 2);
            e_key(e, "level"), e_cstr(e, LEVEL[v->range[i].level]);
            e_key(e, "offset_mm"), e_f64(e, v->range[i].offset_mm);
            e_map_end(e);
        }
        e_map_end(e);
        break;
    case CC_VIEW_SECTION:
        e_map(e, 4);
        e_key(e, "type"), e_cstr(e, "section");
        e_key(e, "origin"), e_vec(e, v->origin, 2);
        e_key(e, "facing"), e_cstr(e, FACING[v->facing]);
        e_key(e, "depth_mm"), e_f64(e, v->depth_mm);
        break;
    default:
        e_map(e, 1);
        e_key(e, "type"), e_cstr(e, "three_d");
        break;
    }
    e_map_end(e);
}

void cc_em_view(cc_em *e, const cc_view *v, const char *op) {
    size_t n = 3 + (size_t)!vis_empty(&v->visibility) + (size_t)(v->nreferences != 0) +
               (size_t)(v->nannotations != 0) + (size_t)(v->true_north != 0) + (size_t)(v->scale != 100) +
               (size_t)(v->has_crop != 0);
    e_map(e, n + (op ? 1 : 0));
    if (op) e_key(e, "op"), e_cstr(e, op);
    e_key(e, "id"), e_gid(e, &v->id);
    e_key(e, "name"), e_str(e, v->name);
    e_key(e, "kind"), e_kind(e, v);
    if (!vis_empty(&v->visibility)) e_key(e, "visibility"), e_visibility(e, &v->visibility);
    if (v->nreferences) {
        e_key(e, "references");
        e_arr(e, v->nreferences);
        for (size_t i = 0; i < v->nreferences; i++) e_reference(e, &v->references[i]);
        e_arr_end(e);
    }
    if (v->nannotations) {
        e_key(e, "annotations");
        e_arr(e, v->nannotations);
        for (size_t i = 0; i < v->nannotations; i++) e_annotation(e, &v->annotations[i]);
        e_arr_end(e);
    }
    if (v->true_north) e_key(e, "true_north"), e_bool(e, 1);
    if (v->scale != 100) e_key(e, "scale"), e_u64(e, v->scale);
    if (v->has_crop) {
        e_key(e, "crop");
        e_map(e, 2);
        e_key(e, "min"), e_vec(e, v->crop_min, 2);
        e_key(e, "max"), e_vec(e, v->crop_max, 2);
        e_map_end(e);
    }
    e_map_end(e);
}

void cc_em_sheet(cc_em *e, const cc_sheet *s, const char *op) {
    size_t n = 4 + (size_t)(s->portrait != 0) + (size_t)(s->nfields != 0) + (size_t)(s->nplaced != 0) +
               (size_t)(s->ntexts != 0);
    e_map(e, n + (op ? 1 : 0));
    if (op) e_key(e, "op"), e_cstr(e, op);
    e_key(e, "id"), e_gid(e, &s->id);
    e_key(e, "number"), e_str(e, s->number);
    e_key(e, "name"), e_str(e, s->name);
    e_key(e, "paper"), e_str(e, s->paper);
    if (s->portrait) e_key(e, "portrait"), e_bool(e, 1);
    if (s->nfields) e_key(e, "fields"), e_kvs(e, s->fields, s->nfields);
    if (s->nplaced) {
        e_key(e, "placed");
        e_arr(e, s->nplaced);
        for (size_t i = 0; i < s->nplaced; i++) {
            const cc_placed *p = &s->placed[i];
            e_map(e, 4);
            e_key(e, "id"), e_gid(e, &p->id);
            e_key(e, "view"), e_gid(e, &p->view);
            e_key(e, "scale"), e_u64(e, p->scale);
            e_key(e, "at_mm"), e_vec(e, p->at_mm, 2);
            e_map_end(e);
        }
        e_arr_end(e);
    }
    if (s->ntexts) {
        e_key(e, "texts");
        e_arr(e, s->ntexts);
        for (size_t i = 0; i < s->ntexts; i++) {
            const cc_sheet_text *t = &s->texts[i];
            e_map(e, 4 + (size_t)(t->angle_deg != 0.0) + (size_t)(t->has_kind != 0));
            e_key(e, "id"), e_gid(e, &t->id);
            e_key(e, "text"), e_str(e, t->text);
            e_key(e, "at_mm"), e_vec(e, t->at_mm, 2);
            e_key(e, "size_mm"), e_f64(e, t->size_mm);
            if (t->angle_deg != 0.0) e_key(e, "angle_deg"), e_f64(e, t->angle_deg);
            if (t->has_kind) e_key(e, "kind"), e_str(e, t->kind);
            e_map_end(e);
        }
        e_arr_end(e);
    }
    e_map_end(e);
}

void cc_em_schedules(cc_em *e, const cc_schedule *s, size_t n) {
    e_arr(e, n);
    for (size_t i = 0; i < n; i++) {
        const cc_schedule *d = &s[i];
        e_map(e, 5 + (size_t)(d->has_family != 0));
        e_key(e, "name"), e_str(e, d->name);
        e_key(e, "classes"), e_strs(e, &d->classes);
        if (d->has_family) e_key(e, "family"), e_cstr(e, CATS[d->family]);
        e_key(e, "fields");
        e_arr(e, d->nfields);
        for (size_t k = 0; k < d->nfields; k++) e_cstr(e, FIELD[d->fields[k]]);
        e_arr_end(e);
        e_key(e, "group_by_level"), e_bool(e, d->group_by_level);
        e_key(e, "totals"), e_bool(e, d->totals);
        e_map_end(e);
    }
    e_arr_end(e);
}

/* ================= lookups ================= */

#define BSEARCH(T, arr, n, id)                              \
    do {                                                    \
        size_t lo = 0, hi = (n);                            \
        while (lo < hi) {                                   \
            size_t mid = lo + (hi - lo) / 2;                \
            int c = cc_gid_cmp(&(arr)[mid].id, (id));       \
            if (c == 0) return &(arr)[mid];                 \
            if (c < 0) lo = mid + 1;                        \
            else hi = mid;                                  \
        }                                                   \
        return (const T *)0;                                \
    } while (0)

const cc_view *cc_model_view(const cc_model *m, const cc_gid *id) { BSEARCH(cc_view, m->views, m->nviews, id); }
const cc_sheet *cc_model_sheet(const cc_model *m, const cc_gid *id) { BSEARCH(cc_sheet, m->sheets, m->nsheets, id); }

static cc_gid derived(const char *pre, const cc_gid *storey) {
    char seed[64];
    size_t n = strlen(pre);
    memcpy(seed, pre, n);
    if (storey) memcpy(seed + n, storey->c, 22), n += 22;
    cc_guid g = cc_guid_derived(seed, n);
    cc_gid out;
    memcpy(out.c, g.c, 22);
    return out;
}

int cc_view_is_default(const cc_model *m, const cc_gid *id) {
    cc_gid g = derived("bimble view 3d", NULL);
    if (!cc_gid_cmp(&g, id)) return 1;
    for (size_t i = 0; i < m->nstoreys; i++) {
        g = derived("bimble view plan down ", &m->storeys[i].id);
        if (!cc_gid_cmp(&g, id)) return 1;
        g = derived("bimble view plan up ", &m->storeys[i].id);
        if (!cc_gid_cmp(&g, id)) return 1;
    }
    return 0;
}

int cc_view_exists(const cc_model *m, const cc_gid *id) {
    return cc_model_view(m, id) != NULL || cc_view_is_default(m, id);
}

/* ================= validation ================= */

/* View ranges (view.rs resolve): the storeys sorted by elevation (stable: id order on ties). */
static int resolve(const cc_model *m, const cc_view *v, Buf *why) {
    size_t n = m->nstoreys;
    size_t *ord = n ? malloc(n * sizeof *ord) : NULL;
    if (n && !ord) {
        buf_puts(why, "out of memory");
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        size_t j = i;
        while (j && m->storeys[ord[j - 1]].elevation > m->storeys[i].elevation) ord[j] = ord[j - 1], j--;
        ord[j] = i;
    }
    size_t at = n;
    for (size_t i = 0; i < n; i++)
        if (v->storey.n == 22 && !memcmp(m->storeys[ord[i]].id.c, v->storey.p, 22)) {
            at = i;
            break;
        }
    if (at == n) {
        free(ord);
        buf_puts(why, "unknown storey ");
        put(why, v->storey);
        return -1;
    }
    double here = m->storeys[ord[at]].elevation;
    volatile double big = 1e308;
    const double INF = big * 10.0;
    double z[4];
    for (int k = 0; k < 4; k++) {
        double unlimited = k == CC_RANGE_TOP || k == CC_RANGE_CUT ? INF
                           : k == CC_RANGE_BOTTOM                 ? -INF
                           : v->look == CC_LOOK_DOWN              ? -INF
                                                                  : INF;
        const cc_range_plane *p = &v->range[k];
        int has = 1;
        double base = here;
        if (p->level == CC_LEVEL_ABOVE) {
            has = 0;
            for (size_t i = at + 1; i < n; i++)
                if (m->storeys[ord[i]].elevation > here + 1.0) {
                    base = m->storeys[ord[i]].elevation, has = 1;
                    break;
                }
        } else if (p->level == CC_LEVEL_BELOW) {
            for (size_t i = at; i-- > 0;)
                if (m->storeys[ord[i]].elevation < here - 1.0) {
                    base = m->storeys[ord[i]].elevation;
                    break;
                }
        } else if (p->level == CC_LEVEL_UNLIMITED) has = 0;
        z[k] = has ? base + p->offset_mm : unlimited;
    }
    free(ord);
    for (int k = 0; k < 4; k++)
        if (!finite(v->range[k].offset_mm) || fabs_(v->range[k].offset_mm) > 1.0e6) {
            buf_puts(why, "offsets must be numbers within ±1000 m");
            return -1;
        }
    if (v->range[CC_RANGE_CUT].level == CC_LEVEL_UNLIMITED) {
        buf_puts(why, "the cut plane needs a level");
        return -1;
    }
    double top = z[0], cut = z[1], bottom = z[2], depth = z[3];
    int ok = v->look == CC_LOOK_DOWN ? depth <= bottom && bottom <= cut && cut <= top
                                     : bottom <= cut && cut <= top && top <= depth;
    if (!ok) {
        buf_puts(why, v->look == CC_LOOK_DOWN
                          ? "looking down, the planes must be in order: view depth ≤ bottom ≤ cut ≤ top"
                          : "looking up, the planes must be in order: bottom ≤ cut ≤ top ≤ view depth");
        return -1;
    }
    return 0;
}

static int check_text(cc_str text, double size, double angle, Buf *why) {
    if (chars(text) > 2000) buf_puts(why, "text longer than 2000 characters");
    else if (!(finite(size) && size >= 0.5 && size <= 50.0)) buf_puts(why, "text size must be 0.5 to 50 mm");
    else if (!(finite(angle) && angle >= -360.0 && angle <= 360.0)) buf_puts(why, "text angle must be within ±360°");
    else return 0;
    return -1;
}

static int pt_ok(const double *p) {
    for (int i = 0; i < 3; i++)
        if (!(finite(p[i]) && fabs_(p[i]) < 1e9)) return 0;
    return 1;
}

static int view_check(const cc_model *m, const cc_view *v, Buf *b) {
    if (!cc_str_trim(v->name).n) return buf_puts(b, "a view needs a name"), -1;
    if (v->has_crop) {
        if (v->kind == CC_VIEW_THREE_D) return buf_puts(b, "a 3D view has no crop region"), -1;
        int ok = finite(v->crop_min[0]) && finite(v->crop_min[1]) && finite(v->crop_max[0]) &&
                 finite(v->crop_max[1]) && v->crop_min[0] < v->crop_max[0] && v->crop_min[1] < v->crop_max[1];
        if (!ok) return buf_puts(b, "a crop region needs finite corners, the first below the second"), -1;
    }
    if (v->scale < 1 || v->scale > 5000) {
        buf_puts(b, "scale 1:");
        buf_u64(b, v->scale);
        buf_puts(b, ": must be between 1:1 and 1:5000");
        return -1;
    }
    switch (v->kind) {
    case CC_VIEW_THREE_D:
        if (v->nreferences || v->nannotations) return buf_puts(b, "reference plans and annotations go in 2D views"), -1;
        break;
    case CC_VIEW_PLAN:
        if (resolve(m, v, b)) return -1;
        break;
    default:
        if (!(finite(v->origin[0]) && finite(v->origin[1]) && finite(v->depth_mm) && v->depth_mm >= 0.0))
            return buf_puts(b, "section origin and depth must be numbers, depth ≥ 0"), -1;
    }
    for (size_t i = 0; i < v->nreferences; i++) {
        const cc_reference *r = &v->references[i];
        uint64_t pts = 0;
        int fin = finite(r->offset_mm[0]) && finite(r->offset_mm[1]) && finite(r->rotation_deg);
        for (size_t k = 0; k < r->npolylines; k++) {
            pts += r->polylines[k].n;
            for (size_t j = 0; j < r->polylines[k].n; j++)
                fin = fin && finite(r->polylines[k].pts[j][0]) && finite(r->polylines[k].pts[j][1]);
        }
        if (pts > 2000000) {
            buf_puts(b, "reference "), put(b, r->name);
            buf_puts(b, ": "), buf_u64(b, pts), buf_puts(b, " points, more than 2000000");
            return -1;
        }
        if (!fin) {
            buf_puts(b, "reference "), put(b, r->name), buf_puts(b, ": non-finite coordinates");
            return -1;
        }
    }
    if (v->nannotations > 20000) return buf_puts(b, "too many dimensions and texts"), -1;
    for (size_t i = 0; i < v->nannotations; i++) {
        const cc_annotation *a = &v->annotations[i];
        if (a->type == CC_ANN_DIMENSION) {
            if (!(pt_ok(a->a.at) && pt_ok(a->b.at) && finite(a->offset_mm)))
                return buf_puts(b, "a dimension needs finite points and offset"), -1;
        } else {
            if (!pt_ok(a->at)) return buf_puts(b, "a text needs a finite position"), -1;
            if (check_text(a->text, a->size_mm, a->angle_deg, b)) return -1;
        }
    }
    return 0;
}

int cc_view_validate(const cc_model *m, const cc_view *v, Buf *why) {
    Buf t;
    buf_init(&t);
    int rc = view_check(m, v, &t);
    if (rc) { /* "view {name} ({id}): {msg}" */
        buf_puts(why, "view "), put(why, v->name);
        buf_puts(why, " ("), cc_buf_gid(why, &v->id), buf_puts(why, "): ");
        buf_append(why, t.p, t.oom ? 0 : t.len);
    }
    buf_free(&t);
    return rc;
}

static int sheet_check(const cc_model *m, const cc_sheet *s, Buf *b) {
    if (!cc_str_trim(s->number).n) return buf_puts(b, "needs a number"), -1;
    if (chars(s->number) > 40 || chars(s->name) > 200) return buf_puts(b, "number or name too long"), -1;
    int p = 0;
    while (p < 5 && !(s->paper.n == 2 && !memcmp(s->paper.p, PAPERS[p], 2))) p++;
    if (p == 5) {
        buf_puts(b, "paper "), cc_buf_debug_str(b, s->paper.p, s->paper.n);
        buf_puts(b, ": one of A4, A3, A2, A1, A0");
        return -1;
    }
    double w = s->portrait ? PAPER_H[p] : PAPER_W[p], h = s->portrait ? PAPER_W[p] : PAPER_H[p];
    if (s->nplaced > 200 || s->ntexts > 2000) return buf_puts(b, "too many views or texts"), -1;
    for (size_t i = 0; i < s->nplaced; i++) {
        const cc_placed *pl = &s->placed[i];
        if (pl->scale < 1 || pl->scale > 5000) {
            buf_puts(b, "scale 1:"), buf_u64(b, pl->scale), buf_puts(b, ": must be 1:1 to 1:5000");
            return -1;
        }
        const double *q = pl->at_mm;
        if (!(finite(q[0]) && finite(q[1]) && q[0] >= 0.0 && q[0] <= w && q[1] >= 0.0 && q[1] <= h))
            return buf_puts(b, "a view is placed off the paper"), -1;
        if (m && !cc_view_exists(m, &pl->view)) {
            buf_puts(b, "no view "), cc_buf_gid(b, &pl->view);
            return -1;
        }
    }
    for (size_t i = 0; i < s->ntexts; i++) {
        const cc_sheet_text *t = &s->texts[i];
        const double *q = t->at_mm;
        if (!(finite(q[0]) && finite(q[1]) && q[0] >= 0.0 && q[0] <= w && q[1] >= 0.0 && q[1] <= h))
            return buf_puts(b, "a text is off the paper"), -1;
        if (check_text(t->text, t->size_mm, t->angle_deg, b)) return -1;
    }
    for (size_t i = 0; i < s->nfields; i++)
        if (chars(s->fields[i].key) > 100 || chars(s->fields[i].value) > 2000) {
            buf_puts(b, "field "), put(b, s->fields[i].key), buf_puts(b, ": too long");
            return -1;
        }
    return 0;
}

int cc_sheet_validate(const cc_model *m, const cc_sheet *s, Buf *why) {
    Buf t;
    buf_init(&t);
    int rc = sheet_check(m, s, &t);
    if (rc) { /* "sheet {number} {name}: {msg}" */
        buf_puts(why, "sheet "), put(why, s->number);
        buf_putc(why, ' '), put(why, s->name);
        buf_puts(why, ": ");
        buf_append(why, t.p, t.oom ? 0 : t.len);
    }
    buf_free(&t);
    return rc;
}

/* str::to_lowercase for comparing names: ASCII and Latin-1 capitals (U+00C0-U+00DE but
 * U+00D7). Other scripts compare as they are (a stricter "two schedules" check in Rust). */
static int lower_eq(cc_str a, cc_str b) {
    if (a.n != b.n) return 0;
    for (size_t i = 0; i < a.n; i++) {
        unsigned char x = (unsigned char)a.p[i], y = (unsigned char)b.p[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (i > 0 && (unsigned char)a.p[i - 1] == 0xC3) {
            if (x >= 0x80 && x <= 0x9E && x != 0x97) x += 32;
            if (y >= 0x80 && y <= 0x9E && y != 0x97) y += 32;
        }
        if (x != y) return 0;
    }
    return 1;
}

int cc_schedules_validate(const cc_schedule *s, size_t n, Buf *why) {
    if (n > 200) return buf_puts(why, "at most 200 schedules"), -1;
    for (size_t i = 0; i < n; i++) {
        const cc_schedule *d = &s[i];
        if (!cc_str_trim(d->name).n || chars(d->name) > 100)
            return buf_puts(why, "a schedule needs a name (at most 100 characters)"), -1;
        if (d->nfields == 0 || d->nfields > 32) {
            buf_puts(why, "schedule "), put(why, d->name), buf_puts(why, ": 1 to 32 columns");
            return -1;
        }
        if (d->classes.n > 64) {
            buf_puts(why, "schedule "), put(why, d->name), buf_puts(why, ": at most 64 classes");
            return -1;
        }
        for (size_t k = 0; k < i; k++)
            if (lower_eq(s[k].name, d->name)) {
                buf_puts(why, "two schedules are named "), put(why, d->name);
                return -1;
            }
    }
    return 0;
}

int cc_schedule_eq(const cc_schedule *a, const cc_schedule *b) {
    if (!str_eq(a->name, b->name) || a->classes.n != b->classes.n || a->has_family != b->has_family ||
        (a->has_family && a->family != b->family) || a->nfields != b->nfields ||
        a->group_by_level != b->group_by_level || a->totals != b->totals)
        return 0;
    for (size_t i = 0; i < a->classes.n; i++)
        if (!str_eq(a->classes.v[i], b->classes.v[i])) return 0;
    return a->nfields == 0 || !memcmp(a->fields, b->fields, a->nfields);
}

/* ================= copies ================= */

static void *dup_arr(Arena *a, const void *src, size_t size) {
    if (!size) return NULL;
    void *p = arena_alloc(a, size, 16);
    if (p) memcpy(p, src, size);
    return p;
}

static int dup_str(Arena *a, cc_str *s) {
    if (!s->n) {
        s->p = NULL;
        return 0;
    }
    char *p = arena_dup(a, s->p, s->n);
    if (!p) return -1;
    s->p = p;
    return 0;
}

static int dup_strs(Arena *a, cc_strs *s) {
    if (!s->n) {
        s->v = NULL;
        return 0;
    }
    if (!(s->v = dup_arr(a, s->v, s->n * sizeof *s->v))) return -1;
    for (size_t i = 0; i < s->n; i++)
        if (dup_str(a, &s->v[i])) return -1;
    return 0;
}

static void canon(double *v, size_t n) {
    for (size_t i = 0; i < n; i++) v[i] = v[i] + 0.0;
}

void cc_view_canonical(cc_view *v) {
    for (int i = 0; i < 4; i++) canon(&v->range[i].offset_mm, 1);
    canon(v->origin, 2);
    canon(&v->depth_mm, 1);
    canon(v->crop_min, 2);
    canon(v->crop_max, 2);
    for (size_t i = 0; i < v->nreferences; i++) {
        cc_reference *r = &v->references[i];
        canon(r->offset_mm, 2);
        canon(&r->rotation_deg, 1);
        for (size_t k = 0; k < r->npolylines; k++)
            if (r->polylines[k].n) canon(r->polylines[k].pts[0], 2 * r->polylines[k].n);
    }
    for (size_t i = 0; i < v->nannotations; i++) {
        cc_annotation *x = &v->annotations[i];
        canon(x->a.at, 3);
        canon(x->b.at, 3);
        canon(&x->offset_mm, 1);
        canon(x->at, 3);
        canon(&x->size_mm, 1);
        canon(&x->angle_deg, 1);
    }
}

void cc_sheet_canonical(cc_sheet *s) {
    for (size_t i = 0; i < s->nplaced; i++) canon(s->placed[i].at_mm, 2);
    for (size_t i = 0; i < s->ntexts; i++) {
        canon(s->texts[i].at_mm, 2);
        canon(&s->texts[i].size_mm, 1);
        canon(&s->texts[i].angle_deg, 1);
    }
}

int cc_view_dup(Arena *a, cc_view *v) {
    cc_visibility *vis = &v->visibility;
    if (dup_str(a, &v->name) || dup_str(a, &v->storey) || dup_strs(a, &vis->hidden_families) ||
        dup_strs(a, &vis->hidden_classes) || dup_strs(a, &vis->hidden_storeys) || dup_strs(a, &vis->hidden_links) ||
        dup_strs(a, &vis->hidden_materials) || dup_strs(a, &vis->hidden_elements) ||
        dup_strs(a, &vis->isolated_elements))
        return -1;
    if (v->nreferences) {
        cc_reference *r = dup_arr(a, v->references, v->nreferences * sizeof *r);
        if (!r) return -1;
        v->references = r;
        for (size_t i = 0; i < v->nreferences; i++) {
            if (dup_str(a, &r[i].name) || dup_str(a, &r[i].format) || dup_str(a, &r[i].source)) return -1;
            if (r[i].npolylines) {
                cc_loop *l = dup_arr(a, r[i].polylines, r[i].npolylines * sizeof *l);
                if (!l) return -1;
                r[i].polylines = l;
                for (size_t k = 0; k < r[i].npolylines; k++)
                    if (l[k].n && !(l[k].pts = dup_arr(a, l[k].pts, l[k].n * sizeof *l[k].pts))) return -1;
            } else r[i].polylines = NULL;
        }
    } else v->references = NULL;
    if (v->nannotations) {
        cc_annotation *x = dup_arr(a, v->annotations, v->nannotations * sizeof *x);
        if (!x) return -1;
        v->annotations = x;
        for (size_t i = 0; i < v->nannotations; i++)
            if (dup_str(a, &x[i].text) || dup_str(a, &x[i].kind)) return -1;
    } else v->annotations = NULL;
    cc_view_canonical(v);
    return 0;
}

int cc_sheet_dup(Arena *a, cc_sheet *s) {
    if (dup_str(a, &s->number) || dup_str(a, &s->name) || dup_str(a, &s->paper)) return -1;
    if (s->nfields) {
        cc_kv *kv = dup_arr(a, s->fields, s->nfields * sizeof *kv);
        if (!kv) return -1;
        s->fields = kv;
        for (size_t i = 0; i < s->nfields; i++)
            if (dup_str(a, &kv[i].key) || dup_str(a, &kv[i].value)) return -1;
    } else s->fields = NULL;
    if (s->nplaced && !(s->placed = dup_arr(a, s->placed, s->nplaced * sizeof *s->placed))) return -1;
    if (s->ntexts) {
        cc_sheet_text *t = dup_arr(a, s->texts, s->ntexts * sizeof *t);
        if (!t) return -1;
        s->texts = t;
        for (size_t i = 0; i < s->ntexts; i++)
            if (dup_str(a, &t[i].text) || dup_str(a, &t[i].kind)) return -1;
    }
    cc_sheet_canonical(s);
    return 0;
}

int cc_schedules_dup(Arena *a, cc_schedule **s, size_t n) {
    if (!n) {
        *s = NULL;
        return 0;
    }
    cc_schedule *d = dup_arr(a, *s, n * sizeof *d);
    if (!d) return -1;
    for (size_t i = 0; i < n; i++) {
        if (dup_str(a, &d[i].name) || dup_strs(a, &d[i].classes)) return -1;
        if (d[i].nfields && !(d[i].fields = dup_arr(a, d[i].fields, d[i].nfields))) return -1;
    }
    *s = d;
    return 0;
}
