/* See verbs_schedules.h (QTY1, ADR 0080). Layer L3: no files, clock, threads or globals. */
#include "verbs_schedules.h"
#include <string.h>
#include "quantity.h"
#include "verbs.h"
#include "view_io.h"

static int bad(Buf *err, const char *a, const char *b, const char *c) {
    buf_clear(err);
    buf_puts(err, "bad arguments: ");
    if (a) buf_puts(err, a);
    if (b) buf_puts(err, b);
    if (c) buf_puts(err, c);
    return -1;
}

static int fail(Buf *err, const char *verb, const char *what, cc_str name) {
    buf_clear(err);
    buf_puts(err, verb);
    buf_puts(err, ": ");
    buf_puts(err, what);
    if (name.n) buf_append(err, name.p, name.n);
    return -1;
}

/* The arguments: every name below; `add` refuses new_name, `remove` takes name only. */
enum { A_NAME, A_NEW_NAME, A_PRESET, A_CLASSES, A_FAMILY, A_COLUMNS, A_PROPERTIES, A_BY_LEVEL, A_BY_MATERIAL, A_SUMMARY,
       A_TOTALS, A_FILTER, A_SORT, A_COUNT };
static const char *const ARG[A_COUNT] = {"name",       "new_name",         "preset",  "classes", "family",
                                         "columns",    "properties",       "group_by_level", "group_by_material",
                                         "summary",    "totals",           "filter",  "sort"};

typedef struct {
    const JrVal *v[A_COUNT]; /* given (a null too) */
} Args;

static int args_of(const JrVal *v, const int *allowed, int nallowed, Args *a, Buf *err) {
    memset(a, 0, sizeof *a);
    if (v->type != JR_OBJ) return bad(err, "expected an object", 0, 0);
    for (size_t j = 0; j < v->n; j++) {
        const JrMember *mb = &v->members[j];
        int i = -1;
        for (int k = 0; k < nallowed && i < 0; k++)
            if (jr_key_is(mb, ARG[allowed[k]])) i = allowed[k];
        if (i < 0) {
            buf_clear(err);
            buf_puts(err, "bad arguments: unknown field `");
            buf_append(err, mb->key, mb->key_n < 200 ? mb->key_n : 200);
            buf_puts(err, "`, expected one of");
            for (int k = 0; k < nallowed; k++) buf_puts(err, k ? ", `" : " `"), buf_puts(err, ARG[allowed[k]]), buf_putc(err, '`');
            return -1;
        }
        a->v[i] = &mb->val;
    }
    return 0;
}

static int a_text(const JrVal *v, const char *field, cc_str *out, Buf *err) {
    if (v->type != JR_STR) return bad(err, "`", field, "`: expected a string");
    *out = cc_str_trim((cc_str){v->s, v->n});
    return 0;
}

static int a_bool(const JrVal *v, const char *field, int *out, Buf *err) {
    if (v->type != JR_BOOL) return bad(err, "`", field, "`: expected true or false");
    *out = v->b;
    return 0;
}

static int a_strs(const JrVal *v, const char *field, cc_strs *out, Arena *ar, Buf *err) {
    if (v->type != JR_ARR) return bad(err, "`", field, "`: expected a list of strings");
    if (v->n > 64) return bad(err, "`", field, "`: at most 64");
    out->n = v->n;
    out->v = v->n ? arena_alloc(ar, v->n * sizeof *out->v, 8) : NULL;
    if (v->n && !out->v) return bad(err, "out of memory", 0, 0);
    for (size_t i = 0; i < v->n; i++)
        if (a_text(&v->items[i], field, &out->v[i], err)) return -1;
    return 0;
}

static int field_of(const JrVal *v, int *f) {
    for (int k = 0; v->type == JR_STR && k < CC_FIELD_COUNT; k++) {
        const char *n = cc_sched_field_name(k);
        if (strlen(n) == v->n && !memcmp(n, v->s, v->n)) return *f = k, 0;
    }
    return -1;
}

