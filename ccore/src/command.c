/* Commands: apply, inverse, describe, JSON (crates/bimble-core/src/command.rs). See command.h.
 * Model arrays live in the model's arena: an insert makes a new array there (the old one stays
 * until the model is freed), a removal shifts in place. Properties and project maps are copied
 * on write, never changed in place, because inverses in the history may still point to them. */
#include "command.h"
#include "georef.h"
#include <stdlib.h>
#include <string.h>
#include "file.h"
#include "grid.h"
#include "relation.h"
#include "view_io.h"

#ifndef __TINYC__ /* tcc never contracts and warns on the pragma */
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#endif

/* ---- lists ---- */

void cc_cmds_init(cc_cmds *l) {
    arena_init(&l->arena, 0);
    l->v = NULL;
    l->n = l->cap = 0;
}

void cc_cmds_free(cc_cmds *l) {
    arena_free(&l->arena);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

cc_command *cc_cmds_push(cc_cmds *l, cc_cmd_op op) {
    if (l->n == l->cap) {
        size_t cap = l->cap ? 2 * l->cap : 8;
        cc_command *v = realloc(l->v, cap * sizeof *v);
        if (!v) return NULL;
        l->v = v;
        l->cap = cap;
    }
    cc_command *c = &l->v[l->n++];
    memset(c, 0, sizeof *c);
    c->op = op;
    return c;
}

static const char *const OPS[] = {"add_storey",     "remove_storey",  "add_material",  "remove_material",
                                  "add_element",    "remove_element", "set_generator", "set_placement",
                                  "rename",         "set_element_material", "set_property", "set_project",
                                  "add_view",       "remove_view",    "set_view",      "add_sheet",
                                  "remove_sheet",   "set_sheet",      "set_schedules", "add_relation",
                                  "remove_relation", "set_relation", "set_storey", "add_grid",
                                  "remove_grid",    "set_grid",       "set_georef",    "set_material",
                                  "set_openings"};

const char *cc_cmd_op_name(cc_cmd_op op) { return (unsigned)op < CC_CMD_COUNT ? OPS[op] : "?"; }

/* ---- text ---- */

static int finite_(double x) { return x - x == 0.0; }
static double fabs_(double x) { return x < 0 ? -x : x; }

/* Decodes one UTF-8 character at s[*i] (input already valid UTF-8), advances *i. */
static uint32_t utf8_at(const char *s, size_t n, size_t *i) {
    const unsigned char *p = (const unsigned char *)s;
    uint32_t c = p[*i];
    size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (*i + len > n) len = n - *i;
    if (len == 2) c &= 0x1F;
    else if (len == 3) c &= 0x0F;
    else if (len == 4) c &= 0x07;
    for (size_t k = 1; k < len; k++) c = (c << 6) | (p[*i + k] & 0x3F);
    *i += len;
    return c;
}

/* char::is_whitespace (Unicode White_Space). */
static int is_ws(uint32_t c) {
    return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

/* char::is_control (general category Cc). */
static int is_cc(uint32_t c) { return c < 0x20 || (c >= 0x7F && c <= 0x9F); }

cc_str cc_str_trim(cc_str s) {
    size_t i = 0, start = 0, end = 0;
    int seen = 0;
    while (i < s.n) {
        size_t at = i;
        uint32_t c = utf8_at(s.p, s.n, &i);
        if (!is_ws(c)) {
            if (!seen) start = at, seen = 1;
            end = i;
        }
    }
    cc_str t = {seen ? s.p + start : s.p, seen ? end - start : 0};
    return t;
}

/* str::eq_ignore_ascii_case on n bytes each. */
static int ascii_ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
        char y = b[i] >= 'A' && b[i] <= 'Z' ? (char)(b[i] + 32) : b[i];
        if (x != y) return 0;
    }
    return 1;
}

static size_t utf8_count(cc_str s) {
    size_t k = 0;
    for (size_t i = 0; i < s.n; i++)
        if (((unsigned char)s.p[i] & 0xC0) != 0x80) k++;
    return k;
}

static int str_eq(cc_str a, cc_str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }

/* ---- errors ---- */

static int err_id(Buf *err, const char *pre, const cc_gid *id, const char *post) {
    if (err) {
        buf_clear(err);
        buf_puts(err, pre);
        cc_buf_gid(err, id);
        buf_puts(err, post);
    }
    return -1;
}
static int not_found(Buf *err, const cc_gid *id) { return err_id(err, "no such object: ", id, ""); }
static int exists(Buf *err, const cc_gid *id) { return err_id(err, "object already exists: ", id, ""); }
static int in_use(Buf *err, const cc_gid *id) { return err_id(err, "", id, " is still used by elements"); }
static int invalid(Buf *err, const char *why, size_t n) {
    if (err) {
        buf_clear(err);
        buf_puts(err, "invalid: ");
        buf_append(err, why, n);
    }
    return -1;
}
static int invalid_buf(Buf *err, Buf *why) {
    invalid(err, why->oom ? "" : why->p, why->oom ? 0 : why->len);
    buf_free(why);
    return -1;
}

/* ---- validation, as model.rs (model.c keeps its copies static) ---- */

static int pos_check(Buf *b, const char *name, double v) {
    if (finite_(v) && v > 0.0) return 0;
    buf_puts(b, name);
    buf_puts(b, " must be > 0, got ");
    cc_buf_display_f64(b, v);
    return -1;
}

static double area2(const cc_loop *l) {
    double s = -0.0;
    for (size_t i = 0; i < l->n; i++) {
        const double *a = l->pts[i], *c = l->pts[(i + 1) % l->n];
        s += a[0] * c[1] - c[0] * a[1];
    }
    return s;
}

static int loop_finite(const cc_loop *l) {
    for (size_t i = 0; i < l->n; i++)
        if (!finite_(l->pts[i][0]) || !finite_(l->pts[i][1])) return 0;
    return 1;
}

static int loop_check(Buf *b, const char *what, size_t idx, int indexed, const cc_loop *l) {
    if (l->n < 3 || !loop_finite(l)) {
        buf_puts(b, what);
        if (indexed) buf_putc(b, ' '), buf_u64(b, idx);
        buf_puts(b, l->n < 3 ? " needs at least 3 points" : " has non-finite coordinates");
        return -1;
    }
    return 0;
}

/* Generator::validate: the bare message in b on failure. */
static int gen_validate(Buf *b, const cc_generator *g) {
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
        return pos_check(b, "width", g->u.col.width) || pos_check(b, "depth", g->u.col.depth) ||
                       pos_check(b, "height", g->u.col.height)
                   ? -1
                   : 0;
    case CC_GEN_RECT_BEAM:
        return pos_check(b, "width", g->u.beam.width) || pos_check(b, "height", g->u.beam.height) ||
                       pos_check(b, "length", g->u.beam.length)
                   ? -1
                   : 0;
    case CC_GEN_WALL:
        return pos_check(b, "length", g->u.wall.length) || pos_check(b, "thickness", g->u.wall.thickness) ||
                       pos_check(b, "height", g->u.wall.height)
                   ? -1
                   : 0;
    case CC_GEN_SLAB: {
        const cc_loop *o = &g->u.slab.outline;
        if (pos_check(b, "thickness", g->u.slab.thickness)) return -1;
        if (o->n < 3) { buf_puts(b, "slab outline needs at least 3 points"); return -1; }
        if (!loop_finite(o)) { buf_puts(b, "slab outline has non-finite coordinates"); return -1; }
        if (area2(o) <= 0.0) {
            buf_puts(b, "slab outline must be counter-clockwise with non-zero area");
            return -1;
        }
        return 0;
    }
    case CC_GEN_EXTRUDED_POLYGON: {
        if (pos_check(b, "height", g->u.poly.height)) return -1;
        if (loop_check(b, "outline", 0, 0, &g->u.poly.outline)) return -1;
        if (area2(&g->u.poly.outline) <= 0.0) {
            buf_puts(b, "extruded polygon outline must be counter-clockwise");
            return -1;
        }
        for (size_t i = 0; i < g->u.poly.nvoids; i++)
            if (loop_check(b, "void", i, 1, &g->u.poly.voids[i])) return -1;
        return 0;
    }
    case CC_GEN_MESH: {
        for (size_t i = 0; i < g->u.mesh.npositions; i++)
            for (int k = 0; k < 3; k++)
                if (!finite_(g->u.mesh.positions[i][k])) {
                    buf_puts(b, "mesh has non-finite coordinates");
                    return -1;
                }
        if ((uint64_t)g->u.mesh.npositions > 0xFFFFFFFFull) {
            buf_puts(b, "mesh has too many vertices");
            return -1;
        }
        for (size_t i = 0; i < g->u.mesh.nindices; i++)
            if ((size_t)g->u.mesh.indices[i] >= g->u.mesh.npositions) {
                buf_puts(b, "mesh index out of range");
                return -1;
            }
        return 0;
    }
    default: buf_puts(b, "unknown generator"); return -1;
    }
}

/* Placement::validate. */
static int placement_validate(Buf *b, const cc_placement *p) {
    const double *v[3] = {p->origin, p->z_axis, p->x_axis};
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++)
            if (!finite_(v[i][k])) { buf_puts(b, "non-finite placement"); return -1; }
    const double *z = p->z_axis, *x = p->x_axis;
    double zz = z[0] * z[0] + z[1] * z[1] + z[2] * z[2];
    double xx = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
    double zx = z[0] * x[0] + z[1] * x[1] + z[2] * x[2];
    const double TOL = 1e-6;
    if (fabs_(zz - 1.0) > TOL || fabs_(xx - 1.0) > TOL) {
        buf_puts(b, "placement axes must be unit vectors");
        return -1;
    }
    if (fabs_(zx) > TOL) {
        buf_puts(b, "placement x_axis must be perpendicular to z_axis");
        return -1;
    }
    return 0;
}

/* MaterialProps::validate. */
/* ADR 0070: an IfcElementAssembly has no generator (its shape is its parts) and no openings;
 * every other element has a generator of its own. The same texts as model.c. */
static int assembly_check(Buf *b, cc_ifc_class cls, const cc_generator *g, size_t nopenings) {
    if (cls != CC_IFC_ELEMENT_ASSEMBLY) {
        if (g->kind == CC_GEN_NONE) {
            buf_puts(b, "missing generator");
            return -1;
        }
        return gen_validate(b, g);
    }
    if (g->kind != CC_GEN_NONE) {
        buf_puts(b, "an IfcElementAssembly has no generator: its shape is its parts");
        return -1;
    }
    if (nopenings) {
        buf_puts(b, "an IfcElementAssembly has no openings");
        return -1;
    }
    return 0;
}

static int props_validate(Buf *b, const cc_material_props *p) {
    const char *what[3] = {"density", "modulus", "strength"};
    const char *maxs[3] = {"30000", "1000000", "10000"};
    const double maxv[3] = {30000.0, 1000000.0, 10000.0};
    const int has[3] = {p->has_density, p->has_e, p->has_strength};
    const double val[3] = {p->density_kg_m3, p->e_mpa, p->strength_mpa};
    for (int j = 0; j < 3; j++)
        if (has[j] && !(finite_(val[j]) && val[j] > 0.0 && val[j] <= maxv[j])) {
            buf_puts(b, "material ");
            buf_puts(b, what[j]);
            buf_puts(b, ": a positive number up to ");
            buf_puts(b, maxs[j]);
            return -1;
        }
    if (utf8_count(p->source) > 200) {
        buf_puts(b, "material source: at most 200 characters");
        return -1;
    }
    return 0;
}

