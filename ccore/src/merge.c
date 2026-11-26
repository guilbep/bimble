/* Three-way merge of models (merge.h; crates/bimble-core/src/repo.rs:587-861 merge_models).
 * Each object is compared in its serde form (cc_model_object_json: serde_json::to_vec's bytes,
 * so equal values give equal bytes, as Rust's PartialEq); objects both sides changed are read as
 * JSON DOMs and merged field by field as Rust's merge_value does over serde_json::Value. The
 * merged model is written as a model file and decoded again (cc_model_load_unchecked), as Rust
 * deserializes each merged object (`serde_json::from_value`). */
#include "merge.h"
#include <stdlib.h>
#include <string.h>
#include "diff.h"
#include "file.h"
#include "georef.h"
#include "json_r.h"
#include "json_w.h"

void cc_conflicts_init(cc_conflicts *c) {
    memset(c, 0, sizeof *c);
    arena_init(&c->arena, 0);
}

void cc_conflicts_free(cc_conflicts *c) {
    free(c->v);
    arena_free(&c->arena);
    memset(c, 0, sizeof *c);
}

void cc_conflict_display(const cc_conflict *c, Buf *b) {
    buf_puts(b, c->kind);
    buf_putc(b, ' ');
    buf_puts(b, c->name);
    if (c->has_id) buf_puts(b, " ["), buf_puts(b, c->id), buf_putc(b, ']');
}

/* ---------------------------------------------------------------- context */

typedef struct {
    cc_prefer prefer;
    cc_conflicts *cf;
    const cc_model *ours;
    int oom;
} Ctx;

static char *adup(Arena *a, const char *p, size_t n) {
    char *s = arena_alloc(a, n + 1, 1);
    if (!s) return NULL;
    if (n) memcpy(s, p, n);
    s[n] = 0;
    return s;
}

/* Conflict { kind, id, name: "<name>" or "<name>: <path>" }. */
static void conflict(Ctx *c, const char *kind, const cc_gid *id, const char *name, size_t nn, const char *path,
                     size_t pn) {
    cc_conflicts *cf = c->cf;
    if (cf->n == cf->cap) {
        size_t cap = cf->cap ? 2 * cf->cap : 16;
        cc_conflict *v = realloc(cf->v, cap * sizeof *v);
        if (!v) {
            c->oom = 1;
            return;
        }
        cf->v = v, cf->cap = cap;
    }
    Buf b;
    buf_init(&b);
    buf_append(&b, name, nn);
    if (pn) buf_puts(&b, ": "), buf_append(&b, path, pn);
    cc_conflict *x = &cf->v[cf->n];
    memset(x, 0, sizeof *x);
    x->kind = kind;
    if (id) x->has_id = 1, memcpy(x->id, id->c, 22);
    x->name = adup(&cf->arena, b.oom ? "" : (b.p ? b.p : ""), b.oom ? 0 : b.len);
    if (b.oom || !x->name) c->oom = 1;
    else
        cf->n++;
    buf_free(&b);
}

/* ---------------------------------------------------------------- JSON values */

/* serde_json::Value's PartialEq (objects: the same keys and values, in any order). Numbers by
 * their token: one writer made every side's, and it writes one text per value. */
static int val_eq(const JrVal *a, const JrVal *b) {
    if (!a || !b) return !a && !b;
    if (a->type != b->type) return 0;
    switch (a->type) {
    case JR_NULL: return 1;
    case JR_BOOL: return a->b == b->b;
    case JR_NUM:
    case JR_STR: return a->n == b->n && !memcmp(a->s, b->s, a->n);
    case JR_ARR:
        if (a->n != b->n) return 0;
        for (size_t i = 0; i < a->n; i++)
            if (!val_eq(&a->items[i], &b->items[i])) return 0;
        return 1;
    case JR_OBJ:
        if (a->n != b->n) return 0;
        for (size_t i = 0; i < a->n; i++) {
            const JrMember *m = &a->members[i];
            const JrVal *o = NULL;
            for (size_t j = 0; j < b->n && !o; j++) {
                size_t k = (i + j) % b->n; /* the same place first: one writer, one order */
                if (b->members[k].key_n == m->key_n && !memcmp(b->members[k].key, m->key, m->key_n))
                    o = &b->members[k].val;
            }
            if (!o || !val_eq(&m->val, o)) return 0;
        }
        return 1;
    }
    return 0;
}

