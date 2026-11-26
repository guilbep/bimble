/* IFC export: crates/bimble-ifc/src/lib.rs `export` (IFC4) and ifc2x3.rs `from_ifc4`, line for
 * line. Comments name the Rust they follow. */
#include "ifc_out.h"
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#include "ifc_out_grid.h"
#include <stdlib.h>
#include <string.h>
#include "fmt.h"
#include "georef.h"
#include "ifc_math.h"
#include "guid.h"
#include "json_w.h"

/* No memchr or qsort in the freestanding wasm32 build: these two instead. */
static const char *find_byte(const void *p, int c, size_t n) {
    const char *s = p;
    for (size_t i = 0; i < n; i++)
        if (s[i] == (char)c) return s + i;
    return NULL;
}

/* Stable bottom-up merge sort. 0 ok, -1 out of memory. */
static int msort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *)) {
    if (n < 2) return 0;
    char *a = base, *t = malloc(n * sz);
    if (!t) return -1;
    char *src = a, *dst = t;
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (cmp(src + j * sz, src + i * sz) < 0) memcpy(dst + k++ * sz, src + j++ * sz, sz);
                else memcpy(dst + k++ * sz, src + i++ * sz, sz);
            }
            while (i < mid) memcpy(dst + k++ * sz, src + i++ * sz, sz);
            while (j < hi) memcpy(dst + k++ * sz, src + j++ * sz, sz);
        }
        char *x = src;
        src = dst, dst = x;
    }
    if (src != a) memcpy(a, src, n * sz);
    free(t);
    return 0;
}

/* ---- STEP text helpers (lib.rs `real`, `string`) ---- */

int cc_step_real(Buf *b, double x) {
    size_t start = b->len;
    if (fmt_f64_debug(b, x)) return -1;
    if (b->oom) return 0;
    size_t e = start, dot = 0;
    while (e < b->len && b->p[e] != 'e') dot |= b->p[e] == '.', e++;
    if (e == b->len) { /* no exponent */
        if (!dot) buf_putc(b, '.');
        return 0;
    }
    b->p[e] = 'E';
    if (!dot) { /* "1e-5" -> "1.E-5": insert '.' before the 'E' */
        buf_putc(b, 0);
        if (b->oom) return 0;
        memmove(b->p + e + 1, b->p + e, b->len - 1 - e);
        b->p[e] = '.';
    }
    return 0;
}

static void hex4(Buf *b, unsigned u) {
    static const char H[] = "0123456789ABCDEF";
    char t[4] = {H[(u >> 12) & 15], H[(u >> 8) & 15], H[(u >> 4) & 15], H[u & 15]};
    buf_append(b, t, 4);
}

/* One UTF-8 scalar at s[0..n): its code point and length; invalid -> U+FFFD, length 1. */
static uint32_t utf8_next(const unsigned char *s, size_t n, size_t *len) {
    unsigned c = s[0];
    *len = 1;
    if (c < 0x80) return c;
    size_t k = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 ? 2 : 0;
    if (k == 0 || k > n || c > 0xF4) return 0xFFFD;
    uint32_t cp = c & (0x7F >> k);
    for (size_t i = 1; i < k; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = cp << 6 | (s[i] & 0x3F);
    }
    if ((k == 3 && cp < 0x800) || (k == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp < 0xE000))
        return 0xFFFD;
    *len = k;
    return cp;
}

void cc_step_string(Buf *b, const char *s, size_t n) {
    const unsigned char *u = (const unsigned char *)s;
    int wide = 0;
    buf_putc(b, '\'');
    for (size_t i = 0; i < n;) {
        size_t len;
        uint32_t cp = utf8_next(u + i, n - i, &len);
        i += len;
        if (cp >= 0x20 && cp < 0x7F) { /* ASCII, not a control character */
            if (wide) buf_puts(b, "\\X0\\"), wide = 0;
            if (cp == '\'') buf_puts(b, "''");
            else if (cp == '\\') buf_puts(b, "\\\\");
            else buf_putc(b, (char)cp);
        } else {
            if (!wide) buf_puts(b, "\\X2\\"), wide = 1;
            if (cp >= 0x10000) {
                cp -= 0x10000;
                hex4(b, 0xD800 | (cp >> 10));
                hex4(b, 0xDC00 | (cp & 0x3FF));
            } else hex4(b, cp);
        }
    }
    if (wide) buf_puts(b, "\\X0\\");
    buf_putc(b, '\'');
}

/* date.rs iso_utc / civil_date (H. Hinnant). */
static int64_t div_euclid(int64_t a, int64_t b) {
    int64_t q = a / b;
    return a % b < 0 ? q - 1 : q;
}

static void put_num(char **o, int64_t v, int width) {
    char t[24];
    int n = 0, neg = v < 0;
    uint64_t u = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do t[n++] = (char)('0' + u % 10), u /= 10;
    while (u);
    if (neg) *(*o)++ = '-';
    for (int k = n; k < width; k++) *(*o)++ = '0';
    while (n) *(*o)++ = t[--n];
}

void cc_iso_utc(int64_t secs, char *out) {
    int64_t d = div_euclid(secs, 86400), s = secs - d * 86400;
    int64_t z = d + 719468, era = div_euclid(z, 146097), doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t dd = doy - (153 * mp + 2) / 5 + 1, mm = mp < 10 ? mp + 3 : mp - 9;
    int64_t y = yoe + era * 400 + (mm <= 2);
    char *o = out;
    put_num(&o, y, 4), *o++ = '-', put_num(&o, mm, 2), *o++ = '-', put_num(&o, dd, 2), *o++ = 'T';
    put_num(&o, s / 3600, 2), *o++ = ':', put_num(&o, s % 3600 / 60, 2), *o++ = ':', put_num(&o, s % 60, 2);
    *o = 0;
}

/* ---- the IFC4 writer (lib.rs `Writer`) ---- */

typedef struct {
    uint64_t *v;
    size_t n, cap;
    int oom;
} Ids;

static void ids_push(Ids *a, uint64_t x) {
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 16;
        uint64_t *v = realloc(a->v, cap * sizeof *v);
        if (!v) { a->oom = 1; return; }
        a->v = v, a->cap = cap;
    }
    a->v[a->n++] = x;
}

typedef struct {
    const cc_model *m;
    Buf *d;
    uint64_t next;
    int err;
    Buf seed, json;
    Ids tmp; /* refs of the entity being written */
} W;