/* project::validate, through the model validator (the same code as a file load). */
static int project_validate(Buf *b, const cc_kv *kv, size_t n) {
    cc_model t;
    memset(&t, 0, sizeof t);
    t.project = (cc_kv *)kv;
    t.nproject = n;
    cc_error e;
    if (cc_model_validate(&t, &e) == 0) return 0;
    buf_puts(b, e.msg);
    return -1;
}

/* property_text: a property name or value typed by a person. */
static int property_text(Buf *err, const char *what, cc_str s, size_t max) {
    Buf b;
    buf_init(&b);
    cc_str t = cc_str_trim(s);
    int bad = 1;
    buf_puts(&b, what);
    if (t.n == 0) buf_puts(&b, " is empty");
    else if (t.n != s.n) buf_puts(&b, " starts or ends with a space");
    else {
        int ctrl = 0;
        for (size_t i = 0; i < s.n && !ctrl;) ctrl = is_cc(utf8_at(s.p, s.n, &i));
        if (ctrl) buf_puts(&b, " has a line break or a control character");
        else if (utf8_count(s) > max) {
            buf_puts(&b, " is longer than ");
            buf_u64(&b, max);
            buf_puts(&b, " characters");
        } else bad = 0;
    }
    if (!bad) {
        buf_free(&b);
        return 0;
    }
    return invalid_buf(err, &b);
}

/* ---- sorted arrays in the model's arena ---- */

/* Index of the first entry with id >= key (entries of `size` bytes, cc_gid first). */
static size_t lower_bound(const void *arr, size_t n, size_t size, const cc_gid *key) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cc_gid_cmp((const cc_gid *)((const char *)arr + mid * size), key) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* A new array of n + 1 entries with `item` at `at`. NULL when out of memory. */
static void *arr_insert(Arena *a, const void *arr, size_t n, size_t size, size_t at, const void *item) {
    char *p = arena_alloc(a, (n + 1) * size, 16);
    if (!p) return NULL;
    if (at) memcpy(p, arr, at * size);
    memcpy(p + at * size, item, size);
    if (n > at) memcpy(p + (at + 1) * size, (const char *)arr + at * size, (n - at) * size);
    return p;
}

/* The same into a growing array (ticket J1): in place while `*cap` has room, else a new array
 * with twice the room. A copy per insert made an add O(n) in time and in arena bytes that are
 * never given back (60k elements: 16 MB per place). *cap 0 means exactly n (a loaded model). */
static void *arr_insert_grow(Arena *a, void *arr, size_t n, size_t *cap, size_t size, size_t at, const void *item) {
    char *p = arr;
    if (n + 1 > *cap) {
        size_t c = n < 8 ? 16 : n * 2;
        if (c > SIZE_MAX / size) return NULL;
        p = arena_alloc(a, c * size, 16);
        if (!p) return NULL;
        if (at) memcpy(p, arr, at * size);
        if (n > at) memcpy(p + (at + 1) * size, (const char *)arr + at * size, (n - at) * size);
        *cap = c;
    } else if (n > at) {
        memmove(p + (at + 1) * size, p + at * size, (n - at) * size);
    }
    memcpy(p + at * size, item, size);
    return p;
}

static void arr_remove(void *arr, size_t n, size_t size, size_t at) {
    char *p = arr;
    if (at + 1 < n) memmove(p + at * size, p + (at + 1) * size, (n - at - 1) * size);
}

static void put_name(Buf *b, cc_str s) { buf_append(b, s.p, s.n < 200 ? s.n : 200); }

static int oom(Buf *err) {
    if (err) {
        buf_clear(err);
        buf_puts(err, "out of memory");
    }
    return -1;
}

static cc_element *find_element(cc_model *m, const cc_gid *id) { return (cc_element *)cc_model_element(m, id); }

/* ---- deep copy into an arena, canonical on the way (x + 0.0: -0.0 becomes 0.0) ---- */

/* arena_dup aligns to 1: arrays of structs need their alignment. */
static void *dup_arr(Arena *a, const void *src, size_t size) {
    void *p = arena_alloc(a, size ? size : 1, 16);
    if (p && size) memcpy(p, src, size);
    return p;
}

static int dup_str(Arena *a, cc_str *s) {
    if (!s->n) {
        s->p = NULL;
        return 0;
    }
    char *p = arena_dup(a, s->p, s->n);
    if (!p) return -1;
    s->p = p;
    return 0;
}

static int dup_loop(Arena *a, cc_loop *l) {
    if (!l->n) {
        l->pts = NULL;
        return 0;
    }
    double(*p)[2] = arena_alloc(a, l->n * sizeof *p, 8);
    if (!p) return -1;
    for (size_t i = 0; i < l->n; i++) p[i][0] = l->pts[i][0] + 0.0, p[i][1] = l->pts[i][1] + 0.0;
    l->pts = p;
    return 0;
}

static int dup_generator(Arena *a, cc_generator *g) {
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
        g->u.col.width += 0.0, g->u.col.depth += 0.0, g->u.col.height += 0.0;
        return 0;
    case CC_GEN_RECT_BEAM:
        g->u.beam.width += 0.0, g->u.beam.height += 0.0, g->u.beam.length += 0.0;
        return 0;
    case CC_GEN_WALL:
        g->u.wall.length += 0.0, g->u.wall.thickness += 0.0, g->u.wall.height += 0.0;
        return 0;
    case CC_GEN_SLAB:
        g->u.slab.thickness += 0.0;
        return dup_loop(a, &g->u.slab.outline);
    case CC_GEN_EXTRUDED_POLYGON: {
        g->u.poly.height += 0.0;
        if (dup_loop(a, &g->u.poly.outline)) return -1;
        if (!g->u.poly.nvoids) {
            g->u.poly.voids = NULL;
            return 0;
        }
        cc_loop *v = dup_arr(a, g->u.poly.voids, g->u.poly.nvoids * sizeof *v);
        if (!v) return -1;
        g->u.poly.voids = v;
        for (size_t i = 0; i < g->u.poly.nvoids; i++)
            if (dup_loop(a, &v[i])) return -1;
        return 0;
    }
    case CC_GEN_MESH: {
        size_t np = g->u.mesh.npositions, ni = g->u.mesh.nindices;
        if (np) {
            double(*p)[3] = arena_alloc(a, np * sizeof *p, 8);
            if (!p) return -1;
            for (size_t i = 0; i < np; i++)
                for (int k = 0; k < 3; k++) p[i][k] = g->u.mesh.positions[i][k] + 0.0;
            g->u.mesh.positions = p;
        } else g->u.mesh.positions = NULL;
        if (ni) {
            uint32_t *ix = dup_arr(a, g->u.mesh.indices, ni * sizeof *ix);
            if (!ix) return -1;
            g->u.mesh.indices = ix;
        } else g->u.mesh.indices = NULL;
        return 0;
    }
    default: return 0;
    }
}

static void canon_placement(cc_placement *p) {
    for (int k = 0; k < 3; k++) {
        p->origin[k] += 0.0;
        p->z_axis[k] += 0.0;
        p->x_axis[k] += 0.0;
    }
}

static int dup_kvs(Arena *a, cc_kv **kv, size_t n) {
    if (!n) {
        *kv = NULL;
        return 0;
    }
    cc_kv *p = dup_arr(a, *kv, n * sizeof *p);
    if (!p) return -1;
    for (size_t i = 0; i < n; i++)
        if (dup_str(a, &p[i].key) || dup_str(a, &p[i].value)) return -1;
    *kv = p;
    return 0;
}

static int dup_material(Arena *a, cc_material *x) {
    x->props.density_kg_m3 += 0.0;
    x->props.e_mpa += 0.0;
    x->props.strength_mpa += 0.0;
    return dup_str(a, &x->name) || dup_str(a, &x->props.source) ? -1 : 0;
}

static int dup_openings(Arena *a, cc_element *e);
static int dup_element(Arena *a, cc_element *e) {
    canon_placement(&e->placement);
    if (dup_str(a, &e->name) || dup_generator(a, &e->generator) ||
        dup_kvs(a, &e->properties, e->nproperties))
        return -1;
    if (e->has_ifc_entity && dup_str(a, &e->ifc_entity)) return -1;
    return dup_openings(a, e);
}
static int dup_openings(Arena *a, cc_element *e) {
    if (!e->nopenings) {
        e->openings = NULL;
        return 0;
    }
    cc_opening *o = dup_arr(a, e->openings, e->nopenings * sizeof *o);
    if (!o) return -1;
    e->openings = o;
    for (size_t i = 0; i < e->nopenings; i++) {
        canon_placement(&o[i].placement);
        if (dup_generator(a, &o[i].generator)) return -1;
    }
    return 0;
}

/* The command's data copied into arena a and made canonical. */
static int dup_command(Arena *a, cc_command *c) {
    switch (c->op) {
    case CC_CMD_ADD_STOREY:
    case CC_CMD_SET_STOREY: c->storey.elevation += 0.0; return dup_str(a, &c->storey.name);
    case CC_CMD_ADD_GRID:
    case CC_CMD_SET_GRID: return cc_grid_dup(a, &c->grid);
    case CC_CMD_SET_GEOREF: return cc_georef_dup(a, &c->georef);
    case CC_CMD_ADD_MATERIAL:
    case CC_CMD_SET_MATERIAL: return dup_material(a, &c->material);
    case CC_CMD_ADD_ELEMENT: return dup_element(a, &c->element);
    case CC_CMD_SET_GENERATOR: return dup_generator(a, &c->generator);
    case CC_CMD_SET_OPENINGS: return dup_openings(a, &c->element); /* CONC1 */
    case CC_CMD_SET_PLACEMENT: canon_placement(&c->placement); return 0;
    case CC_CMD_RENAME: return dup_str(a, &c->name);
    case CC_CMD_SET_PROPERTY: return dup_str(a, &c->key) || (c->has_value && dup_str(a, &c->value)) ? -1 : 0;
    case CC_CMD_SET_PROJECT: return dup_kvs(a, &c->project, c->nproject);
    case CC_CMD_ADD_VIEW:
    case CC_CMD_SET_VIEW: return cc_view_dup(a, &c->view);
    case CC_CMD_ADD_SHEET:
    case CC_CMD_SET_SHEET: return cc_sheet_dup(a, &c->sheet);
    case CC_CMD_SET_SCHEDULES: return cc_schedules_dup(a, &c->schedules, c->nschedules);
    case CC_CMD_ADD_RELATION:
    case CC_CMD_SET_RELATION: return cc_relation_dup(a, &c->relation);
    default: return 0;
    }
}

/* ---- apply ---- */

static void inv_init(cc_command *inv, cc_cmd_op op) {
    memset(inv, 0, sizeof *inv);
    inv->op = op;
}

/* Sets or removes one property, copy on write. *old gets the previous value (has_old 0: none). */
static int set_prop(Arena *a, cc_element *e, cc_str key, const cc_str *value, cc_str *old, int *has_old) {
    size_t lo = 0, hi = e->nproperties;
    while (lo < hi) { /* lower bound by key bytes */
        size_t mid = lo + (hi - lo) / 2;
        cc_str k = e->properties[mid].key;
        size_t n = k.n < key.n ? k.n : key.n;
        int c = n ? memcmp(k.p, key.p, n) : 0;
        if (c < 0 || (c == 0 && k.n < key.n)) lo = mid + 1;
        else hi = mid;
    }
    int found = lo < e->nproperties && str_eq(e->properties[lo].key, key);
    *has_old = found;
    if (found) *old = e->properties[lo].value;
    if (value) {
        if (found) {
            cc_kv *p = dup_arr(a, e->properties, e->nproperties * sizeof *p);
            if (!p) return -1;
            p[lo].value = *value;
            e->properties = p;
        } else {
            cc_kv item = {key, *value};
            cc_kv *p = arr_insert(a, e->properties, e->nproperties, sizeof item, lo, &item);
            if (!p) return -1;
            e->properties = p;
            e->nproperties++;
        }
    } else if (found) {
        size_t n = e->nproperties - 1;
        cc_kv *p = n ? arena_alloc(a, n * sizeof *p, 16) : NULL;
        if (n && !p) return -1;
        if (lo) memcpy(p, e->properties, lo * sizeof *p);
        if (n > lo) memcpy(p + lo, e->properties + lo + 1, (n - lo) * sizeof *p);
        e->properties = p;
        e->nproperties = n;
    }
    return 0;
}

