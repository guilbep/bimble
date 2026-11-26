/* What the paper drawing draws (ticket LNK1): see paper_scene.h. */
#include "paper_scene.h"
#include <stdlib.h>
#include <string.h>
#include "georef.h"
#include "guid.h"
#include "json_r.h"
#include "links.h"
#include "session.h"
#include "view_effective.h"
#include "view_io.h"
#include "views.h"

static int str_eq(cc_str a, cc_str b) { return a.n == b.n && (!a.n || !memcmp(a.p, b.p, a.n)); }
static int cmp_storey(const void *a, const void *b) { return memcmp(((const cc_storey *)a)->id.c, ((const cc_storey *)b)->id.c, 22); }
static int cmp_material(const void *a, const void *b) { return memcmp(((const cc_material *)a)->id.c, ((const cc_material *)b)->id.c, 22); }

static cc_gid derived(const char *pre, cc_str text) {
    char seed[256];
    size_t n = strlen(pre), k = text.n < sizeof seed - n ? text.n : sizeof seed - n;
    memcpy(seed, pre, n);
    if (k) memcpy(seed + n, text.p, k);
    cc_guid g = cc_guid_derived(seed, n + k);
    cc_gid out;
    memcpy(out.c, g.c, 22);
    return out;
}

cc_gid cc_paper_src_storey_key(cc_str key) {
    cc_gid g;
    if (key.n == 22) memcpy(g.c, key.p, 22);
    else g = derived("bimble paper storey ", key);
    return g;
}

/* The model being made: growable arrays in `a`. */
typedef struct {
    cc_model *m;
    Arena *a;
    size_t capmat, capel;
    size_t nhost_storeys; /* the storeys a link's element may take, by name */
} B;

static int grow(Arena *a, void **p, size_t *cap, size_t need, size_t size) {
    if (need <= *cap) return 0;
    size_t c = *cap ? *cap : 16;
    while (c < need) c *= 2;
    void *q = arena_alloc(a, c * size, sizeof(double));
    if (!q) return -1;
    if (*p && *cap) memcpy(q, *p, *cap * size);
    *p = q, *cap = c;
    return 0;
}

/* The material named `name` (made when new: an id derived from the name). */
static int material_of(B *b, cc_str name, cc_category cat, cc_gid *out) {
    cc_model *m = b->m;
    for (size_t i = 0; i < m->nmaterials; i++)
        if (str_eq(m->materials[i].name, name)) return *out = m->materials[i].id, 0;
    void *p = m->materials;
    if (grow(b->a, &p, &b->capmat, m->nmaterials + 1, sizeof *m->materials)) return -1;
    m->materials = p;
    cc_material *mt = &m->materials[m->nmaterials++];
    memset(mt, 0, sizeof *mt);
    mt->id = derived("bimble paper material ", name);
    mt->name = name;
    mt->category = cat;
    *out = mt->id;
    return 0;
}

static cc_gid storey_named(const B *b, cc_str name) {
    cc_gid z;
    memset(z.c, 0, 22);
    for (size_t i = 0; i < b->nhost_storeys; i++)
        if (str_eq(b->m->storeys[i].name, name)) return b->m->storeys[i].id;
    return z;
}

/* One element (model_index = its index), its texts as a read-only scene read them. */
static int element_add(B *b, cc_str gid, cc_str name, cc_str storey, cc_str material, cc_category cat) {
    cc_model *m = b->m;
    void *p = m->elements;
    if (grow(b->a, &p, &b->capel, m->nelements + 1, sizeof *m->elements)) return -1;
    m->elements = p;
    cc_element *e = &m->elements[m->nelements];
    memset(e, 0, sizeof *e);
    if (gid.n == 22) memcpy(e->id.c, gid.p, 22);
    else e->id = derived("bimble paper element ", gid);
    e->cls = CC_IFC_PROXY;
    e->name = name;
    e->storey = storey_named(b, storey);
    e->generator.kind = CC_GEN_NONE;
    e->placement.z_axis[2] = 1, e->placement.x_axis[0] = 1;
    if (material_of(b, material, cat, &e->material)) return -1;
    m->nelements++;
    return 0;
}

