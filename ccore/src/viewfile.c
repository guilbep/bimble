/* The .bimbleview writer and reader, see viewfile.h. A port of crates/bimble-geom/src/viewfile.rs. */
#include "viewfile.h"
#include "ifc_psets.h"
#include "georef.h"
#include <stdlib.h>
#include <string.h>
#include "inflate.h"
#include "json_r.h"
#include "meshcodec.h"

static const char MAGIC[8] = {'B', 'I', 'M', 'B', 'L', 'E', 'V', 'W'};
#define MAX_SECTION ((uint64_t)1 << 30)
#define MAX_EXPANSION 64u

/* MaterialCategory::label(), in MaterialCategory::ALL order. */
static const char *const FAMILY[CC_CAT_COUNT] = {"Concrete", "Steel", "Timber", "Masonry",
                                                 "Insulation", "Earth", "Glass", "Other"};

int cc_viewfile_sniff(const uint8_t *p, size_t n) {
    return (n >= 8 && memcmp(p, MAGIC, 8) == 0) || (n >= 2 && p[0] == 0x1f && p[1] == 0x8b);
}

/* ---- writing ---- */

static void put_u64le(Buf *b, uint64_t v) {
    char t[8];
    for (int i = 0; i < 8; i++) t[i] = (char)(v >> (8 * i));
    buf_append(b, t, 8);
}

static void put_f64(Buf *b, double v) {
    uint64_t u;
    if (v == 0.0) v = 0.0; /* no -0.0 */
    memcpy(&u, &v, 8);
    put_u64le(b, u);
}

static void put_f32(Buf *b, float v) {
    uint32_t u;
    char t[4];
    if (v == 0.0f) v = 0.0f;
    memcpy(&u, &v, 4);
    for (int i = 0; i < 4; i++) t[i] = (char)(u >> (8 * i));
    buf_append(b, t, 4);
}

static void put_str(Buf *b, const char *p, size_t n) {
    cc_put_varint(b, n);
    buf_append(b, p, n);
}

static void section(Buf *out, const char kind[4], const Buf *payload) {
    buf_append(out, kind, 4);
    put_u64le(out, payload->len);
    buf_append(out, payload->p, payload->len);
}

static int str_cmp(const cc_str *a, const cc_str *b) {
    size_t n = a->n < b->n ? a->n : b->n;
    int c = n ? memcmp(a->p, b->p, n) : 0;
    if (c) return c;
    return a->n < b->n ? -1 : a->n > b->n;
}

static int str_qcmp(const void *a, const void *b) { return str_cmp(a, b); }

/* f64::total_cmp as a signed key. */
static int64_t total_key(double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    int64_t k = (int64_t)u;
    return k ^ (int64_t)((uint64_t)(k >> 63) >> 1);
}

typedef struct {
    cc_view_storey s;
    size_t order;
} StoreyRef;

static int storey_qcmp(const void *a, const void *b) {
    const StoreyRef *x = a, *y = b;
    int64_t kx = total_key(x->s.elevation), ky = total_key(y->s.elevation);
    if (kx != ky) return kx < ky ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order; /* stable */
}

typedef struct {
    cc_view_density d;
    size_t order;
} DensityRef;

static int density_qcmp(const void *a, const void *b) {
    const DensityRef *x = a, *y = b;
    int c = str_cmp(&x->d.material, &y->d.material);
    if (c) return c;
    return x->order < y->order ? -1 : x->order > y->order;
}

void cc_viewfile_meta_init(cc_viewfile_meta *mt) {
    memset(mt, 0, sizeof *mt);
    arena_init(&mt->arena, 0);
}

void cc_viewfile_meta_free(cc_viewfile_meta *mt) {
    arena_free(&mt->arena);
    memset(mt, 0, sizeof *mt);
}

/* Scene::from_model's META: storeys sorted by elevation (stable), no grids, densities by
 * material name (BTreeMap: the last one inserted, model order, wins), and the model's
 * georeference (GEOR, its serde_json text) when it is not the default (scene.rs:351). */