int cc_command_apply_inverse(cc_model *m, const cc_command *c, cc_command *inv, Buf *err) {
    Arena *a = &m->arena;
    Buf why;
    buf_init(&why);
    switch (c->op) {
    case CC_CMD_ADD_STOREY: {
        const cc_storey *s = &c->storey;
        if (cc_model_storey(m, &s->id) || cc_model_relation(m, &s->id) || cc_grid_uses_id(m, &s->id))
            return exists(err, &s->id);
        if (!finite_(s->elevation)) return invalid(err, "storey elevation", 16);
        size_t at = lower_bound(m->storeys, m->nstoreys, sizeof *s, &s->id);
        cc_storey *p = arr_insert(a, m->storeys, m->nstoreys, sizeof *s, at, s);
        if (!p) return oom(err);
        m->storeys = p;
        m->nstoreys++;
        inv_init(inv, CC_CMD_REMOVE_STOREY);
        inv->id = s->id;
        return 0;
    }
    case CC_CMD_REMOVE_STOREY: {
        for (size_t i = 0; i < m->nviews; i++)
            if (m->views[i].kind == CC_VIEW_PLAN && m->views[i].storey.n == 22 &&
                !memcmp(m->views[i].storey.p, c->id.c, 22))
                return in_use(err, &c->id);
        for (size_t i = 0; i < m->nelements; i++)
            if (!cc_gid_cmp(&m->elements[i].storey, &c->id)) return in_use(err, &c->id);
        const cc_storey *s = cc_model_storey(m, &c->id);
        if (!s) return not_found(err, &c->id);
        inv_init(inv, CC_CMD_ADD_STOREY);
        inv->storey = *s;
        arr_remove(m->storeys, m->nstoreys--, sizeof *s, (size_t)(s - m->storeys));
        return 0;
    }
    case CC_CMD_ADD_MATERIAL: {
        const cc_material *x = &c->material;
        if (cc_model_material(m, &x->id) || cc_model_relation(m, &x->id) || cc_grid_uses_id(m, &x->id))
            return exists(err, &x->id);
        if (props_validate(&why, &x->props)) return invalid_buf(err, &why);
        size_t at = lower_bound(m->materials, m->nmaterials, sizeof *x, &x->id);
        cc_material *p = arr_insert(a, m->materials, m->nmaterials, sizeof *x, at, x);
        if (!p) return oom(err);
        m->materials = p;
        m->nmaterials++;
        inv_init(inv, CC_CMD_REMOVE_MATERIAL);
        inv->id = x->id;
        return 0;
    }
    case CC_CMD_REMOVE_MATERIAL: {
        for (size_t i = 0; i < m->nelements; i++)
            if (!cc_gid_cmp(&m->elements[i].material, &c->id)) return in_use(err, &c->id);
        const cc_material *x = cc_model_material(m, &c->id);
        if (!x) return not_found(err, &c->id);
        inv_init(inv, CC_CMD_ADD_MATERIAL);
        inv->material = *x;
        arr_remove(m->materials, m->nmaterials--, sizeof *x, (size_t)(x - m->materials));
        return 0;
    }
    case CC_CMD_SET_MATERIAL: { /* command.rs:317-341 */
        const cc_material *x = &c->material;
        cc_material *old = (cc_material *)cc_model_material(m, &x->id);
        if (!old) return not_found(err, &x->id);
        if (cc_str_trim(x->name).n == 0 || utf8_count(x->name) > 200)
            return invalid(err, "a material needs a name (at most 200 characters)", 48);
        if (props_validate(&why, &x->props)) return invalid_buf(err, &why);
        /* Materials are shown, scheduled and selected by name: two of one name would be one in
         * every list. */
        cc_str t = cc_str_trim(x->name);
        for (size_t i = 0; i < m->nmaterials; i++) {
            cc_str o = cc_str_trim(m->materials[i].name);
            if (cc_gid_cmp(&m->materials[i].id, &x->id) && o.n == t.n && ascii_ieq_n(o.p, t.p, t.n)) {
                buf_puts(&why, "there is already a material named ");
                cc_buf_debug_str(&why, t.p, t.n);
                return invalid_buf(err, &why);
            }
        }
        buf_free(&why);
        inv_init(inv, CC_CMD_SET_MATERIAL);
        inv->material = *old;
        *old = *x;
        return 0;
    }
    case CC_CMD_ADD_ELEMENT: {
        const cc_element *e = &c->element;
        if (cc_model_element(m, &e->id) || cc_model_relation(m, &e->id) || cc_grid_uses_id(m, &e->id))
            return exists(err, &e->id);
        const cc_storey *est = cc_model_storey(m, &e->storey);
        if (!est) return not_found(err, &e->storey);
        if (est->not_storey) { /* ADR 0056 */
            buf_puts(&why, "the level ");
            put_name(&why, est->name);
            buf_puts(&why, " is not a storey: no element belongs to it");
            return invalid_buf(err, &why);
        }
        if (!cc_model_material(m, &e->material)) return not_found(err, &e->material);
        if (assembly_check(&why, e->cls, &e->generator, e->nopenings) || placement_validate(&why, &e->placement))
            return invalid_buf(err, &why);
        size_t at = lower_bound(m->elements, m->nelements, sizeof *e, &e->id);
        cc_element *p = arr_insert_grow(a, m->elements, m->nelements, &m->cap_elements, sizeof *e, at, e);
        if (!p) return oom(err);
        m->elements = p;
        m->nelements++;
        inv_init(inv, CC_CMD_REMOVE_ELEMENT);
        inv->id = e->id;
        return 0;
    }
    case CC_CMD_REMOVE_ELEMENT: {
        cc_element *e = find_element(m, &c->id);
        if (!e) return not_found(err, &c->id);
        const cc_relation *r = cc_relation_using(m, &c->id); /* X2's verbs decide what delete does */
        if (r) {
            if (err) {
                buf_clear(err);
                cc_buf_gid(err, &c->id);
                buf_puts(err, " is still used by the relation ");
                cc_buf_gid(err, &r->id);
                buf_puts(err, " (");
                buf_append(err, r->kind.p, r->kind.n);
                buf_putc(err, ')');
            }
            return -1;
        }
        inv_init(inv, CC_CMD_ADD_ELEMENT);
        inv->element = *e;
        arr_remove(m->elements, m->nelements--, sizeof *e, (size_t)(e - m->elements));
        return 0;
    }
    case CC_CMD_SET_GENERATOR: {
        if (gen_validate(&why, &c->generator)) return invalid_buf(err, &why);
        cc_element *e = find_element(m, &c->id);
        if (!e) return not_found(err, &c->id);
        if (assembly_check(&why, e->cls, &c->generator, 0)) return invalid_buf(err, &why);
        inv_init(inv, CC_CMD_SET_GENERATOR);
        inv->id = c->id;
        inv->generator = e->generator;
        e->generator = c->generator;
        return 0;
    }
    case CC_CMD_SET_OPENINGS: { /* CONC1 (ADR 0077): the whole list, the old one is the inverse */
        cc_element *e = find_element(m, &c->id);
        if (!e) return not_found(err, &c->id);
        if (assembly_check(&why, e->cls, &e->generator, c->element.nopenings)) return invalid_buf(err, &why);
        if (c->element.nopenings > CC_MAX_OPENINGS) return invalid(err, "too many openings", 17);
        for (size_t i = 0; i < c->element.nopenings; i++) {
            const cc_opening *o = &c->element.openings[i];
            if (placement_validate(&why, &o->placement) || gen_validate(&why, &o->generator)) return invalid_buf(err, &why);
            for (size_t j = 0; o->has_id && j < i; j++)
                if (c->element.openings[j].has_id && !cc_gid_cmp(&c->element.openings[j].id, &o->id))
                    return exists(err, &o->id);
        }
        inv_init(inv, CC_CMD_SET_OPENINGS);
        inv->id = c->id;
        inv->element.openings = e->openings;
        inv->element.nopenings = e->nopenings;
        e->openings = c->element.nopenings ? c->element.openings : NULL;
        e->nopenings = c->element.nopenings;
        return 0;
    }
    case CC_CMD_SET_PLACEMENT: {
        if (placement_validate(&why, &c->placement)) return invalid_buf(err, &why);
        cc_element *e = find_element(m, &c->id);
        if (!e) return not_found(err, &c->id);
        inv_init(inv, CC_CMD_SET_PLACEMENT);
        inv->id = c->id;
        inv->placement = e->placement;
        e->placement = c->placement;
        return 0;
    }
    case CC_CMD_RENAME: {
        cc_element *e = find_element(m, &c->id);
        if (!e) return not_found(err, &c->id);
        inv_init(inv, CC_CMD_RENAME);
        inv->id = c->id;
        inv->name = e->name;
        e->name = c->name;
        return 0;
    }
    case CC_CMD_SET_ELEMENT_MATERIAL: {
        if (!cc_model_material(m, &c->material_id)) return not_found(err, &c->material_id);
        cc_element *e = find_element(m, &c->id);
        if (!e) return not_found(err, &c->id);
        inv_init(inv, CC_CMD_SET_ELEMENT_MATERIAL);
        inv->id = c->id;
        inv->material_id = e->material;
        e->material = c->material_id;
        return 0;
    }
    case CC_CMD_SET_PROPERTY: {
        cc_element *e = find_element(m, &c->id);
        if (!e) return not_found(err, &c->id);
        cc_str old = {0, 0};
        int has_old = 0;
        if (set_prop(a, e, c->key, c->has_value ? &c->value : NULL, &old, &has_old)) return oom(err);
        inv_init(inv, CC_CMD_SET_PROPERTY);
        inv->id = c->id;
        inv->key = c->key;
        inv->value = old;
        inv->has_value = has_old;
        return 0;
    }
    case CC_CMD_SET_PROJECT: {
        if (project_validate(&why, c->project, c->nproject)) return invalid_buf(err, &why);
        buf_free(&why);
        inv_init(inv, CC_CMD_SET_PROJECT);
        inv->project = m->project;
        inv->nproject = m->nproject;
        m->project = c->project;
        m->nproject = c->nproject;
        return 0;
    }
    case CC_CMD_ADD_VIEW: {
        const cc_view *v = &c->view;
        if (cc_model_view(m, &v->id)) return exists(err, &v->id);
        if (cc_view_validate(m, v, &why)) return invalid_buf(err, &why);
        buf_free(&why);
        size_t at = lower_bound(m->views, m->nviews, sizeof *v, &v->id);
        cc_view *p = arr_insert(a, m->views, m->nviews, sizeof *v, at, v);
        if (!p) return oom(err);
        m->views = p;
        m->nviews++;
        inv_init(inv, CC_CMD_REMOVE_VIEW);
        inv->id = v->id;
        return 0;
    }
    case CC_CMD_REMOVE_VIEW: {
        /* A view placed on a sheet stays (a default view is reset, not removed: it keeps its id,
         * so it stays placed). */
        if (!cc_view_is_default(m, &c->id))
            for (size_t i = 0; i < m->nsheets; i++)
                for (size_t k = 0; k < m->sheets[i].nplaced; k++)
                    if (!cc_gid_cmp(&m->sheets[i].placed[k].view, &c->id)) return in_use(err, &c->id);
        const cc_view *v = cc_model_view(m, &c->id);
        if (!v) return not_found(err, &c->id);
        inv_init(inv, CC_CMD_ADD_VIEW);
        inv->view = *v;
        arr_remove(m->views, m->nviews--, sizeof *v, (size_t)(v - m->views));
        return 0;
    }
    case CC_CMD_SET_VIEW: {
        cc_view *v = (cc_view *)cc_model_view(m, &c->view.id);
        if (!v) return not_found(err, &c->view.id);
        if (cc_view_validate(m, &c->view, &why)) return invalid_buf(err, &why);
        buf_free(&why);
        inv_init(inv, CC_CMD_SET_VIEW);
        inv->view = *v;
        *v = c->view;
        return 0;
    }
    case CC_CMD_ADD_SHEET:
    case CC_CMD_SET_SHEET: {
        const cc_sheet *s = &c->sheet;
        cc_sheet *old = (cc_sheet *)cc_model_sheet(m, &s->id);
        if (c->op == CC_CMD_ADD_SHEET && old) return exists(err, &s->id);
        if (c->op == CC_CMD_SET_SHEET && !old) return not_found(err, &s->id);
        /* check_sheet: valid on its own, placing views of the model, its own number */
        if (cc_sheet_validate(m, s, &why)) return invalid_buf(err, &why);
        for (size_t i = 0; i < m->nsheets; i++)
            if (cc_gid_cmp(&m->sheets[i].id, &s->id) && str_eq(m->sheets[i].number, s->number)) {
                buf_puts(&why, "sheet number ");
                buf_append(&why, s->number.p, s->number.n);
                buf_puts(&why, " is already used");
                return invalid_buf(err, &why);
            }
        buf_free(&why);
        if (old) {
            inv_init(inv, CC_CMD_SET_SHEET);
            inv->sheet = *old;
            *old = *s;
            return 0;
        }
        size_t at = lower_bound(m->sheets, m->nsheets, sizeof *s, &s->id);
        cc_sheet *p = arr_insert(a, m->sheets, m->nsheets, sizeof *s, at, s);
        if (!p) return oom(err);
        m->sheets = p;
        m->nsheets++;
        inv_init(inv, CC_CMD_REMOVE_SHEET);
        inv->id = s->id;
        return 0;
    }
    case CC_CMD_REMOVE_SHEET: {
        const cc_sheet *s = cc_model_sheet(m, &c->id);
        if (!s) return not_found(err, &c->id);
        inv_init(inv, CC_CMD_ADD_SHEET);
        inv->sheet = *s;
        arr_remove(m->sheets, m->nsheets--, sizeof *s, (size_t)(s - m->sheets));
        return 0;
    }
    case CC_CMD_ADD_RELATION:
    case CC_CMD_SET_RELATION: {
        const cc_relation *r = &c->relation;
        cc_relation *old = (cc_relation *)cc_model_relation(m, &r->id);
        if (c->op == CC_CMD_ADD_RELATION && old) return exists(err, &r->id);
        if (c->op == CC_CMD_SET_RELATION && !old) return not_found(err, &r->id);
        /* The whole table as it would be, validated as a file load does (one whole per part,
         * no cycle across relations). */
        size_t n = m->nrelations + (old ? 0 : 1);
        size_t at = lower_bound(m->relations, m->nrelations, sizeof *r, &r->id);
        cc_relation *t = malloc(n * sizeof *t);
        if (!t) return oom(err);
        if (m->nrelations) memcpy(t, m->relations, m->nrelations * sizeof *t);
        if (old) t[at] = *r;
        else {
            if (m->nrelations > at) memmove(t + at + 1, t + at, (m->nrelations - at) * sizeof *t);
            t[at] = *r;
        }
        int bad = cc_relations_validate(m, t, n, NULL, &why);
        free(t);
        if (bad && why.oom) return buf_free(&why), oom(err);
        if (bad) return invalid_buf(err, &why);
        buf_free(&why);
        if (old) {
            inv_init(inv, CC_CMD_SET_RELATION);
            inv->relation = *old;
            *old = *r;
            return 0;
        }
        cc_relation *p = arr_insert(a, m->relations, m->nrelations, sizeof *r, at, r);
        if (!p) return oom(err);
        m->relations = p;
        m->nrelations++;
        inv_init(inv, CC_CMD_REMOVE_RELATION);
        inv->id = r->id;
        return 0;
    }
    case CC_CMD_REMOVE_RELATION: {
        const cc_relation *r = cc_model_relation(m, &c->id);
        if (!r) return not_found(err, &c->id);
        inv_init(inv, CC_CMD_ADD_RELATION);
        inv->relation = *r;
        arr_remove(m->relations, m->nrelations--, sizeof *r, (size_t)(r - m->relations));
        return 0;
    }
    case CC_CMD_SET_STOREY: {
        const cc_storey *s = &c->storey;
        cc_storey *old = (cc_storey *)cc_model_storey(m, &s->id);
        if (!old) return not_found(err, &s->id);
        if (!finite_(s->elevation)) return invalid(err, "storey elevation", 16);
        if (s->not_storey && !old->not_storey)
            for (size_t i = 0; i < m->nelements; i++)
                if (!cc_gid_cmp(&m->elements[i].storey, &s->id)) {
                    buf_puts(&why, "the level ");
                    put_name(&why, old->name);
                    buf_puts(&why, " holds elements: it stays a storey");
                    return invalid_buf(err, &why);
                }
        inv_init(inv, CC_CMD_SET_STOREY);
        inv->storey = *old;
        *old = *s; /* placements are relative to their storey: its elements move with it */
        return 0;
    }
    case CC_CMD_ADD_GRID:
    case CC_CMD_SET_GRID: {
        const cc_grid *g = &c->grid;
        cc_grid *old = (cc_grid *)cc_model_grid(m, &g->id);
        if (c->op == CC_CMD_ADD_GRID && old) return exists(err, &g->id);
        if (c->op == CC_CMD_SET_GRID && !old) return not_found(err, &g->id);
        /* The whole table as it would be, validated as a file load does (ids unique across
         * grids and axes). */
        size_t n = m->ngrids + (old ? 0 : 1);
        size_t at = lower_bound(m->grids, m->ngrids, sizeof *g, &g->id);
        cc_grid *t = malloc(n * sizeof *t);
        if (!t) return oom(err);
        if (m->ngrids) memcpy(t, m->grids, m->ngrids * sizeof *t);
        if (!old && m->ngrids > at) memmove(t + at + 1, t + at, (m->ngrids - at) * sizeof *t);
        t[at] = *g;
        int bad = cc_grids_validate(m, t, n, NULL, &why);
        free(t);
        if (bad && why.oom) return buf_free(&why), oom(err);
        if (bad) return invalid_buf(err, &why);
        buf_free(&why);
        if (old) {
            inv_init(inv, CC_CMD_SET_GRID);
            inv->grid = *old;
            *old = *g;
            return 0;
        }
        cc_grid *p = arr_insert(a, m->grids, m->ngrids, sizeof *g, at, g);
        if (!p) return oom(err);
        m->grids = p;
        m->ngrids++;
        inv_init(inv, CC_CMD_REMOVE_GRID);
        inv->id = g->id;
        return 0;
    }
    case CC_CMD_REMOVE_GRID: {
        const cc_grid *g = cc_model_grid(m, &c->id);
        if (!g) return not_found(err, &c->id);
        inv_init(inv, CC_CMD_ADD_GRID);
        inv->grid = *g;
        arr_remove(m->grids, m->ngrids--, sizeof *g, (size_t)(g - m->grids));
        return 0;
    }
    case CC_CMD_SET_GEOREF: { /* ADR 0045: the whole georeference, the old one is the inverse */
        if (cc_georef_validate(&c->georef, &why)) return invalid_buf(err, &why);
        buf_free(&why);
        inv_init(inv, CC_CMD_SET_GEOREF);
        inv->georef = m->georef;
        m->georef = c->georef;
        return 0;
    }
    case CC_CMD_SET_SCHEDULES: {
        if (cc_schedules_validate(c->schedules, c->nschedules, &why)) return invalid_buf(err, &why);
        buf_free(&why);
        inv_init(inv, CC_CMD_SET_SCHEDULES);
        inv->schedules = m->schedules;
        inv->nschedules = m->nschedules;
        m->schedules = c->schedules;
        m->nschedules = c->nschedules;
        return 0;
    }
    default: return invalid(err, "unknown command", 15);
    }
}

