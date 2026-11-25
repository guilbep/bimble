/* Relations as model objects (ADR 0070). See relation.h. No Rust oracle: the texts and rules
 * are this file's own (ccore/golden/DEVIATIONS.md). */
#include "relation.h"
#include <stdlib.h>
#include <string.h>
#include "cbor.h"
#include "grid.h"

/* The kinds, in name order. */
const cc_rel_kind cc_rel_kinds[] = {
    {"IfcRelAggregates", 0, 0, 1, 0},
    {"IfcRelConnectsElements", 1, 0, 0, 0},
    {"IfcRelConnectsWithRealizingElements", 1, 1, 0, 0},
    {"IfcRelDefinesByType", 0, 0, 0, 1},
};
const size_t cc_rel_nkinds = sizeof cc_rel_kinds / sizeof cc_rel_kinds[0];

static int str_is(cc_str s, const char *z) {
    size_t n = strlen(z);
    return s.n == n && (n == 0 || memcmp(s.p, z, n) == 0);
}

const cc_rel_kind *cc_rel_kind_find(cc_str kind) {
    for (size_t i = 0; i < cc_rel_nkinds; i++)
        if (str_is(kind, cc_rel_kinds[i].name)) return &cc_rel_kinds[i];
    return NULL;
}

/* ---- lookups ---- */