int cc_viewfile_meta_from_model(cc_viewfile_meta *mt, const cc_model *m, const cc_scene *s) {
    cc_viewfile_meta_init(mt);
    Arena *a = &mt->arena;
    size_t ne = s->nelements;
    cc_view_info *info = arena_alloc(a, (ne + 1) * sizeof *info, 8);
    const cc_element **els = arena_alloc(a, (ne + 1) * sizeof *els, 8); /* QTY1 */
    StoreyRef *st = arena_alloc(a, (m->nstoreys + 1) * sizeof *st, 8);
    DensityRef *dens = arena_alloc(a, (m->nmaterials + 1) * sizeof *dens, 8);
    cc_view_storey *storeys = arena_alloc(a, (m->nstoreys + 1) * sizeof *storeys, 8);
    cc_view_density *dout = arena_alloc(a, (m->nmaterials + 1) * sizeof *dout, 8);
    if (!info || !els || !st || !dens || !storeys || !dout) return -1;
    for (size_t k = 0; k < ne; k++) {
        const cc_scene_element *e = &s->elements[k];
        if (e->model_index >= m->nelements) return -1;
        const cc_element *me = &m->elements[e->model_index];
        const cc_storey *sto = cc_model_storey(m, &me->storey);
        const cc_material *mat = cc_model_material(m, &me->material);
        if (!sto || !mat) return -1;
        info[k].global_id.p = e->id.c, info[k].global_id.n = 22;
        info[k].volume_fix_m3 = cc_sleeve_volume_fix_m3(me); /* WAVE1B (B1) */
        info[k].name = me->name;
        info[k].storey = sto->name;
        info[k].material = mat->name;
        els[k] = me; /* QTY1 */
    }
    mt->elements = els;
    for (size_t i = 0; i < m->nstoreys; i++) {
        const cc_storey *x = &m->storeys[i];
        st[i].s.name = x->name;
        st[i].s.elevation = x->elevation;
        st[i].s.id.p = x->id.c, st[i].s.id.n = 22;
        st[i].order = i;
    }
    if (m->nstoreys) qsort(st, m->nstoreys, sizeof *st, storey_qcmp);
    for (size_t i = 0; i < m->nstoreys; i++) storeys[i] = st[i].s;
    size_t nd = 0;
    for (size_t i = 0; i < m->nmaterials; i++)
        if (m->materials[i].props.has_density) {
            dens[nd].d.material = m->materials[i].name;
            dens[nd].d.density = m->materials[i].props.density_kg_m3;
            dens[nd].order = i;
            nd++;
        }
    if (nd) qsort(dens, nd, sizeof *dens, density_qcmp);
    size_t ndu = 0;
    for (size_t i = 0; i < nd; i++) {
        if (ndu && str_cmp(&dout[ndu - 1].material, &dens[i].d.material) == 0) dout[ndu - 1] = dens[i].d;
        else dout[ndu++] = dens[i].d;
    }
    mt->info = info, mt->ninfo = ne;
    mt->storeys = storeys, mt->nstoreys = m->nstoreys;
    mt->densities = dout, mt->ndensities = ndu;
    if (!cc_georef_is_default(&m->georef)) {
        Buf j;
        buf_init(&j);
        cc_georef_json(&j, &m->georef);
        char *t = j.oom ? NULL : arena_dup(a, j.p, j.len);
        size_t n = j.len;
        buf_free(&j);
        if (!t) return -1;
        mt->has_georef = 1, mt->georef.p = t, mt->georef.n = n;
    }
    return 0;
}

/* viewfile.rs read -> Scene: what the file holds, as it is (share again = the same bytes). */
int cc_viewfile_meta_from_file(cc_viewfile_meta *mt, const cc_viewfile *v) {
    cc_viewfile_meta_init(mt);
    cc_view_info *info = arena_alloc_n(&mt->arena, v->nelements + 1, sizeof *info, 8);
    if (!info) return -1;
    for (size_t k = 0; k < v->nelements; k++) {
        const cc_view_element *x = &v->elements[k];
        info[k].global_id = x->global_id;
        info[k].name = x->name;
        info[k].storey = x->storey;
        info[k].material = x->material;
        info[k].volume_fix_m3 = 0.0; /* WAVE1B (B1): a model's sleeves only */
    }
    mt->info = info, mt->ninfo = v->nelements;
    mt->storeys = v->storeys, mt->nstoreys = v->nstoreys;
    mt->grids = v->grids, mt->ngrids = v->ngrids;
    mt->densities = v->densities, mt->ndensities = v->ndensities;
    mt->has_georef = v->has_georef, mt->georef = v->georef;
    return 0;
}

/* The four META strings of scene element k: class, storey, material, family. */
static int element_strings(const cc_viewfile_meta *mt, const cc_scene *s, size_t k, cc_str out[4]) {
    const cc_scene_element *e = &s->elements[k];
    if ((unsigned)e->category >= CC_CAT_COUNT) return -1;
    out[0] = e->class_name;
    out[1] = mt->info[k].storey;
    out[2] = mt->info[k].material;
    out[3].p = FAMILY[e->category];
    out[3].n = strlen(FAMILY[e->category]);
    return 0;
}

static uint64_t table_index(const cc_str *table, size_t n, const cc_str *key) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = str_cmp(&table[mid], key);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0; /* not reached: every key is in the table */
}

static void flush_chunk(Buf *out, Buf *shapes, uint64_t *nshapes, Buf *elems, uint64_t *nelems) {
    Buf head;
    buf_init(&head);
    cc_put_varint(&head, *nshapes);
    Buf mid;
    buf_init(&mid);
    cc_put_varint(&mid, *nelems);
    uint64_t len = head.len + shapes->len + mid.len + elems->len;
    buf_append(out, "ELEM", 4);
    put_u64le(out, len);
    buf_append(out, head.p, head.len);
    buf_append(out, shapes->p, shapes->len);
    buf_append(out, mid.p, mid.len);
    buf_append(out, elems->p, elems->len);
    if (head.oom || mid.oom) out->oom = 1;
    buf_free(&head), buf_free(&mid);
    buf_clear(shapes), buf_clear(elems);
    *nshapes = *nelems = 0;
}

