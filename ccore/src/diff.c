/* Element-level diff (diff.h; crates/bimble-core/src/version.rs:127-334). Each model is written
 * as its file (the serde form of every object) and read back as a JSON DOM: the field changes
 * are then the same walk over the same values as Rust's `serde_json::to_value`. Numbers compare
 * by their token, which the writer makes canonical (serde's text of the same f64). */
#include "diff.h"
#include <stdlib.h>
#include <string.h>
#include "file.h"

/* arena_dup, 8-aligned (arrays of structs with pointers). */
static void *dup8(Arena *a, const void *src, size_t n) {
    void *p = arena_alloc(a, n, 8);
    if (p) memcpy(p, src, n);
    return p;
}

void cc_changes_init(cc_changes *c) {
    memset(c, 0, sizeof *c);
    arena_init(&c->arena, 0);
}

void cc_changes_free(cc_changes *c) {
    free(c->v);
    arena_free(&c->arena);
    memset(c, 0, sizeof *c);
}

static const JrVal NULLV = {.type = JR_NULL};
static const JrVal EMPTY_ARR = {.type = JR_ARR};
static const JrVal EMPTY_OBJ = {.type = JR_OBJ};

/* ---------------------------------------------------------------- values */

/* serde_json::Value's PartialEq (objects: the same keys and values, in any order). */
static int val_eq(const JrVal *a, const JrVal *b) {
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
            /* Both sides come from one writer: the same key at the same place, unless the
             * objects differ; the search is only the fallback (a large property map stays linear). */
            if (b->members[i].key_n == m->key_n && !memcmp(b->members[i].key, m->key, m->key_n))
                o = &b->members[i].val;
            for (size_t j = 0; j < b->n && !o; j++)
                if (b->members[j].key_n == m->key_n && !memcmp(b->members[j].key, m->key, m->key_n))
                    o = &b->members[j].val;
            if (!o || !val_eq(&m->val, o)) return 0;
        }
        return 1;
    }
    return 0;
}

static int key_cmp(const char *a, size_t an, const char *b, size_t bn) {
    int c = memcmp(a, b, an < bn ? an : bn);
    return c ? c : (an < bn ? -1 : an > bn);
}

static int member_cmp(const void *x, const void *y) {
    const JrMember *a = *(const JrMember *const *)x, *b = *(const JrMember *const *)y;
    return key_cmp(a->key, a->key_n, b->key, b->key_n);
}

/* The members of an object sorted by key bytes (a BTreeMap's order), malloc'd. */
static const JrMember **sorted_members(const JrVal *o) {
    const JrMember **v = malloc((o->n ? o->n : 1) * sizeof *v);
    if (!v) return NULL;
    for (size_t i = 0; i < o->n; i++) v[i] = &o->members[i];
    qsort(v, o->n, sizeof *v, member_cmp);
    return v;
}

void cc_json_value_write(const JrVal *v, Buf *b) {
    switch (v->type) {
    case JR_NULL: buf_puts(b, "null"); return;
    case JR_BOOL: buf_puts(b, v->b ? "true" : "false"); return;
    case JR_NUM: buf_append(b, v->s, v->n); return;
    case JR_STR: json_escape(b, v->s, v->n); return;
    case JR_ARR:
        buf_putc(b, '[');
        for (size_t i = 0; i < v->n; i++) {
            if (i) buf_putc(b, ',');
            cc_json_value_write(&v->items[i], b);
        }
        buf_putc(b, ']');
        return;
    case JR_OBJ: {
        const JrMember **m = sorted_members(v);
        if (!m) {
            b->oom = 1;
            return;
        }
        buf_putc(b, '{');
        for (size_t i = 0; i < v->n; i++) {
            if (i) buf_putc(b, ',');
            json_escape(b, m[i]->key, m[i]->key_n);
            buf_putc(b, ':');
            cc_json_value_write(&m[i]->val, b);
        }
        buf_putc(b, '}');
        free(m);
        return;
    }
    }
}