static const JrVal *getk(const JrVal *o, const char *k, size_t kn) {
    if (!o || o->type != JR_OBJ) return NULL;
    for (size_t i = 0; i < o->n; i++)
        if (o->members[i].key_n == kn && !memcmp(o->members[i].key, k, kn)) return &o->members[i].val;
    return NULL;
}

/* repo.rs pick: (value, conflict). */
static const JrVal *pickv(const JrVal *b, const JrVal *o, const JrVal *t, cc_prefer p, int *conflict) {
    *conflict = 0;
    if (val_eq(o, t) || val_eq(t, b)) return o;
    if (val_eq(o, b)) return t;
    *conflict = 1;
    return p == CC_PREFER_THEIRS ? t : o;
}

/* repo.rs same_variant: the "kind" and "type" tags (when strings) equal on both. */
static int same_variant(const JrVal *a, const JrVal *b) {
    static const char *const K[] = {"kind", "type"};
    for (int i = 0; i < 2; i++) {
        const JrVal *x = getk(a, K[i], 4), *y = getk(b, K[i], 4);
        if (x && x->type != JR_STR) x = NULL;
        if (y && y->type != JR_STR) y = NULL;
        if (!x || !y) {
            if (x || y) return 0;
            continue;
        }
        if (x->n != y->n || memcmp(x->s, y->s, x->n)) return 0;
    }
    return 1;
}

typedef struct {
    char **v;
    size_t *len;
    size_t n, cap;
    int oom;
} Paths;

static void paths_add(Paths *ps, const char *p, size_t n) {
    if (ps->n == ps->cap) {
        size_t cap = ps->cap ? 2 * ps->cap : 8;
        char **v = realloc(ps->v, cap * sizeof *v);
        if (v) ps->v = v;
        size_t *l = realloc(ps->len, cap * sizeof *l);
        if (l) ps->len = l;
        if (!v || !l) {
            ps->oom = 1;
            return;
        }
        ps->cap = cap;
    }
    char *s = malloc(n + 1);
    if (!s) {
        ps->oom = 1;
        return;
    }
    if (n) memcpy(s, p, n);
    s[n] = 0;
    ps->v[ps->n] = s, ps->len[ps->n++] = n;
}

static void paths_clear(Paths *ps) {
    for (size_t i = 0; i < ps->n; i++) free(ps->v[i]);
    ps->n = 0;
}

static void paths_free(Paths *ps) {
    paths_clear(ps);
    free(ps->v), free(ps->len);
}

typedef struct {
    const char *k;
    size_t n;
} Key;

static int key_cmp(const void *x, const void *y) {
    const Key *a = x, *b = y;
    int c = memcmp(a->k, b->k, a->n < b->n ? a->n : b->n);
    return c ? c : (a->n < b->n ? -1 : a->n > b->n);
}

/* repo.rs merge_value: writes the merged value to `out` and returns 1, or 0 for none (removed).
 * `path` is the dotted path so far (restored on return). */
/* WAVE1B (B4, DEVIATIONS 28): an element's `openings`, merged opening by opening (matched by their
 * GlobalIds, as objects are): one added on one side and the host changed on the other merge; the
 * same opening changed on both sides differently is a conflict on `openings.<id>.<field>`. Only when
 * every opening of the three sides has an id (else the list is picked whole, as Rust does). 1 when a
 * value was written, 0 when the merged list is empty (the key left out, as the file does), -1 not
 * this case. */