static int write_raw(const cc_viewfile_meta *mt, const cc_scene *s, Buf *out) {
    int rc = -1;
    size_t ne = s->nelements;
    if (mt->ninfo != ne || (ne && !mt->info)) return -1;
    cc_str *table = malloc((4 * ne + 1) * sizeof *table);
    int64_t *shape_id = malloc((s->nshapes + 1) * sizeof *shape_id);
    Buf p, ns, es;
    buf_init(&p), buf_init(&ns), buf_init(&es);
    if (!table || !shape_id) goto done;

    buf_append(out, MAGIC, 8);
    char ver[4] = {(char)CC_VIEWFILE_VERSION, 0, 0, 0};
    buf_append(out, ver, 4);

    /* HEAD. Shapes are numbered by the first element that uses them. */
    for (size_t i = 0; i < s->nshapes; i++) shape_id[i] = -1;
    uint64_t nused = 0, stored = 0;
    for (size_t k = 0; k < ne; k++) {
        const cc_scene_element *e = &s->elements[k];
        if (e->shape < 0) {
            stored += e->mesh.ni / 3;
            continue;
        }
        if ((size_t)e->shape >= s->nshapes) goto done;
        if (shape_id[e->shape] < 0) {
            shape_id[e->shape] = (int64_t)nused++;
            stored += 1 + s->shapes[e->shape].ni / 3;
        } else {
            stored += 1;
        }
    }
    cc_put_varint(&p, ne);
    cc_put_varint(&p, nused);
    cc_put_varint(&p, stored);
    for (int k = 0; k < 3; k++) put_f64(&p, s->origin[k]);
    section(out, "HEAD", &p);

    /* META */
    size_t nt = 0;
    for (size_t k = 0; k < ne; k++) {
        if (element_strings(mt, s, k, &table[nt])) goto done;
        nt += 4;
    }
    if (nt) qsort(table, nt, sizeof *table, str_qcmp);
    size_t nu = 0;
    for (size_t i = 0; i < nt; i++)
        if (nu == 0 || str_cmp(&table[nu - 1], &table[i]) != 0) table[nu++] = table[i];
    buf_clear(&p);
    cc_put_varint(&p, nu);
    for (size_t i = 0; i < nu; i++) put_str(&p, table[i].p, table[i].n);
    cc_put_varint(&p, mt->nstoreys);
    for (size_t i = 0; i < mt->nstoreys; i++) {
        const cc_view_storey *st = &mt->storeys[i];
        put_str(&p, st->name.p, st->name.n);
        put_f64(&p, st->elevation);
        put_str(&p, st->id.p, st->id.n);
    }
    cc_put_varint(&p, mt->ngrids);
    for (size_t i = 0; i < mt->ngrids; i++) {
        const cc_view_grid *g = &mt->grids[i];
        put_str(&p, g->tag.p, g->tag.n);
        put_f64(&p, g->start[0]), put_f64(&p, g->start[1]);
        put_f64(&p, g->end[0]), put_f64(&p, g->end[1]);
    }
    cc_put_varint(&p, mt->ndensities);
    for (size_t i = 0; i < mt->ndensities; i++) {
        put_str(&p, mt->densities[i].material.p, mt->densities[i].material.n);
        put_f64(&p, mt->densities[i].density);
    }
    cc_put_varint(&p, s->nwarnings);
    for (size_t i = 0; i < s->nwarnings; i++) put_str(&p, s->warnings[i].p, s->warnings[i].n);
    section(out, "META", &p);
    if (mt->has_georef) { /* only when known (not the default): older readers skip it */
        buf_clear(&p);
        put_str(&p, mt->georef.p, mt->georef.n);
        section(out, "GEOR", &p);
    }

    /* ELEM chunks */
    uint64_t n_shapes = 0, n_elems = 0;
    int64_t written = 0;
    for (size_t k = 0; k < ne; k++) {
        const cc_scene_element *e = &s->elements[k];
        const cc_view_info *in = &mt->info[k];
        cc_str str[4];
        element_strings(mt, s, k, str);
        put_str(&es, in->global_id.p, in->global_id.n);
        put_str(&es, in->name.p, in->name.n);
        for (int j = 0; j < 4; j++) cc_put_varint(&es, table_index(table, nu, &str[j]));
        if (e->shape < 0) {
            buf_putc(&es, 0);
            buf_clear(&p);
            if (cc_meshcodec_encode(&e->mesh, &p)) goto done;
            cc_put_varint(&es, p.len);
            buf_append(&es, p.p, p.len);
        } else {
            int64_t id = shape_id[e->shape];
            if (id == written) {
                buf_clear(&p);
                if (cc_meshcodec_encode(&s->shapes[e->shape], &p)) goto done;
                cc_put_varint(&ns, p.len);
                buf_append(&ns, p.p, p.len);
                n_shapes++;
                written++;
            }
            buf_putc(&es, 1);
            cc_put_varint(&es, (uint64_t)id);
            const cc_placement32 *t = &e->to_scene;
            const float *cols[4] = {t->x, t->y, t->z, t->t};
            for (int c = 0; c < 4; c++)
                for (int i = 0; i < 3; i++) put_f32(&es, cols[c][i]);
        }
        n_elems++;
        if (ns.len + es.len >= CC_VIEWFILE_CHUNK_BYTES) flush_chunk(out, &ns, &n_shapes, &es, &n_elems);
    }
    if (n_elems > 0) flush_chunk(out, &ns, &n_shapes, &es, &n_elems);
    rc = (p.oom || ns.oom || es.oom || out->oom) ? -1 : 0;
done:
    buf_free(&p), buf_free(&ns), buf_free(&es);
    free(table), free(shape_id);
    return rc;
}

int cc_viewfile_write_meta(const cc_viewfile_meta *mt, const cc_scene *s, int gzip, Buf *out) {
    if (!gzip) return write_raw(mt, s, out);
    Buf raw;
    buf_init(&raw);
    int rc = write_raw(mt, s, &raw);
    if (rc == 0) rc = cc_gzip((const unsigned char *)raw.p, raw.len, out);
    buf_free(&raw);
    return rc;
}

int cc_viewfile_write(const cc_model *m, const cc_scene *s, int gzip, Buf *out) {
    cc_viewfile_meta mt;
    int rc = cc_viewfile_meta_from_model(&mt, m, s);
    if (rc == 0) rc = cc_viewfile_write_meta(&mt, s, gzip, out);
    cc_viewfile_meta_free(&mt);
    return rc;
}

/* ---- reading ---- */

enum { E_NOT = 1, E_VERSION, E_TRUNC, E_INVALID, E_OOM };

typedef struct {
    const uint8_t *p;
    size_t n;
} Rd;

typedef struct {
    int kind;
    uint32_t version;
    const char *why; /* E_INVALID */
    const char *why2; /* a codec reason, appended */
} Err;

