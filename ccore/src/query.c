/* Model queries (crates/bimble-api/src/query.rs and the query part of dispatch.rs).
 *
 * Rust answers with serde_json::to_value(...) then to_string: the Value's objects are BTreeMaps,
 * so every key of every answer comes out sorted, struct fields included.
 *
 * Arguments: Rust turns the body into a serde_json::Value first (objects sorted by key, the
 * last of duplicate keys wins), then derives the argument struct from it. Errors therefore
 * come in sorted key order; the texts below are serde's and serde_json's. */
#include "query.h"
#include "verbs_materials.h" /* MAT1 */
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#include "grid.h"
#include "georef.h"
#include <stdint.h>
#include <string.h>
#include "arena.h"
#include "fmt.h"
#include "guid.h"
#include "json_r.h"
#include "json_w.h"
#include "verb_table.h"

/* ---- arguments as a serde_json::Value ---- */

typedef struct {
    const char *k;
    size_t kn;
    const JrVal *v;
} Ent;

static int key_cmp(const Ent *a, const Ent *b) {
    size_t n = a->kn < b->kn ? a->kn : b->kn;
    int c = memcmp(a->k, b->k, n);
    if (c) return c;
    return a->kn < b->kn ? -1 : a->kn > b->kn;
}

/* Stable merge sort of n entries (tmp holds n). */
static void sort_ents(Ent *e, Ent *tmp, size_t n) {
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = key_cmp(&e[j], &e[i]) < 0 ? e[j++] : e[i++];
            while (i < mid) tmp[k++] = e[i++];
            while (j < hi) tmp[k++] = e[j++];
        }
        memcpy(e, tmp, n * sizeof *e);
    }
}

/* The object's members as a serde_json Map: sorted by key bytes, the last duplicate kept.
 * NULL (and *count 0) for an empty object or out of memory (*oom set). */
static Ent *map_of(Arena *a, const JrVal *o, size_t *count, int *oom) {
    *count = 0;
    if (o->n == 0) return NULL;
    Ent *e = arena_alloc(a, o->n * sizeof *e, 16), *tmp = arena_alloc(a, o->n * sizeof *e, 16);
    if (!e || !tmp) {
        *oom = 1;
        return NULL;
    }
    for (size_t i = 0; i < o->n; i++) e[i] = (Ent){o->members[i].key, o->members[i].key_n, &o->members[i].val};
    sort_ents(e, tmp, o->n);
    size_t k = 0;
    for (size_t i = 0; i < o->n; i++) {
        if (i + 1 < o->n && key_cmp(&e[i], &e[i + 1]) == 0) continue; /* a later one wins */
        e[k++] = e[i];
    }
    *count = k;
    return e;
}

static int key_is(const Ent *e, const char *s) { return e->kn == strlen(s) && memcmp(e->k, s, e->kn) == 0; }

/* serde_json's Number: u64, i64 (negative only) or f64, as its parser makes them. */
enum { NUM_U64, NUM_I64, NUM_F64 };
typedef struct {
    int kind;
    uint64_t u;
    int64_t i;
    double f;
} Num;

static Num num_of(const JrVal *v) {
    Num r = {NUM_F64, 0, 0, v->num};
    if (!v->num_int) return r;
    const char *s = v->s;
    size_t n = v->n, i = v->num_neg ? 1 : 0;
    uint64_t mag = 0;
    for (; i < n; i++) {
        unsigned d = (unsigned)(s[i] - '0');
        if (mag > (UINT64_MAX - d) / 10) return r; /* overflow: serde_json reads an f64 */
        mag = mag * 10 + d;
    }
    if (!v->num_neg) {
        r.kind = NUM_U64;
        r.u = mag;
    } else if (mag != 0 && mag <= (uint64_t)1 << 63) { /* "-0" and below i64::MIN are f64 */
        r.kind = NUM_I64;
        r.i = (int64_t)(0 - mag);
    }
    return r;
}

/* serde's Unexpected, as serde_json's JsonUnexpected prints it. */
static void unexpected(Buf *e, const JrVal *v) {
    switch (v->type) {
    case JR_NULL: buf_puts(e, "null"); break;
    case JR_BOOL: buf_puts(e, v->b ? "boolean `true`" : "boolean `false`"); break;
    case JR_NUM: {
        Num x = num_of(v);
        if (x.kind == NUM_U64) {
            buf_puts(e, "integer `");
            buf_u64(e, x.u);
        } else if (x.kind == NUM_I64) {
            buf_puts(e, "integer `");
            buf_i64(e, x.i);
        } else {
            buf_puts(e, "floating point `");
            fmt_f64_json(e, x.f);
        }
        buf_putc(e, '`');
        break;
    }
    case JR_STR:
        buf_puts(e, "string ");
        cc_buf_debug_str(e, v->s, v->n);
        break;
    case JR_ARR: buf_puts(e, "sequence"); break;
    default: buf_puts(e, "map"); break;
    }
}

static int invalid_type(Buf *e, const JrVal *v, const char *expected) {
    buf_puts(e, "invalid type: ");
    unexpected(e, v);
    buf_puts(e, ", expected ");
    buf_puts(e, expected);
    return -1;
}