static uint64_t begin(W *w) {
    uint64_t id = w->next++;
    buf_putc(w->d, '#'), buf_u64(w->d, id), buf_putc(w->d, '=');
    return id;
}
static uint64_t end(W *w, uint64_t id) {
    buf_puts(w->d, ";\n");
    return id;
}
static uint64_t add(W *w, const char *s) { return end(w, (begin(w), buf_puts(w->d, s), w->next - 1)); }
static void put(W *w, const char *s) { buf_puts(w->d, s); }
static void ref(W *w, uint64_t id) { buf_putc(w->d, '#'), buf_u64(w->d, id); }
static void refs(W *w, const uint64_t *v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (i) buf_putc(w->d, ',');
        ref(w, v[i]);
    }
}
static void real(W *w, double x) {
    if (cc_step_real(w->d, x)) w->err = 1;
}
static void str(W *w, const char *s, size_t n) { cc_step_string(w->d, s, n); }
static void cstr(W *w, cc_str s) { cc_step_string(w->d, s.n ? s.p : "", s.n); }
static void gid_lit(W *w, const cc_gid *g) { /* string(id.as_str()): the alphabet needs no escape */
    buf_putc(w->d, '\''), buf_append(w->d, g->c, 22), buf_putc(w->d, '\'');
}

/* gid(seed): string(GlobalId::derived("<project id>/<seed>")). The caller writes the seed's
 * tail into w->seed after seed_begin. */
static Buf *seed_begin(W *w) {
    buf_clear(&w->seed);
    buf_append(&w->seed, w->m->project_id.c, 22);
    buf_putc(&w->seed, '/');
    return &w->seed;
}
static void gid_end(W *w) {
    if (w->seed.oom) { w->err = 1; return; }
    cc_guid g = cc_guid_derived(w->seed.p, w->seed.len);
    buf_putc(w->d, '\''), buf_append(w->d, g.c, 22), buf_putc(w->d, '\'');
}
static void gid(W *w, const char *seed) { buf_puts(seed_begin(w), seed), gid_end(w); }
static void gid_id(W *w, const char *prefix, const cc_gid *id) { /* "<prefix><id>" */
    Buf *s = seed_begin(w);
    buf_puts(s, prefix), buf_append(s, id->c, 22);
    gid_end(w);
}

static uint64_t point3(W *w, const double p[3]) {
    uint64_t id = begin(w);
    put(w, "IFCCARTESIANPOINT((");
    real(w, p[0]), put(w, ","), real(w, p[1]), put(w, ","), real(w, p[2]);
    put(w, "))");
    return end(w, id);
}
static uint64_t dir3(W *w, const double d[3]) {
    uint64_t id = begin(w);
    put(w, "IFCDIRECTION((");
    real(w, d[0]), put(w, ","), real(w, d[1]), put(w, ","), real(w, d[2]);
    put(w, "))");
    return end(w, id);
}

/* serde_json::to_string(&generator), as file.c writes it (compact). */
static void jw_vec(JsonW *j, const double *v, int n) {
    jw_arr_begin(j);
    for (int i = 0; i < n; i++) jw_f64(j, v[i]);
    jw_arr_end(j);
}
static void jw_loop(JsonW *j, const cc_loop *l) {
    jw_arr_begin(j);
    for (size_t i = 0; i < l->n; i++) jw_vec(j, l->pts[i], 2);
    jw_arr_end(j);
}
static int generator_json(const cc_generator *g, Buf *b) {
    JsonW j;
    jw_init(&j, b, 0);
    jw_obj_begin(&j);
    jw_key(&j, "kind"), jw_str(&j, cc_gen_kind_name(g->kind));
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
        jw_key(&j, "width"), jw_f64(&j, g->u.col.width);
        jw_key(&j, "depth"), jw_f64(&j, g->u.col.depth);
        jw_key(&j, "height"), jw_f64(&j, g->u.col.height);
        break;
    case CC_GEN_RECT_BEAM:
        jw_key(&j, "width"), jw_f64(&j, g->u.beam.width);
        jw_key(&j, "height"), jw_f64(&j, g->u.beam.height);
        jw_key(&j, "length"), jw_f64(&j, g->u.beam.length);
        break;
    case CC_GEN_WALL:
        jw_key(&j, "length"), jw_f64(&j, g->u.wall.length);
        jw_key(&j, "thickness"), jw_f64(&j, g->u.wall.thickness);
        jw_key(&j, "height"), jw_f64(&j, g->u.wall.height);
        break;
    case CC_GEN_SLAB:
        jw_key(&j, "outline"), jw_loop(&j, &g->u.slab.outline);
        jw_key(&j, "thickness"), jw_f64(&j, g->u.slab.thickness);
        break;
    case CC_GEN_EXTRUDED_POLYGON:
        jw_key(&j, "outline"), jw_loop(&j, &g->u.poly.outline);
        if (g->u.poly.nvoids) {
            jw_key(&j, "voids");
            jw_arr_begin(&j);
            for (size_t i = 0; i < g->u.poly.nvoids; i++) jw_loop(&j, &g->u.poly.voids[i]);
            jw_arr_end(&j);
        }
        jw_key(&j, "height"), jw_f64(&j, g->u.poly.height);
        break;
    case CC_GEN_MESH:
        jw_key(&j, "positions");
        jw_arr_begin(&j);
        for (size_t i = 0; i < g->u.mesh.npositions; i++) jw_vec(&j, g->u.mesh.positions[i], 3);
        jw_arr_end(&j);
        jw_key(&j, "indices");
        jw_arr_begin(&j);
        for (size_t i = 0; i < g->u.mesh.nindices; i++) jw_u64(&j, g->u.mesh.indices[i]);
        jw_arr_end(&j);
        break;
    default: break;
    }
    jw_obj_end(&j);
    return jw_finish(&j) == JW_OK ? 0 : -1;
}

/* ---- geometry (lib.rs faceted_brep, write_geometry) ---- */

/* Shared points of a mesh, keyed by the bits of their coordinates, in first-use order. */
typedef struct {
    uint64_t key[3], id;
} Slot;

static uint64_t bits(double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    return u;
}

static uint64_t faceted_brep(W *w, const cc_generator *g) {
    size_t np = g->u.mesh.npositions, ni = g->u.mesh.nindices, cap = 16;
    while (cap < np * 2) cap *= 2;
    Slot *slots = calloc(cap, sizeof *slots); /* id 0: empty */
    Ids faces = {0};
    if (!slots) { w->err = 1; return 0; }
    for (size_t t = 0; t + 3 <= ni; t += 3) {
        uint64_t loop[3];
        for (int k = 0; k < 3; k++) {
            uint32_t i = g->u.mesh.indices[t + k];
            if (i >= np) { w->err = 1; loop[k] = 0; continue; }
            const double *p = g->u.mesh.positions[i];
            uint64_t key[3] = {bits(p[0]), bits(p[1]), bits(p[2])};
            uint64_t h = (key[0] * 0x9E3779B97F4A7C15u) ^ (key[1] * 0xC2B2AE3D27D4EB4Fu) ^ (key[2] * 0x165667B19E3779F9u);
            size_t s = (size_t)(h ^ (h >> 29)) & (cap - 1);
            while (slots[s].id && memcmp(slots[s].key, key, sizeof key)) s = (s + 1) & (cap - 1);
            if (!slots[s].id) memcpy(slots[s].key, key, sizeof key), slots[s].id = point3(w, p);
            loop[k] = slots[s].id;
        }
        uint64_t poly = begin(w);
        put(w, "IFCPOLYLOOP(("), refs(w, loop, 3), put(w, "))"), end(w, poly);
        uint64_t bound = begin(w);
        put(w, "IFCFACEOUTERBOUND("), ref(w, poly), put(w, ",.T.)"), end(w, bound);
        uint64_t face = begin(w);
        put(w, "IFCFACE(("), ref(w, bound), put(w, "))"), end(w, face);
        ids_push(&faces, face);
    }
    free(slots);
    uint64_t shell = begin(w);
    put(w, "IFCCLOSEDSHELL(("), refs(w, faces.v, faces.n), put(w, "))"), end(w, shell);
    if (faces.oom) w->err = 1;
    free(faces.v);
    uint64_t brep = begin(w);
    put(w, "IFCFACETEDBREP("), ref(w, shell), put(w, ")");
    return end(w, brep);
}