int cc_command_apply(cc_model *m, const cc_command *c, cc_command *inverse, Buf *err) {
    if (c->op == CC_CMD_RENAME && cc_str_trim(c->name).n == 0) return invalid(err, "the name is empty", 17);
    if (c->op == CC_CMD_SET_PROPERTY) {
        if (property_text(err, "the property name", c->key, 128)) return -1;
        if (c->has_value && property_text(err, "the value", c->value, 2000)) return -1;
    }
    cc_command copy = *c;
    if (dup_command(&m->arena, &copy)) return oom(err);
    return cc_command_apply_inverse(m, &copy, inverse, err);
}

/* ---- describe ---- */

static void put_str(Buf *b, cc_str s) { buf_append(b, s.p, s.n); }

static void el_name(Buf *b, const cc_model *m, const cc_gid *id) {
    const cc_element *e = cc_model_element(m, id);
    if (e) put_str(b, e->name);
    else buf_puts(b, "an element");
}

void cc_command_describe(const cc_command *c, const cc_model *m, Buf *b) {
    switch (c->op) {
    case CC_CMD_ADD_STOREY: buf_puts(b, "add the storey "), put_str(b, c->storey.name); break;
    case CC_CMD_REMOVE_STOREY: {
        const cc_storey *s = cc_model_storey(m, &c->id);
        buf_puts(b, "delete the storey ");
        if (s) put_str(b, s->name);
        break;
    }
    case CC_CMD_ADD_MATERIAL: buf_puts(b, "add the material "), put_str(b, c->material.name); break;
    case CC_CMD_REMOVE_MATERIAL: {
        const cc_material *x = cc_model_material(m, &c->id);
        buf_puts(b, "delete the material ");
        if (x) put_str(b, x->name);
        break;
    }
    case CC_CMD_SET_MATERIAL: { /* command.rs:454 */
        const cc_material *old = cc_model_material(m, &c->material.id);
        if (old && (old->name.n != c->material.name.n || memcmp(old->name.p, c->material.name.p, old->name.n))) {
            buf_puts(b, "rename the material "), put_str(b, old->name);
            buf_puts(b, " to "), put_str(b, c->material.name);
        } else {
            buf_puts(b, "change the material "), put_str(b, c->material.name);
        }
        break;
    }
    case CC_CMD_ADD_ELEMENT: buf_puts(b, "add "), put_str(b, c->element.name); break;
    case CC_CMD_REMOVE_ELEMENT: buf_puts(b, "delete "), el_name(b, m, &c->id); break;
    case CC_CMD_SET_GENERATOR: buf_puts(b, "change the sizes of "), el_name(b, m, &c->id); break;
    case CC_CMD_SET_OPENINGS: buf_puts(b, "change the openings of "), el_name(b, m, &c->id); break; /* CONC1 */
    case CC_CMD_SET_PLACEMENT: buf_puts(b, "move "), el_name(b, m, &c->id); break;
    case CC_CMD_RENAME:
        buf_puts(b, "rename "), el_name(b, m, &c->id);
        buf_puts(b, " to "), put_str(b, c->name);
        break;
    case CC_CMD_SET_ELEMENT_MATERIAL: {
        const cc_material *x = cc_model_material(m, &c->material_id);
        buf_puts(b, "set the material of "), el_name(b, m, &c->id);
        buf_puts(b, " to ");
        if (x) put_str(b, x->name);
        break;
    }
    case CC_CMD_SET_PROPERTY:
        if (c->has_value) {
            buf_puts(b, "set "), put_str(b, c->key);
            buf_puts(b, " of "), el_name(b, m, &c->id);
        } else {
            buf_puts(b, "remove "), put_str(b, c->key);
            buf_puts(b, " from "), el_name(b, m, &c->id);
        }
        break;
    case CC_CMD_SET_PROJECT: buf_puts(b, "change the project information"); break;
    case CC_CMD_ADD_VIEW: buf_puts(b, "add the view "), put_str(b, c->view.name); break;
    case CC_CMD_REMOVE_VIEW: {
        const cc_view *v = cc_model_view(m, &c->id);
        buf_puts(b, "delete ");
        if (v) buf_puts(b, "the view "), put_str(b, v->name);
        else buf_puts(b, "a view");
        break;
    }
    case CC_CMD_SET_VIEW: {
        const cc_view *old = cc_model_view(m, &c->view.id);
        if (old && !str_eq(old->name, c->view.name)) {
            buf_puts(b, "rename the view "), put_str(b, old->name);
            buf_puts(b, " to "), put_str(b, c->view.name);
        } else buf_puts(b, "change the view "), put_str(b, c->view.name);
        break;
    }
    case CC_CMD_ADD_SHEET: buf_puts(b, "add the sheet "), put_str(b, c->sheet.number); break;
    case CC_CMD_REMOVE_SHEET: {
        const cc_sheet *s = cc_model_sheet(m, &c->id);
        buf_puts(b, "delete ");
        if (s) buf_puts(b, "the sheet "), put_str(b, s->number);
        else buf_puts(b, "a sheet");
        break;
    }
    case CC_CMD_SET_SHEET: buf_puts(b, "change the sheet "), put_str(b, c->sheet.number); break;
    case CC_CMD_SET_SCHEDULES: {
        const cc_str *added = NULL, *removed = NULL;
        for (size_t i = 0; !added && i < c->nschedules; i++) {
            int in_old = 0;
            for (size_t k = 0; k < m->nschedules; k++) in_old |= str_eq(c->schedules[i].name, m->schedules[k].name);
            if (!in_old) added = &c->schedules[i].name;
        }
        for (size_t k = 0; !removed && k < m->nschedules; k++) {
            int in_new = 0;
            for (size_t i = 0; i < c->nschedules; i++) in_new |= str_eq(c->schedules[i].name, m->schedules[k].name);
            if (!in_new) removed = &m->schedules[k].name;
        }
        if (added && !removed) buf_puts(b, "add the schedule "), put_str(b, *added);
        else if (!added && removed) buf_puts(b, "delete the schedule "), put_str(b, *removed);
        else if (added) {
            buf_puts(b, "rename the schedule "), put_str(b, *removed);
            buf_puts(b, " to "), put_str(b, *added);
        } else {
            const cc_schedule *changed = NULL;
            for (size_t i = 0; !changed && i < c->nschedules; i++) {
                int same = 0;
                for (size_t k = 0; k < m->nschedules; k++) same |= cc_schedule_eq(&c->schedules[i], &m->schedules[k]);
                if (!same) changed = &c->schedules[i];
            }
            if (changed) buf_puts(b, "change the schedule "), put_str(b, changed->name);
            else buf_puts(b, "change the schedules");
        }
        break;
    }
    case CC_CMD_ADD_RELATION:
        buf_puts(b, "add the "), put_str(b, c->relation.kind);
        buf_puts(b, " of "), el_name(b, m, &c->relation.relating);
        break;
    case CC_CMD_REMOVE_RELATION: {
        const cc_relation *r = cc_model_relation(m, &c->id);
        buf_puts(b, "delete ");
        if (r) buf_puts(b, "the "), put_str(b, r->kind), buf_puts(b, " of "), el_name(b, m, &r->relating);
        else buf_puts(b, "a relation");
        break;
    }
    case CC_CMD_SET_RELATION:
        buf_puts(b, "change the "), put_str(b, c->relation.kind);
        buf_puts(b, " of "), el_name(b, m, &c->relation.relating);
        break;
    case CC_CMD_SET_STOREY: {
        const cc_storey *old = cc_model_storey(m, &c->storey.id);
        if (old && !str_eq(old->name, c->storey.name)) {
            buf_puts(b, "rename the level "), put_str(b, old->name);
            buf_puts(b, " to "), put_str(b, c->storey.name);
        } else buf_puts(b, "change the level "), put_str(b, c->storey.name);
        break;
    }
    case CC_CMD_ADD_GRID: buf_puts(b, "add the grid "), put_str(b, c->grid.name); break;
    case CC_CMD_SET_GRID: buf_puts(b, "change the grid "), put_str(b, c->grid.name); break;
    case CC_CMD_SET_GEOREF:
        buf_puts(b, cc_georef_is_default(&c->georef) ? "clear the georeference" : "set the georeference");
        break;
    case CC_CMD_REMOVE_GRID: {
        const cc_grid *g = cc_model_grid(m, &c->id);
        buf_puts(b, "delete ");
        if (g) buf_puts(b, "the grid "), put_str(b, g->name);
        else buf_puts(b, "a grid");
        break;
    }
    default: break;
    }
}