static const JrVal *op_by_id(const JrVal *arr, const JrVal *id) {
    for (size_t i = 0; arr && i < arr->n; i++) {
        const JrVal *x = getk(&arr->items[i], "id", 2);
        if (x && val_eq(x, id)) return &arr->items[i];
    }
    return NULL;
}
static int merge_value(Ctx *c, const JrVal *b, const JrVal *o, const JrVal *t, Buf *path, Buf *out, Paths *ps);
static int merge_openings(Ctx *c, const JrVal *b, const JrVal *o, const JrVal *t, Buf *path, Buf *out, Paths *ps) {
    const JrVal *s3[3] = {b, o, t};
    for (int k = 0; k < 3; k++) {
        if (s3[k] && s3[k]->type != JR_ARR) return -1;
        for (size_t i = 0; s3[k] && i < s3[k]->n; i++) {
            const JrVal *id = getk(&s3[k]->items[i], "id", 2);
            if (s3[k]->items[i].type != JR_OBJ || !id || id->type != JR_STR) return -1;
        }
    }
    size_t plen = path->len, mark = out->len;
    int n = 0;
    buf_putc(out, '[');
    for (int side = 0; side < 2; side++) { /* ours' order, then the ones only theirs has */
        const JrVal *arr = side ? t : o;
        for (size_t i = 0; arr && i < arr->n; i++) {
            const JrVal *id = getk(&arr->items[i], "id", 2);
            if (side && op_by_id(o, id)) continue;
            const JrVal *bo = op_by_id(b, id), *oo = op_by_id(o, id), *to = op_by_id(t, id);
            if (plen) buf_putc(path, '.');
            buf_append(path, id->s, id->n);
            size_t at = out->len;
            if (n) buf_putc(out, ',');
            if (merge_value(c, bo, oo, to, path, out, ps)) n++;
            else out->len = at;
            path->len = plen;
        }
    }
    buf_putc(out, ']');
    if (!n) out->len = mark;
    return n ? 1 : 0;
}

static int merge_value(Ctx *c, const JrVal *b, const JrVal *o, const JrVal *t, Buf *path, Buf *out, Paths *ps) {
    if (path->len == 8 && !memcmp(path->p, "openings", 8) && !(val_eq(o, t))) { /* WAVE1B (B4) */
        int r = merge_openings(c, b, o, t, path, out, ps);
        if (r >= 0) return r;
    }
    if (b && o && t && b->type == JR_OBJ && o->type == JR_OBJ && t->type == JR_OBJ && same_variant(b, o) &&
        same_variant(o, t) && !val_eq(o, t)) {
        size_t nk = b->n + o->n + t->n, u = 0;
        Key *ks = malloc((nk ? nk : 1) * sizeof *ks);
        if (!ks) return c->oom = 1, 0;
        const JrVal *sides[3] = {b, o, t};
        for (int s = 0; s < 3; s++)
            for (size_t i = 0; i < sides[s]->n; i++) ks[u++] = (Key){sides[s]->members[i].key, sides[s]->members[i].key_n};
        qsort(ks, u, sizeof *ks, key_cmp);
        size_t plen = path->len;
        int first = 1;
        buf_putc(out, '{');
        for (size_t i = 0; i < u; i++) {
            if (i && !key_cmp(&ks[i], &ks[i - 1])) continue; /* a BTreeSet: once each */
            if (plen) buf_putc(path, '.');
            buf_append(path, ks[i].k, ks[i].n);
            size_t mark = out->len;
            if (!first) buf_putc(out, ',');
            json_escape(out, ks[i].k, ks[i].n);
            buf_putc(out, ':');
            if (merge_value(c, getk(b, ks[i].k, ks[i].n), getk(o, ks[i].k, ks[i].n), getk(t, ks[i].k, ks[i].n), path, out,
                            ps))
                first = 0;
            else
                out->len = mark;
            path->len = plen;
        }
        buf_putc(out, '}');
        free(ks);
        return 1;
    }
    int conf;
    const JrVal *v = pickv(b, o, t, c->prefer, &conf);
    if (conf) paths_add(ps, path->p ? path->p : "", path->len);
    if (!v) return 0;
    cc_json_value_write(v, out);
    return 1;
}

/* ---------------------------------------------------------------- objects */

static size_t count_of(const cc_model *m, int kind) {
    switch (kind) {
    case CC_OBJ_STOREY: return m->nstoreys;
    case CC_OBJ_MATERIAL: return m->nmaterials;
    case CC_OBJ_ELEMENT: return m->nelements;
    case CC_OBJ_VIEW: return m->nviews;
    case CC_OBJ_SHEET: return m->nsheets;
    case CC_OBJ_RELATION: return m->nrelations;
    default: return m->ngrids;
    }
}