static uint64_t polyline(W *w, const double (*pts)[2], size_t n) {
    if (!n) { w->err = 1; return 0; }
    Ids ids = {0};
    for (size_t i = 0; i < n; i++) {
        uint64_t id = begin(w);
        put(w, "IFCCARTESIANPOINT((");
        real(w, pts[i][0]), put(w, ","), real(w, pts[i][1]);
        put(w, "))"), end(w, id);
        ids_push(&ids, id);
    }
    if (!ids.oom) ids_push(&ids, ids.v[0]); /* a closed polyline repeats the first point */
    uint64_t id = begin(w);
    put(w, "IFCPOLYLINE(("), refs(w, ids.v, ids.n), put(w, "))"), end(w, id);
    if (ids.oom) w->err = 1;
    free(ids.v);
    return id;
}

/* The body solid; *repr = "SweptSolid" or "FacetedBrep". */
static uint64_t write_geometry(W *w, const cc_generator *g, uint64_t world, uint64_t z, const char **repr) {
    if (g->kind == CC_GEN_MESH) {
        *repr = "FacetedBrep";
        return faceted_brep(w, g);
    }
    *repr = "SweptSolid";
    uint64_t profile;
    double depth;
    if (g->kind == CC_GEN_RECT_COLUMN || g->kind == CC_GEN_PAD_FOOTING || g->kind == CC_GEN_RECT_BEAM) {
        double x = g->kind == CC_GEN_RECT_BEAM ? g->u.beam.width : g->u.col.width;
        double y = g->kind == CC_GEN_RECT_BEAM ? g->u.beam.height : g->u.col.depth;
        depth = g->kind == CC_GEN_RECT_BEAM ? g->u.beam.length : g->u.col.height;
        profile = begin(w);
        put(w, "IFCRECTANGLEPROFILEDEF(.AREA.,$,$,"), real(w, x), put(w, ","), real(w, y), put(w, ")");
        end(w, profile);
    } else {
        double wall[4][2];
        const double (*pts)[2];
        size_t n, nvoids = 0;
        const cc_loop *voids = NULL;
        if (g->kind == CC_GEN_WALL) {
            double l = g->u.wall.length, t = g->u.wall.thickness;
            wall[0][0] = 0.0, wall[0][1] = -t / 2.0;
            wall[1][0] = l, wall[1][1] = -t / 2.0;
            wall[2][0] = l, wall[2][1] = t / 2.0;
            wall[3][0] = 0.0, wall[3][1] = t / 2.0;
            pts = (const double (*)[2])wall, n = 4, depth = g->u.wall.height;
        } else if (g->kind == CC_GEN_SLAB) {
            pts = (const double (*)[2])g->u.slab.outline.pts, n = g->u.slab.outline.n, depth = g->u.slab.thickness;
        } else if (g->kind == CC_GEN_EXTRUDED_POLYGON) {
            pts = (const double (*)[2])g->u.poly.outline.pts, n = g->u.poly.outline.n, depth = g->u.poly.height;
            voids = g->u.poly.voids, nvoids = g->u.poly.nvoids;
        } else {
            w->err = 1;
            return 0;
        }
        uint64_t outer = polyline(w, pts, n);
        if (nvoids) {
            Ids inner = {0};
            for (size_t i = 0; i < nvoids; i++)
                ids_push(&inner, polyline(w, (const double (*)[2])voids[i].pts, voids[i].n));
            profile = begin(w);
            put(w, "IFCARBITRARYPROFILEDEFWITHVOIDS(.AREA.,$,"), ref(w, outer), put(w, ",(");
            refs(w, inner.v, inner.n), put(w, "))"), end(w, profile);
            if (inner.oom) w->err = 1;
            free(inner.v);
        } else {
            profile = begin(w);
            put(w, "IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,$,"), ref(w, outer), put(w, ")"), end(w, profile);
        }
    }
    uint64_t solid = begin(w);
    put(w, "IFCEXTRUDEDAREASOLID("), ref(w, profile), put(w, ","), ref(w, world), put(w, ","), ref(w, z);
    put(w, ","), real(w, depth), put(w, ")");
    return end(w, solid);
}

static const char *predefined(const cc_element *e) {
    switch (e->cls) {
    case CC_IFC_COLUMN: return ".COLUMN.";
    case CC_IFC_BEAM: return ".BEAM.";
    case CC_IFC_SLAB: return ".FLOOR.";
    case CC_IFC_WALL: return ".SOLIDWALL.";
    case CC_IFC_FOOTING: return ".PAD_FOOTING.";
    default: return e->has_ifc_entity ? ".USERDEFINED." : ".NOTDEFINED.";
    }
}

static const char *ifc_category(cc_category c) {
    static const char *const N[CC_CAT_COUNT] = {"concrete", "steel", "wood", "brick", "insulation", "earth", "glass", ""};
    return (unsigned)c < CC_CAT_COUNT ? N[c] : "";
}

/* char::is_whitespace (Unicode White_Space). */
static int is_ws(uint32_t c) {
    return (c >= 9 && c <= 13) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}
static int blank(cc_str s) { /* v.trim().is_empty() */
    for (size_t i = 0; i < s.n;) {
        size_t len;
        if (!is_ws(utf8_next((const unsigned char *)s.p + i, s.n - i, &len))) return 0;
        i += len;
    }
    return 1;
}

/* m.project.get(k), non-blank; NULL otherwise. */
static const cc_str *project_value(const cc_model *m, const char *k) {
    size_t kn = strlen(k);
    for (size_t i = 0; i < m->nproject; i++)
        if (m->project[i].key.n == kn && !memcmp(m->project[i].key.p, k, kn))
            return blank(m->project[i].value) ? NULL : &m->project[i].value;
    return NULL;
}
static void opt(W *w, const char *k) {
    const cc_str *v = project_value(w->m, k);
    if (v) cstr(w, *v);
    else put(w, "$");
}

/* An element property split into its set and name (lib.rs: "Pset/Property", else
 * Bimble_Properties). */
typedef struct {
    const char *set, *name;
    size_t setn, namen, at;
    cc_str value;
} Prop;