static void value_jw(const JrVal *v, JsonW *w) {
    switch (v->type) {
    case JR_NULL: jw_null(w); return;
    case JR_BOOL: jw_bool(w, v->b); return;
    case JR_NUM:
        if (v->num_int) { /* the token's digits (a JSON integer from the model writer fits 64 bits) */
            uint64_t u = 0;
            for (size_t i = v->num_neg; i < v->n; i++) u = u * 10 + (uint64_t)(v->s[i] - '0');
            if (v->num_neg) jw_i64(w, u > (uint64_t)INT64_MAX ? INT64_MIN : -(int64_t)u);
            else
                jw_u64(w, u);
        } else
            jw_f64(w, v->num);
        return;
    case JR_STR: jw_str_n(w, v->s, v->n); return;
    case JR_ARR:
        jw_arr_begin(w);
        for (size_t i = 0; i < v->n; i++) value_jw(&v->items[i], w);
        jw_arr_end(w);
        return;
    case JR_OBJ: {
        const JrMember **m = sorted_members(v);
        if (!m) {
            w->err = JW_ERR_OOM;
            return;
        }
        jw_obj_begin(w);
        for (size_t i = 0; i < v->n; i++) {
            jw_key_n(w, m[i]->key, m[i]->key_n);
            value_jw(&m[i]->val, w);
        }
        jw_obj_end(w);
        free(m);
        return;
    }
    }
}

/* ---------------------------------------------------------------- building the list */

typedef struct {
    cc_changes *out;
    cc_field_change *f; /* the fields of the change being built (malloc'd) */
    size_t nf, capf;
    int oom;
} D;

static const char *astr(D *d, const char *s, size_t n) {
    char *z = arena_alloc(&d->out->arena, n + 1, 1);
    if (!z) return d->oom = 1, "";
    memcpy(z, s, n);
    z[n] = 0;
    return z;
}

static const JrVal *astrval(D *d, const char *s, size_t n) {
    JrVal *v = arena_alloc(&d->out->arena, sizeof *v, 8);
    if (!v) return d->oom = 1, &NULLV;
    v->type = JR_STR;
    v->s = astr(d, s, n);
    v->n = n;
    return v;
}

static void field(D *d, const char *path, size_t pn, const JrVal *a, const JrVal *b) {
    if (d->nf == d->capf) {
        size_t c = d->capf ? 2 * d->capf : 8;
        cc_field_change *f = realloc(d->f, c * sizeof *f);
        if (!f) {
            d->oom = 1;
            return;
        }
        d->f = f, d->capf = c;
    }
    d->f[d->nf++] = (cc_field_change){astr(d, path, pn), a, b};
}

static void push(D *d, int change, const char *kind, const char *id, const char *name, size_t name_n) {
    cc_changes *o = d->out;
    if (o->n == o->cap) {
        size_t c = o->cap ? 2 * o->cap : 16;
        cc_change *v = realloc(o->v, c * sizeof *v);
        if (!v) {
            d->oom = 1;
            d->nf = 0;
            return;
        }
        o->v = v, o->cap = c;
    }
    cc_change *ch = &o->v[o->n++];
    memset(ch, 0, sizeof *ch);
    ch->change = change;
    ch->kind = astr(d, kind, strlen(kind));
    memcpy(ch->id, id, 22);
    ch->name = astr(d, name, name_n);
    if (d->nf) {
        ch->fields = dup8(&o->arena, d->f, d->nf * sizeof *d->f);
        if (!ch->fields) d->oom = 1;
        else
            ch->nfields = d->nf;
    }
    d->nf = 0;
}

static const JrVal *get_or(const JrVal *o, const char *key, const JrVal *dflt) {
    const JrVal *v = o && o->type == JR_OBJ ? jr_get(o, key) : NULL;
    return v ? v : dflt;
}

static const JrVal *get_n(const JrVal *o, const char *key, size_t n) {
    if (!o || o->type != JR_OBJ) return NULL;
    for (size_t i = 0; i < o->n; i++)
        if (o->members[i].key_n == n && !memcmp(o->members[i].key, key, n)) return &o->members[i].val;
    return NULL;
}

/* version.rs diff_value. `path` is a Buf grown and shrunk around each step. */
/* WAVE1B (B4): an element's openings matched by their GlobalIds ("openings.<id>.<field>"; one added:
 * "openings.<id>" null -> its value) when every opening of both sides has one; else by index, as
 * Rust. 1 done, 0 not this case. */
