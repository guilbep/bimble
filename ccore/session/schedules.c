/* Schedules in the session (ticket Q2): see schedules.h. */
#ifndef CC_VIEW_ONLY
/* quantity.h's function cc_measure has the name of measure.h's tool type (session.h): renamed
 * here, unused. */
#define cc_measure cc_quantity_measure_
#include "quantity.h"
#undef cc_measure
#endif
#include "schedules.h"
#include <stdlib.h>
#include <string.h>
#include "calls.h"
#include "json_r.h"
#include "json_w.h"
#include "session.h"

#ifdef CC_VIEW_ONLY
#include "view_only.h"
void cc_schedules_cache_free(struct cc_session *s) { (void)s; }
int cc_schedules_query(struct cc_session *s, const char *name, const char *body, size_t n, Buf *out) {
    (void)s, (void)body, (void)n;
    if (!strcmp(name, "schedule_list")) return buf_puts(out, "[]"), CC_OK;
    if (!strcmp(name, "schedule_rows") || !strcmp(name, "schedule_csv") || !strcmp(name, "schedule_columns") /* QTY1 */)
        return buf_puts(out, CC_VIEW_ONLY_TEXT), CC_ERR;
    return -1;
}
#else

/* The table of the last (rev, name) asked, with the META it was made with (kept for the JSON). */
struct cc_sched_cache {
    uint64_t rev;
    char *name; /* malloc'd */
    cc_sched_table t;
    cc_viewfile_meta mt;
};

void cc_schedules_cache_free(struct cc_session *s) {
    struct cc_sched_cache *c = s->sched;
    if (!c) return;
    free(c->name);
    cc_sched_table_free(&c->t);
    cc_viewfile_meta_free(&c->mt);
    free(c);
    s->sched = NULL;
}

/* effective(stored): the open model's schedules, none for a read-only scene (schedules.h). */
static const cc_schedule **effective(const cc_session *s, size_t *n) {
    const cc_schedule *stored = s->open ? s->model.schedules : NULL;
    size_t ns = s->open ? s->model.nschedules : 0, cap = ns + 16;
    const cc_schedule **v = malloc(cap * sizeof *v);
    if (!v) return NULL;
    *n = cc_schedules_effective(stored, ns, v, cap);
    if (*n > cap) *n = cap; /* never: cap >= nstored + 10 holds all */
    return v;
}

static int oom(Buf *out) {
    buf_clear(out);
    buf_puts(out, "out of memory");
    return CC_ERR;
}

