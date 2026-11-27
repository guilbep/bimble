/* The property and quantity sets of an IFC's products (ticket M1LINKS): see ifc_psets.h. */
#include "ifc_psets.h"
#include <stdlib.h>
#include <string.h>
#include "fmt.h"
#include "json_w.h"
#include "step.h"
#include "inflate.h"

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

/* ---- SIZE2: the packed table (ifc_psets.h) ---- */

static void put32(Buf *b, uint32_t v) {
    char t[4] = {(char)(v & 0xff), (char)(v >> 8 & 0xff), (char)(v >> 16 & 0xff), (char)(v >> 24 & 0xff)};
    buf_append(b, t, 4);
}

/* FNV-1a of a text. */
static uint64_t fnv(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

typedef struct { const uint8_t *p; uint32_t n; } Span;

/* The top-level items of a JSON array text [a,b,...] (compact, as written above): their spans, into
 * `items` (at most `cap`). The count, or -1 when the text is not an array of objects. */
static int split_sets(const uint8_t *p, size_t n, Span *items, int cap) {
    if (n < 2 || p[0] != '[' || p[n - 1] != ']') return -1;
    int k = 0, depth = 0, str = 0;
    size_t start = 1;
    for (size_t i = 1; i + 1 < n; i++) {
        uint8_t c = p[i];
        if (str) {
            if (c == '\\') i++;
            else if (c == '"') str = 0;
            continue;
        }
        if (c == '"') str = 1;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
        else if (c == ',' && depth == 0) {
            if (k >= cap) return -1;
            items[k].p = p + start, items[k].n = (uint32_t)(i - start), k++;
            start = i + 1;
        }
        if (depth < 0) return -1;
    }
    if (depth || str || k >= cap || n - 1 <= start) return -1;
    items[k].p = p + start, items[k].n = (uint32_t)(n - 1 - start), k++;
    return k;
}

int cc_psets_pack(const uint8_t *t, size_t n, Buf *out) {
    if (n < 4) return -1;
    uint64_t ne = get_u32(t);
    if (cc_psets_head(ne) > n) return -1;
    int rc = -1;
    size_t cap = 1024; /* the distinct sets: an open hash of their texts */
    Span *sets = NULL;
    uint32_t *slot = NULL, *first = NULL, *refs = NULL;
    size_t nsets = 0, nrefs = 0, rcap = 0, hcap = 2048;
    sets = malloc(cap * sizeof *sets), slot = calloc(hcap, sizeof *slot), first = malloc((size_t)(ne + 1) * sizeof *first);
    if (!sets || !slot || !first) goto done;
    first[0] = 0;
    for (size_t i = 0; i < ne; i++) {
        const uint8_t *p;
        size_t len;
        Span it[CC_PSETS_SETS + 1];
        int k = 0;
        if (cc_psets_entry(t, n, i, &p, &len)) goto done;
        if (len && (k = split_sets(p, len, it, CC_PSETS_SETS + 1)) < 0) goto done;
        for (int q = 0; q < k; q++) {
            if (nsets + 1 > hcap / 2) { /* grow the hash */
                size_t nh = 2 * hcap;
                uint32_t *ns = calloc(nh, sizeof *ns);
                if (!ns) goto done;
                for (size_t j = 0; j < nsets; j++) {
                    size_t h = (size_t)(fnv(sets[j].p, sets[j].n) & (nh - 1));
                    while (ns[h]) h = (h + 1) & (nh - 1);
                    ns[h] = (uint32_t)j + 1;
                }
                free(slot), slot = ns, hcap = nh;
            }
            size_t h = (size_t)(fnv(it[q].p, it[q].n) & (hcap - 1));
            while (slot[h] && !(sets[slot[h] - 1].n == it[q].n && !memcmp(sets[slot[h] - 1].p, it[q].p, it[q].n))) h = (h + 1) & (hcap - 1);
            if (!slot[h]) {
                if (nsets == cap) {
                    Span *g = realloc(sets, 2 * cap * sizeof *sets);
                    if (!g) goto done;
                    sets = g, cap *= 2;
                }
                sets[nsets] = it[q];
                slot[h] = (uint32_t)++nsets;
            }
            if (nrefs == rcap) {
                size_t nc = rcap ? 2 * rcap : 4096;
                uint32_t *g = realloc(refs, nc * sizeof *refs);
                if (!g) goto done;
                refs = g, rcap = nc;
            }
            refs[nrefs++] = slot[h] - 1;
        }
        first[i + 1] = (uint32_t)nrefs;
    }
    /* the blocks: sets cut every CC_PSETS_BLOCK bytes of text */
    size_t nb = 0, bcap = 16;
    uint32_t *bset = malloc(bcap * sizeof *bset);
    if (!bset) goto done;
    bset[0] = 0;
    for (size_t j = 0, acc = 0; j < nsets; j++) {
        acc += sets[j].n;
        if (acc >= CC_PSETS_BLOCK || j + 1 == nsets) {
            if (nb + 2 > bcap) {
                uint32_t *g = realloc(bset, 2 * bcap * sizeof *bset);
                if (!g) { free(bset); goto done; }
                bset = g, bcap *= 2;
            }
            bset[++nb] = (uint32_t)(j + 1), acc = 0;
        }
    }
    Buf blob, one;
    buf_init(&blob), buf_init(&one);
    uint32_t *boff = malloc((nb + 1) * sizeof *boff);
    int bad = !boff;
    if (boff) boff[0] = 0;
    for (size_t b = 0; !bad && b < nb; b++) {
        buf_clear(&one);
        for (uint32_t j = bset[b]; j < bset[b + 1]; j++) buf_append(&one, sets[j].p, sets[j].n);
        bad = one.oom || cc_gzip((const unsigned char *)one.p, one.len, &blob) || blob.len > UINT32_MAX;
        if (!bad) boff[b + 1] = (uint32_t)blob.len;
    }
    if (!bad) {
        put32(out, (uint32_t)ne), put32(out, (uint32_t)nsets), put32(out, (uint32_t)nrefs), put32(out, (uint32_t)nb);
        for (size_t i = 0; i <= ne; i++) put32(out, first[i]);
        for (size_t i = 0; i < nrefs; i++) put32(out, refs[i]);
        uint64_t end = 0;
        for (size_t j = 0; j < nsets; j++) end += sets[j].n, put32(out, (uint32_t)end);
        for (size_t b = 0; b <= nb; b++) put32(out, bset[b]);
        for (size_t b = 0; b <= nb; b++) put32(out, boff[b]);
        buf_append(out, blob.p ? blob.p : "", blob.len);
        rc = out->oom || end > UINT32_MAX ? -1 : 0;
    }
    buf_free(&blob), buf_free(&one);
    free(boff), free(bset);
done:
    free(sets), free(slot), free(first), free(refs);
    return rc;
}

uint64_t cc_psets_packed_head(const uint8_t *p16) {
    uint64_t ne = get_u32(p16), ns = get_u32(p16 + 4), nr = get_u32(p16 + 8), nb = get_u32(p16 + 12);
    return 16 + 4 * (ne + 1) + 4 * nr + 4 * ns + 8 * (nb + 1);
}

int cc_psets_packed_check(const uint8_t *p, size_t n, uint64_t len, uint64_t nelements) {
    if (n < 16 || len < 16) return -1;
    uint64_t ne = get_u32(p), nr = get_u32(p + 8), head = cc_psets_packed_head(p);
    if (ne != nelements || head > len || head > n) return -1;
    uint64_t last = 0;
    for (uint64_t i = 0; i <= ne; i++) {
        uint64_t f = get_u32(p + 16 + 4 * i);
        if (f < last || (i == 0 && f) || f > nr) return -1;
        last = f;
    }
    return last == nr ? 0 : -1;
}

int cc_psets_packed_entry(cc_psets_read_fn rd, void *ctx, uint64_t len, size_t i, Buf *out) {
    uint8_t h[16], w[8];
    if (len < 16 || rd(ctx, 0, 16, h)) return -1;
    uint64_t ne = get_u32(h), ns = get_u32(h + 4), nr = get_u32(h + 8), nb = get_u32(h + 12), head = cc_psets_packed_head(h);
    if (i >= ne || head > len || nb > ns) return -1;
    uint64_t at_first = 16, at_refs = at_first + 4 * (ne + 1), at_send = at_refs + 4 * nr, at_bset = at_send + 4 * ns,
             at_boff = at_bset + 4 * (nb + 1);
    if (rd(ctx, at_first + 4 * i, 8, w)) return -1;
    uint64_t a = get_u32(w), b = get_u32(w + 4);
    if (a > b || b > nr || b - a > CC_PSETS_SETS) return -1;
    if (a == b) return 0; /* none */
    uint32_t refs[CC_PSETS_SETS];
    uint8_t rb[4 * CC_PSETS_SETS];
    if (rd(ctx, at_refs + 4 * a, (size_t)(4 * (b - a)), rb)) return -1;
    for (uint64_t k = 0; k < b - a; k++) {
        refs[k] = get_u32(rb + 4 * k);
        if (refs[k] >= ns) return -1;
    }
    uint8_t *bset = malloc((size_t)(4 * (nb + 1)));
    if (!bset) return -2;
    int rc = rd(ctx, at_bset, (size_t)(4 * (nb + 1)), bset) ? -1 : 0;
    Buf blk, zb;
    buf_init(&blk), buf_init(&zb);
    int64_t cur = -1; /* the block inflated in blk */
    uint64_t cur_s0 = 0; /* its first set's start in the sets' text */
    size_t at0 = out->len;
    buf_putc(out, '[');
    for (uint64_t k = 0; !rc && k < b - a; k++) {
        uint32_t j = refs[k];
        uint64_t lo = 0, hi = nb; /* the block holding set j: bset[blk] <= j < bset[blk + 1] */
        while (lo < hi) {
            uint64_t mid = (lo + hi + 1) / 2;
            if (get_u32(bset + 4 * mid) <= j) lo = mid;
            else hi = mid - 1;
        }
        uint64_t bk = lo;
        if (bk >= nb) { rc = -1; break; }
        uint64_t s0 = get_u32(bset + 4 * bk), s1 = get_u32(bset + 4 * (bk + 1));
        if (s0 > j || s1 <= j || s1 > ns) { rc = -1; break; }
        if ((int64_t)bk != cur) {
            uint8_t e0[4] = {0, 0, 0, 0}, e1[4], o[8];
            if ((s0 && rd(ctx, at_send + 4 * (s0 - 1), 4, e0)) || rd(ctx, at_send + 4 * (s1 - 1), 4, e1) || rd(ctx, at_boff + 4 * bk, 8, o)) { rc = -1; break; }
            uint64_t t0 = get_u32(e0), t1 = get_u32(e1), z0 = get_u32(o), z1 = get_u32(o + 4);
            if (t1 < t0 || t1 - t0 > CC_PSETS_BLOCK_MAX || z1 < z0 || at_boff + 4 * (nb + 1) + z1 > len) { rc = -1; break; }
            buf_clear(&zb), buf_clear(&blk);
            if (buf_reserve(&zb, (size_t)(z1 - z0) + 1)) { rc = -2; break; }
            if (rd(ctx, at_boff + 4 * (nb + 1) + z0, (size_t)(z1 - z0), zb.p)) { rc = -1; break; }
            const char *why = NULL;
            if (cc_gunzip((const unsigned char *)zb.p, (size_t)(z1 - z0), (size_t)(t1 - t0), &blk, &why) || blk.len != t1 - t0) { rc = -1; break; }
            cur = (int64_t)bk, cur_s0 = t0;
        }
        uint8_t e[8];
        uint64_t x0, x1;
        if (j) {
            if (rd(ctx, at_send + 4 * ((uint64_t)j - 1), 8, e)) { rc = -1; break; }
            x0 = get_u32(e), x1 = get_u32(e + 4);
        } else {
            if (rd(ctx, at_send, 4, e)) { rc = -1; break; }
            x0 = 0, x1 = get_u32(e);
        }
        if (x0 > x1 || x0 < cur_s0 || x1 - cur_s0 > blk.len) { rc = -1; break; }
        if (k) buf_putc(out, ',');
        buf_append(out, blk.p + (x0 - cur_s0), (size_t)(x1 - x0));
    }
    buf_putc(out, ']');
    if (!rc && out->oom) rc = -2;
    if (rc) out->len = at0;
    buf_free(&blk), buf_free(&zb);
    free(bset);
    return rc;
}