static int prop_cmp(const void *a, const void *b) { /* BTreeMap<&str> order, stable by `at` */
    const Prop *x = a, *y = b;
    size_t n = x->setn < y->setn ? x->setn : y->setn;
    int c = memcmp(x->set, y->set, n);
    if (c) return c;
    if (x->setn != y->setn) return x->setn < y->setn ? -1 : 1;
    return x->at < y->at ? -1 : x->at > y->at;
}

static void element_psets(W *w, const cc_element *e, uint64_t el) {
    if (!e->nproperties) return;
    Prop *p = malloc(e->nproperties * sizeof *p);
    if (!p) { w->err = 1; return; }
    for (size_t i = 0; i < e->nproperties; i++) {
        cc_str k = e->properties[i].key;
        const char *s = k.n ? find_byte(k.p, '/', k.n) : NULL;
        if (s) p[i] = (Prop){k.p, s + 1, (size_t)(s - k.p), k.n - (size_t)(s - k.p) - 1, i, e->properties[i].value};
        else p[i] = (Prop){"Bimble_Properties", k.n ? k.p : "", 17, k.n, i, e->properties[i].value};
    }
    if (msort(p, e->nproperties, sizeof *p, prop_cmp)) w->err = 1;
    for (size_t i = 0; i < e->nproperties;) {
        size_t j = i;
        w->tmp.n = 0;
        while (j < e->nproperties && p[j].setn == p[i].setn && !memcmp(p[j].set, p[i].set, p[i].setn)) {
            uint64_t v = begin(w);
            put(w, "IFCPROPERTYSINGLEVALUE("), str(w, p[j].name, p[j].namen), put(w, ",$,IFCLABEL(");
            cstr(w, p[j].value), put(w, "),$)"), end(w, v);
            ids_push(&w->tmp, v);
            j++;
        }
        uint64_t pset = begin(w);
        put(w, "IFCPROPERTYSET(");
        Buf *s = seed_begin(w);
        buf_puts(s, "pset-"), buf_append(s, p[i].set, p[i].setn), buf_putc(s, '/'), buf_append(s, e->id.c, 22);
        gid_end(w);
        put(w, ",$,"), str(w, p[i].set, p[i].setn), put(w, ",$,("), refs(w, w->tmp.v, w->tmp.n), put(w, "))");
        end(w, pset);
        uint64_t rel = begin(w);
        put(w, "IFCRELDEFINESBYPROPERTIES(");
        s = seed_begin(w);
        buf_puts(s, "rel-pset-"), buf_append(s, p[i].set, p[i].setn), buf_putc(s, '/'), buf_append(s, e->id.c, 22);
        gid_end(w);
        put(w, ",$,$,$,("), ref(w, el), put(w, "),"), ref(w, pset), put(w, ")"), end(w, rel);
        i = j;
    }
    free(p);
}

typedef struct {
    double elevation;
    const cc_storey *s;
} StoreyRef;

static int storey_cmp(const void *a, const void *b) { /* total_cmp(elevation), then id */
    const StoreyRef *x = a, *y = b;
    int64_t bx = (int64_t)bits(x->elevation), by = (int64_t)bits(y->elevation);
    bx ^= (int64_t)((uint64_t)(bx >> 63) >> 1), by ^= (int64_t)((uint64_t)(by >> 63) >> 1);
    if (bx != by) return bx < by ? -1 : 1;
    return cc_gid_cmp(&x->s->id, &y->s->id);
}

/* Elements grouped by an index (storey or material), each group in element order. */
static void group(const size_t *key, size_t n, size_t ngroups, size_t *start, size_t *order) {
    memset(start, 0, (ngroups + 1) * sizeof *start);
    for (size_t i = 0; i < n; i++) start[key[i] + 1]++;
    for (size_t g = 0; g < ngroups; g++) start[g + 1] += start[g];
    for (size_t i = 0; i < n; i++) order[start[key[i]]++] = i;
    for (size_t g = ngroups; g > 0; g--) start[g] = start[g - 1];
    start[0] = 0;
}