void cc_command_describe_group(const cc_command *c, size_t n, const cc_model *m, Buf *b) {
    if (n == 0) {
        buf_puts(b, "nothing");
        return;
    }
    cc_command_describe(&c[0], m, b);
    if (n > 1) {
        buf_puts(b, " and ");
        buf_u64(b, n - 1);
        buf_puts(b, n == 2 ? " more change" : " more changes");
    }
}

/* ---- JSON out ---- */

static void w_str(JsonW *w, cc_str s) { jw_str_n(w, s.n ? s.p : "", s.n); }
static void w_gid(JsonW *w, const cc_gid *g) { jw_str_n(w, g->c, 22); }

static void w_vec(JsonW *w, const double *v, int n) {
    jw_arr_begin(w);
    for (int i = 0; i < n; i++) jw_f64(w, v[i]);
    jw_arr_end(w);
}

static void w_loop(JsonW *w, const cc_loop *l) {
    jw_arr_begin(w);
    for (size_t i = 0; i < l->n; i++) w_vec(w, l->pts[i], 2);
    jw_arr_end(w);
}

static void w_kvs(JsonW *w, const cc_kv *kv, size_t n) {
    jw_obj_begin(w);
    for (size_t i = 0; i < n; i++) {
        jw_key_n(w, kv[i].key.n ? kv[i].key.p : "", kv[i].key.n);
        w_str(w, kv[i].value);
    }
    jw_obj_end(w);
}

static void w_generator(JsonW *w, const cc_generator *g) {
    jw_obj_begin(w);
    jw_key(w, "kind");
    jw_str(w, cc_gen_kind_name(g->kind));
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
        jw_key(w, "width"), jw_f64(w, g->u.col.width);
        jw_key(w, "depth"), jw_f64(w, g->u.col.depth);
        jw_key(w, "height"), jw_f64(w, g->u.col.height);
        break;
    case CC_GEN_RECT_BEAM:
        jw_key(w, "width"), jw_f64(w, g->u.beam.width);
        jw_key(w, "height"), jw_f64(w, g->u.beam.height);
        jw_key(w, "length"), jw_f64(w, g->u.beam.length);
        break;
    case CC_GEN_WALL:
        jw_key(w, "length"), jw_f64(w, g->u.wall.length);
        jw_key(w, "thickness"), jw_f64(w, g->u.wall.thickness);
        jw_key(w, "height"), jw_f64(w, g->u.wall.height);
        break;
    case CC_GEN_SLAB:
        jw_key(w, "outline"), w_loop(w, &g->u.slab.outline);
        jw_key(w, "thickness"), jw_f64(w, g->u.slab.thickness);
        break;
    case CC_GEN_EXTRUDED_POLYGON:
        jw_key(w, "outline"), w_loop(w, &g->u.poly.outline);
        if (g->u.poly.nvoids) {
            jw_key(w, "voids");
            jw_arr_begin(w);
            for (size_t i = 0; i < g->u.poly.nvoids; i++) w_loop(w, &g->u.poly.voids[i]);
            jw_arr_end(w);
        }
        jw_key(w, "height"), jw_f64(w, g->u.poly.height);
        break;
    case CC_GEN_MESH:
        jw_key(w, "positions");
        jw_arr_begin(w);
        for (size_t i = 0; i < g->u.mesh.npositions; i++) w_vec(w, g->u.mesh.positions[i], 3);
        jw_arr_end(w);
        jw_key(w, "indices");
        jw_arr_begin(w);
        for (size_t i = 0; i < g->u.mesh.nindices; i++) jw_u64(w, g->u.mesh.indices[i]);
        jw_arr_end(w);
        break;
    default: break;
    }
    jw_obj_end(w);
}

static void w_placement(JsonW *w, const cc_placement *p) {
    jw_obj_begin(w);
    jw_key(w, "origin"), w_vec(w, p->origin, 3);
    jw_key(w, "z_axis"), w_vec(w, p->z_axis, 3);
    jw_key(w, "x_axis"), w_vec(w, p->x_axis, 3);
    jw_obj_end(w);
}

