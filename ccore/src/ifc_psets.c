/* The property and quantity sets of an IFC's products (ticket M1LINKS): see ifc_psets.h. */
#include "ifc_psets.h"
#include <stdlib.h>
#include <string.h>
#include "fmt.h"
#include "json_w.h"
#include "step.h"

static const char *const KEEP[] = {
    "IFCRELDEFINESBYPROPERTIES", "IFCPROPERTYSET", "IFCELEMENTQUANTITY", "IFCPROPERTYSINGLEVALUE",
    "IFCPROPERTYENUMERATEDVALUE", "IFCPROPERTYLISTVALUE", "IFCPROPERTYBOUNDEDVALUE", "IFCQUANTITYLENGTH",
    "IFCQUANTITYAREA", "IFCQUANTITYVOLUME", "IFCQUANTITYCOUNT", "IFCQUANTITYWEIGHT", "IFCQUANTITYTIME"};

typedef struct {
    uint64_t obj, def;
    size_t order;
} Pair;

static int pair_cmp(const void *x, const void *y) {
    const Pair *a = x, *b = y;
    if (a->obj != b->obj) return a->obj < b->obj ? -1 : 1;
    return (a->order > b->order) - (a->order < b->order);
}

static void put_u32(Buf *b, uint64_t x) {
    unsigned char c[4] = {(unsigned char)x, (unsigned char)(x >> 8), (unsigned char)(x >> 16), (unsigned char)(x >> 24)};
    buf_append(b, (const char *)c, 4);
}

static void jstr(Buf *b, const char *p, size_t n) { json_escape(b, p ? p : "", n); }

/* One value as text (ifc_psets.h), into `b` (not quoted). Depth-bounded. */
static void value_text(Buf *b, const cc_sv *v, int depth) {
    if (!v || depth > 4) return;
    switch (v->kind) {
    case CC_SV_STR: buf_append(b, v->u.s.p, v->u.s.n); break;
    case CC_SV_ENUM:
        if (!strcmp(v->u.s.p, "T")) buf_puts(b, "true");
        else if (!strcmp(v->u.s.p, "F")) buf_puts(b, "false");
        else if (!strcmp(v->u.s.p, "U")) buf_puts(b, "unknown");
        else buf_append(b, v->u.s.p, v->u.s.n);
        break;
    case CC_SV_INT: fmt_f64_json(b, (double)v->u.i); break;
    case CC_SV_REAL: fmt_f64_json(b, v->u.r); break;
    case CC_SV_TYPED: value_text(b, v->u.typed.inner, depth + 1); break;
    case CC_SV_LIST:
        for (size_t i = 0; i < v->u.list.n; i++) {
            if (i) buf_puts(b, ", ");
            value_text(b, &v->u.list.items[i], depth + 1);
        }
        break;
    default: break;
    }
}

/* A property or quantity: its name and value text. 0: not one we read. */
static int prop_of(const cc_step_entity *e, cc_str *name, Buf *val) {
    const char *p;
    size_t n;
    if (!cc_sv_as_str(cc_step_arg(e, 0), &p, &n)) return 0;
    *name = (cc_str){p, n};
    buf_clear(val);
    if (!strcmp(e->name, "IFCPROPERTYSINGLEVALUE")) value_text(val, cc_step_arg(e, 2), 0);
    else if (!strcmp(e->name, "IFCPROPERTYENUMERATEDVALUE") || !strcmp(e->name, "IFCPROPERTYLISTVALUE")) value_text(val, cc_step_arg(e, 2), 0);
    else if (!strcmp(e->name, "IFCPROPERTYBOUNDEDVALUE")) { /* (upper, lower): "lower .. upper" */
        value_text(val, cc_step_arg(e, 3), 0);
        buf_puts(val, " .. ");
        value_text(val, cc_step_arg(e, 2), 0);
    } else if (!strncmp(e->name, "IFCQUANTITY", 11)) value_text(val, cc_step_arg(e, 3), 0);
    else return 0;
    return 1;
}

/* One set as JSON into `b`: 1 written, 0 not a set we read. */
static int set_json(const cc_step_file *f, const cc_step_entity *d, Buf *b, Buf *val) {
    int pset = !strcmp(d->name, "IFCPROPERTYSET"), qset = !strcmp(d->name, "IFCELEMENTQUANTITY");
    if (!pset && !qset) return 0;
    const char *p;
    size_t n;
    if (!cc_sv_as_str(cc_step_arg(d, 2), &p, &n)) p = "", n = 0;
    const cc_sv *items;
    size_t ni;
    if (!cc_sv_as_list(cc_step_arg(d, pset ? 4 : 5), &items, &ni)) ni = 0;
    buf_puts(b, "{\"name\":");
    jstr(b, p, n);
    buf_puts(b, ",\"properties\":[");
    size_t k = 0;
    for (size_t i = 0; i < ni && k < CC_PSETS_PROPS; i++) {
        const cc_step_entity *x = cc_step_deref(f, &items[i]);
        cc_str nm;
        if (!x || !prop_of(x, &nm, val)) continue;
        if (k++) buf_putc(b, ',');
        buf_puts(b, "{\"name\":");
        jstr(b, nm.p, nm.n);
        buf_puts(b, ",\"value\":");
        jstr(b, val->p ? val->p : "", val->len);
        buf_putc(b, '}');
    }
    buf_puts(b, pset ? "],\"type\":\"IfcPropertySet\"}" : "],\"type\":\"IfcElementQuantity\"}");
    return 1;
}