/* The elements of a read-only scene or a linked model, in model_index order. */
static int elements_of(B *b, int kind, const cc_ifc_scene *ifc, const cc_viewfile *vf, const cc_model *lm,
                       const cc_scene *sc) {
    static const cc_str none = {"", 0};
    if (kind == CC_LINK_IFC) {
        for (size_t i = 0; i < sc->nelements; i++) {
            const cc_ifc_info *f = &ifc->info[i];
            if (element_add(b, f->global_id, f->name, f->storey, f->material, sc->elements[i].category)) return -1;
        }
    } else if (kind == CC_LINK_VIEW) {
        for (size_t i = 0; i < vf->nelements; i++) {
            const cc_view_element *f = &vf->elements[i];
            if (element_add(b, f->global_id, f->name, f->storey, f->material, f->category)) return -1;
        }
    } else {
        for (size_t i = 0; i < lm->nelements; i++) {
            const cc_element *f = &lm->elements[i];
            const cc_storey *st = cc_model_storey(lm, &f->storey);
            const cc_material *mt = cc_model_material(lm, &f->material);
            cc_str id = {f->id.c, 22};
            if (element_add(b, id, f->name, st ? st->name : none, mt ? mt->name : none,
                            mt ? mt->category : CC_CAT_OTHER))
                return -1;
        }
    }
    return 0;
}

/* The made model of a read-only scene: storeys, grids, georeference, then its elements. */
static int model_of_scene(cc_session *s, B *b) {
    cc_model *m = b->m;
    Arena *a = b->a;
    cc_level *lv;
    size_t nl;
    if (cc_sv_scene_levels(s->view, s->view == CC_VIEW_IFC ? (const void *)s->ifc : (const void *)s->vf, a, &lv, &nl))
        return -1;
    m->storeys = arena_alloc(a, (nl ? nl : 1) * sizeof *m->storeys, sizeof(double));
    if (!m->storeys) return -1;
    for (size_t i = 0; i < nl; i++) {
        cc_storey *st = &m->storeys[i];
        memset(st, 0, sizeof *st);
        st->id = cc_paper_src_storey_key(lv[i].key);
        st->name = lv[i].name;
        st->elevation = lv[i].elevation;
    }
    m->nstoreys = nl;
    b->nhost_storeys = nl;
    size_t ng = s->view == CC_VIEW_IFC ? s->ifc->ngrids : s->vf->ngrids;
    if (ng) {
        cc_grid *g = arena_alloc(a, sizeof *g, sizeof(double));
        cc_grid_axis *ax = arena_alloc(a, ng * sizeof *ax, sizeof(double));
        if (!g || !ax) return -1;
        memset(g, 0, sizeof *g);
        g->id = derived("bimble paper grid", (cc_str){"", 0});
        for (size_t i = 0; i < ng; i++) {
            memset(&ax[i], 0, sizeof ax[i]);
            ax[i].id = derived("bimble paper axis ", (cc_str){(const char *)&i, sizeof i});
            if (s->view == CC_VIEW_IFC) {
                const cc_ifc_grid *x = &s->ifc->grids[i];
                ax[i].tag = x->tag;
                memcpy(ax[i].start, x->start_mm, sizeof ax[i].start), memcpy(ax[i].end, x->end_mm, sizeof ax[i].end);
            } else {
                const cc_view_grid *x = &s->vf->grids[i];
                ax[i].tag = x->tag;
                memcpy(ax[i].start, x->start, sizeof ax[i].start), memcpy(ax[i].end, x->end, sizeof ax[i].end);
            }
        }
        g->axes = ax, g->naxes = ng;
        m->grids = g, m->ngrids = 1;
    }
    if (s->view == CC_VIEW_IFC) m->georef = s->ifc->georef;
    else if (s->vf->has_georef) { /* GEOR's JSON text; a text that does not decode keeps the default */
        JrError je;
        const JrVal *v = jr_parse(a, s->vf->georef.p, s->vf->georef.n, &je);
        cc_vd vd;
        cc_vd_init(&vd, a);
        cc_georef g;
        memset(&g, 0, sizeof g);
        if (v && cc_vd_georef(&vd, v, NULL, &g) == 0 && !vd.failed) m->georef = g;
        cc_vd_free(&vd);
    }
    qsort(m->storeys, m->nstoreys, sizeof *m->storeys, cmp_storey);
    b->nhost_storeys = m->nstoreys;
    return elements_of(b, s->view == CC_VIEW_IFC ? CC_LINK_IFC : CC_LINK_VIEW, s->ifc, s->vf, NULL,
                       s->view == CC_VIEW_IFC ? &s->ifc->scene : &s->vscene);
}