/* f64::round: half away from zero. */
static double round_away(double x) {
    double ax = x < 0 ? -x : x;
    if (!(ax < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x, d = x - t;
    if (d >= 0.5) t += 1.0;
    else if (d <= -0.5) t -= 1.0;
    if (t == 0.0) return x < 0 || 1.0 / x < 0 ? -0.0 : 0.0;
    return t;
}

/* BuildingFrame::local's turn: world axes into the building's (cos, sin). */
static void turn_frame(double c, double s, const double v[3], double out[3]) {
    out[0] = c * v[0] + s * v[1] + 0.0;
    out[1] = -s * v[0] + c * v[1] + 0.0;
    out[2] = v[2];
}

static void export_ifc4(W *w, const char *file_name, const char *timestamp, Buf *out) {
    const cc_model *m = w->m;
    Buf *d = w->d;

    /* Header (lib.rs, the closing format!). */
    buf_puts(out, "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('ViewDefinition [DesignTransferView]'),'2;1');\nFILE_NAME(");
    cc_step_string(out, file_name, strlen(file_name));
    buf_putc(out, ',');
    cc_step_string(out, timestamp, strlen(timestamp));
    buf_puts(out, ",(''),(''),'bimble " CC_IFC_VERSION "','bimble','');\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n");

    /* Units: mm, m², m³, radian; kg, Pa, kg/m³ (ADR 0031). */
    uint64_t u[7];
    u[0] = add(w, "IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.)");
    u[1] = add(w, "IFCSIUNIT(*,.AREAUNIT.,$,.SQUARE_METRE.)");
    u[2] = add(w, "IFCSIUNIT(*,.VOLUMEUNIT.,$,.CUBIC_METRE.)");
    u[3] = add(w, "IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.)");
    u[4] = add(w, "IFCSIUNIT(*,.MASSUNIT.,.KILO.,.GRAM.)");
    u[5] = add(w, "IFCSIUNIT(*,.PRESSUREUNIT.,$,.PASCAL.)");
    uint64_t u_m = add(w, "IFCSIUNIT(*,.LENGTHUNIT.,$,.METRE.)");
    uint64_t de_kg = begin(w);
    put(w, "IFCDERIVEDUNITELEMENT("), ref(w, u[4]), put(w, ",1)"), end(w, de_kg);
    uint64_t de_m = begin(w);
    put(w, "IFCDERIVEDUNITELEMENT("), ref(w, u_m), put(w, ",-3)"), end(w, de_m);
    uint64_t rho = begin(w);
    put(w, "IFCDERIVEDUNIT(("), ref(w, de_kg), put(w, ","), ref(w, de_m), put(w, "),.MASSDENSITYUNIT.,$)");
    u[6] = end(w, rho);
    uint64_t units = begin(w);
    put(w, "IFCUNITASSIGNMENT(("), refs(w, u, 7), put(w, "))"), end(w, units);

    static const double O[3] = {0, 0, 0}, Z[3] = {0, 0, 1};
    uint64_t origin = point3(w, O);
    uint64_t world = begin(w);
    put(w, "IFCAXIS2PLACEMENT3D("), ref(w, origin), put(w, ",$,$)"), end(w, world);
    uint64_t ctx = begin(w);
    put(w, "IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-05,"), ref(w, world), put(w, ",$)"), end(w, ctx);
    uint64_t body = begin(w);
    put(w, "IFCGEOMETRICREPRESENTATIONSUBCONTEXT('Body','Model',*,*,*,*,"), ref(w, ctx);
    put(w, ",$,.MODEL_VIEW.,$)"), end(w, body);

    /* Project information (ADR 0030). */
    uint64_t project = begin(w);
    put(w, "IFCPROJECT("), gid_lit(w, &m->project_id), put(w, ",$,"), cstr(w, m->project_name), put(w, ",$,$,");
    opt(w, "project_title"), put(w, ","), opt(w, "phase"), put(w, ",("), ref(w, ctx), put(w, "),"), ref(w, units);
    put(w, ")"), end(w, project);
    if (m->nproject) {
        w->tmp.n = 0;
        for (size_t i = 0; i < m->nproject; i++) {
            uint64_t v = begin(w);
            put(w, "IFCPROPERTYSINGLEVALUE("), cstr(w, m->project[i].key), put(w, ",$,IFCTEXT(");
            cstr(w, m->project[i].value), put(w, "),$)"), end(w, v);
            ids_push(&w->tmp, v);
        }
        uint64_t pset = begin(w);
        put(w, "IFCPROPERTYSET("), gid(w, "project-information"), put(w, ",$,'Bimble_ProjectInformation',$,(");
        refs(w, w->tmp.v, w->tmp.n), put(w, "))"), end(w, pset);
        uint64_t rel = begin(w);
        put(w, "IFCRELDEFINESBYPROPERTIES("), gid(w, "project-information/rel"), put(w, ",$,$,$,("), ref(w, project);
        put(w, "),"), ref(w, pset), put(w, ")"), end(w, rel);
    }
    uint64_t site_pl = begin(w);
    put(w, "IFCLOCALPLACEMENT($,"), ref(w, world), put(w, ")"), end(w, site_pl);
    uint64_t site = begin(w);
    put(w, "IFCSITE("), gid(w, "site"), put(w, ",$,'Site',$,$,"), ref(w, site_pl);
    put(w, ",$,$,.ELEMENT.,$,$,$,$,$)"), end(w, site);
    /* The building keeps the model's place and project north (lib.rs BuildingFrame, ADR 0045):
     * placed at a whole metre near the model (the mean of the elements' origins), turned by the
     * building's angle; elements are placed in its frame. None for the default georeference or an
     * empty model: the building sits at the world origin. */
    int framed = !cc_georef_is_default(&m->georef) && m->nelements > 0;
    double fat[3] = {0, 0, 0}, fcos = 1.0, fsin = 0.0;
    uint64_t bldg_axes = world;
    if (framed) {
        double n = (double)m->nelements, sx = 0.0, sy = 0.0;
        for (size_t i = 0; i < m->nelements; i++) sx += m->elements[i].placement.origin[0], sy += m->elements[i].placement.origin[1];
        fat[0] = round_away(sx / n / 1000.0) * 1000.0;
        fat[1] = round_away(sy / n / 1000.0) * 1000.0;
        double a = m->georef.rotation_deg * (3.14159265358979323846 / 180.0);
        fsin = cc_sin(a) + 0.0, fcos = cc_cos(a);
        uint64_t o = point3(w, fat);
        double xd[3] = {fcos, fsin, 0.0};
        uint64_t x = dir3(w, xd);
        bldg_axes = begin(w);
        put(w, "IFCAXIS2PLACEMENT3D("), ref(w, o), put(w, ",$,"), ref(w, x), put(w, ")"), end(w, bldg_axes);
    }
    uint64_t bldg_pl = begin(w);
    put(w, "IFCLOCALPLACEMENT("), ref(w, site_pl), put(w, ","), ref(w, bldg_axes), put(w, ")"), end(w, bldg_pl);
    /* IFC4: the survey system by name, and how the model's coordinates map onto it. */
    if (m->georef.has_crs) {
        const cc_georef *g = &m->georef;
        uint64_t target = begin(w);
        put(w, "IFCPROJECTEDCRS("), cstr(w, g->crs), put(w, ",$,$,$,$,$,$)"), end(w, target);
        double a = g->map_rotation_deg * (3.14159265358979323846 / 180.0);
        uint64_t mc = begin(w);
        put(w, "IFCMAPCONVERSION("), ref(w, ctx), put(w, ","), ref(w, target), put(w, ",");
        real(w, g->east_m), put(w, ","), real(w, g->north_m), put(w, ","), real(w, g->height_m), put(w, ",");
        real(w, cc_cos(a)), put(w, ","), real(w, cc_sin(a) + 0.0), put(w, ",$)"), end(w, mc);
    }
    uint64_t bldg = begin(w);
    put(w, "IFCBUILDING("), gid(w, "building"), put(w, ",$,");
    const cc_str *bname = project_value(m, "building");
    if (bname) cstr(w, *bname);
    else put(w, "'Building'");
    put(w, ",$,$,"), ref(w, bldg_pl), put(w, ",$,$,.ELEMENT.,$,$,$)"), end(w, bldg);
    uint64_t agg = begin(w);
    put(w, "IFCRELAGGREGATES("), gid(w, "agg/project"), put(w, ",$,$,$,"), ref(w, project), put(w, ",(");
    ref(w, site), put(w, "))"), end(w, agg);
    agg = begin(w);
    put(w, "IFCRELAGGREGATES("), gid(w, "agg/site"), put(w, ",$,$,$,"), ref(w, site), put(w, ",(");
    ref(w, bldg), put(w, "))"), end(w, agg);

    /* Storeys, sorted by elevation. */
    size_t ns = m->nstoreys, ne = m->nelements, nm = m->nmaterials;
    StoreyRef *sorted = malloc((ns ? ns : 1) * sizeof *sorted);
    uint64_t *st_ent = malloc((ns ? ns : 1) * sizeof *st_ent), *st_pl = malloc((ns ? ns : 1) * sizeof *st_pl);
    uint64_t *el_ent = malloc((ne ? ne : 1) * sizeof *el_ent);
    size_t *el_st = malloc((ne ? ne : 1) * sizeof *el_st), *el_mat = malloc((ne ? ne : 1) * sizeof *el_mat);
    size_t *order = malloc((ne ? ne : 1) * sizeof *order);
    size_t *start = malloc((ns + nm + 2) * sizeof *start);
    if (!sorted || !st_ent || !st_pl || !el_ent || !el_st || !el_mat || !order || !start) {
        w->err = 1;
        goto done;
    }
    for (size_t i = 0; i < ns; i++) sorted[i] = (StoreyRef){m->storeys[i].elevation, &m->storeys[i]};
    if (msort(sorted, ns, sizeof *sorted, storey_cmp)) { w->err = 1; goto done; }
    w->tmp.n = 0;
    for (size_t i = 0; i < ns; i++) {
        const cc_storey *s = sorted[i].s;
        size_t k = (size_t)(s - m->storeys);
        if (s->not_storey) continue; /* ADR 0056: a datum level is not an IfcBuildingStorey (holds no element) */
        double p[3] = {0.0, 0.0, s->elevation};
        uint64_t o = point3(w, p);
        uint64_t a = begin(w);
        put(w, "IFCAXIS2PLACEMENT3D("), ref(w, o), put(w, ",$,$)"), end(w, a);
        uint64_t pl = begin(w);
        put(w, "IFCLOCALPLACEMENT("), ref(w, bldg_pl), put(w, ","), ref(w, a), put(w, ")"), end(w, pl);
        uint64_t id = begin(w);
        put(w, "IFCBUILDINGSTOREY("), gid_lit(w, &s->id), put(w, ",$,"), cstr(w, s->name), put(w, ",$,$,");
        ref(w, pl), put(w, ",$,$,.ELEMENT."), put(w, ","), real(w, s->elevation), put(w, ")"), end(w, id);
        st_ent[k] = id, st_pl[k] = pl;
        ids_push(&w->tmp, id);
    }
    if (w->tmp.n) {
        agg = begin(w);
        put(w, "IFCRELAGGREGATES("), gid(w, "agg/building"), put(w, ",$,$,$,"), ref(w, bldg), put(w, ",(");
        refs(w, w->tmp.v, w->tmp.n), put(w, "))"), end(w, agg);
    }

    uint64_t z = dir3(w, Z);
    for (size_t i = 0; i < ne && !w->err; i++) {
        const cc_element *e = &m->elements[i];
        const cc_storey *st = cc_model_storey(m, &e->storey);
        const cc_material *mat = cc_model_material(m, &e->material);
        if (!st || !mat || st->not_storey) { w->err = 1; break; }
        el_st[i] = (size_t)(st - m->storeys), el_mat[i] = (size_t)(mat - m->materials);
        const cc_placement *p = &e->placement;
        cc_placement local;
        if (framed) { /* BuildingFrame::local: the storey-relative placement in the building's frame */
            const double o0[3] = {p->origin[0] - fat[0], p->origin[1] - fat[1], p->origin[2]};
            turn_frame(fcos, fsin, o0, local.origin);
            turn_frame(fcos, fsin, p->z_axis, local.z_axis);
            turn_frame(fcos, fsin, p->x_axis, local.x_axis);
            p = &local;
        }
        uint64_t o = point3(w, p->origin), az = dir3(w, p->z_axis), ax = dir3(w, p->x_axis);
        uint64_t a = begin(w);
        put(w, "IFCAXIS2PLACEMENT3D("), ref(w, o), put(w, ","), ref(w, az), put(w, ","), ref(w, ax), put(w, ")");
        end(w, a);
        uint64_t pl = begin(w);
        put(w, "IFCLOCALPLACEMENT("), ref(w, st_pl[el_st[i]]), put(w, ","), ref(w, a), put(w, ")"), end(w, pl);

        const char *repr;
        uint64_t solid = write_geometry(w, &e->generator, world, z, &repr);
        uint64_t shape = begin(w);
        put(w, "IFCSHAPEREPRESENTATION("), ref(w, body), put(w, ",'Body','"), put(w, repr), put(w, "',(");
        ref(w, solid), put(w, "))"), end(w, shape);
        uint64_t pds = begin(w);
        put(w, "IFCPRODUCTDEFINITIONSHAPE($,$,("), ref(w, shape), put(w, "))"), end(w, pds);

        uint64_t el = begin(w);
        const char *cls = cc_ifc_class_name(e->cls);
        for (const char *c = cls; *c; c++) buf_putc(d, *c >= 'a' && *c <= 'z' ? (char)(*c - 32) : *c);
        put(w, "("), gid_lit(w, &e->id), put(w, ",$,"), cstr(w, e->name), put(w, ",$,");
        if (e->has_ifc_entity) cstr(w, e->ifc_entity);
        else put(w, "$");
        put(w, ","), ref(w, pl), put(w, ","), ref(w, pds), put(w, ",$,"), put(w, predefined(e)), put(w, ")");
        el_ent[i] = end(w, el);

        /* Generator pset: parameters as canonical JSON (data only, never executed). */
        buf_clear(&w->json);
        if (generator_json(&e->generator, &w->json) || w->json.oom) w->err = 1;
        uint64_t p1 = begin(w);
        put(w, "IFCPROPERTYSINGLEVALUE('Generator',$,IFCLABEL(");
        const char *gname = cc_gen_kind_name(e->generator.kind);
        str(w, gname, strlen(gname)), put(w, "),$)"), end(w, p1);
        uint64_t p2 = add(w, "IFCPROPERTYSINGLEVALUE('Version',$,IFCINTEGER(1),$)");
        uint64_t p3 = begin(w);
        put(w, "IFCPROPERTYSINGLEVALUE('Parameters',$,IFCTEXT("), str(w, w->json.p ? w->json.p : "", w->json.len);
        put(w, "),$)"), end(w, p3);
        uint64_t pset = begin(w), ps[3] = {p1, p2, p3};
        put(w, "IFCPROPERTYSET("), gid_id(w, "pset/", &e->id), put(w, ",$,'Bimble_Generator',$,(");
        refs(w, ps, 3), put(w, "))"), end(w, pset);
        uint64_t rel = begin(w);
        put(w, "IFCRELDEFINESBYPROPERTIES("), gid_id(w, "rel-pset/", &e->id), put(w, ",$,$,$,("), ref(w, el);
        put(w, "),"), ref(w, pset), put(w, ")"), end(w, rel);

        /* No openings in the C model. Property sets beyond the material (ADR 0032). */
        element_psets(w, e, el);
    }
    if (w->err) goto done;

    /* Containment, by storey id. */
    group(el_st, ne, ns, start, order);
    for (size_t s = 0; s < ns; s++) {
        if (start[s] == start[s + 1]) continue;
        w->tmp.n = 0;
        for (size_t k = start[s]; k < start[s + 1]; k++) ids_push(&w->tmp, el_ent[order[k]]);
        uint64_t rel = begin(w);
        put(w, "IFCRELCONTAINEDINSPATIALSTRUCTURE("), gid_id(w, "contains/", &m->storeys[s].id), put(w, ",$,$,$,(");
        refs(w, w->tmp.v, w->tmp.n), put(w, "),"), ref(w, st_ent[s]), put(w, ")"), end(w, rel);
    }
    /* Materials in use, by id, with their values in the standard property sets (ADR 0031). */
    group(el_mat, ne, nm, start, order);
    for (size_t k = 0; k < nm; k++) {
        if (start[k] == start[k + 1]) continue;
        const cc_material *mat = &m->materials[k];
        uint64_t me = begin(w);
        put(w, "IFCMATERIAL("), cstr(w, mat->name), put(w, ",$,");
        const char *cat = ifc_category(mat->category);
        str(w, cat, strlen(cat)), put(w, ")"), end(w, me);
        const cc_material_props *p = &mat->props;
        const char *set_name[3];
        uint64_t set_val[3];
        int nsets = 0;
        if (p->has_density) {
            uint64_t v = begin(w);
            put(w, "IFCPROPERTYSINGLEVALUE('MassDensity',$,IFCMASSDENSITYMEASURE("), real(w, p->density_kg_m3);
            put(w, "),$)"), end(w, v);
            set_name[nsets] = "Pset_MaterialCommon", set_val[nsets++] = v;
        }
        if (p->has_e) {
            uint64_t v = begin(w);
            put(w, "IFCPROPERTYSINGLEVALUE('YoungModulus',$,IFCMODULUSOFELASTICITYMEASURE(");
            real(w, p->e_mpa * 1e6), put(w, "),$)"), end(w, v);
            set_name[nsets] = "Pset_MaterialMechanical", set_val[nsets++] = v;
        }
        if (p->has_strength && (mat->category == CC_CAT_CONCRETE || mat->category == CC_CAT_STEEL)) {
            int concrete = mat->category == CC_CAT_CONCRETE;
            uint64_t v = begin(w);
            put(w, concrete ? "IFCPROPERTYSINGLEVALUE('CompressiveStrength',$,IFCPRESSUREMEASURE("
                            : "IFCPROPERTYSINGLEVALUE('YieldStress',$,IFCPRESSUREMEASURE(");
            real(w, p->strength_mpa * 1e6), put(w, "),$)"), end(w, v);
            set_name[nsets] = concrete ? "Pset_MaterialConcrete" : "Pset_MaterialSteel", set_val[nsets++] = v;
        }
        for (int s = 0; s < nsets; s++) {
            uint64_t mp = begin(w);
            put(w, "IFCMATERIALPROPERTIES('"), put(w, set_name[s]), put(w, "',$,("), ref(w, set_val[s]);
            put(w, "),"), ref(w, me), put(w, ")"), end(w, mp);
        }
        w->tmp.n = 0;
        for (size_t q = start[k]; q < start[k + 1]; q++) ids_push(&w->tmp, el_ent[order[q]]);
        uint64_t rel = begin(w);
        put(w, "IFCRELASSOCIATESMATERIAL("), gid_id(w, "material/", &mat->id), put(w, ",$,$,$,(");
        refs(w, w->tmp.v, w->tmp.n), put(w, "),"), ref(w, me), put(w, ")"), end(w, rel);
    }
    if (cc_ifc_out_grids(m, d, &w->next, ctx, world, st_pl, st_ent, bldg_pl, bldg)) w->err = 1; /* ADR 0056 */
done:
    free(sorted), free(st_ent), free(st_pl), free(el_ent), free(el_st), free(el_mat), free(order), free(start);
    if (w->tmp.oom) w->err = 1;
    buf_append(out, d->p ? d->p : "", d->len);
    buf_puts(out, "ENDSEC;\nEND-ISO-10303-21;\n");
}

/* ---- IFC2x3 (ifc2x3.rs from_ifc4) ---- */

static int in_list(const char *s, size_t n, const char *const *list) {
    for (; *list; list++)
        if (strlen(*list) == n && !memcmp(*list, s, n)) return 1;
    return 0;
}

static const char *const ROOTED[] = {"IFCPROJECT", "IFCSITE", "IFCBUILDING", "IFCBUILDINGSTOREY", "IFCCOLUMN",
                                     "IFCBEAM", "IFCSLAB", "IFCWALL", "IFCFOOTING", "IFCBUILDINGELEMENTPROXY",
                                     "IFCOPENINGELEMENT", "IFCPROPERTYSET", "IFCRELAGGREGATES",
                                     "IFCRELCONTAINEDINSPATIALSTRUCTURE", "IFCRELDEFINESBYPROPERTIES",
                                     "IFCRELVOIDSELEMENT", "IFCRELASSOCIATESMATERIAL", "IFCGRID", NULL};
static const char *const DROPPED[] = {"IFCMATERIALPROPERTIES", "IFCPROJECTEDCRS", "IFCMAPCONVERSION", NULL};

#define MAX_ARGS 64

typedef struct {
    uint64_t id;
    const char *name, *body;
    size_t namen, bodyn;
} Ent;

/* `#12=NAME(a,b,…);` -> id, name, body (between the parentheses). ifc2x3.rs `entity`. */
static int parse_entity(const char *l, size_t n, Ent *e) {
    if (n < 1 || l[0] != '#') return -1;
    const char *eq = find_byte(l, '=', n);
    if (!eq || eq == l + 1) return -1;
    uint64_t id = 0;
    for (const char *c = l + 1; c < eq; c++) {
        if (*c < '0' || *c > '9' || id > (UINT64_MAX - 9) / 10) return -1;
        id = id * 10 + (uint64_t)(*c - '0');
    }
    const char *rest = eq + 1, *endp = l + n;
    const char *open = find_byte(rest, '(', (size_t)(endp - rest));
    if (!open || endp - open < 3 || memcmp(endp - 2, ");", 2)) return -1;
    *e = (Ent){id, rest, open + 1, (size_t)(open - rest), (size_t)(endp - 2 - (open + 1))};
    return 0;
}

/* Top-level arguments as written. Returns the count, -1 above MAX_ARGS. */
static int split_args(const char *s, size_t n, cc_str *args) {
    int na = 0, depth = 0, quoted = 0;
    size_t cur = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\'' && quoted && i + 1 < n && s[i + 1] == '\'') { i++; continue; }
        if (c == '\'') quoted = !quoted;
        else if (c == '(' && !quoted) depth++;
        else if (c == ')' && !quoted) depth--;
        else if (c == ',' && !quoted && depth == 0) {
            if (na == MAX_ARGS) return -1;
            args[na++] = (cc_str){s + cur, i - cur};
            cur = i + 1;
        }
    }
    if (na == MAX_ARGS) return -1;
    args[na++] = (cc_str){s + cur, n - cur};
    return na;
}