static int invalid_length(Buf *e, size_t len, const char *expected) {
    buf_puts(e, "invalid length ");
    buf_u64(e, len);
    buf_puts(e, ", expected ");
    buf_puts(e, expected);
    return -1;
}

static int unknown_field(Buf *e, const Ent *f, const char *expected) {
    buf_puts(e, "unknown field `");
    buf_append(e, f->k, f->kn);
    buf_puts(e, "`, ");
    buf_puts(e, expected);
    return -1;
}

/* GlobalId: try_from = "String". */
static int read_gid(Buf *e, const JrVal *v, cc_gid *out) {
    if (v->type != JR_STR) return invalid_type(e, v, "a string");
    cc_guid g;
    const char *msg = NULL;
    if (cc_guid_parse(v->s, v->n, &g, &msg)) {
        buf_puts(e, msg ? msg : "invalid IFC GlobalId");
        return -1;
    }
    memcpy(out->c, g.c, 22);
    return 0;
}

/* IfcClass, a unit-variant enum: "IfcBeam" or {"IfcBeam": null}. */
static const char CLASS_EXPECTED[] =
    "expected one of `IfcColumn`, `IfcBeam`, `IfcSlab`, `IfcWall`, `IfcFooting`, `IfcBuildingElementProxy`";

static int read_class(Arena *a, Buf *e, const JrVal *v, cc_ifc_class *out, int *oom) {
    const char *name;
    size_t n;
    const JrVal *unit = NULL;
    if (v->type == JR_STR) {
        name = v->s;
        n = v->n;
    } else if (v->type == JR_OBJ) {
        size_t cnt;
        Ent *m = map_of(a, v, &cnt, oom);
        if (*oom) return -1;
        if (cnt != 1) {
            buf_puts(e, "invalid value: map, expected map with a single key");
            return -1;
        }
        name = m[0].k;
        n = m[0].kn;
        unit = m[0].v;
    } else {
        return invalid_type(e, v, "string or map");
    }
    for (int c = 0; c < CC_IFC_CLASS_COUNT; c++) {
        const char *s = cc_ifc_class_name((cc_ifc_class)c);
        if (strlen(s) == n && memcmp(s, name, n) == 0) {
            if (unit && unit->type != JR_NULL) return invalid_type(e, unit, "unit");
            *out = (cc_ifc_class)c;
            return 0;
        }
    }
    buf_puts(e, "unknown variant `");
    buf_append(e, name, n);
    buf_puts(e, "`, ");
    buf_puts(e, CLASS_EXPECTED);
    return -1;
}

static int read_usize(Buf *e, const JrVal *v, uint64_t *out) {
    if (v->type != JR_NUM) return invalid_type(e, v, "usize");
    Num x = num_of(v);
    if (x.kind == NUM_U64) {
        *out = x.u;
        return 0;
    }
    if (x.kind == NUM_I64) {
        buf_puts(e, "invalid value: integer `");
        buf_i64(e, x.i);
        buf_puts(e, "`, expected usize");
        return -1;
    }
    return invalid_type(e, v, "usize");
}

typedef struct {
    int has_class, has_storey, has_limit;
    cc_ifc_class cls;
    cc_gid storey;
    uint64_t limit;
} Filter;

/* Filter fields by position (0 class, 1 storey, 2 limit), each an Option: null is None. */
static int filter_field(Arena *a, Buf *e, int field, const JrVal *v, Filter *f, int *oom) {
    if (v->type == JR_NULL) return 0;
    switch (field) {
    case 0: f->has_class = 1; return read_class(a, e, v, &f->cls, oom);
    case 1: f->has_storey = 1; return read_gid(e, v, &f->storey);
    default: f->has_limit = 1; return read_usize(e, v, &f->limit);
    }
}

/* ElementFilter: #[serde(default, deny_unknown_fields)] {class, storey, limit}. */
static int parse_filter(Arena *a, Buf *e, const JrVal *v, Filter *f, int *oom) {
    memset(f, 0, sizeof *f);
    if (v->type == JR_OBJ) {
        size_t cnt;
        Ent *m = map_of(a, v, &cnt, oom);
        if (*oom) return -1;
        for (size_t i = 0; i < cnt; i++) {
            int field = key_is(&m[i], "class") ? 0 : key_is(&m[i], "storey") ? 1 : key_is(&m[i], "limit") ? 2 : -1;
            if (field < 0) return unknown_field(e, &m[i], "expected one of `class`, `storey`, `limit`");
            if (filter_field(a, e, field, m[i].v, f, oom)) return -1;
        }
        return 0;
    }
    if (v->type == JR_ARR) { /* missing elements take the default */
        for (size_t i = 0; i < v->n && i < 3; i++)
            if (filter_field(a, e, (int)i, &v->items[i], f, oom)) return -1;
        return v->n > 3 ? invalid_length(e, v->n, "fewer elements in array") : 0;
    }
    return invalid_type(e, v, "struct ElementFilter");
}