static int fail(Err *e, int kind, const char *why) {
    e->kind = kind;
    e->why = why;
    return -1;
}

static int take(Rd *r, size_t n, const uint8_t **out, Err *e) {
    if (n > r->n) return fail(e, E_TRUNC, NULL);
    *out = r->p;
    r->p += n, r->n -= n;
    return 0;
}

static int varint(Rd *r, uint64_t *v, Err *e) {
    *v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (!r->n) return fail(e, E_TRUNC, NULL);
        uint8_t b = *r->p++;
        r->n--;
        *v |= (uint64_t)(b & 0x7f) << shift;
        if (b < 0x80) return 0;
    }
    return fail(e, E_INVALID, "invalid mesh bytes: varint too long");
}

static int rd_str(Rd *r, cc_str *s, Err *e) {
    uint64_t n;
    const uint8_t *p;
    if (varint(r, &n, e)) return -1;
    if (n > r->n) return fail(e, E_TRUNC, NULL);
    if (take(r, (size_t)n, &p, e)) return -1;
    if (jr_utf8_check((const char *)p, (size_t)n) != -1) return fail(e, E_INVALID, "text not UTF-8");
    s->p = (const char *)p;
    s->n = (size_t)n;
    return 0;
}

static int rd_f64(Rd *r, double *v, Err *e) {
    const uint8_t *p;
    if (take(r, 8, &p, e)) return -1;
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u |= (uint64_t)p[i] << (8 * i);
    if ((u & 0x7ff0000000000000ull) == 0x7ff0000000000000ull) return fail(e, E_INVALID, "number not finite");
    memcpy(v, &u, 8);
    return 0;
}

/* A count of items each at least `min` bytes long, checked against what is left. */
static int count(Rd *r, size_t min, size_t *n, Err *e) {
    uint64_t v;
    if (varint(r, &v, e)) return -1;
    uint64_t need = v > UINT64_MAX / min ? UINT64_MAX : v * min;
    if (need > r->n) return fail(e, E_TRUNC, NULL);
    *n = (size_t)v;
    return 0;
}

static int index_of(Rd *r, size_t ntable, uint64_t *i, Err *e) {
    if (varint(r, i, e)) return -1;
    if (*i >= ntable) return fail(e, E_INVALID, "string index");
    return 0;
}

/* SIZE3: a mapped read (cc_viewfile_read_mapped) decodes each mesh into a scratch arena to check it
 * and keeps its counts and where its bytes are (`ref`), not its arrays. */
static int mesh(Rd *r, cc_viewfile *v, cc_mesh *m, cc_viewfile_ref *ref, Err *e) {
    size_t len;
    const uint8_t *p;
    const char *why;
    if (count(r, 1, &len, e) || take(r, len, &p, e)) return -1;
    Arena *a = &v->arena;
    if (v->lazy) {
        if (len > UINT32_MAX) return fail(e, E_INVALID, "mesh too large");
        arena_trim(v->lazy_scratch, (size_t)1 << 22);
        a = v->lazy_scratch;
        ref->at = (uint64_t)(p - v->base), ref->len = (uint32_t)len;
    }
    int rc = cc_meshcodec_decode(p, len, a, m, &why);
    if (v->lazy && rc == CC_MC_OK) m->pos = NULL, m->idx = NULL, m->edges = NULL, m->normals = NULL;
    switch (rc) {
    case CC_MC_OK: return 0;
    case CC_MC_TRUNCATED: return fail(e, E_TRUNC, NULL);
    case CC_MC_BAD_MAGIC: return fail(e, E_INVALID, "not Bimble mesh bytes");
    case CC_MC_OOM: return fail(e, E_OOM, NULL);
    default: e->why2 = why; return fail(e, E_INVALID, "invalid mesh bytes: ");
    }
}

/* Arrays of the arena, sized by counts already checked against the bytes. */
static void *alloc_n(Arena *a, size_t n, size_t size, Err *e) {
    void *p = arena_alloc_n(a, n + 1, size, 8);
    if (!p) fail(e, E_OOM, NULL);
    return p;
}

static int grow(void **p, size_t *cap, size_t need, size_t size, Err *e) {
    if (need <= *cap) return 0;
    /* a count from the file: c * size must not wrap (wasm32), nor the doubling below */
    if (need > (size_t)-1 / 2 / size) return fail(e, E_OOM, NULL);
    size_t c = *cap ? *cap : 16;
    while (c < need) c *= 2;
    void *q = realloc(*p, c * size);
    if (!q) return fail(e, E_OOM, NULL);
    *p = q;
    *cap = c;
    return 0;
}

static int read_meta(Rd *s, cc_viewfile *v, Err *e) {
    size_t n;
    if (count(s, 1, &n, e)) return -1;
    if (!(v->table = alloc_n(&v->arena, n, sizeof *v->table, e))) return -1;
    for (size_t i = 0; i < n; i++)
        if (rd_str(s, &v->table[i], e)) return -1;
    v->ntable = n;
    if (count(s, 10, &n, e)) return -1;
    if (!(v->storeys = alloc_n(&v->arena, n, sizeof *v->storeys, e))) return -1;
    for (size_t i = 0; i < n; i++) {
        cc_view_storey *st = &v->storeys[i];
        if (rd_str(s, &st->name, e) || rd_f64(s, &st->elevation, e) || rd_str(s, &st->id, e)) return -1;
        v->nstoreys = i + 1;
    }
    if (count(s, 33, &n, e)) return -1;
    if (!(v->grids = alloc_n(&v->arena, n, sizeof *v->grids, e))) return -1;
    for (size_t i = 0; i < n; i++) {
        cc_view_grid *g = &v->grids[i];
        if (rd_str(s, &g->tag, e) || rd_f64(s, &g->start[0], e) || rd_f64(s, &g->start[1], e) ||
            rd_f64(s, &g->end[0], e) || rd_f64(s, &g->end[1], e))
            return -1;
        v->ngrids = i + 1;
    }
    if (count(s, 9, &n, e)) return -1;
    if (!(v->densities = alloc_n(&v->arena, n, sizeof *v->densities, e))) return -1;
    for (size_t i = 0; i < n; i++) {
        if (rd_str(s, &v->densities[i].material, e) || rd_f64(s, &v->densities[i].density, e)) return -1;
        v->ndensities = i + 1;
    }
    if (count(s, 1, &n, e)) return -1;
    if (!(v->warnings = alloc_n(&v->arena, n, sizeof *v->warnings, e))) return -1;
    for (size_t i = 0; i < n; i++) {
        if (rd_str(s, &v->warnings[i], e)) return -1;
        v->nwarnings = i + 1;
    }
    return 0;
}