static int a_columns(const JrVal *v, cc_schedule *d, Arena *ar, Buf *err) {
    if (v->type != JR_ARR) return bad(err, "`columns`: expected a list of field names", 0, 0);
    if (v->n > 32) return bad(err, "`columns`: 1 to 32", 0, 0);
    uint8_t *f = v->n ? arena_alloc(ar, v->n, 1) : NULL;
    if (v->n && !f) return bad(err, "out of memory", 0, 0);
    for (size_t i = 0; i < v->n; i++) {
        int k;
        if (field_of(&v->items[i], &k)) {
            bad(err, "`columns`: unknown field ", 0, 0);
            if (v->items[i].type == JR_STR) cc_buf_debug_str(err, v->items[i].s, v->items[i].n < 200 ? v->items[i].n : 200);
            return -1;
        }
        for (size_t j = 0; j < i; j++)
            if (f[j] == k) return bad(err, "`columns`: a field twice", 0, 0);
        f[i] = (uint8_t)k;
    }
    d->fields = f, d->nfields = v->n;
    return 0;
}

/* {column, value} / {column, descending?}; null: none. */
static int a_filter(const JrVal *v, cc_schedule *d, Buf *err) {
    if (v->type == JR_NULL) return d->has_filter = 0, 0;
    const JrVal *c = v->type == JR_OBJ ? jr_get(v, "column") : NULL, *x = v->type == JR_OBJ ? jr_get(v, "value") : NULL;
    if (!c || !x || c->type != JR_STR || x->type != JR_STR || v->n != 2)
        return bad(err, "`filter`: expected {\"column\": a column, \"value\": a text}", 0, 0);
    d->has_filter = 1, d->filter_column = (cc_str){c->s, c->n}, d->filter_value = cc_str_trim((cc_str){x->s, x->n});
    return 0;
}

static int a_sort(const JrVal *v, cc_schedule *d, Buf *err) {
    if (v->type == JR_NULL) return d->has_sort = 0, d->sort_desc = 0, 0;
    const JrVal *c = v->type == JR_OBJ ? jr_get(v, "column") : NULL, *x = v->type == JR_OBJ ? jr_get(v, "descending") : NULL;
    if (!c || c->type != JR_STR || (x && x->type != JR_BOOL) || v->n != 1u + (x != NULL))
        return bad(err, "`sort`: expected {\"column\": a column, \"descending\": true or false}", 0, 0);
    d->has_sort = 1, d->sort_column = (cc_str){c->s, c->n}, d->sort_desc = x ? x->b : 0;
    return 0;
}

/* The fields given (not name, new_name, preset) into `d`. */
static int apply_args(const Args *a, cc_schedule *d, Arena *ar, Buf *err) {
    const JrVal *const *v = a->v;
    if (v[A_CLASSES] && v[A_CLASSES]->type != JR_NULL && a_strs(v[A_CLASSES], "classes", &d->classes, ar, err)) return -1;
    if (v[A_CLASSES] && v[A_CLASSES]->type == JR_NULL) d->classes.v = NULL, d->classes.n = 0;
    if (v[A_FAMILY]) {
        if (v[A_FAMILY]->type == JR_NULL) d->has_family = 0, d->family = 0;
        else {
            int ok = 0;
            for (int c = 0; c < CC_CAT_COUNT && !ok; c++) {
                const char *n = cc_category_name((cc_category)c);
                if (v[A_FAMILY]->type == JR_STR && strlen(n) == v[A_FAMILY]->n && !memcmp(n, v[A_FAMILY]->s, v[A_FAMILY]->n))
                    ok = 1, d->has_family = 1, d->family = c;
            }
            if (!ok) return bad(err, "`family`: one of concrete, steel, timber, masonry, insulation, earth, glass, other", 0, 0);
        }
    }
    if (v[A_COLUMNS] && a_columns(v[A_COLUMNS], d, ar, err)) return -1;
    if (v[A_PROPERTIES]) {
        if (v[A_PROPERTIES]->type == JR_NULL) d->properties.v = NULL, d->properties.n = 0;
        else if (a_strs(v[A_PROPERTIES], "properties", &d->properties, ar, err)) return -1;
    }
    if (v[A_BY_LEVEL] && a_bool(v[A_BY_LEVEL], "group_by_level", &d->group_by_level, err)) return -1;
    if (v[A_BY_MATERIAL] && a_bool(v[A_BY_MATERIAL], "group_by_material", &d->group_by_material, err)) return -1;
    if (v[A_SUMMARY] && a_bool(v[A_SUMMARY], "summary", &d->summary, err)) return -1;
    if (v[A_TOTALS] && a_bool(v[A_TOTALS], "totals", &d->totals, err)) return -1;
    if (v[A_FILTER] && a_filter(v[A_FILTER], d, err)) return -1;
    if (v[A_SORT] && a_sort(v[A_SORT], d, err)) return -1;
    return 0;
}