/* NoArgs: #[serde(deny_unknown_fields)] struct NoArgs {}. */
static int parse_noargs(Arena *a, Buf *e, const JrVal *v, int *oom) {
    if (v->type == JR_OBJ) {
        size_t cnt;
        Ent *m = map_of(a, v, &cnt, oom);
        if (*oom) return -1;
        return cnt ? unknown_field(e, &m[0], "there are no fields") : 0;
    }
    if (v->type == JR_ARR) return v->n ? invalid_length(e, v->n, "fewer elements in array") : 0;
    return invalid_type(e, v, "struct NoArgs");
}

/* IdArgs: #[serde(deny_unknown_fields)] struct IdArgs { id: GlobalId }. */
static int parse_id(Arena *a, Buf *e, const JrVal *v, cc_gid *id, int *oom) {
    if (v->type == JR_OBJ) {
        size_t cnt;
        Ent *m = map_of(a, v, &cnt, oom);
        if (*oom) return -1;
        int have = 0;
        for (size_t i = 0; i < cnt; i++) {
            if (!key_is(&m[i], "id")) return unknown_field(e, &m[i], "expected `id`");
            if (read_gid(e, m[i].v, id)) return -1;
            have = 1;
        }
        if (!have) {
            buf_puts(e, "missing field `id`");
            return -1;
        }
        return 0;
    }
    if (v->type == JR_ARR) {
        if (v->n == 0) return invalid_length(e, 0, "struct IdArgs with 1 element");
        if (read_gid(e, &v->items[0], id)) return -1;
        return v->n > 1 ? invalid_length(e, v->n, "fewer elements in array") : 0;
    }
    return invalid_type(e, v, "struct IdArgs");
}

/* ---- answers ---- */

static void w_gid(JsonW *w, const cc_gid *g) { jw_str_n(w, g->c, 22); }
static void w_s(JsonW *w, cc_str s) { jw_str_n(w, s.p, s.n); }

static void w_kv_map(JsonW *w, const cc_kv *kv, size_t n) {
    jw_obj_begin(w);
    for (size_t i = 0; i < n; i++) {
        jw_key_n(w, kv[i].key.p, kv[i].key.n);
        w_s(w, kv[i].value);
    }
    jw_obj_end(w);
}

/* f64::total_cmp key. */
static int64_t total_key(double x) {
    int64_t k;
    memcpy(&k, &x, 8);
    return k ^ (int64_t)((uint64_t)(k >> 63) >> 1);
}

static int storey_before(const cc_storey *a, const cc_storey *b) {
    int64_t ka = total_key(a->elevation), kb = total_key(b->elevation);
    if (ka != kb) return ka < kb;
    return cc_gid_cmp(&a->id, &b->id) < 0;
}