static int read_elem(Rd *s, cc_viewfile *v, size_t *shape_cap, size_t *elem_cap, Err *e) {
    size_t n;
    if (count(s, 1, &n, e)) return -1;
    size_t cap0 = *shape_cap;
    if (grow((void **)&v->shapes, shape_cap, v->nshapes + n, sizeof *v->shapes, e)) return -1;
    if (v->lazy && *shape_cap != cap0) { /* SIZE3: the shapes' refs beside them */
        void *q = realloc(v->shape_refs, *shape_cap * sizeof *v->shape_refs);
        if (!q) return fail(e, E_OOM, NULL);
        v->shape_refs = q;
    }
    for (size_t i = 0; i < n; i++) {
        cc_viewfile_ref none;
        if (mesh(s, v, &v->shapes[v->nshapes], v->lazy ? &v->shape_refs[v->nshapes] : &none, e)) return -1;
        v->nshapes++;
    }
    if (count(s, 7, &n, e)) return -1;
    if (grow((void **)&v->elements, elem_cap, v->nelements + n, sizeof *v->elements, e)) return -1;
    for (size_t i = 0; i < n; i++) {
        cc_view_element *x = &v->elements[v->nelements];
        memset(x, 0, sizeof *x);
        uint64_t t[4];
        if (rd_str(s, &x->global_id, e) || rd_str(s, &x->name, e)) return -1;
        for (int j = 0; j < 4; j++)
            if (index_of(s, v->ntable, &t[j], e)) return -1;
        x->class_name = v->table[t[0]];
        x->storey = v->table[t[1]];
        x->material = v->table[t[2]];
        x->family = v->table[t[3]];
        x->category = CC_CAT_OTHER;
        for (int c = 0; c < CC_CAT_COUNT; c++)
            if (x->family.n == strlen(FAMILY[c]) && memcmp(x->family.p, FAMILY[c], x->family.n) == 0)
                x->category = (cc_category)c;
        const uint8_t *kind;
        if (take(s, 1, &kind, e)) return -1;
        if (*kind == 0) {
            x->shape = -1;
            if (mesh(s, v, &x->mesh, &x->ref, e)) return -1;
        } else if (*kind == 1) {
            uint64_t k;
            if (varint(s, &k, e)) return -1;
            if (k >= v->nshapes) return fail(e, E_INVALID, "shape index");
            x->shape = (int32_t)k;
            float f[12];
            for (int j = 0; j < 12; j++) {
                const uint8_t *b;
                if (take(s, 4, &b, e)) return -1;
                uint32_t u = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
                if ((u & 0x7f800000u) == 0x7f800000u) return fail(e, E_INVALID, "placement not finite");
                memcpy(&f[j], &u, 4);
            }
            memcpy(x->to_scene.x, f, 12);
            memcpy(x->to_scene.y, f + 3, 12);
            memcpy(x->to_scene.z, f + 6, 12);
            memcpy(x->to_scene.t, f + 9, 12);
        } else {
            return fail(e, E_INVALID, "geometry kind");
        }
        v->nelements++;
    }
    return 0;
}