/* calls.rs:399 schedule_list: Vec<ScheduleDef> through serde, struct field order. */
static int list_json(cc_session *s, Buf *out) {
    size_t n;
    const cc_schedule **v = effective(s, &n);
    if (!v) return oom(out);
    JsonW w;
    jw_init(&w, out, 0);
    jw_arr_begin(&w);
    for (size_t i = 0; i < n; i++) {
        const cc_schedule *d = v[i];
        jw_obj_begin(&w);
        jw_key(&w, "name"), jw_str_n(&w, d->name.p, d->name.n);
        jw_key(&w, "classes"), jw_arr_begin(&w);
        for (size_t k = 0; k < d->classes.n; k++) jw_str_n(&w, d->classes.v[k].p, d->classes.v[k].n);
        jw_arr_end(&w);
        if (d->has_family) jw_key(&w, "family"), jw_str(&w, cc_category_name((cc_category)d->family));
        jw_key(&w, "fields"), jw_arr_begin(&w);
        for (size_t k = 0; k < d->nfields; k++) jw_str(&w, cc_sched_field_name(d->fields[k]));
        jw_arr_end(&w);
        jw_key(&w, "group_by_level"), jw_bool(&w, d->group_by_level);
        jw_key(&w, "totals"), jw_bool(&w, d->totals);
        /* ---- QTY1 (ADR 0080): the C-only keys, only when set (a Rust list keeps its text) ---- */
        if (d->properties.n) {
            jw_key(&w, "properties"), jw_arr_begin(&w);
            for (size_t k = 0; k < d->properties.n; k++) jw_str_n(&w, d->properties.v[k].p, d->properties.v[k].n);
            jw_arr_end(&w);
        }
        if (d->group_by_material) jw_key(&w, "group_by_material"), jw_bool(&w, 1);
        if (d->summary) jw_key(&w, "summary"), jw_bool(&w, 1);
        if (d->has_filter) {
            jw_key(&w, "filter"), jw_obj_begin(&w);
            jw_key(&w, "column"), jw_str_n(&w, d->filter_column.p ? d->filter_column.p : "", d->filter_column.n);
            jw_key(&w, "value"), jw_str_n(&w, d->filter_value.p ? d->filter_value.p : "", d->filter_value.n);
            jw_obj_end(&w);
        }
        if (d->has_sort) {
            jw_key(&w, "sort"), jw_obj_begin(&w);
            jw_key(&w, "column"), jw_str_n(&w, d->sort_column.p ? d->sort_column.p : "", d->sort_column.n);
            jw_key(&w, "descending"), jw_bool(&w, d->sort_desc);
            jw_obj_end(&w);
        }
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    free(v);
    return jw_finish(&w) == JW_OK && !out->oom ? CC_OK : oom(out);
}

/* calls.rs:461-475: the named schedule's table for the scene shown, kept per (rev, name).
 * NULL with the error in `out`. */
static struct cc_sched_cache *table_for(cc_session *s, const char *body, size_t n, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    struct cc_sched_cache *r = NULL;
    const JrVal *v = jr_parse(&a, body, n, &je);
    const JrVal *nm = v && v->type == JR_OBJ ? jr_get(v, "name") : NULL;
    if (!v) {
        buf_puts(out, je.msg);
        goto end;
    }
    if (v->type != JR_OBJ) {
        buf_puts(out, "invalid type: expected struct NameArgs");
        goto end;
    }
    if (!nm || nm->type != JR_STR) {
        buf_puts(out, !nm ? "missing field `name`" : "name: invalid type: expected a string");
        goto end;
    }
    size_t nd;
    const cc_schedule **defs = effective(s, &nd);
    if (!defs) {
        oom(out);
        goto end;
    }
    const cc_schedule *def = NULL;
    for (size_t i = 0; i < nd && !def; i++)
        if (defs[i]->name.n == nm->n && !memcmp(defs[i]->name.p, nm->s, nm->n)) def = defs[i]; /* d.name == a.name */
    if (!def) {
        free(defs);
        buf_puts(out, "no schedule with this name");
        goto end;
    }
    struct cc_sched_cache *c = s->sched;
    if (c && c->rev == s->rev && strlen(c->name) == nm->n && !memcmp(c->name, nm->s, nm->n)) {
        free(defs);
        r = c;
        goto end;
    }
    const cc_scene *sc = s->open || s->view ? cc_session_scene(s) : NULL;
    if (!sc) {
        free(defs);
        buf_puts(out, s->open || s->view ? "the scene cannot be built" : "no model open");
        goto end;
    }
    cc_schedules_cache_free(s);
    c = calloc(1, sizeof *c);
    if (c) c->name = malloc(nm->n + 1);
    if (!c || !c->name) {
        free(c);
        free(defs);
        oom(out);
        goto end;
    }
    memcpy(c->name, nm->s, nm->n), c->name[nm->n] = 0;
    c->rev = s->rev;
    int mrc = s->view == CC_VIEW_IFC    ? cc_ifc_scene_meta(s->ifc, &c->mt)
              : s->view == CC_VIEW_FILE ? cc_viewfile_meta_from_file(&c->mt, s->vf)
                                        : cc_viewfile_meta_from_model(&c->mt, &s->model, sc);
    cc_error err;
    int trc = mrc ? -1 : cc_schedule_table(&c->t, sc, &c->mt, def, &err);
    if (mrc) memset(&c->t, 0, sizeof c->t); /* cc_schedule_table always initializes it; not called */
    free(defs);
    s->sched = c;
    if (mrc || trc) {
        cc_schedules_cache_free(s);
        oom(out);
        goto end;
    }
    r = c;
end:
    arena_free(&a);
    return r;
}

/* ---- QTY1 (ADR 0080): schedule_columns: what a list's columns can be ----
 * {"builtin": [names], "fields": [{"id", "summed", "title"}], "properties": [keys]}: the fields in
 * view.c's order, the property keys the open model's elements have (sorted bytes, distinct, the
 * first 500), the built-in list names. */
static int kv_qcmp(const void *a, const void *b) {
    const cc_str *x = a, *y = b;
    size_t n = x->n < y->n ? x->n : y->n;
    int c = n ? memcmp(x->p, y->p, n) : 0;
    return c ? c : (x->n > y->n) - (x->n < y->n);
}

static int columns_json(cc_session *s, Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "builtin"), jw_arr_begin(&w);
    size_t nb;
    const cc_schedule *b = cc_schedule_builtins(&nb);
    for (size_t i = 0; i < nb; i++) jw_str_n(&w, b[i].name.p, b[i].name.n);
    jw_arr_end(&w);
    jw_key(&w, "fields"), jw_arr_begin(&w);
    for (int f = 0; f < CC_FIELD_COUNT; f++) {
        jw_obj_begin(&w);
        jw_key(&w, "id"), jw_str(&w, cc_sched_field_name(f));
        jw_key(&w, "summed"),
            jw_bool(&w, f == CC_FIELD_LENGTH || f == CC_FIELD_AREA || f == CC_FIELD_VOLUME || f == CC_FIELD_MASS ||
                            f == CC_FIELD_ELEMENTS || f == CC_FIELD_GROSS_VOLUME);
        jw_key(&w, "title"), jw_str(&w, cc_sched_field_title(f));
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "properties"), jw_arr_begin(&w);
    size_t total = 0;
    for (size_t i = 0; s->open && i < s->model.nelements; i++) total += s->model.elements[i].nproperties;
    cc_str *keys = total ? malloc(total * sizeof *keys) : NULL;
    if (total && !keys) {
        free(keys);
        return oom(out);
    }
    size_t nk = 0;
    for (size_t i = 0; s->open && i < s->model.nelements; i++)
        for (size_t k = 0; k < s->model.elements[i].nproperties; k++) keys[nk++] = s->model.elements[i].properties[k].key;
    if (nk) qsort(keys, nk, sizeof *keys, kv_qcmp);
    size_t shown = 0;
    for (size_t i = 0; i < nk && shown < 500; i++) {
        if (i && !kv_qcmp(&keys[i], &keys[i - 1])) continue;
        if (!keys[i].n || keys[i].n > CC_SCHED_MAX_TEXT) continue;
        jw_str_n(&w, keys[i].p, keys[i].n), shown++;
    }
    free(keys);
    jw_arr_end(&w);
    jw_obj_end(&w);
    return jw_finish(&w) == JW_OK && !out->oom ? CC_OK : oom(out);
}

