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
    if (!strcmp(name, "schedule_rows") || !strcmp(name, "schedule_csv")) return buf_puts(out, CC_VIEW_ONLY_TEXT), CC_ERR;
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

int cc_schedules_query(struct cc_session *s, const char *name, const char *body, size_t n, Buf *out) {
    if (!strcmp(name, "schedule_list")) return list_json(s, out);
    int rows = !strcmp(name, "schedule_rows");
    if (!rows && strcmp(name, "schedule_csv")) return -1;
    struct cc_sched_cache *c = table_for(s, body, n, out);
    if (!c) return CC_ERR;
    if (rows) cc_schedule_json(out, &c->t, &c->mt, 50); /* calls.rs:49 MAX_LIST */
    else {
        Buf csv;
        buf_init(&csv);
        cc_schedule_csv(&csv, &c->t);
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