static int read_raw(const uint8_t *p, size_t n, cc_viewfile *v, Err *e) {
    const uint8_t *file0 = p; /* M1LINKS: PROP's offset in the file */
    if (n < 12 || memcmp(p, MAGIC, 8) != 0) return fail(e, E_NOT, NULL);
    uint32_t version = (uint32_t)p[8] | (uint32_t)p[9] << 8 | (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
    if (version != CC_VIEWFILE_VERSION) {
        e->version = version;
        return fail(e, E_VERSION, NULL);
    }
    Rd r = {p + 12, n - 12};
    int has_header = 0, has_table = 0;
    uint64_t want_elems = 0, want_shapes = 0;
    size_t shape_cap = 0, elem_cap = 0;
    while (r.n) {
        if (r.n < 12) return fail(e, E_TRUNC, NULL);
        const uint8_t *kind = r.p;
        uint64_t len = 0;
        for (int i = 0; i < 8; i++) len |= (uint64_t)r.p[4 + i] << (8 * i);
        r.p += 12, r.n -= 12;
        if (len > MAX_SECTION) return fail(e, E_INVALID, "section too large");
        if (len > r.n) return fail(e, E_TRUNC, NULL);
        Rd s = {r.p, (size_t)len};
        r.p += len, r.n -= (size_t)len;
        if (memcmp(kind, "HEAD", 4) == 0 && !has_header) {
            uint64_t stored;
            if (varint(&s, &want_elems, e) || varint(&s, &want_shapes, e) || varint(&s, &stored, e)) return -1;
            for (int k = 0; k < 3; k++)
                if (rd_f64(&s, &v->origin[k], e)) return -1;
            v->stored_triangles = stored;
            has_header = 1;
        } else if (!has_header) {
            return fail(e, E_INVALID, "no header first");
        } else if (memcmp(kind, "META", 4) == 0 && !has_table) {
            if (read_meta(&s, v, e)) return -1;
            has_table = 1;
        } else if (memcmp(kind, "ELEM", 4) == 0) {
            if (!has_table) return fail(e, E_INVALID, "elements before the string table");
            if (read_elem(&s, v, &shape_cap, &elem_cap, e)) return -1;
        } else if (memcmp(kind, "GEOR", 4) == 0) {
            cc_str g;
            if (rd_str(&s, &g, e)) return -1;
            Arena scratch;
            arena_init(&scratch, 0);
            JrError je;
            int ok = jr_parse(&scratch, g.p, g.n, &je) != NULL;
            arena_free(&scratch);
            if (!ok) return fail(e, E_INVALID, "georef: not JSON");
            v->georef = g;
            v->has_georef = 1;
        } else if (memcmp(kind, "SRCE", 4) == 0 && !v->has_source) { /* LINKF: a link cache's source */
            cc_str g;
            if (rd_str(&s, &g, e)) return -1;
            Arena scratch;
            arena_init(&scratch, 0);
            JrError je;
            int ok = jr_parse(&scratch, g.p, g.n, &je) != NULL;
            arena_free(&scratch);
            if (!ok) return fail(e, E_INVALID, "source: not JSON");
            v->source = g;
            v->has_source = 1;
        } else if (memcmp(kind, "PROP", 4) == 0 && !v->has_psets) { /* M1LINKS: kept on disk (ifc_psets.h) */
            if (s.n < 4) return fail(e, E_INVALID, "psets: no head");
            uint64_t ne = (uint64_t)s.p[0] | (uint64_t)s.p[1] << 8 | (uint64_t)s.p[2] << 16 | (uint64_t)s.p[3] << 24;
            if (ne != want_elems || cc_psets_head(ne) > s.n) return fail(e, E_INVALID, "psets: entries");
            uint64_t last = 0;
            for (uint64_t i = 0; i < ne; i++) {
                const uint8_t *q = s.p + 4 + 4 * i;
                uint64_t end = (uint64_t)q[0] | (uint64_t)q[1] << 8 | (uint64_t)q[2] << 16 | (uint64_t)q[3] << 24;
                if (end < last || cc_psets_head(ne) + end > s.n) return fail(e, E_INVALID, "psets: entries");
                last = end;
            }
            v->has_psets = 1;
            v->psets_off = (uint64_t)(s.p - file0), v->psets_len = s.n;
            s.n = 0;
        } else if (memcmp(kind, "PRPZ", 4) == 0 && !v->has_psets) { /* SIZE2: the packed table, kept on disk */
            if (cc_psets_packed_check(s.p, s.n, s.n, want_elems)) return fail(e, E_INVALID, "psets: entries");
            v->has_psets = 1, v->psets_packed = 1;
            v->psets_off = (uint64_t)(s.p - file0), v->psets_len = s.n;
            s.n = 0;
        } else if (memcmp(kind, "HEAD", 4) == 0 || memcmp(kind, "META", 4) == 0) {
            return fail(e, E_INVALID, "section repeated");
        } else {
            continue; /* a later addition: skipped */
        }
        if (s.n) return fail(e, E_INVALID, "bytes left in a section");
    }
    if (!has_header) return fail(e, E_NOT, NULL);
    if (want_elems != v->nelements || want_shapes != v->nshapes) return fail(e, E_TRUNC, NULL);
    return 0;
}

static void set_msg(cc_error *err, const Err *e) {
    Buf b;
    buf_init(&b);
    switch (e->kind) {
    case E_NOT: buf_puts(&b, "not a .bimbleview file"); break;
    case E_VERSION:
        buf_puts(&b, "a .bimbleview file of format version ");
        buf_u64(&b, e->version);
        buf_puts(&b, "; this Bimble reads version 1 (a newer Bimble made it: update Bimble)");
        break;
    case E_TRUNC: buf_puts(&b, "the .bimbleview file is cut short"); break;
    case E_OOM: buf_puts(&b, "out of memory"); break;
    default:
        buf_puts(&b, "damaged .bimbleview file: ");
        buf_puts(&b, e->why ? e->why : "");
        if (e->why2) buf_puts(&b, e->why2);
    }
    const char *t = buf_cstr(&b);
    size_t n = strlen(t);
    if (n >= sizeof err->msg) n = sizeof err->msg - 1;
    memcpy(err->msg, t, n);
    err->msg[n] = 0;
    err->offset = CC_NO_OFFSET;
    buf_free(&b);
}

int cc_viewfile_append_source(Buf *out, const char *json, size_t n) {
    Buf p;
    buf_init(&p);
    put_str(&p, json, n);
    section(out, "SRCE", &p);
    int bad = p.oom || out->oom;
    buf_free(&p);
    return bad ? -1 : 0;
}

int cc_viewfile_append_hlod(Buf *out, const char *t, size_t n) {
    Buf p;
    buf_init(&p);
    buf_append(&p, t, n);
    section(out, "HLOD", &p);
    int bad = p.oom || out->oom;
    buf_free(&p);
    return bad ? -1 : 0;
}

int cc_viewfile_append_psets_packed(Buf *out, const char *t, size_t n) {
    Buf p;
    buf_init(&p);
    buf_append(&p, t, n);
    section(out, "PRPZ", &p);
    int bad = p.oom || out->oom;
    buf_free(&p);
    return bad ? -1 : 0;
}

int cc_viewfile_append_psets(Buf *out, const char *t, size_t n) {
    Buf p;
    buf_init(&p);
    buf_append(&p, t, n);
    section(out, "PROP", &p);
    int bad = p.oom || out->oom;
    buf_free(&p);
    return bad ? -1 : 0;
}

int cc_viewfile_read(const uint8_t *p, size_t n, cc_viewfile *out, cc_error *err) {
    memset(out, 0, sizeof *out);
    arena_init(&out->arena, 0);
    Err e = {0, 0, NULL, NULL};
    int rc;
    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b) {
        size_t limit = n > SIZE_MAX / MAX_EXPANSION ? SIZE_MAX : n * MAX_EXPANSION;
        if (limit < ((size_t)1 << 26)) limit = (size_t)1 << 26;
        Buf raw;
        buf_init(&raw);
        const char *why = NULL;
        if (cc_gunzip(p, n, limit, &raw, &why)) {
            buf_free(&raw);
            e.kind = E_INVALID;
            e.why = why ? why : "gzip";
            set_msg(err, &e);
            return -1;
        }
        /* Strings point into the stream: keep it in the arena. */
        uint8_t *copy = arena_dup(&out->arena, raw.p, raw.len);
        size_t len = raw.len;
        buf_free(&raw);
        rc = copy ? read_raw(copy, len, out, &e) : fail(&e, E_OOM, NULL);
        out->has_psets = 0; /* M1LINKS: offsets in the inflated stream, not the file: not read lazily */
        out->has_hlod = 0;  /* LNK3D: the same */
    } else {
        /* M1LINKS: a last section PROP stays out of the copy (read lazily from the file, ifc_psets.h):
         * the sections' heads walked, the copy cut where it starts. LNK3D: so does a section HLOD (a link
         * cache's baked proxies, BPRX; served from the file and checked then: hlod.h
         * cc_hlod_bprx_check), when only HLOD and PROP sections follow it. */
        size_t cut = n, at = 12, pset = 0, lazy = 0, hlod = 0;
        while (n >= 12 && at + 12 <= n) {
            uint64_t len = 0;
            for (int i = 0; i < 8; i++) len |= (uint64_t)p[at + 4 + i] << (8 * i);
            if (len > n - at - 12) break;
            int is_prop = !memcmp(p + at, "PROP", 4) || !memcmp(p + at, "PRPZ", 4), is_hlod = !memcmp(p + at, "HLOD", 4); /* SIZE2: PRPZ */
            if (is_prop && at + 12 + len == n) pset = at;
            if (is_hlod && !hlod) hlod = at;
            if ((is_prop || is_hlod) && !lazy) lazy = at;
            else if (!is_prop && !is_hlod) lazy = 0, hlod = 0;
            at += 12 + (size_t)len;
        }
        if (at == n && lazy) cut = lazy;
        else if (pset && at == n) cut = pset;
        if (at != n || !lazy) hlod = 0;
        uint8_t *copy = arena_dup(&out->arena, p, cut);
        rc = copy ? read_raw(copy, cut, out, &e) : fail(&e, E_OOM, NULL);
        if (!rc && hlod) { /* LNK3D: where it is in the file; its bytes checked when served */
            uint64_t len = 0;
            for (int i = 0; i < 8; i++) len |= (uint64_t)p[hlod + 4 + i] << (8 * i);
            out->has_hlod = 1, out->hlod_off = hlod + 12, out->hlod_len = len;
        }
        if (!rc && pset && cut < n && !memcmp(p + pset, "PRPZ", 4)) { /* SIZE2: the packed head checked */
            const uint8_t *q = p + pset + 12;
            size_t ln = n - pset - 12;
            if (cc_psets_packed_check(q, ln, ln, out->nelements)) rc = fail(&e, E_INVALID, "psets: entries");
            else out->has_psets = 1, out->psets_packed = 1, out->psets_off = pset + 12, out->psets_len = ln;
        } else if (!rc && pset && cut < n) { /* the head checked as read_raw does */
            const uint8_t *q = p + pset + 12;
            size_t ln = n - pset - 12;
            uint64_t ne = ln >= 4 ? ((uint64_t)q[0] | (uint64_t)q[1] << 8 | (uint64_t)q[2] << 16 | (uint64_t)q[3] << 24) : 0;
            int ok = ln >= 4 && ne == out->nelements && cc_psets_head(ne) <= ln;
            uint64_t last = 0;
            for (uint64_t i = 0; ok && i < ne; i++) {
                const uint8_t *x = q + 4 + 4 * i;
                uint64_t end = (uint64_t)x[0] | (uint64_t)x[1] << 8 | (uint64_t)x[2] << 16 | (uint64_t)x[3] << 24;
                ok = end >= last && cc_psets_head(ne) + end <= ln;
                last = end;
            }
            if (!ok) rc = fail(&e, E_INVALID, "psets: entries");
            else out->has_psets = 1, out->psets_off = pset + 12, out->psets_len = ln;
        }
    }
    if (rc) set_msg(err, &e);
    return rc;
}