static const cc_gid *id_of(const cc_model *m, int kind, size_t i) {
    switch (kind) {
    case CC_OBJ_STOREY: return &m->storeys[i].id;
    case CC_OBJ_MATERIAL: return &m->materials[i].id;
    case CC_OBJ_ELEMENT: return &m->elements[i].id;
    case CC_OBJ_VIEW: return &m->views[i].id;
    case CC_OBJ_SHEET: return &m->sheets[i].id;
    case CC_OBJ_RELATION: return &m->relations[i].id;
    default: return &m->grids[i].id;
    }
}

static void put_str(Buf *b, cc_str s) { buf_append(b, s.p, s.n); }

/* The name a conflict shows (repo.rs:791-857's closures; relations and grids: DEVIATIONS 28). */
static void name_of(const cc_model *m, int kind, size_t i, Buf *b) {
    switch (kind) {
    case CC_OBJ_STOREY: put_str(b, m->storeys[i].name); return;
    case CC_OBJ_MATERIAL: put_str(b, m->materials[i].name); return;
    case CC_OBJ_ELEMENT: {
        const cc_element *e = &m->elements[i];
        if (e->has_ifc_entity) put_str(b, e->ifc_entity);
        else
            buf_puts(b, cc_ifc_class_name(e->cls));
        buf_putc(b, ' ');
        put_str(b, e->name);
        return;
    }
    case CC_OBJ_VIEW: put_str(b, m->views[i].name); return;
    case CC_OBJ_SHEET:
        put_str(b, m->sheets[i].number);
        buf_putc(b, ' ');
        put_str(b, m->sheets[i].name);
        return;
    case CC_OBJ_RELATION: put_str(b, m->relations[i].kind); return;
    default: put_str(b, m->grids[i].name); return;
    }
}

static const char *const SECTION[] = {"storeys", "materials", "elements", "views", "sheets", "relations", "grids"};
static const char *const KIND[] = {"IfcBuildingStorey", "IfcMaterial", "element", "view", "sheet", "relation", "IfcGrid"};

/* serde_json::from_value::<T>: the merged object decodes as one of its kind (a model holding it
 * alone, decoded without validation). */