/* CONC1: model.rs Opening as file.c writes it: {"id"?, "placement", "generator"}. */
static void w_openings(JsonW *w, const cc_element *e) {
    jw_arr_begin(w);
    for (size_t i = 0; i < e->nopenings; i++) {
        const cc_opening *o = &e->openings[i];
        jw_obj_begin(w);
        if (o->has_id) jw_key(w, "id"), w_gid(w, &o->id);
        jw_key(w, "placement"), w_placement(w, &o->placement);
        jw_key(w, "generator"), w_generator(w, &o->generator);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

void cc_command_write(JsonW *w, const cc_command *c) {
    cc_em em = {w, NULL};
    if (c->op == CC_CMD_ADD_VIEW || c->op == CC_CMD_SET_VIEW) {
        cc_em_view(&em, &c->view, cc_cmd_op_name(c->op));
        return;
    }
    if (c->op == CC_CMD_ADD_SHEET || c->op == CC_CMD_SET_SHEET) {
        cc_em_sheet(&em, &c->sheet, cc_cmd_op_name(c->op));
        return;
    }
    if (c->op == CC_CMD_ADD_RELATION || c->op == CC_CMD_SET_RELATION) {
        cc_em_relation(&em, &c->relation, cc_cmd_op_name(c->op));
        return;
    }
    if (c->op == CC_CMD_ADD_GRID || c->op == CC_CMD_SET_GRID) {
        cc_em_grid(&em, &c->grid, cc_cmd_op_name(c->op));
        return;
    }
    if (c->op == CC_CMD_SET_GEOREF) {
        jw_obj_begin(w);
        jw_key(w, "op"), jw_str(w, cc_cmd_op_name(c->op));
        jw_key(w, "georef"), cc_em_georef(&em, &c->georef);
        jw_obj_end(w);
        return;
    }
    jw_obj_begin(w);
    jw_key(w, "op"), jw_str(w, cc_cmd_op_name(c->op));
    switch (c->op) {
    case CC_CMD_ADD_STOREY:
    case CC_CMD_SET_STOREY:
        jw_key(w, "id"), w_gid(w, &c->storey.id);
        jw_key(w, "name"), w_str(w, c->storey.name);
        jw_key(w, "elevation"), jw_f64(w, c->storey.elevation);
        if (c->storey.not_storey) jw_key(w, "is_storey"), jw_bool(w, 0);
        break;
    case CC_CMD_ADD_MATERIAL:
    case CC_CMD_SET_MATERIAL: {
        const cc_material *x = &c->material;
        const cc_material_props *p = &x->props;
        jw_key(w, "id"), w_gid(w, &x->id);
        jw_key(w, "name"), w_str(w, x->name);
        jw_key(w, "category"), jw_str(w, cc_category_name(x->category));
        if (p->has_density || p->has_e || p->has_strength || p->source.n) {
            jw_key(w, "props");
            jw_obj_begin(w);
            if (p->has_density) jw_key(w, "density_kg_m3"), jw_f64(w, p->density_kg_m3);
            if (p->has_e) jw_key(w, "e_mpa"), jw_f64(w, p->e_mpa);
            if (p->has_strength) jw_key(w, "strength_mpa"), jw_f64(w, p->strength_mpa);
            if (p->source.n) jw_key(w, "source"), w_str(w, p->source);
            jw_obj_end(w);
        }
        break;
    }
    case CC_CMD_ADD_ELEMENT: {
        const cc_element *e = &c->element;
        jw_key(w, "id"), w_gid(w, &e->id);
        jw_key(w, "class"), jw_str(w, cc_ifc_class_name(e->cls));
        jw_key(w, "name"), w_str(w, e->name);
        jw_key(w, "storey"), w_gid(w, &e->storey);
        jw_key(w, "material"), w_gid(w, &e->material);
        jw_key(w, "placement"), w_placement(w, &e->placement);
        if (e->generator.kind != CC_GEN_NONE) jw_key(w, "generator"), w_generator(w, &e->generator);
        if (e->nopenings) jw_key(w, "openings"), w_openings(w, e); /* CONC1: kept in the journal */
        if (e->nproperties) jw_key(w, "properties"), w_kvs(w, e->properties, e->nproperties);
        if (e->has_ifc_entity) jw_key(w, "ifc_entity"), w_str(w, e->ifc_entity);
        break;
    }
    case CC_CMD_REMOVE_STOREY:
    case CC_CMD_REMOVE_MATERIAL:
    case CC_CMD_REMOVE_ELEMENT:
    case CC_CMD_REMOVE_VIEW:
    case CC_CMD_REMOVE_SHEET:
    case CC_CMD_REMOVE_RELATION:
    case CC_CMD_REMOVE_GRID: jw_key(w, "id"), w_gid(w, &c->id); break;
    case CC_CMD_SET_SCHEDULES: jw_key(w, "schedules"), cc_em_schedules(&em, c->schedules, c->nschedules); break;
    case CC_CMD_SET_GENERATOR:
        jw_key(w, "id"), w_gid(w, &c->id);
        jw_key(w, "generator"), w_generator(w, &c->generator);
        break;
    case CC_CMD_SET_PLACEMENT:
        jw_key(w, "id"), w_gid(w, &c->id);
        jw_key(w, "placement"), w_placement(w, &c->placement);
        break;
    case CC_CMD_SET_OPENINGS: /* CONC1 */
        jw_key(w, "id"), w_gid(w, &c->id);
        jw_key(w, "openings"), w_openings(w, &c->element);
        break;
    case CC_CMD_RENAME:
        jw_key(w, "id"), w_gid(w, &c->id);
        jw_key(w, "name"), w_str(w, c->name);
        break;
    case CC_CMD_SET_ELEMENT_MATERIAL:
        jw_key(w, "id"), w_gid(w, &c->id);
        jw_key(w, "material"), w_gid(w, &c->material_id);
        break;
    case CC_CMD_SET_PROPERTY:
        jw_key(w, "id"), w_gid(w, &c->id);
        jw_key(w, "key"), w_str(w, c->key);
        if (c->has_value) jw_key(w, "value"), w_str(w, c->value);
        break;
    case CC_CMD_SET_PROJECT: jw_key(w, "project"), w_kvs(w, c->project, c->nproject); break;
    default: break;
    }
    jw_obj_end(w);
}

void cc_commands_json(const cc_command *c, size_t n, Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    jw_arr_begin(&w);
    for (size_t i = 0; i < n; i++) cc_command_write(&w, &c[i]);
    jw_arr_end(&w);
    jw_finish(&w);
}

/* ---- JSON in: the decoders of file.c (static there), copied unchanged ---- */

typedef struct {
    Arena *a; /* the model's arena */
    cc_error *err;
    int failed;
    Buf msg;
} D;

static int f_finite(double x) { return x - x == 0.0; }

static int fail_buf(D *d, size_t off) {
    if (!d->failed) {
        d->failed = 1;
        const char *pre = "parse: ";
        size_t pn = strlen(pre), n = d->msg.oom ? 0 : d->msg.len;
        if (off == CC_NO_OFFSET) pn = 0;
        size_t cap = sizeof d->err->msg - 1 - pn;
        if (n > cap) {
            n = cap;
            while (n && ((unsigned char)d->msg.p[n] & 0xC0) == 0x80) n--;
        }
        memcpy(d->err->msg, pre, pn);
        if (n) memcpy(d->err->msg + pn, d->msg.p, n);
        d->err->msg[pn + n] = 0;
        d->err->offset = off;
    }
    return -1;
}

static int dfail(D *d, size_t off, const char *a, const char *b, const char *c) {
    if (d->failed) return -1;
    buf_clear(&d->msg);
    if (a) buf_puts(&d->msg, a);
    if (b) buf_puts(&d->msg, b);
    if (c) buf_puts(&d->msg, c);
    return fail_buf(d, off);
}

/* serde's "invalid type: <unexpected>, expected <what>". */
static int type_err(D *d, const JrVal *v, const char *expected) {
    if (d->failed) return -1;
    buf_clear(&d->msg);
    buf_puts(&d->msg, "invalid type: ");
    switch (v->type) {
    case JR_NULL: buf_puts(&d->msg, "null"); break;
    case JR_BOOL: buf_puts(&d->msg, v->b ? "boolean `true`" : "boolean `false`"); break;
    case JR_NUM:
        buf_puts(&d->msg, v->num_int ? "integer `" : "floating point `");
        buf_append(&d->msg, v->s, v->n < 64 ? v->n : 64);
        buf_putc(&d->msg, '`');
        break;
    case JR_STR:
        buf_puts(&d->msg, "string ");
        cc_buf_debug_str(&d->msg, v->s, v->n < 200 ? v->n : 200);
        break;
    case JR_ARR: buf_puts(&d->msg, "sequence"); break;
    default: buf_puts(&d->msg, "map"); break;
    }
    buf_puts(&d->msg, ", expected ");
    buf_puts(&d->msg, expected);
    return fail_buf(d, v->off);
}

static int d_f64(D *d, const JrVal *v, double *out) {
    if (v->type != JR_NUM) return type_err(d, v, "f64");
    if (!f_finite(v->num)) return dfail(d, v->off, "number out of range", 0, 0);
    *out = v->num;
    return 0;
}

static int d_u32(D *d, const JrVal *v, uint32_t *out) {
    if (v->type != JR_NUM || !v->num_int) return type_err(d, v, "u32");
    if (v->num_neg || v->num > 4294967295.0) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "invalid value: integer `");
        buf_append(&d->msg, v->s, v->n < 64 ? v->n : 64);
        buf_puts(&d->msg, "`, expected u32");
        return fail_buf(d, v->off);
    }
    *out = (uint32_t)v->num;
    return 0;
}

static int d_str(D *d, const JrVal *v, cc_str *out) {
    if (v->type != JR_STR) return type_err(d, v, "a string");
    char *p = v->n ? arena_dup(d->a, v->s, v->n) : NULL;
    if (v->n && !p) return dfail(d, v->off, "out of memory", 0, 0);
    out->p = p;
    out->n = v->n;
    return 0;
}

static int gid_from(D *d, const char *s, size_t n, size_t off, cc_gid *out) {
    if (!cc_gid_valid(s, n)) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "invalid IFC GlobalId: ");
        cc_buf_debug_str(&d->msg, s, n < 200 ? n : 200);
        return fail_buf(d, off);
    }
    memcpy(out->c, s, 22);
    return 0;
}

static int d_gid(D *d, const JrVal *v, cc_gid *out) {
    if (v->type != JR_STR) return type_err(d, v, "a string");
    return gid_from(d, v->s, v->n, v->off, out);
}

/* Struct fields by name: out[i] = the member's value or NULL. Duplicates are errors; unknown
 * members are ignored, or refused when `deny_unknown`. */
static int d_fields(D *d, const JrVal *obj, const char *const *names, int nn, const JrVal **out,
                    const char *what, int deny_unknown) {
    if (obj->type != JR_OBJ) return type_err(d, obj, what);
    for (int i = 0; i < nn; i++) out[i] = NULL;
    for (size_t j = 0; j < obj->n; j++) {
        const JrMember *m = &obj->members[j];
        int i = 0;
        while (i < nn && !jr_key_is(m, names[i])) i++;
        if (i == nn) {
            if (!deny_unknown) continue;
            buf_clear(&d->msg);
            buf_puts(&d->msg, "unknown field `");
            buf_append(&d->msg, m->key, m->key_n < 200 ? m->key_n : 200);
            buf_puts(&d->msg, "`, expected one of ");
            for (int k = 0; k < nn; k++) {
                if (k) buf_puts(&d->msg, ", ");
                buf_putc(&d->msg, '`');
                buf_puts(&d->msg, names[k]);
                buf_putc(&d->msg, '`');
            }
            return fail_buf(d, m->val.off);
        }
        if (out[i]) return dfail(d, m->val.off, "duplicate field `", names[i], "`");
        out[i] = &m->val;
    }
    return 0;
}

static int need(D *d, const JrVal *obj, const JrVal *f, const char *name) {
    if (f) return 0;
    return dfail(d, obj->off, "missing field `", name, "`");
}

static int d_vec(D *d, const JrVal *v, double *out, size_t n) {
    if (v->type != JR_ARR) return type_err(d, v, n == 2 ? "an array of length 2" : "an array of length 3");
    if (v->n != n) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "invalid length ");
        buf_u64(&d->msg, v->n);
        buf_puts(&d->msg, n == 2 ? ", expected an array of length 2" : ", expected an array of length 3");
        return fail_buf(d, v->off);
    }
    for (size_t i = 0; i < n; i++)
        if (d_f64(d, &v->items[i], &out[i])) return -1;
    return 0;
}

static void *alloc_n(D *d, size_t n, size_t sz, size_t off) {
    if (n == 0) return NULL;
    if (n > ((size_t)-1) / sz) { dfail(d, off, "out of memory", 0, 0); return NULL; }
    void *p = arena_alloc(d->a, n * sz, 8);
    if (!p) dfail(d, off, "out of memory", 0, 0);
    return p;
}

static int d_loop(D *d, const JrVal *v, cc_loop *out) {
    if (v->type != JR_ARR) return type_err(d, v, "a sequence");
    out->n = v->n;
    out->pts = alloc_n(d, v->n, sizeof *out->pts, v->off);
    if (v->n && !out->pts) return -1;
    for (size_t i = 0; i < v->n; i++)
        if (d_vec(d, &v->items[i], out->pts[i], 2)) return -1;
    return 0;
}

static int d_placement(D *d, const JrVal *v, cc_placement *p) {
    static const char *const F[] = {"origin", "z_axis", "x_axis"};
    const JrVal *f[3];
    if (d_fields(d, v, F, 3, f, "struct Placement", 0)) return -1;
    for (int i = 0; i < 3; i++)
        if (need(d, v, f[i], F[i])) return -1;
    if (d_vec(d, f[0], p->origin, 3) || d_vec(d, f[1], p->z_axis, 3) || d_vec(d, f[2], p->x_axis, 3))
        return -1;
    return 0;
}