static const cc_gid *gid_bsearch(const cc_gid *arr, size_t n, const cc_gid *id) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cc_gid_cmp(&arr[mid], id);
        if (c == 0) return &arr[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

const cc_relation *cc_model_relation(const cc_model *m, const cc_gid *id) {
    size_t lo = 0, hi = m->nrelations;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cc_gid_cmp(&m->relations[mid].id, id);
        if (c == 0) return &m->relations[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

const cc_relation *cc_relation_using(const cc_model *m, const cc_gid *element) {
    for (size_t i = 0; i < m->nrelations; i++) {
        const cc_relation *r = &m->relations[i];
        if (!cc_gid_cmp(&r->relating, element) || gid_bsearch(r->related, r->nrelated, element) ||
            gid_bsearch(r->realizing, r->nrealizing, element))
            return r;
    }
    return NULL;
}

/* ---- validation ---- */

static int key_cmp(cc_str a, cc_str b) {
    size_t n = a.n < b.n ? a.n : b.n;
    int c = n ? memcmp(a.p, b.p, n) : 0;
    if (c) return c;
    return a.n < b.n ? -1 : a.n > b.n;
}

/* "relation <id> (<kind>): " */
static void ctx(Buf *why, const cc_relation *r) {
    buf_puts(why, "relation ");
    cc_buf_gid(why, &r->id);
    buf_puts(why, " (");
    buf_append(why, r->kind.p, r->kind.n < 200 ? r->kind.n : 200);
    buf_puts(why, "): ");
}

/* An id list: 1..CC_REL_MAX_RELATED ids, sorted, no duplicates, each an element, none the
 * relating element. */
static int ids_check(const cc_model *m, const cc_relation *r, const char *what, const cc_gid *ids, size_t n, Buf *why) {
    if (n == 0) {
        ctx(why, r);
        buf_puts(why, "no ");
        buf_puts(why, what);
        buf_puts(why, " element");
        return -1;
    }
    if (n > CC_REL_MAX_RELATED) {
        ctx(why, r);
        buf_puts(why, "at most 10000 ");
        buf_puts(why, what);
        buf_puts(why, " elements");
        return -1;
    }
    for (size_t j = 0; j < n; j++) {
        if (j && cc_gid_cmp(&ids[j - 1], &ids[j]) >= 0) {
            ctx(why, r);
            buf_puts(why, what);
            buf_puts(why, " elements must be sorted, without duplicates");
            return -1;
        }
        if (!cc_gid_cmp(&ids[j], &r->relating)) {
            ctx(why, r);
            buf_puts(why, "relates ");
            cc_buf_gid(why, &ids[j]);
            buf_puts(why, " to itself");
            return -1;
        }
        if (!cc_model_element(m, &ids[j])) {
            ctx(why, r);
            buf_puts(why, "unknown element ");
            cc_buf_gid(why, &ids[j]);
            return -1;
        }
    }
    return 0;
}

static int id_taken(const cc_model *m, const cc_gid *id) {
    return !cc_gid_cmp(&m->project_id, id) || cc_model_storey(m, id) || cc_model_material(m, id) ||
           cc_model_element(m, id) || cc_model_view(m, id) || cc_model_sheet(m, id) || cc_grid_uses_id(m, id);
}

static int one_check(const cc_model *m, const cc_relation *r, Buf *why) {
    const cc_rel_kind *k = cc_rel_kind_find(r->kind);
    if (!k) {
        buf_puts(why, "relation ");
        cc_buf_gid(why, &r->id);
        buf_puts(why, ": unknown kind ");
        cc_buf_debug_str(why, r->kind.p, r->kind.n < 200 ? r->kind.n : 200);
        buf_puts(why, ", expected one of");
        for (size_t i = 0; i < cc_rel_nkinds; i++) buf_puts(why, i ? ", " : " "), buf_puts(why, cc_rel_kinds[i].name);
        return -1;
    }
    if (id_taken(m, &r->id)) {
        ctx(why, r);
        buf_puts(why, "the id is already used by another object");
        return -1;
    }
    if (k->needs_types) {
        ctx(why, r);
        buf_puts(why, "needs a type object, and the model holds no types yet");
        return -1;
    }
    if (!cc_model_element(m, &r->relating)) {
        ctx(why, r);
        buf_puts(why, "unknown element ");
        cc_buf_gid(why, &r->relating);
        return -1;
    }
    if (k->related_one && r->nrelated != 1) {
        ctx(why, r);
        buf_puts(why, "relates exactly one element, got ");
        buf_u64(why, r->nrelated);
        return -1;
    }
    if (ids_check(m, r, "related", r->related, r->nrelated, why)) return -1;
    if (!k->realizing && r->nrealizing) {
        ctx(why, r);
        buf_puts(why, "only IfcRelConnectsWithRealizingElements has realizing elements");
        return -1;
    }
    if (k->realizing) {
        if (ids_check(m, r, "realizing", r->realizing, r->nrealizing, why)) return -1;
        for (size_t j = 0; j < r->nrealizing; j++)
            if (gid_bsearch(r->related, r->nrelated, &r->realizing[j])) {
                ctx(why, r);
                cc_buf_gid(why, &r->realizing[j]);
                buf_puts(why, " is both connected and realizing");
                return -1;
            }
    }
    if (r->nproperties > CC_REL_MAX_PROPERTIES) {
        ctx(why, r);
        buf_puts(why, "at most 1000 properties");
        return -1;
    }
    for (size_t j = 0; j < r->nproperties; j++) {
        if (r->properties[j].key.n == 0) {
            ctx(why, r);
            buf_puts(why, "empty property key");
            return -1;
        }
        if (j && key_cmp(r->properties[j - 1].key, r->properties[j].key) >= 0) {
            ctx(why, r);
            buf_puts(why, "properties must be sorted by key, without duplicates");
            return -1;
        }
    }
    return 0;
}

typedef struct {
    cc_gid part, whole;
    size_t rel;
} Edge;

static int edge_cmp(const void *a, const void *b) {
    const Edge *x = a, *y = b;
    int c = cc_gid_cmp(&x->part, &y->part);
    if (c) return c;
    return x->rel < y->rel ? -1 : x->rel > y->rel;
}

static size_t edge_find(const Edge *e, size_t n, const cc_gid *part) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cc_gid_cmp(&e[mid].part, part);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return (size_t)-1;
}

/* IfcRelAggregates across the table: each part has one whole, and wholes never lead back to a
 * part of their own (each part has one parent, so a walk up the parents is a path; every edge
 * is walked at most twice). */
static int aggregates_check(const cc_relation *r, size_t n, Buf *why) {
    size_t ne = 0;
    for (size_t i = 0; i < n; i++) {
        const cc_rel_kind *k = cc_rel_kind_find(r[i].kind);
        if (k && k->is_aggregate) ne += r[i].nrelated;
    }
    if (!ne) return 0;
    Edge *e = malloc(ne * sizeof *e);
    unsigned char *state = calloc(ne, 1);
    if (!e || !state) {
        free(e), free(state);
        buf_puts(why, "out of memory");
        return -1;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const cc_rel_kind *kd = cc_rel_kind_find(r[i].kind);
        if (!kd || !kd->is_aggregate) continue;
        for (size_t j = 0; j < r[i].nrelated; j++) e[k].part = r[i].related[j], e[k].whole = r[i].relating, e[k].rel = i, k++;
    }
    qsort(e, ne, sizeof *e, edge_cmp);
    int rc = 0;
    for (size_t i = 1; !rc && i < ne; i++)
        if (!cc_gid_cmp(&e[i - 1].part, &e[i].part)) {
            cc_buf_gid(why, &e[i].part);
            buf_puts(why, " is a part of two wholes (relations ");
            cc_buf_gid(why, &r[e[i - 1].rel].id);
            buf_puts(why, " and ");
            cc_buf_gid(why, &r[e[i].rel].id);
            buf_putc(why, ')');
            rc = -1;
        }
    for (size_t i = 0; !rc && i < ne; i++) {
        if (state[i]) continue;
        size_t j = i;
        while (j != (size_t)-1 && state[j] == 0) state[j] = 1, j = edge_find(e, ne, &e[j].whole);
        if (j != (size_t)-1 && state[j] == 1) {
            buf_puts(why, "IfcRelAggregates makes a cycle through ");
            cc_buf_gid(why, &e[j].part);
            rc = -1;
        }
        for (j = i; j != (size_t)-1 && state[j] == 1; j = edge_find(e, ne, &e[j].whole)) state[j] = 2;
    }
    free(e);
    free(state);
    return rc;
}

int cc_relations_validate(const cc_model *m, const cc_relation *r, size_t n, const cc_gid *keys, Buf *why) {
    for (size_t i = 0; i < n; i++) {
        if (keys && cc_gid_cmp(&keys[i], &r[i].id)) {
            buf_puts(why, "relation ");
            cc_buf_gid(why, &keys[i]);
            buf_puts(why, ": key does not match id");
            return -1;
        }
        if (i && cc_gid_cmp(&r[i - 1].id, &r[i].id) >= 0) { /* a broken caller, not a file */
            buf_puts(why, "relations must be sorted by id, without duplicates");
            return -1;
        }
        if (one_check(m, &r[i], why)) return -1;
    }
    return aggregates_check(r, n, why);
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

int cc_relation_dup(Arena *a, cc_relation *r) {
    if (dup_str(a, &r->kind)) return -1;
    if (r->nrelated && !(r->related = dup_bytes(a, r->related, r->nrelated * sizeof *r->related))) return -1;
    if (r->nrealizing && !(r->realizing = dup_bytes(a, r->realizing, r->nrealizing * sizeof *r->realizing))) return -1;
    if (r->nproperties) {
        cc_kv *kv = dup_bytes(a, r->properties, r->nproperties * sizeof *kv);
        if (!kv) return -1;
        for (size_t i = 0; i < r->nproperties; i++)
            if (dup_str(a, &kv[i].key) || dup_str(a, &kv[i].value)) return -1;
        r->properties = kv;
    }
    return 0;
}

static int str_eq(cc_str a, cc_str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }

int cc_relation_eq(const cc_relation *a, const cc_relation *b) {
    if (cc_gid_cmp(&a->id, &b->id) || !str_eq(a->kind, b->kind) || cc_gid_cmp(&a->relating, &b->relating) ||
        a->nrelated != b->nrelated || a->nrealizing != b->nrealizing || a->nproperties != b->nproperties)
        return 0;
    if (a->nrelated && memcmp(a->related, b->related, a->nrelated * sizeof *a->related)) return 0;
    if (a->nrealizing && memcmp(a->realizing, b->realizing, a->nrealizing * sizeof *a->realizing)) return 0;
    for (size_t i = 0; i < a->nproperties; i++)
        if (!str_eq(a->properties[i].key, b->properties[i].key) || !str_eq(a->properties[i].value, b->properties[i].value))
            return 0;
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

static int gid_of(cc_vd *d, const char *s, size_t n, size_t off, cc_gid *out) {
    if (!cc_gid_valid(s, n)) {
        if (d->failed) return -1;
        buf_clear(&d->msg);
        buf_puts(&d->msg, "invalid IFC GlobalId: ");
        cc_buf_debug_str(&d->msg, s, n < 200 ? n : 200);
        return vfail(d, off);
    }
    memcpy(out->c, s, 22);
    return 0;
}

static int d_gid(cc_vd *d, const JrVal *v, cc_gid *out) {
    if (v->type != JR_STR) return type_err(d, v, "a string");
    return gid_of(d, v->s, v->n, v->off, out);
}

static int d_gids(cc_vd *d, const JrVal *v, cc_gid **out, size_t *n) {
    if (v->type != JR_ARR) return type_err(d, v, "a sequence");
    *out = NULL;
    *n = 0;
    if (!v->n) return 0;
    if (v->n > ((size_t)-1) / sizeof(cc_gid)) return vfail3(d, v->off, "out of memory", 0, 0);
    cc_gid *g = arena_alloc(d->a, v->n * sizeof *g, 8);
    if (!g) return vfail3(d, v->off, "out of memory", 0, 0);
    for (size_t i = 0; i < v->n; i++)
        if (d_gid(d, &v->items[i], &g[i])) return -1;
    *out = g;
    *n = v->n;
    return 0;
}

typedef struct {
    const JrMember *m;
    size_t idx;
} PEnt;

static int pent_cmp(const void *a, const void *b) {
    const PEnt *x = a, *y = b;
    cc_str kx = {x->m->key, x->m->key_n}, ky = {y->m->key, y->m->key_n};
    int c = key_cmp(kx, ky);
    if (c) return c;
    return x->idx < y->idx ? -1 : x->idx > y->idx;
}

/* BTreeMap<String, String>: sorted by key bytes, the last of equal keys kept. */
static int d_props(cc_vd *d, const JrVal *v, cc_kv **out, size_t *n) {
    if (v->type != JR_OBJ) return type_err(d, v, "a map");
    *out = NULL;
    *n = 0;
    if (!v->n) return 0;
    for (size_t i = 0; i < v->n; i++)
        if (v->members[i].val.type != JR_STR) return type_err(d, &v->members[i].val, "a string");
    PEnt *e = malloc(v->n * sizeof *e);
    cc_kv *kv = v->n <= ((size_t)-1) / sizeof *kv ? arena_alloc(d->a, v->n * sizeof *kv, 8) : NULL;
    if (!e || !kv) {
        free(e);
        return vfail3(d, v->off, "out of memory", 0, 0);
    }
    for (size_t i = 0; i < v->n; i++) e[i].m = &v->members[i], e[i].idx = i;
    qsort(e, v->n, sizeof *e, pent_cmp);
    size_t c = 0;
    int rc = 0;
    for (size_t i = 0; !rc && i < v->n; i++) {
        if (i + 1 < v->n && e[i].m->key_n == e[i + 1].m->key_n &&
            (e[i].m->key_n == 0 || !memcmp(e[i].m->key, e[i + 1].m->key, e[i].m->key_n)))
            continue; /* a later equal key wins */
        cc_str key = {NULL, e[i].m->key_n};
        if (key.n && !(key.p = arena_dup(d->a, e[i].m->key, key.n))) rc = vfail3(d, v->off, "out of memory", 0, 0);
        else rc = d_str(d, &e[i].m->val, &kv[c].value);
        kv[c++].key = key;
    }
    free(e);
    if (rc) return -1;
    *out = kv;
    *n = c;
    return 0;
}

int cc_vd_relation(cc_vd *d, const JrVal *v, const char *skip, cc_relation *out) {
    static const char *const F[] = {"id", "kind", "relating", "related", "realizing", "properties"};
    const JrVal *f[6] = {0};
    memset(out, 0, sizeof *out);
    if (v->type != JR_OBJ) return type_err(d, v, "struct Relation");
    for (size_t j = 0; j < v->n; j++) {
        const JrMember *mb = &v->members[j];
        if (skip && jr_key_is(mb, skip)) continue;
        int i = 0;
        while (i < 6 && !jr_key_is(mb, F[i])) i++;
        if (i == 6) continue; /* unknown fields are ignored */
        if (f[i]) return vfail3(d, mb->val.off, "duplicate field `", F[i], "`");
        f[i] = &mb->val;
    }
    for (int i = 0; i < 4; i++)
        if (!f[i]) return vfail3(d, v->off, "missing field `", F[i], "`");
    if (d_gid(d, f[0], &out->id) || d_str(d, f[1], &out->kind) || d_gid(d, f[2], &out->relating) ||
        d_gids(d, f[3], &out->related, &out->nrelated))
        return -1;
    if (f[4] && d_gids(d, f[4], &out->realizing, &out->nrealizing)) return -1;
    if (f[5] && d_props(d, f[5], &out->properties, &out->nproperties)) return -1;
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

static void e_gids(cc_em *e, const cc_gid *g, size_t n) {
    if (e->j) jw_arr_begin(e->j);
    else cb_head(e->c, CB_ARRAY, n);
    for (size_t i = 0; i < n; i++) e_gid(e, &g[i]);
    if (e->j) jw_arr_end(e->j);
}

void cc_em_relation(cc_em *e, const cc_relation *r, const char *op) {
    if (e->j) {
        jw_obj_begin(e->j);
        if (op) jw_key(e->j, "op"), jw_str(e->j, op);
    } else {
        cb_head(e->c, CB_MAP, 4 + (r->nrealizing ? 1 : 0) + (r->nproperties ? 1 : 0));
    }
    e_key(e, "id"), e_gid(e, &r->id);
    e_key(e, "kind"), e_str(e, r->kind);
    e_key(e, "relating"), e_gid(e, &r->relating);
    e_key(e, "related"), e_gids(e, r->related, r->nrelated);
    if (r->nrealizing) e_key(e, "realizing"), e_gids(e, r->realizing, r->nrealizing);
    if (r->nproperties) {
        e_key(e, "properties");
        if (e->j) jw_obj_begin(e->j);
        else cb_head(e->c, CB_MAP, r->nproperties);
        for (size_t i = 0; i < r->nproperties; i++) {
            if (e->j) jw_key_n(e->j, r->properties[i].key.n ? r->properties[i].key.p : "", r->properties[i].key.n);
            else e_str(e, r->properties[i].key);
            e_str(e, r->properties[i].value);
        }
        if (e->j) jw_obj_end(e->j);
    }
    if (e->j) jw_obj_end(e->j);
}