static const JrVal *d_op(const JrVal *arr, const JrVal *id) {
    for (size_t i = 0; i < arr->n; i++) {
        const JrVal *x = jr_get(&arr->items[i], "id");
        if (x && val_eq(x, id)) return &arr->items[i];
    }
    return NULL;
}
static void diff_value(D *d, Buf *path, const JrVal *a, const JrVal *b);
static int diff_openings(D *d, Buf *path, const JrVal *a, const JrVal *b) {
    const JrVal *s2[2] = {a, b};
    for (int k = 0; k < 2; k++) {
        if (s2[k]->type != JR_ARR) return 0;
        for (size_t i = 0; i < s2[k]->n; i++) {
            const JrVal *id = s2[k]->items[i].type == JR_OBJ ? jr_get(&s2[k]->items[i], "id") : NULL;
            if (!id || id->type != JR_STR) return 0;
        }
    }
    size_t at = path->len;
    for (int side = 0; side < 2; side++)
        for (size_t i = 0; i < s2[side]->n; i++) {
            const JrVal *id = jr_get(&s2[side]->items[i], "id");
            if (side && d_op(a, id)) continue;
            const JrVal *x = d_op(a, id), *y = d_op(b, id);
            buf_putc(path, '.');
            buf_append(path, id->s, id->n);
            diff_value(d, path, x ? x : &NULLV, y ? y : &NULLV);
            path->len = at;
        }
    return 1;
}

static void diff_value(D *d, Buf *path, const JrVal *a, const JrVal *b) {
    if (val_eq(a, b)) return;
    size_t at = path->len;
    if (at == 8 && !memcmp(path->p, "openings", 8) && diff_openings(d, path, a->type == JR_NULL ? &EMPTY_ARR : a,
                                                                       b->type == JR_NULL ? &EMPTY_ARR : b))
        return; /* WAVE1B (B4) */
    if (a->type == JR_OBJ && b->type == JR_OBJ) {
        const JrVal *ka = jr_get(a, "kind"), *kb = jr_get(b, "kind");
        if ((!ka && !kb) || (ka && kb && val_eq(ka, kb))) {
            /* The union of the keys, sorted. */
            const JrMember **ma = sorted_members(a), **mb = sorted_members(b);
            if (!ma || !mb) {
                free(ma), free(mb);
                d->oom = 1;
                return;
            }
            size_t i = 0, j = 0;
            while (i < a->n || j < b->n) {
                const JrMember *x = i < a->n ? ma[i] : NULL, *y = j < b->n ? mb[j] : NULL;
                int c = !x ? 1 : !y ? -1 : key_cmp(x->key, x->key_n, y->key, y->key_n);
                const JrMember *k = c <= 0 ? x : y;
                if (at) buf_putc(path, '.');
                buf_append(path, k->key, k->key_n);
                diff_value(d, path, c <= 0 ? &x->val : &NULLV, c >= 0 ? &y->val : &NULLV);
                path->len = at;
                if (c <= 0) i++;
                if (c >= 0) j++;
            }
            free(ma), free(mb);
            return;
        }
    } else if (a->type == JR_ARR && b->type == JR_ARR && a->n == b->n) {
        for (size_t i = 0; i < a->n; i++) {
            if (at) buf_putc(path, '.');
            buf_u64(path, i);
            diff_value(d, path, &a->items[i], &b->items[i]);
            path->len = at;
        }
        return;
    }
    field(d, path->p ? path->p : "", path->len, a, b);
}

enum { K_STOREY, K_MATERIAL, K_ELEMENT, K_VIEW, K_SHEET };

/* The kind and name a change shows for an object. */
static const char *obj_kind(int k, const JrVal *v) {
    switch (k) {
    case K_STOREY: return "IfcBuildingStorey";
    case K_MATERIAL: return "IfcMaterial";
    case K_VIEW: return "view";
    case K_SHEET: return "sheet";
    }
    const JrVal *e = get_or(v, "ifc_entity", NULL), *c = get_or(v, "class", NULL);
    if (e && e->type == JR_STR) return e->s;
    return c && c->type == JR_STR ? c->s : "";
}