/* Three f64 fields in order (the rectangle generators and the wall). */
static int d_three(D *d, const JrVal *v, const char *a, const char *b, const char *c, double *out) {
    const char *names[3] = {a, b, c};
    const JrVal *f[3];
    if (d_fields(d, v, names, 3, f, "struct variant", 0)) return -1;
    for (int i = 0; i < 3; i++)
        if (need(d, v, f[i], names[i]) || d_f64(d, f[i], &out[i])) return -1;
    return 0;
}

static int d_generator(D *d, const JrVal *v, cc_generator *g) {
    if (v->type != JR_OBJ) return type_err(d, v, "internally tagged enum Generator");
    const JrVal *kind = NULL;
    for (size_t j = 0; j < v->n; j++)
        if (jr_key_is(&v->members[j], "kind")) {
            if (kind) return dfail(d, v->members[j].val.off, "duplicate field `kind`", 0, 0);
            kind = &v->members[j].val;
        }
    if (!kind) return dfail(d, v->off, "missing field `kind`", 0, 0);
    if (kind->type != JR_STR) return type_err(d, kind, "variant identifier");
    int k = 0;
    while (k < CC_GEN_COUNT && !(strlen(cc_gen_kind_name((cc_gen_kind)k)) == kind->n &&
                                 memcmp(cc_gen_kind_name((cc_gen_kind)k), kind->s, kind->n) == 0))
        k++;
    if (k == CC_GEN_COUNT) {
        buf_clear(&d->msg);
        buf_puts(&d->msg, "unknown variant ");
        buf_putc(&d->msg, '`');
        buf_append(&d->msg, kind->s, kind->n < 200 ? kind->n : 200);
        buf_puts(&d->msg, "`, expected one of `rect_column`, `rect_beam`, `slab`, `wall`, "
                          "`pad_footing`, `extruded_polygon`, `mesh`");
        return fail_buf(d, kind->off);
    }
    g->kind = (cc_gen_kind)k;
    double t[3];
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
        if (d_three(d, v, "width", "depth", "height", t)) return -1;
        g->u.col.width = t[0], g->u.col.depth = t[1], g->u.col.height = t[2];
        return 0;
    case CC_GEN_RECT_BEAM:
        if (d_three(d, v, "width", "height", "length", t)) return -1;
        g->u.beam.width = t[0], g->u.beam.height = t[1], g->u.beam.length = t[2];
        return 0;
    case CC_GEN_WALL:
        if (d_three(d, v, "length", "thickness", "height", t)) return -1;
        g->u.wall.length = t[0], g->u.wall.thickness = t[1], g->u.wall.height = t[2];
        return 0;
    case CC_GEN_SLAB: {
        static const char *const F[] = {"outline", "thickness"};
        const JrVal *f[2];
        if (d_fields(d, v, F, 2, f, "struct variant Generator::Slab", 0)) return -1;
        if (need(d, v, f[0], F[0]) || need(d, v, f[1], F[1])) return -1;
        if (d_loop(d, f[0], &g->u.slab.outline) || d_f64(d, f[1], &g->u.slab.thickness)) return -1;
        return 0;
    }
    case CC_GEN_EXTRUDED_POLYGON: {
        static const char *const F[] = {"outline", "voids", "height"};
        const JrVal *f[3];
        if (d_fields(d, v, F, 3, f, "struct variant Generator::ExtrudedPolygon", 0)) return -1;
        if (need(d, v, f[0], F[0]) || need(d, v, f[2], F[2])) return -1;
        if (d_loop(d, f[0], &g->u.poly.outline)) return -1;
        g->u.poly.voids = NULL;
        g->u.poly.nvoids = 0;
        if (f[1]) {
            if (f[1]->type != JR_ARR) return type_err(d, f[1], "a sequence");
            g->u.poly.nvoids = f[1]->n;
            g->u.poly.voids = alloc_n(d, f[1]->n, sizeof(cc_loop), f[1]->off);
            if (f[1]->n && !g->u.poly.voids) return -1;
            for (size_t i = 0; i < f[1]->n; i++)
                if (d_loop(d, &f[1]->items[i], &g->u.poly.voids[i])) return -1;
        }
        return d_f64(d, f[2], &g->u.poly.height);
    }
    case CC_GEN_MESH: {
        static const char *const F[] = {"positions", "indices"};
        const JrVal *f[2];
        if (d_fields(d, v, F, 2, f, "struct variant Generator::Mesh", 0)) return -1;
        if (need(d, v, f[0], F[0]) || need(d, v, f[1], F[1])) return -1;
        for (int i = 0; i < 2; i++)
            if (f[i]->type == JR_NUM || f[i]->type == JR_STR)
                return dfail(d, f[i]->off, "unsupported by the C core: mesh arrays by index or hash", 0, 0);
            else if (f[i]->type != JR_ARR)
                return type_err(d, f[i], "a mesh array, its index in the file or its hash");
        g->u.mesh.npositions = f[0]->n;
        g->u.mesh.positions = alloc_n(d, f[0]->n, sizeof *g->u.mesh.positions, f[0]->off);
        if (f[0]->n && !g->u.mesh.positions) return -1;
        for (size_t i = 0; i < f[0]->n; i++)
            if (d_vec(d, &f[0]->items[i], g->u.mesh.positions[i], 3)) return -1;
        g->u.mesh.nindices = f[1]->n;
        g->u.mesh.indices = alloc_n(d, f[1]->n, sizeof(uint32_t), f[1]->off);
        if (f[1]->n && !g->u.mesh.indices) return -1;
        for (size_t i = 0; i < f[1]->n; i++)
            if (d_u32(d, &f[1]->items[i], &g->u.mesh.indices[i])) return -1;
        return 0;
    }
    default: return -1;
    }
}

static int d_enum(D *d, const JrVal *v, const char *(*name)(int), int count, const char *expected,
                  int *out) {
    if (v->type != JR_STR) return type_err(d, v, "variant identifier");
    for (int k = 0; k < count; k++) {
        const char *s = name(k);
        if (strlen(s) == v->n && memcmp(s, v->s, v->n) == 0) {
            *out = k;
            return 0;
        }
    }
    buf_clear(&d->msg);
    buf_puts(&d->msg, "unknown variant `");
    buf_append(&d->msg, v->s, v->n < 200 ? v->n : 200);
    buf_puts(&d->msg, "`, expected one of ");
    buf_puts(&d->msg, expected);
    return fail_buf(d, v->off);
}

static const char *class_name_i(int k) { return cc_ifc_class_name((cc_ifc_class)k); }
static const char *cat_name_i(int k) { return cc_category_name((cc_category)k); }

/* ---- sorted maps ---- */

typedef struct {
    const char *key;
    size_t key_n;
    size_t idx; /* member index in the object */
} Ent;

static int ent_cmp(const Ent *a, const Ent *b) {
    size_t n = a->key_n < b->key_n ? a->key_n : b->key_n;
    int c = memcmp(a->key, b->key, n);
    if (c) return c;
    return a->key_n < b->key_n ? -1 : a->key_n > b->key_n;
}

/* Stable merge sort, then keep the last of equal keys (BTreeMap insert). Returns the count. */
static size_t sort_dedupe(Ent *e, size_t n, Ent *tmp) {
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = ent_cmp(&e[j], &e[i]) < 0 ? e[j++] : e[i++];
            while (i < mid) tmp[k++] = e[i++];
            while (j < hi) tmp[k++] = e[j++];
        }
        memcpy(e, tmp, n * sizeof *e);
    }
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        if (out && ent_cmp(&e[out - 1], &e[i]) == 0) e[out - 1] = e[i];
        else e[out++] = e[i];
    }
    return out;
}

/* The object's members sorted by key, duplicates resolved. *ents is malloc'd (caller frees). */
static int map_entries(D *d, const JrVal *v, const char *expected, Ent **ents, size_t *count) {
    *ents = NULL;
    *count = 0;
    if (v->type != JR_OBJ) return type_err(d, v, expected);
    if (!v->n) return 0;
    Ent *e = malloc(2 * v->n * sizeof *e);
    if (!e) return dfail(d, v->off, "out of memory", 0, 0);
    for (size_t i = 0; i < v->n; i++) {
        e[i].key = v->members[i].key;
        e[i].key_n = v->members[i].key_n;
        e[i].idx = i;
    }
    *count = sort_dedupe(e, v->n, e + v->n);
    *ents = e;
    return 0;
}

/* BTreeMap<String, String>. */
static int d_strmap(D *d, const JrVal *v, cc_kv **out, size_t *n) {
    Ent *e;
    size_t c;
    *out = NULL;
    *n = 0;
    if (v->type == JR_OBJ)
        for (size_t i = 0; i < v->n; i++)
            if (v->members[i].val.type != JR_STR) return type_err(d, &v->members[i].val, "a string");
    if (map_entries(d, v, "a map", &e, &c)) return -1;
    cc_kv *kv = alloc_n(d, c, sizeof *kv, v->off);
    int rc = c && !kv ? -1 : 0;
    for (size_t i = 0; !rc && i < c; i++) {
        const JrMember *m = &v->members[e[i].idx];
        kv[i].key.n = m->key_n;
        kv[i].key.p = m->key_n ? arena_dup(d->a, m->key, m->key_n) : NULL;
        if (m->key_n && !kv[i].key.p) rc = dfail(d, v->off, "out of memory", 0, 0);
        else rc = d_str(d, &m->val, &kv[i].value);
    }
    free(e);
    if (rc) return -1;
    *out = kv;
    *n = c;
    return 0;
}

static int d_storey(D *d, const JrVal *v, cc_storey *s) {
    static const char *const F[] = {"id", "name", "elevation", "is_storey"};
    const JrVal *f[4];
    if (d_fields(d, v, F, 4, f, "struct Storey", 0)) return -1;
    for (int i = 0; i < 3; i++)
        if (need(d, v, f[i], F[i])) return -1;
    if (d_gid(d, f[0], &s->id) || d_str(d, f[1], &s->name) || d_f64(d, f[2], &s->elevation)) return -1;
    if (f[3] && f[3]->type != JR_BOOL) return type_err(d, f[3], "a boolean"); /* ADR 0056 */
    s->not_storey = f[3] && !f[3]->b;
    return 0;
}

static int d_props(D *d, const JrVal *v, cc_material_props *p) {
    static const char *const F[] = {"density_kg_m3", "e_mpa", "strength_mpa", "source"};
    const JrVal *f[4];
    if (d_fields(d, v, F, 4, f, "struct MaterialProps", 1)) return -1;
    int *has[3] = {&p->has_density, &p->has_e, &p->has_strength};
    double *val[3] = {&p->density_kg_m3, &p->e_mpa, &p->strength_mpa};
    for (int i = 0; i < 3; i++)
        if (f[i] && f[i]->type != JR_NULL) {
            if (d_f64(d, f[i], val[i])) return -1;
            *has[i] = 1;
        }
    if (f[3] && d_str(d, f[3], &p->source)) return -1;
    return 0;
}

static int d_material(D *d, const JrVal *v, cc_material *m) {
    static const char *const F[] = {"id", "name", "category", "props"};
    const JrVal *f[4];
    if (d_fields(d, v, F, 4, f, "struct Material", 0)) return -1;
    for (int i = 0; i < 3; i++)
        if (need(d, v, f[i], F[i])) return -1;
    int cat = 0;
    if (d_gid(d, f[0], &m->id) || d_str(d, f[1], &m->name) ||
        d_enum(d, f[2], cat_name_i, CC_CAT_COUNT,
               "`concrete`, `steel`, `timber`, `masonry`, `insulation`, `earth`, `glass`, `other`", &cat))
        return -1;
    m->category = (cc_category)cat;
    if (f[3] && d_props(d, f[3], &m->props)) return -1;
    return 0;
}

/* CONC1: model.rs Opening: { id?: GlobalId, placement, generator } (file.c d_opening), a list of
 * at most CC_MAX_OPENINGS. */
