/* What the paper drawing draws (ticket LNK1): see paper_scene.h. */
#include "paper_scene.h"
#include "viewfile.h"
#include "bvh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "georef.h"
#include "guid.h"
#include "json_r.h"
#include "links.h"
#include "visibility.h" /* MEAS3: cc_vis_far */
#include "m1links.h"
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

/* M1LINKS (m1links.h): the levels from links become storeys of the made model (id the level's key,
 * its name, its placed elevation), so that a plan "from links" finds its storey and a link's
 * element takes the storey of its name. */
static int link_levels_add(cc_session *s, B *b) {
    cc_model *m = b->m;
    cc_level *own = arena_alloc(b->a, (m->nstoreys ? m->nstoreys : 1) * sizeof *own, sizeof(double)), *ll;
    if (!own) return -1;
    for (size_t i = 0; i < m->nstoreys; i++) own[i].key.p = m->storeys[i].id.c, own[i].key.n = 22, own[i].name = m->storeys[i].name,
                                             own[i].elevation = m->storeys[i].elevation;
    size_t nll;
    if (cc_link_levels(s, own, m->nstoreys, b->a, &ll, &nll, NULL)) return -1;
    if (!nll) return 0;
    cc_storey *st = arena_alloc(b->a, (m->nstoreys + nll) * sizeof *st, sizeof(double));
    if (!st) return -1;
    if (m->nstoreys) memcpy(st, m->storeys, m->nstoreys * sizeof *st);
    for (size_t i = 0; i < nll; i++) {
        cc_storey *x = &st[m->nstoreys + i];
        memset(x, 0, sizeof *x);
        memcpy(x->id.c, ll[i].key.p, 22);
        x->name = ll[i].name;
        x->elevation = ll[i].elevation;
    }
    m->storeys = st, m->nstoreys += nll;
    qsort(m->storeys, m->nstoreys, sizeof *m->storeys, cmp_storey);
    b->nhost_storeys = m->nstoreys;
    return 0;
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

/* SIZE3: link k's placed (expanded) triangles, from its counts (nothing decoded): each copy of a shape counted. */
static uint64_t link_expanded_tris(const cc_link *k) {
    uint64_t t = 0;
    if (k->vf && k->vf->lazy) {
        for (size_t e = 0; e < k->vf->nelements; e++) {
            const cc_view_element *x = &k->vf->elements[e];
            t += (x->shape >= 0 && (size_t)x->shape < k->vf->nshapes ? k->vf->shapes[x->shape].ni : x->mesh.ni) / 3;
        }
        return t;
    }
    const cc_scene *sc = cc_link_rows(k);
    for (size_t e = 0; e < sc->nelements; e++) t += cc_scene_local_mesh(sc, e)->ni / 3;
    return t;
}
#define PAPER_BYTES_PER_TRI 96 /* SIZE3: a placed copy in the merged scene and the cut's working memory (measured: 6.5 GB for 110 M) */

/* SIZE3: link lk drawn simplified: each element as its box (its own frame's box, placed as the link), no mesh
 * decoded (a link read in place: the boxes its mapped read kept), moved by `d`, model_index from `base`. */
static int scene_add_link_boxes(cc_scene *out, size_t *cap, const cc_link *lk, const cc_view *view, const float d[3], size_t base) {
    const cc_scene *ls = cc_link_rows(lk);
    double c = lk->pwant ? lk->pc : 1.0, sn = lk->pwant ? lk->ps : 0.0;
    static const uint32_t TRI[36] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    static const uint8_t EDGE[24] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 2, 1, 3, 4, 6, 5, 7, 0, 4, 1, 5, 2, 6, 3, 7};
    for (size_t k = 0; k < ls->nelements; k++) {
        if (view && cc_link_hidden_by_rule(lk, k, &view->visibility)) continue;
        float lo[3] = {1.0f / 0.0f, 1.0f / 0.0f, 1.0f / 0.0f}, hi[3] = {-1.0f / 0.0f, -1.0f / 0.0f, -1.0f / 0.0f};
        if (lk->vf && lk->vf->lazy && k < lk->vf->nelements) {
            const cc_view_element *x = &lk->vf->elements[k];
            const cc_viewfile_ref *r = x->shape >= 0 ? &lk->vf->shape_refs[x->shape] : &x->ref;
            if (!(r->lo[0] <= r->hi[0])) continue;
            for (int q = 0; q < 8; q++) {
                float p[3] = {q & 1 ? r->hi[0] : r->lo[0], q & 2 ? r->hi[1] : r->lo[1], q & 4 ? r->hi[2] : r->lo[2]}, w[3];
                for (int j = 0; j < 3; j++) w[j] = x->shape >= 0 ? x->to_scene.x[j] * p[0] + x->to_scene.y[j] * p[1] + x->to_scene.z[j] * p[2] + x->to_scene.t[j] : p[j];
                for (int j = 0; j < 3; j++) lo[j] = w[j] < lo[j] ? w[j] : lo[j], hi[j] = w[j] > hi[j] ? w[j] : hi[j];
            }
        } else {
            cc_aabb b = cc_scene_element_bounds(ls, k);
            if (cc_aabb_is_empty(&b)) continue;
            memcpy(lo, b.min, sizeof lo), memcpy(hi, b.max, sizeof hi);
        }
        cc_mesh m;
        memset(&m, 0, sizeof m);
        m.nv = 8, m.ni = 36, m.ne = 12;
        m.pos = arena_alloc(&out->arena, 8 * sizeof *m.pos, 4);
        m.idx = arena_alloc(&out->arena, 36 * sizeof *m.idx, 4);
        m.edges = arena_alloc(&out->arena, 12 * sizeof *m.edges, 4);
        if (!m.pos || !m.idx || !m.edges) return -1;
        for (int q = 0; q < 8; q++) {
            double x = q & 1 ? hi[0] : lo[0], y = q & 2 ? hi[1] : lo[1], z = q & 4 ? hi[2] : lo[2];
            m.pos[q][0] = (float)(c * x - sn * y) + d[0], m.pos[q][1] = (float)(sn * x + c * y) + d[1], m.pos[q][2] = (float)z + d[2];
        }
        memcpy(m.idx, TRI, sizeof TRI);
        for (int q = 0; q < 12; q++)
            for (int j = 0; j < 3; j++) m.edges[q][j] = m.pos[EDGE[2 * q]][j], m.edges[q][3 + j] = m.pos[EDGE[2 * q + 1]][j];
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

/* Appends link `k`'s elements to the merged scene, moved by `d`, model_index from `base`. */
static int scene_add_link(cc_scene *out, size_t *cap, const cc_link *lk, const cc_view *view, const cc_scene *ls,
                          const float d[3], size_t base) {
    Arena tmp; /* SIZE3: each placed copy made in a scratch, then copied moved (it was made twice in the scene's arena) */
    arena_init(&tmp, 0);
    for (size_t k = 0; k < ls->nelements; k++) {
        if (view && cc_link_hidden_by_rule(lk, k, &view->visibility)) continue; /* M1LINKS: a link storey hidden */
        cc_mesh w, m;
        arena_trim(&tmp, (size_t)1 << 22);
        if (cc_scene_world_mesh(ls, k, &tmp, &w)) return arena_free(&tmp), -1;
        memset(&m, 0, sizeof m);
        m.nv = w.nv, m.ni = w.ni, m.ne = w.ne, m.nn = w.nn;
        m.pos = arena_alloc(&out->arena, ((size_t)w.nv + 1) * sizeof *m.pos, 4);
        m.idx = arena_alloc(&out->arena, ((size_t)w.ni + 1) * sizeof *m.idx, 4);
        m.edges = arena_alloc(&out->arena, ((size_t)w.ne + 1) * sizeof *m.edges, 4);
        m.normals = arena_alloc(&out->arena, ((size_t)w.nn + 1) * sizeof *m.normals, 4);
        if (!m.pos || !m.idx || !m.edges || !m.normals) return arena_free(&tmp), -1;
        for (uint32_t i = 0; i < w.nv; i++)
            for (int j = 0; j < 3; j++) m.pos[i][j] = w.pos[i][j] + d[j];
        if (w.ni) memcpy(m.idx, w.idx, (size_t)w.ni * sizeof *m.idx);
        for (uint32_t i = 0; i < w.ne; i++)
            for (int j = 0; j < 6; j++) m.edges[i][j] = w.edges[i][j] + d[j % 3];
        if (w.nn) memcpy(m.normals, w.normals, (size_t)w.nn * sizeof *m.normals);
        void *p = out->elements;
        if (grow(&out->arena, &p, cap, out->nelements + 1, sizeof *out->elements)) return arena_free(&tmp), -1;
        out->elements = p;
        cc_scene_element *e = &out->elements[out->nelements++];
        *e = ls->elements[k];
        e->model_index = base + ls->elements[k].model_index;
        e->shape = -1;
        e->mesh = m;
    }
    arena_free(&tmp);
    return 0;
}

int cc_paper_src_get(cc_session *s, const cc_scene *sc, const cc_view *view, cc_paper_src *src, Buf *err) {
    memset(src, 0, sizeof *src);
    s->paper_notice[0] = 0; /* SIZE3 */
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
    if (nl && link_levels_add(s, &b)) goto oom; /* M1LINKS: the levels from links are storeys of the drawing */
    size_t nhost = m->nelements;
    if (nl) {
        cc_scene *o = &src->own_scene;
        memset(o, 0, sizeof *o);
        arena_init(&o->arena, 0);
        src->made_scene = 1;
        memcpy(o->origin, sc->origin, sizeof o->origin);
        if (!sc->nelements) /* MEAS3 (ADR 0087): links alone, the first drawn one far away: the drawing in its frame */
            for (size_t i = 0; i < s->nlinks; i++) {
                cc_link *k = &s->links[i];
                if (!link_drawn(k, view)) continue;
                const cc_scene *ls = cc_link_rows(k);
                double O[3];
                memcpy(O, ls->origin, sizeof O);
                if (k->pwant || k->placed) O[0] = k->pc * ls->origin[0] - k->ps * ls->origin[1] + k->pt[0], O[1] = k->ps * ls->origin[0] + k->pc * ls->origin[1] + k->pt[1], O[2] = ls->origin[2] + k->pt[2];
                double d[3] = {O[0] - sc->origin[0], O[1] - sc->origin[1], O[2] - sc->origin[2]};
                if (cc_vis_far(d)) memcpy(o->origin, O, sizeof O);
                break;
            }
        o->shapes = sc->shapes, o->nshapes = sc->nshapes; /* borrowed: the host elements' shared shapes */
        size_t cap = 0;
        if (grow(&o->arena, (void **)&o->elements, &cap, sc->nelements + 1, sizeof *o->elements)) goto oom;
        if (sc->nelements) memcpy(o->elements, sc->elements, sc->nelements * sizeof *o->elements);
        o->nelements = sc->nelements;
        src->link_first = arena_alloc(&m->arena, nl * sizeof *src->link_first, _Alignof(size_t)); /* DXF1 */
        src->link_name = arena_alloc(&m->arena, nl * sizeof *src->link_name, _Alignof(const char *));
        if (!src->link_first || !src->link_name) goto oom;
        /* SIZE3: the placed copies are expanded for the cut (110 M triangles, 6.5 GB on a folder of 17): past half the
         * memory available, the links that do not fit are drawn simplified (each element its box), said in a notice */
        uint64_t avail = s->pf->mem_available ? s->pf->mem_available(s->pf->ctx) : 0, room = avail ? avail / 2 : (uint64_t)1 << 30, used = 0;
        size_t simplified = 0;
        for (size_t i = 0; i < s->nlinks; i++) {
            cc_link *k = &s->links[i];
            if (!link_drawn(k, view)) continue;
            src->link_first[src->nlinks] = m->nelements, src->link_name[src->nlinks++] = k->name; /* DXF1 */
            uint64_t want = link_expanded_tris(k) * PAPER_BYTES_PER_TRI;
            int boxes = used + want > room;
            if (!boxes) used += want;
            const cc_scene *ls = boxes ? cc_link_rows(k) : cc_link_scene(k);
            double O[3];
            memcpy(O, ls->origin, sizeof O);
            if (boxes && k->pwant) O[0] = k->pc * ls->origin[0] - k->ps * ls->origin[1] + k->pt[0], O[1] = k->ps * ls->origin[0] + k->pc * ls->origin[1] + k->pt[1], O[2] = ls->origin[2] + k->pt[2];
            float d[3];
            for (int j = 0; j < 3; j++) d[j] = (float)(O[j] - o->origin[j]); /* lib.rs:1499 offset; MEAS3: the drawing's frame */
            size_t base = m->nelements;
            if (elements_of(&b, k->kind, k->ifc, k->vf, k->model, ls)) goto oom;
            if (boxes ? scene_add_link_boxes(o, &cap, k, view, d, base) : scene_add_link(o, &cap, k, view, ls, d, base)) goto oom;
            simplified += boxes;
        }
        if (simplified) {
            snprintf(s->paper_notice, sizeof s->paper_notice, "too large to cut exactly with the memory free: %zu link%s simplified (each element its box)",
                     simplified, simplified == 1 ? "" : "s");
            cc_log(s->pf, "bimble-ccore: %s", s->paper_notice);
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