static void obj_name(int k, const JrVal *v, Buf *b) {
    buf_clear(b);
    const JrVal *n = get_or(v, "name", NULL);
    if (k == K_SHEET) {
        const JrVal *num = get_or(v, "number", NULL);
        if (num && num->type == JR_STR) buf_append(b, num->s, num->n);
        buf_putc(b, ' ');
    }
    if (n && n->type == JR_STR) buf_append(b, n->s, n->n);
}

static void diff_map(D *d, const JrVal *a, const JrVal *b, int k) {
    const JrVal *ea = a && a->type == JR_OBJ ? a : &EMPTY_OBJ, *eb = b && b->type == JR_OBJ ? b : &EMPTY_OBJ;
    const JrMember **ma = sorted_members(ea), **mb = sorted_members(eb);
    Buf name, path;
    buf_init(&name), buf_init(&path);
    if (!ma || !mb) {
        d->oom = 1;
        goto done;
    }
    size_t i = 0, j = 0;
    while (i < ea->n || j < eb->n) {
        const JrMember *x = i < ea->n ? ma[i] : NULL, *y = j < eb->n ? mb[j] : NULL;
        int c = !x ? 1 : !y ? -1 : key_cmp(x->key, x->key_n, y->key, y->key_n);
        char id[23] = {0};
        const JrMember *km = c <= 0 ? x : y;
        memcpy(id, km->key, km->key_n < 22 ? km->key_n : 22);
        if (c < 0) {
            obj_name(k, &x->val, &name);
            push(d, CC_CHANGE_REMOVED, obj_kind(k, &x->val), id, name.p ? name.p : "", name.len);
            i++;
        } else if (c > 0) {
            obj_name(k, &y->val, &name);
            push(d, CC_CHANGE_ADDED, obj_kind(k, &y->val), id, name.p ? name.p : "", name.len);
            j++;
        } else {
            if (!val_eq(&x->val, &y->val)) {
                buf_clear(&path);
                diff_value(d, &path, &x->val, &y->val);
                obj_name(k, &y->val, &name);
                push(d, CC_CHANGE_MODIFIED, obj_kind(k, &y->val), id, name.p ? name.p : "", name.len);
            }
            i++, j++;
        }
    }
done:
    free(ma), free(mb);
    buf_free(&name), buf_free(&path);
}

/* The model as its file, read back: the "model" object, in `a`. */
static const JrVal *model_dom(const cc_model *m, Arena *a, Buf *err) {
    Buf b;
    buf_init(&b);
    if (cc_model_save(m, &b) || b.oom) {
        buf_free(&b);
        if (err) buf_clear(err), buf_puts(err, "the model does not serialize");
        return NULL;
    }
    char *text = arena_alloc(a, b.len + 1, 1);
    if (!text) {
        buf_free(&b);
        if (err) buf_clear(err), buf_puts(err, "out of memory");
        return NULL;
    }
    memcpy(text, b.p, b.len);
    size_t n = b.len;
    buf_free(&b);
    JrError je;
    const JrVal *root = jr_parse(a, text, n, &je);
    const JrVal *mv = root ? jr_get(root, "model") : NULL;
    if (!mv || mv->type != JR_OBJ) {
        if (err) buf_clear(err), buf_puts(err, "the model does not read back");
        return NULL;
    }
    return mv;
}