static int d_openings(D *d, const JrVal *v, cc_element *e) {
    if (v->type != JR_ARR) return type_err(d, v, "a sequence");
    if (v->n > CC_MAX_OPENINGS) return dfail(d, v->off, "too many openings", 0, 0);
    e->nopenings = v->n;
    e->openings = v->n ? arena_alloc(d->a, v->n * sizeof *e->openings, 8) : NULL;
    if (v->n && !e->openings) return dfail(d, v->off, "out of memory", 0, 0);
    for (size_t i = 0; i < v->n; i++) {
        static const char *const F[] = {"id", "placement", "generator"};
        const JrVal *f[3], *x = &v->items[i];
        cc_opening *o = &e->openings[i];
        memset(o, 0, sizeof *o);
        if (d_fields(d, x, F, 3, f, "struct Opening", 0)) return -1;
        for (int k = 1; k < 3; k++)
            if (need(d, x, f[k], F[k])) return -1;
        if (f[0] && f[0]->type != JR_NULL) {
            if (d_gid(d, f[0], &o->id)) return -1;
            o->has_id = 1;
        }
        if (d_placement(d, f[1], &o->placement) || d_generator(d, f[2], &o->generator)) return -1;
    }
    return 0;
}

static int d_element(D *d, const JrVal *v, cc_element *e) {
    static const char *const F[] = {"id", "class", "name", "storey", "material", "placement",
                                    "generator", "openings", "properties", "ifc_entity"};
    const JrVal *f[10];
    if (d_fields(d, v, F, 10, f, "struct Element", 0)) return -1;
    for (int i = 0; i < 6; i++)
        if (need(d, v, f[i], F[i])) return -1;
    int cls = 0;
    if (d_gid(d, f[0], &e->id) ||
        d_enum(d, f[1], class_name_i, CC_IFC_CLASS_COUNT,
               "`IfcColumn`, `IfcBeam`, `IfcSlab`, `IfcWall`, `IfcFooting`, `IfcBuildingElementProxy`, "
               "`IfcElementAssembly`",
               &cls) ||
        d_str(d, f[2], &e->name) || d_gid(d, f[3], &e->storey) || d_gid(d, f[4], &e->material) ||
        d_placement(d, f[5], &e->placement))
        return -1;
    e->cls = (cc_ifc_class)cls;
    /* An IfcElementAssembly has no generator (ADR 0070): the key is left out. */
    if (!f[6] && e->cls == CC_IFC_ELEMENT_ASSEMBLY) e->generator.kind = CC_GEN_NONE;
    else if (need(d, v, f[6], F[6]) || d_generator(d, f[6], &e->generator)) return -1;
    if (f[7] && d_openings(d, f[7], e)) return -1; /* CONC1 */
    if (f[8] && d_strmap(d, f[8], &e->properties, &e->nproperties)) return -1;
    if (f[9] && f[9]->type != JR_NULL) {
        if (d_str(d, f[9], &e->ifc_entity)) return -1;
        e->has_ifc_entity = 1;
    }
    return 0;
}


/* ---- commands from JSON ---- */

static const char *op_name_i(int k) { return cc_cmd_op_name((cc_cmd_op)k); }

static int to_err(D *d, Buf *err) {
    const char *msg = d->err->msg;
    if (memcmp(msg, "parse: ", 7) == 0) msg += 7; /* msg is NUL-terminated, 512 bytes */
    if (err) {
        buf_clear(err);
        buf_puts(err, msg);
    }
    return -1;
}

/* An internally tagged enum: the fields other than "op" decoded as the variant's struct. */
static int d_command(D *d, const JrVal *v, cc_command *c) {
    if (v->type != JR_OBJ) return type_err(d, v, "internally tagged enum Command");
    const JrVal *tag = NULL;
    for (size_t j = 0; j < v->n; j++)
        if (jr_key_is(&v->members[j], "op")) {
            if (tag) return dfail(d, v->members[j].val.off, "duplicate field `op`", 0, 0);
            tag = &v->members[j].val;
        }
    if (!tag) return dfail(d, v->off, "missing field `op`", 0, 0);
    int op = 0;
    if (tag->type != JR_STR) return type_err(d, tag, "variant identifier");
    for (op = 0; op < CC_CMD_COUNT; op++)
        if (strlen(op_name_i(op)) == tag->n && memcmp(op_name_i(op), tag->s, tag->n) == 0) break;
    if (op == CC_CMD_COUNT) {
        /* Rust's variants, in its order. */
        return d_enum(d, tag, op_name_i, CC_CMD_COUNT,
                      "`add_storey`, `remove_storey`, `add_material`, `remove_material`, `add_element`, "
                      "`remove_element`, `set_generator`, `set_placement`, `rename`, `set_element_material`, "
                      "`set_property`, `add_view`, `remove_view`, `set_view`, `add_sheet`, `remove_sheet`, "
                      "`set_sheet`, `set_material`, `set_schedules`, `set_project`, `add_relation`, "
                      "`remove_relation`, `set_relation`, `set_storey`, `add_grid`, `remove_grid`, `set_grid`, "
                      "`set_georef`, `set_openings`",
                      &op);
    }
    memset(c, 0, sizeof *c);
    c->op = (cc_cmd_op)op;
    switch (c->op) {
    case CC_CMD_ADD_STOREY:
    case CC_CMD_SET_STOREY: return d_storey(d, v, &c->storey);
    case CC_CMD_ADD_MATERIAL:
    case CC_CMD_SET_MATERIAL: return d_material(d, v, &c->material);
    case CC_CMD_ADD_ELEMENT: return d_element(d, v, &c->element);
    case CC_CMD_ADD_VIEW:
    case CC_CMD_SET_VIEW:
    case CC_CMD_ADD_SHEET:
    case CC_CMD_SET_SHEET: {
        cc_vd vd;
        cc_vd_init(&vd, d->a);
        int rc = c->op == CC_CMD_ADD_VIEW || c->op == CC_CMD_SET_VIEW ? cc_vd_view(&vd, v, "op", &c->view)
                                                                     : cc_vd_sheet(&vd, v, "op", &c->sheet);
        if (rc) {
            buf_clear(&d->msg);
            buf_append(&d->msg, vd.msg.p, vd.msg.oom ? 0 : vd.msg.len);
            fail_buf(d, vd.off);
        }
        cc_vd_free(&vd);
        return rc ? -1 : 0;
    }
    case CC_CMD_ADD_RELATION:
    case CC_CMD_SET_RELATION:
    case CC_CMD_ADD_GRID:
    case CC_CMD_SET_GRID: {
        cc_vd vd;
        cc_vd_init(&vd, d->a);
        int rc = c->op == CC_CMD_ADD_GRID || c->op == CC_CMD_SET_GRID ? cc_vd_grid(&vd, v, "op", &c->grid)
                                                                     : cc_vd_relation(&vd, v, "op", &c->relation);
        if (rc) {
            buf_clear(&d->msg);
            buf_append(&d->msg, vd.msg.p, vd.msg.oom ? 0 : vd.msg.len);
            fail_buf(d, vd.off);
        }
        cc_vd_free(&vd);
        return rc ? -1 : 0;
    }
    case CC_CMD_SET_GEOREF: { /* {"op": "set_georef", "georef": {...}}: {} clears it */
        const JrVal *g = NULL;
        for (size_t j = 0; j < v->n; j++)
            if (jr_key_is(&v->members[j], "georef")) {
                if (g) return dfail(d, v->members[j].val.off, "duplicate field `georef`", 0, 0);
                g = &v->members[j].val;
            }
        if (!g) return dfail(d, v->off, "missing field `georef`", 0, 0);
        cc_vd vd;
        cc_vd_init(&vd, d->a);
        int rc = cc_vd_georef(&vd, g, NULL, &c->georef);
        if (rc) {
            buf_clear(&d->msg);
            buf_append(&d->msg, vd.msg.p, vd.msg.oom ? 0 : vd.msg.len);
            fail_buf(d, vd.off);
        }
        cc_vd_free(&vd);
        return rc ? -1 : 0;
    }
    default: break;
    }
    static const char *const F[] = {"id", "generator", "placement", "name", "material", "key", "value", "project",
                                    "schedules", "openings"};
    const JrVal *f[10];
    /* Only the variant's own fields count; others are ignored, as serde does without
     * deny_unknown_fields ("op" among them). */
    if (d_fields(d, v, F, 10, f, "struct variant", 0)) return -1;
    if (c->op == CC_CMD_SET_SCHEDULES) {
        if (need(d, v, f[8], "schedules")) return -1;
        cc_vd vd;
        cc_vd_init(&vd, d->a);
        int rc = cc_vd_schedules(&vd, f[8], &c->schedules, &c->nschedules);
        if (rc) {
            buf_clear(&d->msg);
            buf_append(&d->msg, vd.msg.p, vd.msg.oom ? 0 : vd.msg.len);
            fail_buf(d, vd.off);
        }
        cc_vd_free(&vd);
        return rc ? -1 : 0;
    }
    if (c->op != CC_CMD_SET_PROJECT && (need(d, v, f[0], "id") || d_gid(d, f[0], &c->id))) return -1;
    switch (c->op) {
    case CC_CMD_SET_GENERATOR: return need(d, v, f[1], "generator") || d_generator(d, f[1], &c->generator) ? -1 : 0;
    case CC_CMD_SET_OPENINGS: return need(d, v, f[9], "openings") || d_openings(d, f[9], &c->element) ? -1 : 0;
    case CC_CMD_SET_PLACEMENT: return need(d, v, f[2], "placement") || d_placement(d, f[2], &c->placement) ? -1 : 0;
    case CC_CMD_RENAME: return need(d, v, f[3], "name") || d_str(d, f[3], &c->name) ? -1 : 0;
    case CC_CMD_SET_ELEMENT_MATERIAL: return need(d, v, f[4], "material") || d_gid(d, f[4], &c->material_id) ? -1 : 0;
    case CC_CMD_SET_PROPERTY:
        if (need(d, v, f[5], "key") || d_str(d, f[5], &c->key)) return -1;
        if (f[6] && f[6]->type != JR_NULL) {
            if (d_str(d, f[6], &c->value)) return -1;
            c->has_value = 1;
        }
        return 0;
    case CC_CMD_SET_PROJECT: return need(d, v, f[7], "project") || d_strmap(d, f[7], &c->project, &c->nproject) ? -1 : 0;
    default: return 0;
    }
}

int cc_command_parse(const JrVal *v, cc_cmds *l, Buf *err) {
    cc_error e;
    e.offset = CC_NO_OFFSET;
    e.msg[0] = 0;
    D d = {&l->arena, &e, 0, {0}};
    buf_init(&d.msg);
    cc_command c;
    int rc = d_command(&d, v, &c);
    buf_free(&d.msg);
    if (rc) return to_err(&d, err);
    cc_command *slot = cc_cmds_push(l, c.op);
    if (!slot) return oom(err);
    *slot = c;
    return 0;
}

int cc_commands_parse(const char *text, size_t n, cc_cmds *l, Buf *err) {
    Arena dom;
    arena_init(&dom, 0);
    JrError je;
    const JrVal *root = jr_parse(&dom, text, n, &je);
    int rc = 0;
    if (!root) {
        if (err) {
            buf_clear(err);
            buf_puts(err, je.msg);
        }
        rc = -1;
    } else if (root->type != JR_ARR) {
        if (err) {
            buf_clear(err);
            buf_puts(err, "expected an array of commands");
        }
        rc = -1;
    } else {
        for (size_t i = 0; !rc && i < root->n; i++) rc = cc_command_parse(&root->items[i], l, err);
    }
    arena_free(&dom);
    return rc;
}