static int ieq(cc_str a, cc_str b) { /* eq_ignore_ascii_case */
    if (a.n != b.n) return 0;
    for (size_t i = 0; i < a.n; i++) {
        unsigned char x = (unsigned char)a.p[i], y = (unsigned char)b.p[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

/* The list named `name` (any ASCII case) among effective(stored), NULL when none. */
static const cc_schedule *find(const cc_model *m, cc_str name) {
    const cc_schedule *v[256];
    size_t n = cc_schedules_effective(m->schedules, m->nschedules, v, 256);
    for (size_t i = 0; i < n && i < 256; i++)
        if (ieq(v[i]->name, name)) return v[i];
    return NULL;
}

static int is_builtin(cc_str name) {
    size_t nb;
    const cc_schedule *b = cc_schedule_builtins(&nb);
    for (size_t i = 0; i < nb; i++)
        if (ieq(b[i].name, name)) return 1;
    return 0;
}

/* Stores `v[0..n)` as one set_schedules Command after the model's checks. */
static int store(const char *verb, cc_schedule *v, size_t n, cc_cmds *l, Buf *err) {
    Buf why;
    buf_init(&why);
    if (cc_schedules_validate(v, n, &why)) {
        buf_clear(err);
        buf_puts(err, verb);
        buf_puts(err, ": ");
        buf_append(err, why.p, why.oom ? 0 : why.len);
        buf_free(&why);
        return -1;
    }
    buf_free(&why);
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_SCHEDULES);
    if (!c) return bad(err, "out of memory", 0, 0);
    c->schedules = n ? v : NULL;
    c->nschedules = n;
    return 0;
}

static cc_schedule *stored_plus(const cc_model *m, size_t extra, Arena *ar) {
    cc_schedule *v = arena_alloc(ar, (m->nschedules + extra + 1) * sizeof *v, 8);
    if (v && m->nschedules) memcpy(v, m->schedules, m->nschedules * sizeof *v);
    return v;
}

static uint8_t DEF_FIELDS[] = {CC_FIELD_LEVEL, CC_FIELD_NAME, CC_FIELD_CLASS, CC_FIELD_MATERIAL, CC_FIELD_VOLUME};
static uint8_t BY_MAT_FIELDS[] = {CC_FIELD_MATERIAL, CC_FIELD_LEVEL, CC_FIELD_ELEMENTS, CC_FIELD_AREA, CC_FIELD_VOLUME,
                                  CC_FIELD_MASS};

int cc_vh_schedule_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const int OK[] = {A_NAME,      A_PRESET,      A_CLASSES, A_FAMILY, A_COLUMNS, A_PROPERTIES, A_BY_LEVEL,
                             A_BY_MATERIAL, A_SUMMARY, A_TOTALS,  A_FILTER, A_SORT};
    Args a;
    if (args_of(args, OK, (int)(sizeof OK / sizeof OK[0]), &a, err)) return -1;
    cc_schedule d;
    memset(&d, 0, sizeof d);
    d.fields = DEF_FIELDS, d.nfields = sizeof DEF_FIELDS; /* schedules.rs new_schedule */
    d.group_by_level = 1, d.totals = 1;
    if (a.v[A_PRESET] && a.v[A_PRESET]->type != JR_NULL) {
        if (a.v[A_PRESET]->type != JR_STR || a.v[A_PRESET]->n != 11 || memcmp(a.v[A_PRESET]->s, "by_material", 11))
            return bad(err, "`preset`: expected \"by_material\"", 0, 0);
        d.name = (cc_str){"By material", 11};
        d.fields = BY_MAT_FIELDS, d.nfields = sizeof BY_MAT_FIELDS;
        d.group_by_material = 1, d.summary = 1;
    }
    if (apply_args(&a, &d, &l->arena, err)) return -1;
    if (a.v[A_NAME] && a.v[A_NAME]->type != JR_NULL) {
        if (a_text(a.v[A_NAME], "name", &d.name, err)) return -1;
    } else if (!d.name.n) { /* "Schedule N", the first free */
        char *nm = arena_alloc(&l->arena, 32, 1);
        if (!nm) return bad(err, "out of memory", 0, 0);
        for (unsigned k = 1; k < 100000; k++) {
            Buf b;
            buf_init(&b);
            buf_puts(&b, "Schedule ");
            buf_u64(&b, k);
            size_t n = b.oom || b.len > 31 ? 0 : b.len;
            if (n) memcpy(nm, b.p, n);
            buf_free(&b);
            d.name = (cc_str){nm, n};
            if (!find(m, d.name)) break;
        }
    }
    if (find(m, d.name)) return fail(err, "schedule_add", "a quantity list is already named ", d.name);
    cc_schedule *v = stored_plus(m, 1, &l->arena);
    if (!v) return bad(err, "out of memory", 0, 0);
    v[m->nschedules] = d;
    return store("schedule_add", v, m->nschedules + 1, l, err);
}

int cc_vh_schedule_edit(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const int OK[] = {A_NAME,     A_NEW_NAME,    A_CLASSES, A_FAMILY, A_COLUMNS, A_PROPERTIES, A_BY_LEVEL,
                             A_BY_MATERIAL, A_SUMMARY, A_TOTALS,  A_FILTER, A_SORT};
    Args a;
    if (args_of(args, OK, (int)(sizeof OK / sizeof OK[0]), &a, err)) return -1;
    if (!a.v[A_NAME]) return bad(err, "missing field `name`", 0, 0);
    cc_str name;
    if (a_text(a.v[A_NAME], "name", &name, err)) return -1;
    const cc_schedule *old = find(m, name);
    if (!old) return fail(err, "schedule_edit", "no quantity list named ", name);
    cc_schedule d = *old;
    if (apply_args(&a, &d, &l->arena, err)) return -1;
    if (a.v[A_NEW_NAME] && a.v[A_NEW_NAME]->type != JR_NULL) {
        cc_str nn;
        if (a_text(a.v[A_NEW_NAME], "new_name", &nn, err)) return -1;
        if (!ieq(nn, old->name) || nn.n != old->name.n || memcmp(nn.p, old->name.p, nn.n)) {
            if (is_builtin(old->name)) return fail(err, "schedule_edit", "a built-in list keeps its name: ", old->name);
            const cc_schedule *o = find(m, nn);
            if (o && o != old) return fail(err, "schedule_edit", "a quantity list is already named ", nn);
            d.name = nn;
        }
    }
    if (cc_schedule_eq(&d, old)) return 0;
    cc_schedule *v = stored_plus(m, 1, &l->arena);
    if (!v) return bad(err, "out of memory", 0, 0);
    size_t n = m->nschedules, k = 0;
    while (k < n && !ieq(v[k].name, old->name)) k++;
    v[k] = d;
    if (k == n) n++;
    return store("schedule_edit", v, n, l, err);
}

int cc_vh_schedule_remove(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const int OK[] = {A_NAME};
    Args a;
    if (args_of(args, OK, 1, &a, err)) return -1;
    if (!a.v[A_NAME]) return bad(err, "missing field `name`", 0, 0);
    cc_str name;
    if (a_text(a.v[A_NAME], "name", &name, err)) return -1;
    size_t k = 0;
    while (k < m->nschedules && !ieq(m->schedules[k].name, name)) k++;
    if (k == m->nschedules)
        return fail(err, "schedule_remove", is_builtin(name) ? "a built-in list as it is cannot be removed: " : "no quantity list named ",
                    name);
    cc_schedule *v = stored_plus(m, 0, &l->arena);
    if (!v) return bad(err, "out of memory", 0, 0);
    memmove(v + k, v + k + 1, (m->nschedules - k - 1) * sizeof *v);
    return store("schedule_remove", v, m->nschedules - 1, l, err);
}