/* ---- SIZE3: a view file read in place (viewfile.h cc_viewfile_read_mapped) ---- */
int cc_viewfile_read_mapped(const uint8_t *p, size_t n, cc_viewfile *out, cc_error *err) {
    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b) return cc_viewfile_read(p, n, out, err); /* gzipped: inflated, as before */
    memset(out, 0, sizeof *out);
    arena_init(&out->arena, 0);
    Arena scratch;
    arena_init(&scratch, 0);
    out->lazy = 1, out->base = p, out->lazy_scratch = &scratch;
    /* the sections HLOD and PROP after the others stay out of the read (cc_viewfile_read's cut) */
    size_t cut = n, at = 12, lazy = 0, pset = 0, hlod = 0;
    while (n >= 12 && at + 12 <= n) {
        uint64_t len = 0;
        for (int i = 0; i < 8; i++) len |= (uint64_t)p[at + 4 + i] << (8 * i);
        if (len > n - at - 12) break;
        int is_prop = !memcmp(p + at, "PROP", 4) || !memcmp(p + at, "PRPZ", 4), is_hlod = !memcmp(p + at, "HLOD", 4);
        if (is_prop && at + 12 + len == n) pset = at;
        if (is_hlod && !hlod) hlod = at;
        if ((is_prop || is_hlod) && !lazy) lazy = at;
        else if (!is_prop && !is_hlod) lazy = 0, hlod = 0;
        at += 12 + (size_t)len;
    }
    if (at == n && lazy) cut = lazy;
    else if (pset && at == n) cut = pset;
    if (at != n || !lazy) hlod = 0, pset = at == n ? pset : 0;
    Err e = {0, 0, NULL, NULL};
    int rc = read_raw(p, cut, out, &e);
    out->lazy_scratch = NULL;
    arena_free(&scratch);
    if (!rc && hlod) {
        uint64_t len = 0;
        for (int i = 0; i < 8; i++) len |= (uint64_t)p[hlod + 4 + i] << (8 * i);
        out->has_hlod = 1, out->hlod_off = hlod + 12, out->hlod_len = len;
    }
    if (!rc && pset && cut < n) {
        const uint8_t *q = p + pset + 12;
        size_t ln = n - pset - 12;
        if (!memcmp(p + pset, "PRPZ", 4)) {
            if (cc_psets_packed_check(q, ln, ln, out->nelements)) rc = fail(&e, E_INVALID, "psets: entries");
            else out->has_psets = 1, out->psets_packed = 1, out->psets_off = pset + 12, out->psets_len = ln;
        } else {
            uint64_t ne = ln >= 4 ? ((uint64_t)q[0] | (uint64_t)q[1] << 8 | (uint64_t)q[2] << 16 | (uint64_t)q[3] << 24) : 0;
            int ok = ln >= 4 && ne == out->nelements && cc_psets_head(ne) <= ln;
            uint64_t last = 0;
            for (uint64_t i = 0; ok && i < ne; i++) {
                const uint8_t *x = q + 4 + 4 * i;
                uint64_t end = (uint64_t)x[0] | (uint64_t)x[1] << 8 | (uint64_t)x[2] << 16 | (uint64_t)x[3] << 24;
                ok = end >= last && cc_psets_head(ne) + end <= ln;
                last = end;
            }
            if (!ok) rc = fail(&e, E_INVALID, "psets: entries");
            else out->has_psets = 1, out->psets_off = pset + 12, out->psets_len = ln;
        }
    }
    if (rc) set_msg(err, &e);
    return rc;
}