/* QTY1: a non-negative integer argument `key` of the body (absent: *has = 0). -1 not one. */
static int body_uint(const char *body, size_t n, const char *key, size_t *v, int *has, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *b = jr_parse(&a, body, n, &je);
    const JrVal *x = b && b->type == JR_OBJ ? jr_get(b, key) : NULL;
    *has = 0;
    int rc = 0;
    if (x && x->type != JR_NULL) {
        if (x->type != JR_NUM || !x->num_int || x->num_neg || x->num > 1e9) {
            buf_puts(out, key), buf_puts(out, ": expected an integer from 0 to 1000000000");
            rc = -1;
        } else *v = (size_t)x->num, *has = 1;
    }
    arena_free(&a);
    return rc;
}

/* QTY1: the body's `headers` (strings, at most 64, each at most 200 bytes) into `h` (malloc'd
 * texts in one block); 0 with *nh = 0 when absent. -1 not valid. */
static int body_headers(const char *body, size_t n, cc_str **h, size_t *nh, char **block, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *b = jr_parse(&a, body, n, &je);
    const JrVal *x = b && b->type == JR_OBJ ? jr_get(b, "headers") : NULL;
    *h = NULL, *nh = 0, *block = NULL;
    int rc = 0;
    if (x && x->type != JR_NULL) {
        size_t len = 0;
        int ok = x->type == JR_ARR && x->n <= 64;
        for (size_t i = 0; ok && i < x->n; i++) ok = x->items[i].type == JR_STR && x->items[i].n <= CC_SCHED_MAX_TEXT, len += x->items[i].n;
        if (!ok) {
            buf_puts(out, "headers: expected at most 64 texts of at most 200 bytes");
            rc = -1;
        } else if (x->n) {
            *h = malloc(x->n * sizeof **h);
            *block = malloc(len + 1);
            if (!*h || !*block) {
                free(*h), free(*block), *h = NULL, *block = NULL;
                oom(out);
                rc = -1;
            } else {
                size_t at = 0;
                for (size_t i = 0; i < x->n; i++) {
                    if (x->items[i].n) memcpy(*block + at, x->items[i].s, x->items[i].n);
                    (*h)[i] = (cc_str){*block + at, x->items[i].n};
                    at += x->items[i].n;
                }
                *nh = x->n;
            }
        }
    }
    arena_free(&a);
    return rc;
}

int cc_schedules_query(struct cc_session *s, const char *name, const char *body, size_t n, Buf *out) {
    if (!strcmp(name, "schedule_list")) return list_json(s, out);
    if (!strcmp(name, "schedule_columns")) return columns_json(s, out); /* QTY1 */
    int rows = !strcmp(name, "schedule_rows");
    if (!rows && strcmp(name, "schedule_csv")) return -1;
    struct cc_sched_cache *c = table_for(s, body, n, out);
    if (!c) return CC_ERR;
    if (rows) {
        /* QTY1: {offset?, limit?}: a window of rows (limit 1..1000, default 200), the page's paging */
        size_t off = 0, lim = 200;
        int has_off, has_lim;
        if (body_uint(body, n, "offset", &off, &has_off, out) || body_uint(body, n, "limit", &lim, &has_lim, out)) return CC_ERR;
        if (has_lim && (lim < 1 || lim > 1000)) return buf_puts(out, "limit: from 1 to 1000"), CC_ERR;
        if (has_off || has_lim) cc_schedule_json_page(out, &c->t, &c->mt, off, lim);
        else cc_schedule_json(out, &c->t, &c->mt, 50); /* calls.rs:49 MAX_LIST */
    } else {
        cc_str *hd;
        size_t nh;
        char *block;
        if (body_headers(body, n, &hd, &nh, &block, out)) return CC_ERR;
        if (nh && nh != c->t.nfields) {
            free(hd), free(block);
            return buf_puts(out, "headers: one per column"), CC_ERR;
        }
        Buf csv;
        buf_init(&csv);
        cc_schedule_csv_headers(&csv, &c->t, hd, nh); /* QTY1: the page's headers, as the table shows them */
        free(hd), free(block);
        buf_puts(out, "{\"csv\":"); /* json!({"csv": ..}) */
        json_escape(out, csv.p ? csv.p : "", csv.len);
        buf_putc(out, '}');
        int bad = csv.oom;
        buf_free(&csv);
        if (bad) return oom(out);
    }
    return out->oom ? oom(out) : CC_OK;
}
#endif