static void put_replaced(Buf *out, const char *l, size_t n, const char *from, const char *to) {
    size_t fn = strlen(from);
    for (size_t i = 0; i < n;) {
        if (n - i >= fn && !memcmp(l + i, from, fn)) buf_puts(out, to), i += fn;
        else buf_putc(out, l[i++]);
    }
}

static int to_ifc2x3(const char *t, size_t n, Buf *out) {
    uint64_t max_id = 0;
    Buf line;
    buf_init(&line);
    /* Pass 1: the largest id (ifc2x3.rs collects it with the data). */
    int in_data = 0;
    for (size_t i = 0; i < n;) {
        const char *nl = find_byte(t + i, '\n', n - i);
        size_t len = nl ? (size_t)(nl - (t + i)) : n - i;
        const char *l = t + i;
        i += len + (nl != NULL);
        if (len && l[len - 1] == '\r') len--;
        if (len == 5 && !memcmp(l, "DATA;", 5)) { in_data = 1; continue; }
        if (in_data && len == 7 && !memcmp(l, "ENDSEC;", 7)) { in_data = 0; continue; }
        Ent e;
        if (in_data && !parse_entity(l, len, &e) && e.id > max_id) max_id = e.id;
    }
    uint64_t oh = max_id + 5, at2 = max_id + 7;
    /* Pass 2: the header lines, then the data. */
    Buf data;
    buf_init(&data);
    cc_str args[MAX_ARGS];
    int rc = 0;
    char ohs[24], at2s[24]; /* "#<oh>", "#<at2>" */
    size_t ohn = 0, at2n = 0;
    {
        Buf t;
        buf_init(&t);
        buf_putc(&t, '#'), buf_u64(&t, oh);
        if (!t.oom) memcpy(ohs, t.p, ohn = t.len);
        buf_clear(&t), buf_putc(&t, '#'), buf_u64(&t, at2);
        if (!t.oom) memcpy(at2s, t.p, at2n = t.len);
        if (t.oom) rc = -1;
        buf_free(&t);
    }
    in_data = 0;
    for (size_t i = 0; i < n;) {
        const char *nl = find_byte(t + i, '\n', n - i);
        size_t len = nl ? (size_t)(nl - (t + i)) : n - i;
        const char *l = t + i;
        i += len + (nl != NULL);
        if (len && l[len - 1] == '\r') len--;
        if (len == 5 && !memcmp(l, "DATA;", 5)) { in_data = 1; continue; }
        if (in_data && len == 7 && !memcmp(l, "ENDSEC;", 7)) { in_data = 0; continue; }
        if (in_data) {
            Ent e;
            if (parse_entity(l, len, &e)) continue;
            if (in_list(e.name, e.namen, DROPPED)) continue;
            int na = split_args(e.body, e.bodyn, args);
            if (na < 0) { rc = -1; continue; }
            if (in_list(e.name, e.namen, ROOTED) && na > 1 && args[1].n == 1 && args[1].p[0] == '$')
                args[1] = (cc_str){ohs, ohn};
#define NAME_IS(s) (e.namen == sizeof(s) - 1 && !memcmp(e.name, s, sizeof(s) - 1))
            if (NAME_IS("IFCCOLUMN") || NAME_IS("IFCBEAM") || NAME_IS("IFCWALL") || NAME_IS("IFCOPENINGELEMENT") ||
                NAME_IS("IFCGRID")) { /* IFC2x3's IfcGrid has no PredefinedType (ADR 0056) */
                na--; /* Rust's pop on a list that always has at least one argument */
            } else if (NAME_IS("IFCBUILDINGELEMENTPROXY")) {
                args[na - 1] = (cc_str){"$", 1};
            } else if (NAME_IS("IFCMATERIAL")) {
                if (na > 1) na = 1;
            } else if (NAME_IS("IFCRECTANGLEPROFILEDEF") && na > 2 && args[2].n == 1 && args[2].p[0] == '$') {
                args[2] = (cc_str){at2s, at2n};
            }
#undef NAME_IS
            buf_putc(&data, '#'), buf_u64(&data, e.id), buf_putc(&data, '=');
            buf_append(&data, e.name, e.namen), buf_putc(&data, '(');
            for (int a = 0; a < na; a++) {
                if (a) buf_putc(&data, ',');
                buf_append(&data, args[a].p, args[a].n);
            }
            buf_puts(&data, ");\n");
            continue;
        }
        if (len == 17 && !memcmp(l, "END-ISO-10303-21;", 17)) continue;
        buf_clear(&line);
        put_replaced(&line, l, len, "FILE_SCHEMA(('IFC4'));", "FILE_SCHEMA(('IFC2X3'));");
        put_replaced(out, line.p ? line.p : "", line.len, "'ViewDefinition [DesignTransferView]'",
                     "'ViewDefinition [CoordinationView_V2.0]'");
        buf_putc(out, '\n');
    }
    buf_puts(out, "DATA;\n");
    buf_append(out, data.p ? data.p : "", data.len);
    /* The owner history every rooted object points to: Bimble, no change tracking. */
    uint64_t h = max_id;
    buf_putc(out, '#'), buf_u64(out, h + 1), buf_puts(out, "=IFCPERSON($,$,'',$,$,$,$,$);\n");
    buf_putc(out, '#'), buf_u64(out, h + 2), buf_puts(out, "=IFCORGANIZATION($,'Bimble',$,$,$);\n");
    buf_putc(out, '#'), buf_u64(out, h + 3), buf_puts(out, "=IFCPERSONANDORGANIZATION(#"), buf_u64(out, h + 1);
    buf_puts(out, ",#"), buf_u64(out, h + 2), buf_puts(out, ",$);\n");
    buf_putc(out, '#'), buf_u64(out, h + 4), buf_puts(out, "=IFCAPPLICATION(#"), buf_u64(out, h + 2);
    buf_puts(out, ",'" CC_IFC_VERSION "','Bimble','bimble');\n");
    buf_putc(out, '#'), buf_u64(out, oh), buf_puts(out, "=IFCOWNERHISTORY(#"), buf_u64(out, h + 3);
    buf_puts(out, ",#"), buf_u64(out, h + 4), buf_puts(out, ",$,.NOCHANGE.,$,$,$,0);\n");
    buf_putc(out, '#'), buf_u64(out, h + 6), buf_puts(out, "=IFCCARTESIANPOINT((0.,0.));\n");
    buf_putc(out, '#'), buf_u64(out, at2), buf_puts(out, "=IFCAXIS2PLACEMENT2D(#"), buf_u64(out, h + 6);
    buf_puts(out, ",$);\n");
    buf_puts(out, "ENDSEC;\nEND-ISO-10303-21;\n");
    if (data.oom || line.oom) rc = -1;
    buf_free(&data);
    buf_free(&line);
    return rc;
}

int cc_ifc_export(const cc_model *m, const char *file_name, const char *timestamp, cc_ifc_schema schema,
                  Buf *out) {
    Buf data;
    buf_init(&data);
    W w = {m, &data, 1, 0, {0}, {0}, {0}};
    buf_init(&w.seed), buf_init(&w.json);
    int rc;
    if (schema == CC_IFC2X3) {
        Buf ifc4;
        buf_init(&ifc4);
        export_ifc4(&w, file_name ? file_name : "", timestamp ? timestamp : "", &ifc4);
        rc = w.err || ifc4.oom ? -1 : to_ifc2x3(ifc4.p, ifc4.len, out);
        buf_free(&ifc4);
    } else {
        export_ifc4(&w, file_name ? file_name : "", timestamp ? timestamp : "", out);
        rc = w.err ? -1 : 0;
    }
    if (data.oom || w.seed.oom || w.json.oom || out->oom) rc = -1;
    buf_free(&data), buf_free(&w.seed), buf_free(&w.json), free(w.tmp.v);
    return rc;
}