int cc_ifc_psets(const char *text, size_t n, const cc_ifc_scene *s, Buf *out, cc_error *err) {
    size_t ne = s->scene.nelements;
    cc_step_file f;
    if (cc_step_parse_only(&f, text, n, KEEP, sizeof KEEP / sizeof *KEEP, err)) {
        cc_step_free(&f);
        return -1;
    }
    /* the relations: (object, set), by object */
    size_t np = 0, cap = 0;
    Pair *pr = NULL;
    for (size_t i = 0; i < f.n; i++) {
        const cc_step_entity *e = &f.ents[i];
        if (strcmp(e->name, "IFCRELDEFINESBYPROPERTIES")) continue;
        uint64_t def;
        const cc_sv *objs;
        size_t no;
        if (!cc_sv_as_ref(cc_step_arg(e, 5), &def) || !cc_sv_as_list(cc_step_arg(e, 4), &objs, &no)) continue;
        for (size_t k = 0; k < no; k++) {
            uint64_t o;
            if (!cc_sv_as_ref(&objs[k], &o)) continue;
            if (np == cap) {
                size_t c = cap ? 2 * cap : 256;
                Pair *q = realloc(pr, c * sizeof *q);
                if (!q) goto oom;
                pr = q, cap = c;
            }
            pr[np].obj = o, pr[np].def = def, pr[np].order = np;
            np++;
        }
    }
    if (np) qsort(pr, np, sizeof *pr, pair_cmp);
    /* the table */
    size_t head = out->len;
    put_u32(out, ne);
    for (size_t i = 0; i < ne; i++) put_u32(out, 0);
    size_t text0 = out->len;
    Buf one, val;
    buf_init(&one), buf_init(&val);
    for (size_t i = 0; i < ne && !out->oom; i++) {
        uint64_t id = s->info[s->scene.elements[i].model_index].step_id;
        size_t lo = 0, hi = np;
        while (lo < hi) { /* the first pair of this object */
            size_t mid = (lo + hi) / 2;
            if (pr[mid].obj < id) lo = mid + 1;
            else hi = mid;
        }
        buf_clear(&one);
        size_t sets = 0;
        for (size_t k = lo; id && k < np && pr[k].obj == id && sets < CC_PSETS_SETS; k++) {
            const cc_step_entity *d = cc_step_get(&f, pr[k].def);
            size_t at = one.len;
            buf_putc(&one, sets ? ',' : '[');
            if (!d || !set_json(&f, d, &one, &val)) {
                one.len = at;
                continue;
            }
            if (one.len + 1 > CC_PSETS_ENTRY) { /* the sets beyond the budget are left out */
                one.len = at;
                break;
            }
            sets++;
        }
        if (sets) buf_putc(&one, ']');
        if (one.oom || val.oom) out->oom = 1;
        if (out->len - text0 + one.len <= CC_PSETS_MAX) buf_append(out, one.p ? one.p : "", one.len);
        uint64_t end = out->len - text0;
        for (int b = 0; b < 4 && !out->oom; b++) out->p[head + 4 + 4 * i + (size_t)b] = (char)(end >> (8 * b));
    }
    buf_free(&one), buf_free(&val);
    free(pr);
    cc_step_free(&f);
    if (out->oom) {
        memcpy(err->msg, "out of memory", 14);
        return -1;
    }
    return 0;
oom:
    free(pr);
    cc_step_free(&f);
    memcpy(err->msg, "out of memory", 14);
    return -1;
}

static uint32_t get_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int cc_psets_entry(const uint8_t *t, size_t n, size_t i, const uint8_t **p, size_t *len) {
    if (n < 4) return -1;
    uint64_t ne = get_u32(t);
    if (i >= ne || cc_psets_head(ne) > n) return -1;
    uint64_t text = cc_psets_head(ne), a = i ? get_u32(t + 4 + 4 * (i - 1)) : 0, b = get_u32(t + 4 + 4 * i);
    if (a > b || text + b > n) return -1;
    *p = t + text + a, *len = (size_t)(b - a);
    return 0;
}
