/* Model helpers, validation (model.rs Model::validate and friends) and canonical form. */
#include "model.h"
#include <string.h>
#include "fmt.h"
#include "relation.h"
#include "view_io.h"

const char cc_gid_alphabet[65] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$";

static int in_alphabet(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
           c == '$';
}

int cc_gid_valid(const char *s, size_t n) {
    if (n != 22 || s[0] < '0' || s[0] > '3') return 0;
    for (size_t i = 0; i < n; i++)
        if (!in_alphabet(s[i])) return 0;
    return 1;
}

int cc_gid_cmp(const cc_gid *a, const cc_gid *b) { return memcmp(a->c, b->c, 22); }

static const char *const CLASS[] = {"IfcColumn", "IfcBeam", "IfcSlab", "IfcWall", "IfcFooting",
                                    "IfcBuildingElementProxy", "IfcElementAssembly"};
static const char *const CAT[] = {"concrete", "steel", "timber", "masonry",
                                  "insulation", "earth", "glass", "other"};
static const char *const KIND[] = {"rect_column", "rect_beam", "slab", "wall",
                                   "pad_footing", "extruded_polygon", "mesh"};

const char *cc_ifc_class_name(cc_ifc_class c) { return (unsigned)c < CC_IFC_CLASS_COUNT ? CLASS[c] : "?"; }
const char *cc_category_name(cc_category c) { return (unsigned)c < CC_CAT_COUNT ? CAT[c] : "?"; }
const char *cc_gen_kind_name(cc_gen_kind k) {
    return (unsigned)k < CC_GEN_COUNT ? KIND[k] : k == CC_GEN_NONE ? "none" : "?";
}

void cc_model_init(cc_model *m) {
    memset(m, 0, sizeof *m);
    arena_init(&m->arena, 0);
}

void cc_model_free(cc_model *m) {
    arena_free(&m->arena);
    memset(m, 0, sizeof *m);
}

#define BSEARCH(T, arr, n, id)                              \
    do {                                                    \
        size_t lo = 0, hi = (n);                            \
        while (lo < hi) {                                   \
            size_t mid = lo + (hi - lo) / 2;                \
            int c = cc_gid_cmp(&(arr)[mid].id, (id));       \
            if (c == 0) return &(arr)[mid];                 \
            if (c < 0) lo = mid + 1;                        \
            else hi = mid;                                  \
        }                                                   \
        return (const T *)0;                                \
    } while (0)

const cc_storey *cc_model_storey(const cc_model *m, const cc_gid *id) { BSEARCH(cc_storey, m->storeys, m->nstoreys, id); }
const cc_material *cc_model_material(const cc_model *m, const cc_gid *id) { BSEARCH(cc_material, m->materials, m->nmaterials, id); }
const cc_element *cc_model_element(const cc_model *m, const cc_gid *id) { BSEARCH(cc_element, m->elements, m->nelements, id); }

/* ---- text helpers ---- */

static int finite(double x) { return x - x == 0.0; }
static double fabs_(double x) { return x < 0 ? -x : x; }

void cc_buf_gid(Buf *b, const cc_gid *g) { buf_append(b, g->c, 22); }

void cc_buf_display_f64(Buf *b, double x) {
    if (x != x) { buf_puts(b, "NaN"); return; }
    if (!finite(x)) { buf_puts(b, x < 0 ? "-inf" : "inf"); return; }
    Buf t;
    buf_init(&t);
    fmt_f64_debug(&t, x);
    const char *s = buf_cstr(&t);
    size_t n = t.len, e = n;
    for (size_t i = 0; i < n; i++)
        if (s[i] == 'e') e = i;
    if (e == n) { /* plain: drop a ".0" */
        if (n >= 2 && s[n - 2] == '.' && s[n - 1] == '0') n -= 2;
        buf_append(b, s, n);
        buf_free(&t);
        return;
    }
    size_t i = 0;
    if (s[0] == '-') buf_putc(b, '-'), i = 1;
    char d[32];
    size_t nd = 0;
    long point = -1;
    for (; i < e && nd < sizeof d; i++) {
        if (s[i] == '.') point = (long)nd;
        else d[nd++] = s[i];
    }
    if (point < 0) point = (long)nd;
    long ex = 0;
    int eneg = 0;
    i = e + 1;
    if (i < n && s[i] == '-') eneg = 1, i++;
    for (; i < n; i++) ex = ex * 10 + (s[i] - '0');
    long pos = point + (eneg ? -ex : ex);
    if (pos <= 0) {
        buf_puts(b, "0.");
        buf_fill(b, '0', (size_t)-pos);
        buf_append(b, d, nd);
    } else if ((size_t)pos >= nd) {
        buf_append(b, d, nd);
        buf_fill(b, '0', (size_t)pos - nd);
    } else {
        buf_append(b, d, (size_t)pos);
        buf_putc(b, '.');
        buf_append(b, d + pos, nd - (size_t)pos);
    }
    buf_free(&t);
}