int cc_diff(const cc_model *ma, const cc_model *mb, cc_changes *out, Buf *err) {
    const JrVal *a = model_dom(ma, &out->arena, err);
    const JrVal *b = a ? model_dom(mb, &out->arena, err) : NULL;
    if (!a || !b) return -1;
    D d = {out, NULL, 0, 0, 0};
    char pid[23] = {0};
    const JrVal *pidv = jr_get(b, "project_id");
    if (pidv && pidv->type == JR_STR && pidv->n == 22) memcpy(pid, pidv->s, 22);
    const JrVal *pna = get_or(a, "project_name", &NULLV), *pnb = get_or(b, "project_name", &NULLV);
    if (!val_eq(pna, pnb)) {
        field(&d, "project_name", 12, pna, pnb);
        push(&d, CC_CHANGE_MODIFIED, "IfcProject", pid, pnb->type == JR_STR ? pnb->s : "",
             pnb->type == JR_STR ? pnb->n : 0);
    }
    const JrVal *sa = get_or(a, "schedules", &EMPTY_ARR), *sb = get_or(b, "schedules", &EMPTY_ARR);
    if (!val_eq(sa, sb)) {
        field(&d, "schedules", 9, sa, sb);
        push(&d, CC_CHANGE_MODIFIED, "schedule", pid, "schedules", 9);
    }
    const JrVal *pa = get_or(a, "project", &EMPTY_OBJ), *pb = get_or(b, "project", &EMPTY_OBJ);
    if (!val_eq(pa, pb) && pa->type == JR_OBJ && pb->type == JR_OBJ) {
        const JrMember **xa = sorted_members(pa), **xb = sorted_members(pb);
        Buf path;
        buf_init(&path);
        if (!xa || !xb) d.oom = 1;
        size_t i = 0, j = 0;
        while (!d.oom && (i < pa->n || j < pb->n)) {
            const JrMember *x = i < pa->n ? xa[i] : NULL, *y = j < pb->n ? xb[j] : NULL;
            int c = !x ? 1 : !y ? -1 : key_cmp(x->key, x->key_n, y->key, y->key_n);
            const JrVal *va = c <= 0 ? &x->val : NULL, *vb = c >= 0 ? &y->val : NULL;
            if (!va || !vb || !val_eq(va, vb)) {
                const JrMember *k = c <= 0 ? x : y;
                buf_clear(&path);
                buf_puts(&path, "project.");
                buf_append(&path, k->key, k->key_n);
                field(&d, path.p, path.len, va ? va : astrval(&d, "", 0), vb ? vb : astrval(&d, "", 0));
            }
            if (c <= 0) i++;
            if (c >= 0) j++;
        }
        free(xa), free(xb);
        buf_free(&path);
        push(&d, CC_CHANGE_MODIFIED, "IfcProject", pid, "project information", 19);
    }
    diff_map(&d, get_n(a, "storeys", 7), get_n(b, "storeys", 7), K_STOREY);
    diff_map(&d, get_n(a, "materials", 9), get_n(b, "materials", 9), K_MATERIAL);
    diff_map(&d, get_n(a, "elements", 8), get_n(b, "elements", 8), K_ELEMENT);
    diff_map(&d, get_n(a, "views", 5), get_n(b, "views", 5), K_VIEW);
    diff_map(&d, get_n(a, "sheets", 6), get_n(b, "sheets", 6), K_SHEET);
    free(d.f);
    if (d.oom) {
        if (err) buf_clear(err), buf_puts(err, "out of memory");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- output */

void cc_change_display(const cc_change *c, Buf *b) {
    buf_putc(b, (char)c->change);
    buf_putc(b, ' ');
    buf_puts(b, c->kind);
    buf_putc(b, ' ');
    buf_puts(b, c->name);
    buf_puts(b, " [");
    buf_append(b, c->id, strlen(c->id));
    buf_putc(b, ']');
    for (size_t i = 0; i < c->nfields; i++) {
        buf_puts(b, "\n    ");
        buf_puts(b, c->fields[i].path);
        buf_puts(b, ": ");
        cc_json_value_write(c->fields[i].before, b);
        buf_puts(b, " \xe2\x86\x92 ");
        cc_json_value_write(c->fields[i].after, b);
    }
}

void cc_version_change_json(const cc_change *c, JsonW *w) {
    jw_obj_begin(w);
    jw_key(w, "change");
    jw_str(w, c->change == CC_CHANGE_ADDED ? "added" : c->change == CC_CHANGE_REMOVED ? "removed" : "modified");
    jw_key(w, "kind");
    jw_str(w, c->kind);
    jw_key(w, "id");
    jw_str(w, c->id);
    jw_key(w, "name");
    jw_str(w, c->name);
    if (c->change == CC_CHANGE_MODIFIED) {
        jw_key(w, "fields");
        jw_arr_begin(w);
        for (size_t i = 0; i < c->nfields; i++) {
            jw_obj_begin(w);
            jw_key(w, "path");
            jw_str(w, c->fields[i].path);
            jw_key(w, "before");
            value_jw(c->fields[i].before, w);
            jw_key(w, "after");
            value_jw(c->fields[i].after, w);
            jw_obj_end(w);
        }
        jw_arr_end(w);
    }
    jw_obj_end(w);
}