static int q_storeys(const cc_model *m, Arena *a, JsonW *w) {
    size_t n = m->nstoreys;
    size_t *cnt = arena_alloc(a, (n + 1) * sizeof *cnt, 16);
    const cc_storey **ord = arena_alloc(a, (n + 1) * sizeof *ord, 16);
    const cc_storey **tmp = arena_alloc(a, (n + 1) * sizeof *tmp, 16);
    if (!cnt || !ord || !tmp) return -1;
    for (size_t i = 0; i < m->nelements; i++) {
        const cc_storey *s = cc_model_storey(m, &m->elements[i].storey);
        if (s) cnt[s - m->storeys]++;
    }
    for (size_t i = 0; i < n; i++) ord[i] = &m->storeys[i];
    for (size_t wd = 1; wd < n; wd *= 2) { /* stable merge sort: elevation, then id */
        for (size_t lo = 0; lo < n; lo += 2 * wd) {
            size_t mid = lo + wd < n ? lo + wd : n, hi = lo + 2 * wd < n ? lo + 2 * wd : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = storey_before(ord[j], ord[i]) ? ord[j++] : ord[i++];
            while (i < mid) tmp[k++] = ord[i++];
            while (j < hi) tmp[k++] = ord[j++];
        }
        memcpy(ord, tmp, n * sizeof *ord);
    }
    /* ADR 0056, a C addition: a model that uses levels or grids also gets is_storey and
     * height_mm (a storey's height to the next storey up, null for the top one and for a level
     * that is not a storey). Other models answer Rust's bytes (golden/query.txt). */
    int levels = m->ngrids > 0;
    for (size_t i = 0; i < n && !levels; i++) levels = m->storeys[i].not_storey;
    jw_arr_begin(w);
    for (size_t i = 0; i < n; i++) {
        const cc_storey *s = ord[i];
        jw_obj_begin(w);
        jw_key(w, "element_count");
        jw_u64(w, cnt[s - m->storeys]);
        jw_key(w, "elevation");
        jw_f64(w, s->elevation);
        if (levels) {
            const cc_storey *up = NULL;
            for (size_t j = i + 1; !s->not_storey && j < n && !up; j++)
                if (!ord[j]->not_storey) up = ord[j];
            jw_key(w, "height_mm");
            if (up) jw_f64(w, up->elevation - s->elevation);
            else jw_null(w);
        }
        jw_key(w, "id");
        w_gid(w, &s->id);
        if (levels) jw_key(w, "is_storey"), jw_bool(w, !s->not_storey);
        jw_key(w, "name");
        w_s(w, s->name);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    return 0;
}

/* ADR 0056, a C addition: [{axes: [{end_mm, id, start_mm, tag}], id, name}], grids by id, axes in
 * their order. */
static void q_grids(const cc_model *m, JsonW *w) {
    jw_arr_begin(w);
    for (size_t i = 0; i < m->ngrids; i++) {
        const cc_grid *g = &m->grids[i];
        jw_obj_begin(w);
        jw_key(w, "axes");
        jw_arr_begin(w);
        for (size_t k = 0; k < g->naxes; k++) {
            const cc_grid_axis *a = &g->axes[k];
            jw_obj_begin(w);
            jw_key(w, "end_mm"), jw_arr_begin(w), jw_f64(w, a->end[0]), jw_f64(w, a->end[1]), jw_arr_end(w);
            jw_key(w, "id"), w_gid(w, &a->id);
            jw_key(w, "start_mm"), jw_arr_begin(w), jw_f64(w, a->start[0]), jw_f64(w, a->start[1]), jw_arr_end(w);
            jw_key(w, "tag"), w_s(w, a->tag);
            jw_obj_end(w);
        }
        jw_arr_end(w);
        jw_key(w, "id"), w_gid(w, &g->id);
        jw_key(w, "name"), w_s(w, g->name);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

static int q_materials(const cc_model *m, Arena *a, JsonW *w) {
    size_t *cnt = arena_alloc(a, (m->nmaterials + 1) * sizeof *cnt, 16);
    if (!cnt) return -1;
    for (size_t i = 0; i < m->nelements; i++) {
        const cc_material *x = cc_model_material(m, &m->elements[i].material);
        if (x) cnt[x - m->materials]++;
    }
    jw_arr_begin(w);
    for (size_t i = 0; i < m->nmaterials; i++) {
        const cc_material *x = &m->materials[i];
        jw_obj_begin(w);
        jw_key(w, "category");
        jw_str(w, cc_category_name(x->category));
        jw_key(w, "element_count");
        jw_u64(w, cnt[i]);
        jw_key(w, "id");
        w_gid(w, &x->id);
        jw_key(w, "name");
        w_s(w, x->name);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    return 0;
}

static void derived_id(JsonW *w, const cc_gid *project, const char *suffix) {
    char seed[64];
    size_t k = strlen(suffix);
    memcpy(seed, project->c, 22);
    memcpy(seed + 22, suffix, k);
    cc_guid g = cc_guid_derived(seed, 22 + k);
    jw_str_n(w, g.c, 22);
}

static void q_project(const cc_model *m, JsonW *w) {
    jw_obj_begin(w);
    jw_key(w, "building_id");
    derived_id(w, &m->project_id, "/building"); /* same seeds as bimble-ifc's `gid` */
    jw_key(w, "id");
    w_gid(w, &m->project_id);
    jw_key(w, "info");
    w_kv_map(w, m->project, m->nproject);
    jw_key(w, "name");
    w_s(w, m->project_name);
    jw_key(w, "site_id");
    derived_id(w, &m->project_id, "/site");
    jw_obj_end(w);
}

/* An ElementSummary, keys sorted. */
static void summary_obj(JsonW *w, const cc_element *e) {
    jw_obj_begin(w);
    jw_key(w, "class");
    jw_str(w, cc_ifc_class_name(e->cls));
    jw_key(w, "generator");
    jw_str(w, cc_gen_kind_name(e->generator.kind));
    jw_key(w, "id");
    w_gid(w, &e->id);
    jw_key(w, "material");
    w_gid(w, &e->material);
    jw_key(w, "name");
    w_s(w, e->name);
    jw_key(w, "storey");
    w_gid(w, &e->storey);
    jw_obj_end(w);
}

static void q_elements(const cc_model *m, const Filter *f, JsonW *w) {
    uint64_t limit = f->has_limit ? f->limit : CC_QUERY_DEFAULT_LIMIT;
    if (limit > CC_QUERY_MAX_ELEMENTS) limit = CC_QUERY_MAX_ELEMENTS;
    uint64_t k = 0;
    jw_arr_begin(w);
    for (size_t i = 0; i < m->nelements && k < limit; i++) {
        const cc_element *e = &m->elements[i];
        if (f->has_class && e->cls != f->cls) continue;
        if (f->has_storey && cc_gid_cmp(&e->storey, &f->storey) != 0) continue;
        summary_obj(w, e);
        k++;
    }
    jw_arr_end(w);
}

static void w_vec3(JsonW *w, const char *key, const double v[3]) {
    jw_key(w, key);
    jw_arr_begin(w);
    for (int i = 0; i < 3; i++) jw_f64(w, v[i]);
    jw_arr_end(w);
}

static void w_loop(JsonW *w, const cc_loop *l) {
    jw_arr_begin(w);
    for (size_t i = 0; i < l->n; i++) {
        jw_arr_begin(w);
        jw_f64(w, l->pts[i][0]);
        jw_f64(w, l->pts[i][1]);
        jw_arr_end(w);
    }
    jw_arr_end(w);
}

static void w_num(JsonW *w, const char *key, double x) {
    jw_key(w, key);
    jw_f64(w, x);
}

/* serde_json::to_value(generator) (tag "kind"), keys sorted; a mesh is reduced to counts. */
static void generator_params(JsonW *w, const cc_generator *g) {
    const char *kind = cc_gen_kind_name(g->kind);
    jw_obj_begin(w);
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
        w_num(w, "depth", g->u.col.depth);
        w_num(w, "height", g->u.col.height);
        jw_key(w, "kind");
        jw_str(w, kind);
        w_num(w, "width", g->u.col.width);
        break;
    case CC_GEN_RECT_BEAM:
        w_num(w, "height", g->u.beam.height);
        jw_key(w, "kind");
        jw_str(w, kind);
        w_num(w, "length", g->u.beam.length);
        w_num(w, "width", g->u.beam.width);
        break;
    case CC_GEN_SLAB:
        jw_key(w, "kind");
        jw_str(w, kind);
        jw_key(w, "outline");
        w_loop(w, &g->u.slab.outline);
        w_num(w, "thickness", g->u.slab.thickness);
        break;
    case CC_GEN_WALL:
        w_num(w, "height", g->u.wall.height);
        jw_key(w, "kind");
        jw_str(w, kind);
        w_num(w, "length", g->u.wall.length);
        w_num(w, "thickness", g->u.wall.thickness);
        break;
    case CC_GEN_EXTRUDED_POLYGON:
        w_num(w, "height", g->u.poly.height);
        jw_key(w, "kind");
        jw_str(w, kind);
        jw_key(w, "outline");
        w_loop(w, &g->u.poly.outline);
        if (g->u.poly.nvoids) { /* skip_serializing_if = "Vec::is_empty" */
            jw_key(w, "voids");
            jw_arr_begin(w);
            for (size_t i = 0; i < g->u.poly.nvoids; i++) w_loop(w, &g->u.poly.voids[i]);
            jw_arr_end(w);
        }
        break;
    default: /* mesh */
        jw_key(w, "kind");
        jw_str(w, "mesh");
        jw_key(w, "triangles");
        jw_u64(w, g->u.mesh.nindices / 3);
        jw_key(w, "vertices");
        jw_u64(w, g->u.mesh.npositions);
        break;
    }
    jw_obj_end(w);
}

/* ElementDetail: the summary flattened in, all keys sorted. */
static void q_element(const cc_model *m, const cc_element *e, JsonW *w) {
    const cc_storey *s = cc_model_storey(m, &e->storey);
    const cc_material *x = cc_model_material(m, &e->material);
    jw_obj_begin(w);
    jw_key(w, "class");
    jw_str(w, cc_ifc_class_name(e->cls));
    jw_key(w, "generator");
    jw_str(w, cc_gen_kind_name(e->generator.kind));
    jw_key(w, "generator_params");
    generator_params(w, &e->generator);
    jw_key(w, "id");
    w_gid(w, &e->id);
    jw_key(w, "material");
    w_gid(w, &e->material);
    jw_key(w, "material_name");
    if (x) w_s(w, x->name);
    else jw_null(w);
    jw_key(w, "name");
    w_s(w, e->name);
    jw_key(w, "openings"); /* query.rs:146 e.openings.len() (CONC1: the C model keeps them) */
    jw_u64(w, e->nopenings);
    jw_key(w, "placement");
    jw_obj_begin(w);
    w_vec3(w, "origin", e->placement.origin);
    w_vec3(w, "x_axis", e->placement.x_axis);
    w_vec3(w, "z_axis", e->placement.z_axis);
    jw_obj_end(w);
    jw_key(w, "properties");
    w_kv_map(w, e->properties, e->nproperties);
    jw_key(w, "storey");
    w_gid(w, &e->storey);
    jw_key(w, "storey_name");
    if (s) w_s(w, s->name);
    else jw_null(w);
    jw_obj_end(w);
}

/* views: ViewSummary (kind flattened), keys sorted. */
static void w_pair(JsonW *w, const double *v) {
    jw_arr_begin(w);
    jw_f64(w, v[0]);
    jw_f64(w, v[1]);
    jw_arr_end(w);
}

static void q_views(const cc_model *m, JsonW *w) {
    static const char *const PLANE[] = {"bottom", "cut", "depth", "top"};
    static const int PIDX[] = {CC_RANGE_BOTTOM, CC_RANGE_CUT, CC_RANGE_DEPTH, CC_RANGE_TOP};
    jw_arr_begin(w);
    for (size_t i = 0; i < m->nviews; i++) {
        const cc_view *v = &m->views[i];
        jw_obj_begin(w);
        jw_key(w, "annotations");
        jw_u64(w, v->nannotations);
        jw_key(w, "crop");
        if (v->has_crop) {
            jw_obj_begin(w);
            jw_key(w, "max");
            w_pair(w, v->crop_max);
            jw_key(w, "min");
            w_pair(w, v->crop_min);
            jw_obj_end(w);
        } else {
            jw_null(w);
        }
        if (v->kind == CC_VIEW_SECTION) jw_key(w, "depth_mm"), jw_f64(w, v->depth_mm);
        if (v->kind == CC_VIEW_SECTION) jw_key(w, "facing"), jw_str(w, cc_facing_name(v->facing));
        jw_key(w, "id");
        w_gid(w, &v->id);
        if (v->kind == CC_VIEW_PLAN) jw_key(w, "look"), jw_str(w, cc_look_name(v->look));
        jw_key(w, "name");
        w_s(w, v->name);
        if (v->kind == CC_VIEW_SECTION) jw_key(w, "origin"), w_pair(w, v->origin);
        if (v->kind == CC_VIEW_PLAN) {
            jw_key(w, "range");
            jw_obj_begin(w);
            for (int k = 0; k < 4; k++) {
                jw_key(w, PLANE[k]);
                jw_obj_begin(w);
                jw_key(w, "level");
                jw_str(w, cc_level_ref_name(v->range[PIDX[k]].level));
                jw_key(w, "offset_mm");
                jw_f64(w, v->range[PIDX[k]].offset_mm);
                jw_obj_end(w);
            }
            jw_obj_end(w);
        }
        jw_key(w, "references");
        jw_u64(w, v->nreferences);
        jw_key(w, "scale");
        jw_u64(w, v->scale);
        if (v->kind == CC_VIEW_PLAN) jw_key(w, "storey"), w_s(w, v->storey);
        jw_key(w, "true_north");
        jw_bool(w, v->true_north);
        jw_key(w, "type");
        jw_str(w, cc_view_kind_name(v->kind));
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

/* sheets: SheetSummary, keys sorted. */
static void q_sheets(const cc_model *m, JsonW *w) {
    jw_arr_begin(w);
    for (size_t i = 0; i < m->nsheets; i++) {
        const cc_sheet *s = &m->sheets[i];
        jw_obj_begin(w);
        jw_key(w, "fields");
        w_kv_map(w, s->fields, s->nfields);
        jw_key(w, "id");
        w_gid(w, &s->id);
        jw_key(w, "name");
        w_s(w, s->name);
        jw_key(w, "number");
        w_s(w, s->number);
        jw_key(w, "paper");
        w_s(w, s->paper);
        jw_key(w, "placed");
        jw_arr_begin(w);
        for (size_t k = 0; k < s->nplaced; k++) {
            const cc_placed *p = &s->placed[k];
            jw_obj_begin(w);
            jw_key(w, "at_mm");
            w_pair(w, p->at_mm);
            jw_key(w, "id");
            w_gid(w, &p->id);
            jw_key(w, "scale");
            jw_u64(w, p->scale);
            jw_key(w, "view");
            w_gid(w, &p->view);
            jw_obj_end(w);
        }
        jw_arr_end(w);
        jw_key(w, "portrait");
        jw_bool(w, s->portrait);
        jw_key(w, "texts");
        jw_u64(w, s->ntexts);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

/* schedules: the stored ScheduleDefs, keys sorted (family only when set). */
static void q_schedules(const cc_model *m, JsonW *w) {
    jw_arr_begin(w);
    for (size_t i = 0; i < m->nschedules; i++) {
        const cc_schedule *d = &m->schedules[i];
        jw_obj_begin(w);
        jw_key(w, "classes");
        jw_arr_begin(w);
        for (size_t k = 0; k < d->classes.n; k++) w_s(w, d->classes.v[k]);
        jw_arr_end(w);
        if (d->has_family) jw_key(w, "family"), jw_str(w, cc_category_name((cc_category)d->family));
        jw_key(w, "fields");
        jw_arr_begin(w);
        for (size_t k = 0; k < d->nfields; k++) jw_str(w, cc_sched_field_name(d->fields[k]));
        jw_arr_end(w);
        jw_key(w, "group_by_level");
        jw_bool(w, d->group_by_level);
        jw_key(w, "name");
        w_s(w, d->name);
        jw_key(w, "totals");
        jw_bool(w, d->totals);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

/* The verb table's queries (the viewer's `verbs` and `describe` calls, calls.rs): a C addition
 * to dispatch::query. 200 with the answer, 400 with the error text. */
static int verb_query(const char *name, const char *args_json, size_t n, Buf *out) {
    if (name[0] == 'v') { /* verbs */
        cc_verbs_list_json(out);
        return out->oom ? 500 : 200;
    }
    Arena a;
    arena_init(&a, 0);
    JrError jerr;
    const JrVal *v = n ? jr_parse(&a, args_json, n, &jerr) : NULL;
    int status = 400;
    const JrVal *nm = v && v->type == JR_OBJ ? jr_get(v, "name") : NULL;
    if (n && !v) buf_puts(out, jerr.msg);
    else if (!nm || nm->type != JR_STR) buf_puts(out, !nm ? "missing field `name`" : "invalid type: expected a string");
    else {
        const cc_verb_def *d = NULL;
        for (size_t i = 0; i < cc_verb_count && !d; i++)
            if (strlen(cc_verb_table[i].name) == nm->n && !memcmp(cc_verb_table[i].name, nm->s, nm->n))
                d = &cc_verb_table[i];
        if (!d) buf_puts(out, "no verb with this name");
        else buf_puts(out, d->schema), status = 200;
    }
    arena_free(&a);
    return out->oom ? 500 : status;
}

/* ---- dispatch ---- */

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* ---- georeference (ADR 0045, C additions) ---- */

/* north: project north and the map's turn, for the viewport and the view cube. */
static void q_north(const cc_model *m, JsonW *w) {
    const cc_georef *g = &m->georef;
    jw_obj_begin(w);
    jw_key(w, "crs");
    if (g->has_crs) jw_str_n(w, g->crs.n ? g->crs.p : "", g->crs.n);
    else jw_null(w);
    jw_key(w, "has_map"), jw_bool(w, cc_georef_has_map(g));
    jw_key(w, "map_rotation_deg"), jw_f64(w, g->map_rotation_deg);
    jw_key(w, "plan_turn_project"), jw_f64(w, cc_georef_plan_turn(g, 0));
    jw_key(w, "plan_turn_true"), jw_f64(w, cc_georef_plan_turn(g, 1));
    jw_key(w, "rotation_deg"), jw_f64(w, g->rotation_deg);
    jw_obj_end(w);
}

/* coordinates {at: [x, y, z] (world mm), system: "survey" (default) | "wgs84" | "utm" |
 * "epsg:<n>"}: the status bar's text of the point (bimble-viewer coords.rs), its survey
 * coordinates (m) and WGS84 latitude and longitude (degrees, null when the model's system is not
 * LV95). 400 when the system cannot be said by the C core (georef.h). */
static int q_coordinates(const cc_model *m, const JrVal *args, JsonW *w, Buf *e) {
    const JrVal *at = NULL, *sys = NULL;
    for (size_t k = 0; k < args->n; k++) {
        const JrMember *mb = &args->members[k];
        if (jr_key_is(mb, "at")) at = &mb->val;
        else if (jr_key_is(mb, "system")) sys = &mb->val;
        else {
            buf_puts(e, "unknown field `");
            buf_append(e, mb->key, mb->key_n < 200 ? mb->key_n : 200);
            buf_puts(e, "`, expected `at` or `system`");
            return -1;
        }
    }
    double p[3];
    if (!at || at->type != JR_ARR || at->n != 3) {
        buf_puts(e, "`at`: [x, y, z], world mm");
        return -1;
    }
    for (int k = 0; k < 3; k++) {
        if (at->items[k].type != JR_NUM || at->items[k].num - at->items[k].num != 0.0) {
            buf_puts(e, "`at`: [x, y, z], world mm");
            return -1;
        }
        p[k] = at->items[k].num;
    }
    cc_coords_target t = CC_COORDS_SURVEY;
    int code = 0;
    if (sys && sys->type != JR_NULL) {
        if (sys->type != JR_STR) {
            buf_puts(e, "`system`: \"survey\", \"wgs84\", \"utm\" or \"epsg:<n>\"");
            return -1;
        }
        if (sys->n == 6 && !memcmp(sys->s, "survey", 6)) t = CC_COORDS_SURVEY;
        else if (sys->n == 5 && !memcmp(sys->s, "wgs84", 5)) t = CC_COORDS_WGS84;
        else if (sys->n == 3 && !memcmp(sys->s, "utm", 3)) t = CC_COORDS_UTM;
        else if (sys->n > 5 && sys->n < 11 && !memcmp(sys->s, "epsg:", 5)) {
            t = CC_COORDS_EPSG;
            for (size_t k = 5; k < sys->n; k++) {
                if (sys->s[k] < '0' || sys->s[k] > '9') {
                    buf_puts(e, "`system`: \"epsg:<n>\", n a number");
                    return -1;
                }
                code = code * 10 + (sys->s[k] - '0');
            }
        } else {
            buf_puts(e, "`system`: \"survey\", \"wgs84\", \"utm\" or \"epsg:<n>\"");
            return -1;
        }
    }
    Buf text;
    buf_init(&text);
    if (cc_georef_text(&text, &m->georef, p, t, code)) {
        buf_append(e, text.p, text.len);
        buf_free(&text);
        return -1;
    }
    double sv[3], ll[2];
    cc_georef_survey(&m->georef, p, sv);
    int has_ll = m->georef.has_crs && m->georef.crs.n == 9 && !memcmp(m->georef.crs.p, CC_LV95, 9) &&
                 !cc_lv95_to_wgs84(sv[0], sv[1], &ll[0], &ll[1]);
    jw_obj_begin(w);
    jw_key(w, "survey"), jw_arr_begin(w), jw_f64(w, sv[0]), jw_f64(w, sv[1]), jw_f64(w, sv[2]), jw_arr_end(w);
    jw_key(w, "text"), jw_str_n(w, text.p ? text.p : "", text.len);
    jw_key(w, "wgs84");
    if (has_ll) jw_arr_begin(w), jw_f64(w, ll[0]), jw_f64(w, ll[1]), jw_arr_end(w);
    else jw_null(w);
    jw_key(w, "world_mm"), jw_arr_begin(w), jw_f64(w, p[0]), jw_f64(w, p[1]), jw_f64(w, p[2]), jw_arr_end(w);
    jw_obj_end(w);
    buf_free(&text);
    return 0;
}

static const char KNOWN[] = "storeys, materials, elements, element, project, views, sheets, schedules";

int cc_query(const cc_model *m, const char *name, const char *args_json, size_t n, Buf *out) {
    size_t start = out->len;
    Arena a;
    arena_init(&a, 0);
    Buf e; /* error text */
    buf_init(&e);
    int status = 400, oom = 0;
    static const JrVal null_val; /* type JR_NULL */
    const JrVal *args = &null_val;
    size_t i = 0;
    while (i < n && is_ws(args_json[i])) i++;
    if (i < n) { /* the transport's serde_json::from_str */
        JrError jerr;
        args = jr_parse(&a, args_json, n, &jerr);
        if (!args) {
            buf_puts(out, jerr.msg);
            goto done;
        }
    }

    if (!strcmp(name, "verbs") || !strcmp(name, "describe")) {
        status = verb_query(name, args_json, n, out);
        goto done;
    }
    /* "grids" (ADR 0056) is a C addition: past Rust's eight, so the unknown query text stays Rust's. */
    /* "north" and "coordinates" (ADR 0045) likewise. */
    /* MAT1 (verbs_materials.h): "materials_full" (every field) and "material_library". */
    enum { Q_STOREYS, Q_MATERIALS, Q_ELEMENTS, Q_ELEMENT, Q_PROJECT, Q_VIEWS, Q_SHEETS, Q_SCHEDULES, Q_GRIDS, Q_NORTH,
           Q_COORDINATES, Q_MATERIALS_FULL, Q_MATERIAL_LIBRARY, Q_COUNT } q;
    static const char *const NAMES[] = {"storeys", "materials", "elements",       "element",         "project",
                                        "views",   "sheets",    "schedules",      "grids",           "north",
                                        "coordinates", "materials_full", "material_library"};
    for (q = 0; q < Q_COUNT && (strlen(name) != strlen(NAMES[q]) || memcmp(name, NAMES[q], strlen(name)) != 0); q++) {
    }
    if (q == Q_COUNT) {
        buf_puts(out, "unknown query ");
        cc_buf_debug_str(out, name, strlen(name));
        buf_puts(out, "; known: ");
        buf_puts(out, KNOWN);
        goto done;
    }

    /* dispatch::parse: before parsing, a huge id list costs nothing to refuse. */
    if (args->type == JR_OBJ) {
        const JrVal *ids = NULL;
        for (size_t k = 0; k < args->n; k++) /* the last duplicate wins */
            if (jr_key_is(&args->members[k], "ids")) ids = &args->members[k].val;
        if (ids && ids->type == JR_ARR && ids->n > CC_QUERY_MAX_IDS) {
            buf_u64(out, ids->n);
            buf_puts(out, " ids: at most ");
            buf_u64(out, CC_QUERY_MAX_IDS);
            buf_puts(out, " per call");
            goto done;
        }
    }

    static const JrVal empty_obj = {.type = JR_OBJ};
    if (args->type == JR_NULL) args = &empty_obj; /* null reads as {} */
    Filter f;
    cc_gid id;
    int bad;
    if (q == Q_ELEMENTS) bad = parse_filter(&a, &e, args, &f, &oom);
    else if (q == Q_ELEMENT) bad = parse_id(&a, &e, args, &id, &oom);
    else if (q == Q_COORDINATES) bad = args->type == JR_OBJ ? 0 : (buf_puts(&e, "expected an object"), 1);
    else bad = parse_noargs(&a, &e, args, &oom);
    if (oom) goto done;
    if (bad) {
        buf_puts(out, "bad arguments: ");
        buf_append(out, e.p, e.len);
        goto done;
    }

    const cc_element *el = NULL;
    if (q == Q_ELEMENT && !(el = cc_model_element(m, &id))) {
        buf_puts(out, "no element GlobalId(");
        buf_append(out, id.c, 22);
        buf_putc(out, ')');
        goto done;
    }

    JsonW w;
    jw_init(&w, out, 0);
    if (q == Q_COORDINATES && q_coordinates(m, args, &w, &e)) {
        out->len = start;
        buf_puts(out, "bad arguments: ");
        buf_append(out, e.p, e.len);
        goto done;
    }
    switch (q) {
    case Q_COORDINATES: break;
    case Q_NORTH: q_north(m, &w); break;
    case Q_STOREYS: oom = q_storeys(m, &a, &w) != 0; break;
    case Q_MATERIALS: oom = q_materials(m, &a, &w) != 0; break;
    case Q_ELEMENTS: q_elements(m, &f, &w); break;
    case Q_ELEMENT: q_element(m, el, &w); break;
    case Q_PROJECT: q_project(m, &w); break;
    case Q_VIEWS: q_views(m, &w); break;
    case Q_SHEETS: q_sheets(m, &w); break;
    case Q_GRIDS: q_grids(m, &w); break;
    case Q_MATERIALS_FULL: oom = cc_materials_full_json(m, &w) != 0; break; /* MAT1 */
    case Q_MATERIAL_LIBRARY: cc_material_library_json(&w); break;     /* MAT1 */
    default: q_schedules(m, &w); break;
    }
    if (oom) goto done;
    int wrc = jw_finish(&w);
    if (wrc == JW_OK) {
        status = 200;
    } else if (wrc != JW_ERR_OOM) { /* a validated model has only finite values */
        out->len = start;
        buf_puts(out, "non-finite value in the model");
        status = 500;
    }

done:
    if (oom || out->oom || e.oom) {
        out->oom = 0;
        out->len = start;
        buf_puts(out, "out of memory");
        status = 500;
    }
    buf_free(&e);
    arena_free(&a);
    return status;
}