void cc_buf_debug_str(Buf *b, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    buf_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        case 0: buf_puts(b, "\\0"); break;
        default:
            if (c < 0x20 || c == 0x7f) {
                buf_puts(b, "\\u{");
                if (c >= 16) buf_putc(b, hex[c >> 4]);
                buf_putc(b, hex[c & 15]);
                buf_putc(b, '}');
            } else {
                buf_putc(b, (char)c);
            }
        }
    }
    buf_putc(b, '"');
}

/* ---- validation ---- */

static size_t utf8_chars(cc_str s) {
    size_t k = 0;
    for (size_t i = 0; i < s.n; i++)
        if (((unsigned char)s.p[i] & 0xC0) != 0x80) k++;
    return k;
}

static int set_err(cc_error *err, Buf *msg) {
    if (err) {
        size_t n = msg->oom ? 0 : msg->len;
        if (n >= sizeof err->msg) {
            n = sizeof err->msg - 1;
            while (n && ((unsigned char)msg->p[n] & 0xC0) == 0x80) n--; /* whole characters */
        }
        if (n) memcpy(err->msg, msg->p, n);
        err->msg[n] = 0;
        err->offset = CC_NO_OFFSET;
    }
    buf_free(msg);
    return -1;
}

static int pos_check(Buf *b, const char *name, double v) {
    if (finite(v) && v > 0.0) return 0;
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
        if (!finite(l->pts[i][0]) || !finite(l->pts[i][1])) return 0;
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

/* Generator::validate. Writes the bare message to b on failure. */
static int gen_validate(Buf *b, const cc_generator *g) {
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING:
        if (pos_check(b, "width", g->u.col.width) || pos_check(b, "depth", g->u.col.depth) ||
            pos_check(b, "height", g->u.col.height))
            return -1;
        return 0;
    case CC_GEN_RECT_BEAM:
        if (pos_check(b, "width", g->u.beam.width) || pos_check(b, "height", g->u.beam.height) ||
            pos_check(b, "length", g->u.beam.length))
            return -1;
        return 0;
    case CC_GEN_WALL:
        if (pos_check(b, "length", g->u.wall.length) || pos_check(b, "thickness", g->u.wall.thickness) ||
            pos_check(b, "height", g->u.wall.height))
            return -1;
        return 0;
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
                if (!finite(g->u.mesh.positions[i][k])) {
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
            if (!finite(v[i][k])) { buf_puts(b, "non-finite placement"); return -1; }
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

static int key_char_ok(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

int cc_model_validate_keys(const cc_model *m, const cc_gid *skeys, const cc_gid *mkeys,
                           const cc_gid *ekeys, const cc_gid *vkeys, const cc_gid *shkeys, const cc_gid *rkeys,
                           cc_error *err) {
    Buf b;
    buf_init(&b);
    /* project::validate */
    if (m->nproject > 200) {
        buf_puts(&b, "project information: at most 200 entries");
        return set_err(err, &b);
    }
    for (size_t i = 0; i < m->nproject; i++) {
        cc_str k = m->project[i].key;
        int ok = k.n > 0 && k.n <= 64;
        for (size_t j = 0; ok && j < k.n; j++) ok = key_char_ok(k.p[j]);
        if (!ok) {
            buf_puts(&b, "project information: ");
            cc_buf_debug_str(&b, k.p, k.n);
            buf_puts(&b, " is not a key (letters, digits and _, at most 64)");
            return set_err(err, &b);
        }
        if (utf8_chars(m->project[i].value) > 1000) {
            buf_puts(&b, "project information ");
            buf_append(&b, k.p, k.n);
            buf_puts(&b, ": at most 1000 characters");
            return set_err(err, &b);
        }
    }
    if (cc_schedules_validate(m->schedules, m->nschedules, &b)) return set_err(err, &b);
    for (size_t i = 0; i < m->nstoreys; i++) {
        const cc_storey *s = &m->storeys[i];
        const cc_gid *k = skeys ? &skeys[i] : &s->id;
        if (cc_gid_cmp(k, &s->id) || !finite(s->elevation)) {
            buf_puts(&b, "storey ");
            cc_buf_gid(&b, k);
            buf_puts(&b, ": inconsistent id or elevation");
            return set_err(err, &b);
        }
    }
    for (size_t i = 0; i < m->nmaterials; i++) {
        const cc_material *mt = &m->materials[i];
        const cc_material_props *p = &mt->props;
        const char *what[3] = {"density", "modulus", "strength"};
        const char *maxs[3] = {"30000", "1000000", "10000"};
        const double maxv[3] = {30000.0, 1000000.0, 10000.0};
        const int has[3] = {p->has_density, p->has_e, p->has_strength};
        const double val[3] = {p->density_kg_m3, p->e_mpa, p->strength_mpa};
        for (int j = 0; j < 3; j++)
            if (has[j] && !(finite(val[j]) && val[j] > 0.0 && val[j] <= maxv[j])) {
                buf_puts(&b, "material ");
                buf_puts(&b, what[j]);
                buf_puts(&b, ": a positive number up to ");
                buf_puts(&b, maxs[j]);
                return set_err(err, &b);
            }
        if (utf8_chars(p->source) > 200) {
            buf_puts(&b, "material source: at most 200 characters");
            return set_err(err, &b);
        }
        const cc_gid *k = mkeys ? &mkeys[i] : &mt->id;
        if (cc_gid_cmp(k, &mt->id)) {
            buf_puts(&b, "material ");
            cc_buf_gid(&b, k);
            buf_puts(&b, ": key does not match id");
            return set_err(err, &b);
        }
    }
    for (size_t i = 0; i < m->nelements; i++) {
        const cc_element *e = &m->elements[i];
        const cc_gid *k = ekeys ? &ekeys[i] : &e->id;
        /* ctx: "{class_name} {k} ({name}): {msg}" */
        if (e->has_ifc_entity) buf_append(&b, e->ifc_entity.p, e->ifc_entity.n);
        else buf_puts(&b, cc_ifc_class_name(e->cls));
        buf_putc(&b, ' ');
        cc_buf_gid(&b, k);
        buf_puts(&b, " (");
        buf_append(&b, e->name.p, e->name.n);
        buf_puts(&b, "): ");
        if (cc_gid_cmp(k, &e->id)) {
            buf_puts(&b, "key does not match id");
            return set_err(err, &b);
        }
        if (!cc_model_storey(m, &e->storey)) {
            buf_puts(&b, "unknown storey ");
            cc_buf_gid(&b, &e->storey);
            return set_err(err, &b);
        }
        if (!cc_model_material(m, &e->material)) {
            buf_puts(&b, "unknown material ");
            cc_buf_gid(&b, &e->material);
            return set_err(err, &b);
        }
        if (placement_validate(&b, &e->placement)) return set_err(err, &b);
        /* ADR 0070: an assembly has no generator (its shape is its parts) and nothing to cut;
         * every other element has one. */
        if (e->cls == CC_IFC_ELEMENT_ASSEMBLY || e->generator.kind == CC_GEN_NONE) {
            if (e->cls != CC_IFC_ELEMENT_ASSEMBLY) {
                buf_puts(&b, "missing generator");
                return set_err(err, &b);
            }
            if (e->generator.kind != CC_GEN_NONE) {
                buf_puts(&b, "an IfcElementAssembly has no generator: its shape is its parts");
                return set_err(err, &b);
            }
            if (e->nopenings) {
                buf_puts(&b, "an IfcElementAssembly has no openings");
                return set_err(err, &b);
            }
        } else if (gen_validate(&b, &e->generator)) return set_err(err, &b);
        for (size_t j = 0; j < e->nopenings; j++) {
            /* "{class_name} {name} (opening {j}): {msg}" */
            buf_clear(&b);
            if (e->has_ifc_entity) buf_append(&b, e->ifc_entity.p, e->ifc_entity.n);
            else buf_puts(&b, cc_ifc_class_name(e->cls));
            buf_putc(&b, ' ');
            buf_append(&b, e->name.p, e->name.n);
            buf_puts(&b, " (opening ");
            buf_u64(&b, j);
            buf_puts(&b, "): ");
            if (placement_validate(&b, &e->openings[j].placement)) return set_err(err, &b);
            if (gen_validate(&b, &e->openings[j].generator)) return set_err(err, &b);
        }
        for (size_t j = 0; j < e->nproperties; j++)
            if (e->properties[j].key.n == 0) {
                buf_puts(&b, "empty property key");
                return set_err(err, &b);
            }
        buf_clear(&b);
    }
    for (size_t i = 0; i < m->nviews; i++) {
        const cc_view *v = &m->views[i];
        const cc_gid *k = vkeys ? &vkeys[i] : &v->id;
        if (cc_gid_cmp(k, &v->id)) {
            buf_puts(&b, "view ");
            cc_buf_gid(&b, k);
            buf_puts(&b, ": key does not match id");
            return set_err(err, &b);
        }
        if (cc_view_validate(m, v, &b)) return set_err(err, &b);
    }
    for (size_t i = 0; i < m->nsheets; i++) {
        const cc_sheet *s = &m->sheets[i];
        const cc_gid *k = shkeys ? &shkeys[i] : &s->id;
        if (cc_gid_cmp(k, &s->id)) {
            buf_puts(&b, "sheet ");
            cc_buf_gid(&b, k);
            buf_puts(&b, ": key does not match id");
            return set_err(err, &b);
        }
        if (cc_sheet_validate(m, s, &b)) return set_err(err, &b);
        for (size_t j = 0; j < i; j++) /* BTreeSet of numbers: the first repeat, in id order */
            if (m->sheets[j].number.n == s->number.n &&
                (s->number.n == 0 || !memcmp(m->sheets[j].number.p, s->number.p, s->number.n))) {
                buf_puts(&b, "two sheets are numbered ");
                buf_append(&b, s->number.p, s->number.n);
                return set_err(err, &b);
            }
    }
    if (m->nrelations && cc_relations_validate(m, m->relations, m->nrelations, rkeys, &b)) return set_err(err, &b);
    buf_free(&b);
    return 0;
}

int cc_model_validate(const cc_model *m, cc_error *err) {
    return cc_model_validate_keys(m, NULL, NULL, NULL, NULL, NULL, NULL, err);
}

/* ---- canonical: x + 0.0 turns -0.0 into 0.0 and keeps every other value ---- */

static void canon(double *v, size_t n) {
    for (size_t i = 0; i < n; i++) v[i] = v[i] + 0.0;
}

static void canon_loop(cc_loop *l) {
    if (l->n) canon(l->pts[0], 2 * l->n);
}

static void canon_placement(cc_placement *p) {
    canon(p->origin, 3);
    canon(p->z_axis, 3);
    canon(p->x_axis, 3);
}

static void canon_generator(cc_generator *g) {
    switch (g->kind) {
    case CC_GEN_RECT_COLUMN:
    case CC_GEN_PAD_FOOTING: canon(&g->u.col.width, 1); canon(&g->u.col.depth, 1); canon(&g->u.col.height, 1); break;
    case CC_GEN_RECT_BEAM: canon(&g->u.beam.width, 1); canon(&g->u.beam.height, 1); canon(&g->u.beam.length, 1); break;
    case CC_GEN_WALL: canon(&g->u.wall.length, 1); canon(&g->u.wall.thickness, 1); canon(&g->u.wall.height, 1); break;
    case CC_GEN_SLAB:
        canon(&g->u.slab.thickness, 1);
        canon_loop(&g->u.slab.outline);
        break;
    case CC_GEN_EXTRUDED_POLYGON:
        canon(&g->u.poly.height, 1);
        canon_loop(&g->u.poly.outline);
        for (size_t j = 0; j < g->u.poly.nvoids; j++)
            canon_loop(&g->u.poly.voids[j]);
        break;
    case CC_GEN_MESH:
        if (g->u.mesh.npositions) canon(g->u.mesh.positions[0], 3 * g->u.mesh.npositions);
        break;
    default: break;
    }
}

void cc_model_canonical(cc_model *m) {
    for (size_t i = 0; i < m->nstoreys; i++) canon(&m->storeys[i].elevation, 1);
    for (size_t i = 0; i < m->nmaterials; i++) {
        cc_material_props *p = &m->materials[i].props;
        canon(&p->density_kg_m3, 1);
        canon(&p->e_mpa, 1);
        canon(&p->strength_mpa, 1);
    }
    for (size_t i = 0; i < m->nelements; i++) {
        cc_element *e = &m->elements[i];
        canon_placement(&e->placement);
        canon_generator(&e->generator);
        for (size_t j = 0; j < e->nopenings; j++) {
            canon_placement(&e->openings[j].placement);
            canon_generator(&e->openings[j].generator);
        }
    }
    for (size_t i = 0; i < m->nviews; i++) cc_view_canonical(&m->views[i]);
    for (size_t i = 0; i < m->nsheets; i++) cc_sheet_canonical(&m->sheets[i]);
}