/* Decodes one mesh at `r` of the mapped bytes into `a`; its counts must stay those read. */
static int decode_ref(const cc_viewfile *v, const cc_viewfile_ref *r, Arena *a, cc_mesh *m) {
    cc_mesh d;
    const char *why;
    if (cc_meshcodec_decode(v->base + r->at, r->len, a, &d, &why) != CC_MC_OK) return -1;
    if (d.nv != m->nv || d.ni != m->ni || d.ne != m->ne || d.nn != m->nn) return -1;
    *m = d;
    return 0;
}

int cc_viewfile_materialize(cc_viewfile *v, Arena *a, uint64_t *bytes) {
    if (!v->lazy) return 0;
    uint64_t b = 0;
    for (size_t i = 0; i < v->nshapes; i++) {
        if (decode_ref(v, &v->shape_refs[i], a, &v->shapes[i])) return cc_viewfile_dematerialize(v), -1;
        b += 12ull * v->shapes[i].nv + 4ull * v->shapes[i].ni + 32ull * v->shapes[i].ne;
    }
    for (size_t i = 0; i < v->nelements; i++) {
        cc_view_element *x = &v->elements[i];
        if (x->shape >= 0) continue;
        if (decode_ref(v, &x->ref, a, &x->mesh)) return cc_viewfile_dematerialize(v), -1;
        b += 12ull * x->mesh.nv + 4ull * x->mesh.ni + 32ull * x->mesh.ne;
    }
    if (bytes) *bytes = b;
    return 0;
}

void cc_viewfile_dematerialize(cc_viewfile *v) {
    if (!v->lazy) return;
    for (size_t i = 0; i < v->nshapes; i++) v->shapes[i].pos = NULL, v->shapes[i].idx = NULL, v->shapes[i].edges = NULL, v->shapes[i].normals = NULL;
    for (size_t i = 0; i < v->nelements; i++) {
        cc_mesh *m = &v->elements[i].mesh;
        m->pos = NULL, m->idx = NULL, m->edges = NULL, m->normals = NULL;
    }
}

int cc_viewfile_block(const cc_viewfile *v, size_t b, const uint8_t **p, size_t *n) {
    if (!v->lazy) return -1;
    const cc_viewfile_ref *r = b < v->nshapes ? &v->shape_refs[b] : b - v->nshapes < v->nelements ? &v->elements[b - v->nshapes].ref : NULL;
    if (!r || (b >= v->nshapes && v->elements[b - v->nshapes].shape >= 0)) return -1;
    *p = v->base + r->at, *n = r->len;
    return 0;
}

void cc_viewfile_free(cc_viewfile *v) {
    free(v->shape_refs);
    free(v->shapes);
    free(v->elements);
    arena_free(&v->arena);
    memset(v, 0, sizeof *v);
}

const cc_mesh *cc_viewfile_local_mesh(const cc_viewfile *v, size_t k) {
    const cc_view_element *e = &v->elements[k];
    return e->shape >= 0 ? &v->shapes[e->shape] : &e->mesh;
}