static int decodes(const Ctx *c, int kind, const cc_gid *id, const char *json, size_t n) {
    Buf t;
    buf_init(&t);
    buf_puts(&t, "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"");
    buf_append(&t, c->ours->project_id.c, 22);
    buf_puts(&t, "\",\"project_name\":\"\"");
    for (int k = 0; k < 7; k++) {
        if (k != kind && k > CC_OBJ_ELEMENT) continue;
        buf_puts(&t, ",\"");
        buf_puts(&t, SECTION[k]);
        buf_puts(&t, "\":{");
        if (k == kind) buf_putc(&t, '"'), buf_append(&t, id->c, 22), buf_puts(&t, "\":"), buf_append(&t, json, n);
        buf_putc(&t, '}');
    }
    buf_puts(&t, "}}");
    int ok = 0;
    if (!t.oom) {
        cc_model m;
        ok = !cc_model_load_unchecked(&m, t.p, t.len, NULL);
        cc_model_free(&m);
    }
    buf_free(&t);
    return ok;
}

static int same(const Buf *a, int ha, const Buf *b, int hb) {
    if (!ha || !hb) return !ha && !hb;
    return a->len == b->len && !memcmp(a->p, b->p, a->len);
}

/* repo.rs map: one section, object by object; `"<id>":<json>` entries appended to `out`. */
static int merge_map(Ctx *c, int kind, const cc_model *B, const cc_model *O, const cc_model *T, Buf *out, Buf *err) {
    const cc_model *M[3] = {B, O, T};
    size_t at[3] = {0, 0, 0}, cnt[3];
    for (int s = 0; s < 3; s++) cnt[s] = count_of(M[s], kind);
    Buf js[3], mv, path, nm;
    for (int s = 0; s < 3; s++) buf_init(&js[s]);
    buf_init(&mv), buf_init(&path), buf_init(&nm);
    Paths ps = {0};
    int rc = -1, first = 1;
    for (;;) {
        const cc_gid *id = NULL;
        for (int s = 0; s < 3; s++)
            if (at[s] < cnt[s]) {
                const cc_gid *g = id_of(M[s], kind, at[s]);
                if (!id || cc_gid_cmp(g, id) < 0) id = g;
            }
        if (!id) break;
        cc_gid key = *id;
        int has[3];
        size_t idx[3];
        for (int s = 0; s < 3; s++) {
            has[s] = at[s] < cnt[s] && !cc_gid_cmp(id_of(M[s], kind, at[s]), &key);
            idx[s] = at[s];
            buf_clear(&js[s]);
            if (has[s] && cc_model_object_json(M[s], kind, at[s], NULL, &js[s])) {
                if (err) buf_clear(err), buf_puts(err, "merge: an object does not serialize");
                goto done;
            }
        }
        for (int s = 0; s < 3; s++)
            if (has[s]) at[s]++;
        /* pick on the whole object */
        int take = -1; /* side whose version is kept; -1: none */
        int conf = 0;
        if (same(&js[1], has[1], &js[2], has[2]) || same(&js[2], has[2], &js[0], has[0])) take = 1;
        else if (same(&js[1], has[1], &js[0], has[0]))
            take = 2;
        else
            conf = 1, take = c->prefer == CC_PREFER_THEIRS ? 2 : 1;
        const char *val = NULL;
        size_t vn = 0;
        if (!conf) {
            if (has[take]) val = js[take].p, vn = js[take].len;
        } else {
            /* Both changed it: merge field by field. */
            Arena a;
            arena_init(&a, 0);
            const JrVal *dom[3] = {NULL, NULL, NULL};
            for (int s = 0; s < 3; s++) {
                JrError je;
                if (has[s] && !(dom[s] = jr_parse(&a, js[s].p, js[s].len, &je))) {
                    arena_free(&a);
                    if (err) buf_clear(err), buf_puts(err, "merge: an object does not read back: "), buf_puts(err, je.msg);
                    goto done;
                }
            }
            paths_clear(&ps);
            buf_clear(&mv), buf_clear(&path);
            int present = merge_value(c, dom[0], dom[1], dom[2], &path, &mv, &ps);
            arena_free(&a);
            if (c->oom || ps.oom || mv.oom || path.oom) goto oom;
            int s = has[1] ? 1 : has[2] ? 2 : 0;
            buf_clear(&nm);
            name_of(M[s], kind, idx[s], &nm);
            if (present && !decodes(c, kind, &key, mv.p, mv.len)) {
                /* A field merge that does not make a valid object again counts as one conflict. */
                paths_clear(&ps);
                paths_add(&ps, "", 0);
                present = has[take];
                if (present) buf_clear(&mv), buf_append(&mv, js[take].p, js[take].len);
            }
            for (size_t i = 0; i < ps.n; i++)
                conflict(c, KIND[kind], &key, nm.p ? nm.p : "", nm.len, ps.v[i], ps.len[i]);
            if (present) val = mv.p, vn = mv.len;
        }
        if (val) {
            if (!first) buf_putc(out, ',');
            first = 0;
            buf_putc(out, '"');
            buf_append(out, key.c, 22);
            buf_puts(out, "\":");
            buf_append(out, val, vn);
        }
        if (c->oom || out->oom) goto oom;
    }
    rc = 0;
    goto done;
oom:
    if (err) buf_clear(err), buf_puts(err, "merge: out of memory");
done:
    for (int s = 0; s < 3; s++) buf_free(&js[s]);
    buf_free(&mv), buf_free(&path), buf_free(&nm);
    paths_free(&ps);
    return rc;
}

/* ---------------------------------------------------------------- the model */

static int str_eq(cc_str a, cc_str b) { return a.n == b.n && (!a.n || !memcmp(a.p, b.p, a.n)); }

/* pick on optional strings: the index of the side taken (0 base, 1 ours, 2 theirs), *conf. */
static int pick_str(const cc_str *b, const cc_str *o, const cc_str *t, cc_prefer p, int *conf) {
#define EQ(x, y) ((!(x) || !(y)) ? (!(x) && !(y)) : str_eq(*(x), *(y)))
    *conf = 0;
    if (EQ(o, t) || EQ(t, b)) return 1;
    if (EQ(o, b)) return 2;
#undef EQ
    *conf = 1;
    return p == CC_PREFER_THEIRS ? 2 : 1;
}

static int kv_cmp(cc_str a, cc_str b) {
    int c = memcmp(a.p, b.p, a.n < b.n ? a.n : b.n);
    return c ? c : (a.n < b.n ? -1 : a.n > b.n);
}

/* Project information, key by key: `"k":"v"` entries into `out`. */
static void merge_project(Ctx *c, const cc_model *const M[3], Buf *out) {
    size_t at[3] = {0, 0, 0};
    int first = 1;
    for (;;) {
        const cc_str *k = NULL;
        for (int s = 0; s < 3; s++)
            if (at[s] < M[s]->nproject && (!k || kv_cmp(M[s]->project[at[s]].key, *k) < 0)) k = &M[s]->project[at[s]].key;
        if (!k) break;
        cc_str key = *k;
        const cc_str *v[3] = {NULL, NULL, NULL};
        for (int s = 0; s < 3; s++)
            if (at[s] < M[s]->nproject && !kv_cmp(M[s]->project[at[s]].key, key)) v[s] = &M[s]->project[at[s]++].value;
        int conf, take = pick_str(v[0], v[1], v[2], c->prefer, &conf);
        if (conf) {
            Buf n;
            buf_init(&n);
            buf_puts(&n, "project information ");
            buf_append(&n, key.p, key.n);
            conflict(c, "IfcProject", NULL, n.p ? n.p : "", n.oom ? 0 : n.len, "", 0);
            buf_free(&n);
        }
        if (v[take]) {
            if (!first) buf_putc(out, ',');
            first = 0;
            json_escape(out, key.n ? key.p : "", key.n);
            buf_putc(out, ':');
            json_escape(out, v[take]->n ? v[take]->p : "", v[take]->n);
        }
    }
}

/* Schedules by name (the order names first appear in base, ours, theirs), each picked whole. */
static int merge_schedules(Ctx *c, const cc_model *const M[3], Buf *out, Buf *err) {
    Arena a;
    arena_init(&a, 0);
    Buf js;
    buf_init(&js);
    const JrVal *list[3] = {NULL, NULL, NULL};
    int rc = -1, first = 1;
    for (int s = 0; s < 3; s++) {
        if (!M[s]->nschedules) continue;
        buf_clear(&js);
        JrError je;
        if (cc_model_object_json(M[s], CC_OBJ_SCHEDULES, 0, NULL, &js) || js.oom ||
            !(list[s] = jr_parse(&a, js.p, js.len, &je)) || list[s]->type != JR_ARR) {
            if (err) buf_clear(err), buf_puts(err, "merge: the schedules do not serialize");
            goto done;
        }
    }
    for (int s = 0; s < 3; s++) {
        for (size_t i = 0; list[s] && i < list[s]->n; i++) {
            const JrVal *nv = getk(&list[s]->items[i], "name", 4);
            if (!nv || nv->type != JR_STR) continue;
            /* a name seen before (in an earlier side, or earlier in this one) is done */
            int seen = 0;
            for (int s2 = 0; s2 <= s && !seen; s2++)
                for (size_t j = 0; list[s2] && j < (s2 == s ? i : list[s2]->n) && !seen; j++) {
                    const JrVal *x = getk(&list[s2]->items[j], "name", 4);
                    seen = x && x->type == JR_STR && x->n == nv->n && !memcmp(x->s, nv->s, nv->n);
                }
            if (seen) continue;
            const JrVal *v[3] = {NULL, NULL, NULL};
            for (int s3 = 0; s3 < 3; s3++)
                for (size_t j = 0; list[s3] && j < list[s3]->n && !v[s3]; j++) {
                    const JrVal *x = getk(&list[s3]->items[j], "name", 4);
                    if (x && x->type == JR_STR && x->n == nv->n && !memcmp(x->s, nv->s, nv->n)) v[s3] = &list[s3]->items[j];
                }
            int conf;
            const JrVal *take = pickv(v[0], v[1], v[2], c->prefer, &conf);
            if (conf) conflict(c, "schedule", NULL, nv->s, nv->n, "", 0);
            if (take) {
                if (!first) buf_putc(out, ',');
                first = 0;
                cc_json_value_write(take, out);
            }
        }
    }
    rc = 0;
done:
    buf_free(&js);
    arena_free(&a);
    return rc;
}

int cc_merge_models(const cc_model *base, const cc_model *ours, const cc_model *theirs, cc_prefer prefer,
                    cc_model *out, cc_conflicts *conflicts, Buf *err) {
    cc_model_init(out);
    Ctx c = {prefer, conflicts, ours, 0};
    const cc_model *const M[3] = {base, ours, theirs};
    Buf t, sec, g0, g1;
    buf_init(&t), buf_init(&sec), buf_init(&g0), buf_init(&g1);
    int rc = -1;
    /* project_name (repo.rs:744), then the project information, then schedules: Rust's order. */
    int conf, take = pick_str(&base->project_name, &ours->project_name, &theirs->project_name, prefer, &conf);
    if (conf) conflict(&c, "IfcProject", NULL, "project name", 12, "", 0);
    buf_puts(&t, "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"");
    buf_append(&t, ours->project_id.c, 22);
    buf_puts(&t, "\",\"project_name\":");
    json_escape(&t, M[take]->project_name.n ? M[take]->project_name.p : "", M[take]->project_name.n);
    buf_clear(&sec);
    merge_project(&c, M, &sec);
    if (sec.len) buf_puts(&t, ",\"project\":{"), buf_append(&t, sec.p, sec.len), buf_putc(&t, '}');
    buf_clear(&sec);
    if (merge_schedules(&c, M, &sec, err)) goto done;
    if (sec.len) buf_puts(&t, ",\"schedules\":["), buf_append(&t, sec.p, sec.len), buf_putc(&t, ']');
    /* The georeference: whichever side changed it (ours when both did). */
    if (cc_model_object_json(ours, CC_OBJ_GEOREF, 0, NULL, &g1) || cc_model_object_json(base, CC_OBJ_GEOREF, 0, NULL, &g0)) {
        if (err) buf_clear(err), buf_puts(err, "merge: the georeference does not serialize");
        goto done;
    }
    const cc_model *geo = g0.len != g1.len || memcmp(g0.p, g1.p, g0.len) ? ours : theirs;
    if (!cc_georef_is_default(&geo->georef)) {
        buf_puts(&t, ",\"georef\":");
        if (cc_model_object_json(geo, CC_OBJ_GEOREF, 0, NULL, &t)) {
            if (err) buf_clear(err), buf_puts(err, "merge: the georeference does not serialize");
            goto done;
        }
    }
    /* The sections, Rust's first (storeys, materials, elements, views, sheets), then the C ones. */
    for (int k = 0; k < 7; k++) {
        buf_clear(&sec);
        if (merge_map(&c, k, base, ours, theirs, &sec, err)) goto done;
        if (k > CC_OBJ_ELEMENT && !sec.len) continue;
        buf_puts(&t, ",\"");
        buf_puts(&t, SECTION[k]);
        buf_puts(&t, "\":{");
        buf_append(&t, sec.p, sec.len);
        buf_putc(&t, '}');
    }
    buf_puts(&t, "}}");
    if (t.oom || sec.oom || c.oom) {
        if (err) buf_clear(err), buf_puts(err, "merge: out of memory");
        goto done;
    }
    cc_error e;
    if (cc_model_load_unchecked(out, t.p, t.len, &e)) {
        if (err) buf_clear(err), buf_puts(err, "merge: the merged model does not decode: "), buf_puts(err, e.msg);
        goto done;
    }
    rc = 0;
done:
    buf_free(&t), buf_free(&sec), buf_free(&g0), buf_free(&g1);
    return rc;
}