static int name_in(const cc_str *v, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (v[i].n == strlen(name) && !memcmp(v[i].p, name, v[i].n)) return 1;
    return 0;
}

/* A link drawn on this paper: shown in the session, not hidden in the view, loaded. */
static int link_drawn(const cc_link *k, const cc_view *view) {
    if (!k->visible || !k->kind) return 0;
    return !(view && name_in(view->visibility.hidden_links.v, view->visibility.hidden_links.n, k->name));
}

/* Appends link `k`'s elements to the merged scene, moved by `d`, model_index from `base`. */
static int scene_add_link(cc_scene *out, size_t *cap, const cc_scene *ls, const float d[3], size_t base) {
    for (size_t k = 0; k < ls->nelements; k++) {
        cc_mesh w, m;
        if (cc_scene_world_mesh(ls, k, &out->arena, &w)) return -1;
        memset(&m, 0, sizeof m);
        m.nv = w.nv, m.ni = w.ni, m.ne = w.ne, m.nn = w.nn;
        m.pos = arena_alloc(&out->arena, ((size_t)w.nv + 1) * sizeof *m.pos, 4);
        m.idx = arena_alloc(&out->arena, ((size_t)w.ni + 1) * sizeof *m.idx, 4);
        m.edges = arena_alloc(&out->arena, ((size_t)w.ne + 1) * sizeof *m.edges, 4);
        m.normals = arena_alloc(&out->arena, ((size_t)w.nn + 1) * sizeof *m.normals, 4);
        if (!m.pos || !m.idx || !m.edges || !m.normals) return -1;
        for (uint32_t i = 0; i < w.nv; i++)
            for (int j = 0; j < 3; j++) m.pos[i][j] = w.pos[i][j] + d[j];
        if (w.ni) memcpy(m.idx, w.idx, (size_t)w.ni * sizeof *m.idx);
        for (uint32_t i = 0; i < w.ne; i++)
            for (int j = 0; j < 6; j++) m.edges[i][j] = w.edges[i][j] + d[j % 3];
        if (w.nn) memcpy(m.normals, w.normals, (size_t)w.nn * sizeof *m.normals);
        void *p = out->elements;
        if (grow(&out->arena, &p, cap, out->nelements + 1, sizeof *out->elements)) return -1;
        out->elements = p;
        cc_scene_element *e = &out->elements[out->nelements++];
        *e = ls->elements[k];
        e->model_index = base + ls->elements[k].model_index;
        e->shape = -1;
        e->mesh = m;
    }
    return 0;
}

int cc_paper_src_get(cc_session *s, const cc_scene *sc, const cc_view *view, cc_paper_src *src, Buf *err) {
    memset(src, 0, sizeof *src);
    if (!sc || !(s->open || s->view)) return buf_puts(err, "no model open"), -1;
    size_t nl = 0;
    for (size_t i = 0; i < s->nlinks; i++) nl += link_drawn(&s->links[i], view);
    if (s->open && !nl) { /* the model as it is: the D4 goldens' path */
        src->scene = sc, src->model = &s->model;
        return 0;
    }
    cc_model *m = &src->own_model;
    memset(m, 0, sizeof *m);
    arena_init(&m->arena, 0);
    src->made_model = 1;
    src->model = m;
    src->scene = sc;
    B b = {m, &m->arena, 0, 0, 0};
    if (s->open) { /* the host model's arrays, its elements first (their model_index) */
        const cc_model *h = &s->model;
        *m = *h; /* shallow: strings and arrays stay the host's */
        arena_init(&m->arena, 0);
        m->cap_elements = 0;
        m->elements = NULL, m->nelements = 0, m->materials = NULL, m->nmaterials = 0;
        if (grow(&m->arena, (void **)&m->elements, &b.capel, h->nelements + 1, sizeof *m->elements) ||
            grow(&m->arena, (void **)&m->materials, &b.capmat, h->nmaterials + 1, sizeof *m->materials))
            goto oom;
        if (h->nelements) memcpy(m->elements, h->elements, h->nelements * sizeof *m->elements);
        if (h->nmaterials) memcpy(m->materials, h->materials, h->nmaterials * sizeof *m->materials);
        m->nelements = h->nelements, m->nmaterials = h->nmaterials;
        b.nhost_storeys = m->nstoreys;
    } else if (model_of_scene(s, &b)) goto oom;
    size_t nhost = m->nelements;
    if (nl) {
        cc_scene *o = &src->own_scene;
        memset(o, 0, sizeof *o);
        arena_init(&o->arena, 0);
        src->made_scene = 1;
        memcpy(o->origin, sc->origin, sizeof o->origin);
        o->shapes = sc->shapes, o->nshapes = sc->nshapes; /* borrowed: the host elements' shared shapes */
        size_t cap = 0;
        if (grow(&o->arena, (void **)&o->elements, &cap, sc->nelements + 1, sizeof *o->elements)) goto oom;
        if (sc->nelements) memcpy(o->elements, sc->elements, sc->nelements * sizeof *o->elements);
        o->nelements = sc->nelements;
        for (size_t i = 0; i < s->nlinks; i++) {
            const cc_link *k = &s->links[i];
            if (!link_drawn(k, view)) continue;
            const cc_scene *ls = cc_link_scene(k);
            float d[3];
            for (int j = 0; j < 3; j++) d[j] = (float)(ls->origin[j] - sc->origin[j]); /* lib.rs:1499 offset */
            size_t base = m->nelements;
            if (elements_of(&b, k->kind, k->ifc, k->vf, k->model, ls) || scene_add_link(o, &cap, ls, d, base)) goto oom;
        }
        src->scene = o;
    }
    qsort(m->materials, m->nmaterials, sizeof *m->materials, cmp_material);
    /* model_index reaches every element; the id lookups (annotation anchors) see the host's, which
     * stay sorted in front. */
    if (s->open) m->nelements = nhost;
    return 0;
oom:
    cc_paper_src_free(src);
    buf_puts(err, "out of memory");
    return -1;
}

void cc_paper_src_free(cc_paper_src *src) {
    if (src->made_scene) arena_free(&src->own_scene.arena);
    if (src->made_model) arena_free(&src->own_model.arena);
    memset(src, 0, sizeof *src);
}

int cc_paper_src_views(cc_session *s, cc_view *v, size_t n, Arena *a) {
    if (s->open) return 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i].kind != CC_VIEW_PLAN || v[i].storey.n == 22) continue;
        cc_gid g = cc_paper_src_storey_key(v[i].storey);
        char *p = arena_alloc(a, 22, 1);
        if (!p) return -1;
        memcpy(p, g.c, 22);
        v[i].storey.p = p, v[i].storey.n = 22;
    }
    return 0;
}
