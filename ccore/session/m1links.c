/* Mission 1 across linked models (ticket M1LINKS, ADR 0081): see m1links.h. */
#include "m1links.h"
#include <stdlib.h>
#include <string.h>
#include "guid.h"
#include "ifc_psets.h"
#include "json_r.h"
#include "platform.h"
#include "links.h"
#include "session.h"
#include "views.h"
#ifndef CC_VIEW_ONLY
#include "draw_style.h" /* cc_category_label: the family names a view hides */
#endif

static int str_cmp(cc_str a, cc_str b) {
    size_t n = a.n < b.n ? a.n : b.n;
    int c = n ? memcmp(a.p, b.p, n) : 0;
    return c ? c : (a.n > b.n) - (a.n < b.n);
}
static int str_is(cc_str a, const char *p, size_t n) { return a.n == n && (!n || !memcmp(a.p, p, n)); }
static double fabs_(double x) { return x < 0 ? -x : x; }

/* ---- levels from links ---- */

/* A link's own levels (its world), lowest first as read. */
static int link_own_levels(const cc_link *k, Arena *a, cc_level **lv, size_t *n) {
    *lv = NULL, *n = 0;
    if (k->kind == CC_LINK_IFC) return cc_sv_scene_levels(CC_VIEW_IFC, k->ifc, a, lv, n);
    if (k->kind == CC_LINK_VIEW) return cc_sv_scene_levels(CC_VIEW_FILE, k->vf, a, lv, n);
    if (k->kind == CC_LINK_MODEL) return cc_levels_of_model(k->model, a, lv, n);
    return 0;
}

static int lv_cmp(const void *x, const void *y) {
    const cc_level *a = x, *b = y;
    return (a->elevation > b->elevation) - (a->elevation < b->elevation);
}

int cc_link_levels(const cc_session *s, const cc_level *own, size_t nown, Arena *a, cc_level **out, size_t *n,
                   size_t **from) {
    *out = NULL, *n = 0;
    if (from) *from = NULL;
    size_t cap = 0;
    for (size_t i = 0; i < s->nlinks; i++) {
        const cc_link *k = &s->links[i];
        cap += k->kind == CC_LINK_IFC ? k->ifc->nstoreys : k->kind == CC_LINK_VIEW ? k->vf->nstoreys
             : k->kind == CC_LINK_MODEL ? k->model->nstoreys : 0;
    }
    if (cap > CC_LINK_LEVELS_MAX) cap = CC_LINK_LEVELS_MAX;
    cc_level *lv = arena_alloc(a, (cap ? cap : 1) * sizeof *lv, sizeof(double));
    size_t *fr = arena_alloc(a, (cap ? cap : 1) * sizeof *fr, sizeof(size_t));
    if (!lv || !fr) return -1;
    size_t m = 0;
    for (size_t i = 0; i < s->nlinks && m < cap; i++) {
        const cc_link *k = &s->links[i];
        if (!k->kind) continue;
        cc_level *kl;
        size_t kn;
        if (link_own_levels(k, a, &kl, &kn)) return -1;
        double c, sn, t[3];
        cc_link_placement(s, k, &c, &sn, t);
        for (size_t j = 0; j < kn && m < cap; j++) {
            double z = kl[j].elevation + t[2];
            if (!(z == z) || z > 1e12 || z < -1e12) continue; /* untrusted: a level at no height */
            int taken = 0;
            for (size_t q = 0; q < nown && !taken; q++) taken = fabs_(own[q].elevation - z) <= CC_LINK_LEVEL_TOL_MM;
            for (size_t q = 0; q < m && !taken; q++) taken = fabs_(lv[q].elevation - z) <= CC_LINK_LEVEL_TOL_MM;
            if (taken) continue;
            /* the key: GlobalId::derived("bimble link level <link> <storey key>") */
            size_t ln = strlen(k->name), need = 18 + ln + 1 + kl[j].key.n;
            char *seed = arena_alloc(a, need, 1), *key = arena_alloc(a, 22, 1);
            if (!seed || !key) return -1;
            memcpy(seed, "bimble link level ", 18);
            memcpy(seed + 18, k->name, ln);
            seed[18 + ln] = ' ';
            if (kl[j].key.n) memcpy(seed + 19 + ln, kl[j].key.p, kl[j].key.n);
            cc_guid g = cc_guid_derived(seed, need);
            memcpy(key, g.c, 22);
            lv[m].key.p = key, lv[m].key.n = 22;
            lv[m].name = kl[j].name;
            lv[m].elevation = z + 0.0;
            fr[m] = i;
            m++;
        }
    }
    /* lowest first, stable for equal heights (insertion: m is small) */
    for (size_t i = 1; i < m; i++) {
        cc_level x = lv[i];
        size_t fx = fr[i], j = i;
        while (j > 0 && lv_cmp(&lv[j - 1], &x) > 0) lv[j] = lv[j - 1], fr[j] = fr[j - 1], j--;
        lv[j] = x, fr[j] = fx;
    }
    *out = lv, *n = m;
    if (from) *from = fr;
    return 0;
}

int cc_link_level_find(const cc_session *s, cc_str key, size_t *link) {
    if (!s->nlinks || key.n != 22) return 0;
    Arena a;
    arena_init(&a, 0);
    cc_level *own = NULL, *lv;
    size_t nown = 0, n, *fr;
    int found = 0;
    if (s->open && cc_levels_of_model(&s->model, &a, &own, &nown)) nown = 0;
    else if (!s->open && s->view && cc_sv_scene_levels(s->view, s->view == CC_VIEW_IFC ? (const void *)s->ifc : (const void *)s->vf, &a, &own, &nown))
        nown = 0;
    if (!cc_link_levels(s, own, nown, &a, &lv, &n, &fr))
        for (size_t i = 0; i < n && !found; i++)
            if (!memcmp(lv[i].key.p, key.p, 22)) found = 1, *link = fr[i];
    arena_free(&a);
    return found;
}

/* ---- the box over the links ---- */

static int link_drawn(const cc_link *k) { return k->kind && k->visible; }

void cc_vis_fit_session(cc_session *s, cc_vis *v, const cc_scene *sc, const cc_gid *primary) {
    size_t nl = 0;
    for (size_t i = 0; i < s->nlinks; i++) nl += link_drawn(&s->links[i]);
    if (!nl || primary) { /* Rust's box, byte for byte: the open scene (or the selection) alone */
        cc_vis_fit(v, sc, primary);
        if (v->has_box || primary) return;
    }
    if (!sc) return;
    float inf = 1e30f * 1e30f, lo[3] = {inf, inf, inf}, hi[3] = {-inf, -inf, -inf};
    static const float zero[3] = {0, 0, 0};
    for (size_t k = 0; k < sc->nelements; k++)
        if (v->reveal || !cc_vis_hidden(v, &sc->elements[k].id)) cc_vis_grow(sc, k, zero, v->yaw, lo, hi);
    for (size_t i = 0; i < s->nlinks; i++) {
        const cc_link *k = &s->links[i];
        if (!link_drawn(k)) continue;
        const cc_scene *ls = cc_link_scene(k);
        float off[3];
        for (int q = 0; q < 3; q++) off[q] = (float)(ls->origin[q] - sc->origin[q]); /* lib.rs:1499 */
        char id[22];
        for (size_t e = 0; e < ls->nelements; e++) {
            if (!v->reveal) {
                cc_link_id(k, e, id);
                cc_gid g;
                memcpy(g.c, id, 22);
                if (cc_gid_valid(id, 22) && cc_vis_hidden(v, &g)) continue;
            }
            cc_vis_grow(ls, e, off, v->yaw, lo, hi);
        }
    }
    if (lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) return;
    for (int k = 0; k < 3; k++) { /* cc_vis_fit's margin */
        float d = hi[k] - lo[k], m = 1.0f + d * 1e-5f;
        v->min[k] = lo[k] - m, v->max[k] = hi[k] + m;
    }
    v->has_box = 1;
}

/* ---- a linked element's texts and the view's rules ---- */

void cc_link_texts(const cc_link *k, size_t i, cc_str *cls, cc_str *name, cc_str *storey, cc_str *material) {
    static const cc_str none = {"", 0};
    const cc_scene *sc = cc_link_scene(k);
    const cc_scene_element *e = &sc->elements[i];
    *cls = e->class_name, *name = none, *storey = none, *material = none;
    if (k->kind == CC_LINK_IFC) {
        const cc_ifc_info *f = &k->ifc->info[e->model_index];
        *cls = f->class_name, *name = f->name, *storey = f->storey, *material = f->material;
    } else if (k->kind == CC_LINK_VIEW) {
        const cc_view_element *f = &k->vf->elements[e->model_index];
        *cls = f->class_name, *name = f->name, *storey = f->storey, *material = f->material;
    } else if (k->kind == CC_LINK_MODEL) {
        const cc_element *f = &k->model->elements[e->model_index];
        const cc_storey *st = cc_model_storey(k->model, &f->storey);
        const cc_material *mt = cc_model_material(k->model, &f->material);
        *name = f->name;
        if (st) *storey = st->name;
        if (mt) *material = mt->name;
    }
    if (!cls->n) *cls = e->class_name;
}

static int strs_has(const cc_strs *l, const char *p, size_t n) {
    for (size_t i = 0; i < l->n; i++)
        if (str_is(l->v[i], p, n)) return 1;
    return 0;
}

/* "<link>/<storey>" among the hidden storeys */
static int link_storey_hidden(const cc_strs *l, const char *link, cc_str storey) {
    size_t ln = strlen(link);
    for (size_t i = 0; i < l->n; i++) {
        cc_str x = l->v[i];
        if (x.n == ln + 1 + storey.n && !memcmp(x.p, link, ln) && x.p[ln] == '/' &&
            (!storey.n || !memcmp(x.p + ln + 1, storey.p, storey.n)))
            return 1;
    }
    return 0;
}

int cc_link_hidden_by_rule(const cc_link *k, size_t i, const cc_visibility *w) {
    if (!w->hidden_storeys.n) return 0;
    cc_str cls, nm, st, mt;
    cc_link_texts(k, i, &cls, &nm, &st, &mt);
    return link_storey_hidden(&w->hidden_storeys, k->name, st);
}

int cc_links_by_view(cc_session *s, const cc_visibility *w, cc_gidset *set) {
    size_t rules = w->hidden_classes.n + w->hidden_families.n + w->hidden_storeys.n + w->hidden_materials.n + w->hidden_links.n;
    if (!rules) return 0;
    for (size_t li = 0; li < s->nlinks; li++) {
        const cc_link *k = &s->links[li];
        if (!k->kind) continue;
        int whole = strs_has(&w->hidden_links, k->name, strlen(k->name));
        const cc_scene *sc = cc_link_scene(k);
        char id[22];
        for (size_t i = 0; i < sc->nelements; i++) {
            cc_str cls, nm, st, mt;
            cc_link_texts(k, i, &cls, &nm, &st, &mt);
            int hide = whole || strs_has(&w->hidden_classes, cls.p, cls.n) ||
                       strs_has(&w->hidden_materials, mt.p, mt.n) || link_storey_hidden(&w->hidden_storeys, k->name, st);
#ifndef CC_VIEW_ONLY
            if (!hide && w->hidden_families.n) {
                const char *fam = cc_category_label(sc->elements[i].category);
                hide = strs_has(&w->hidden_families, fam, strlen(fam));
            }
#endif
            if (!hide) continue;
            cc_link_id(k, i, id);
            if (!cc_gid_valid(id, 22)) continue;
            cc_gid g;
            memcpy(g.c, id, 22);
            if (cc_gidset_add(set, &g)) return -1;
        }
    }
    return 0;
}

/* ---- a link's object tree ---- */

typedef struct {
    cc_str storey, cls, name;
    char id[22];
    int has_id;
    size_t at;
} lrow;

static int lrow_cmp(const void *x, const void *y) {
    const lrow *a = x, *b = y;
    int c = str_cmp(a->storey, b->storey);
    if (!c) c = str_cmp(a->cls, b->cls);
    return c ? c : (a->at > b->at) - (a->at < b->at);
}

static lrow *link_rows(const cc_link *k, size_t *n) {
    const cc_scene *sc = cc_link_scene(k);
    *n = sc->nelements;
    lrow *r = malloc((*n ? *n : 1) * sizeof *r);
    if (!r) return NULL;
    for (size_t i = 0; i < *n; i++) {
        cc_str mt;
        cc_link_texts(k, i, &r[i].cls, &r[i].name, &r[i].storey, &mt);
        cc_link_id(k, i, r[i].id);
        r[i].has_id = cc_gid_valid(r[i].id, 22);
        r[i].at = i;
    }
    qsort(r, *n, sizeof *r, lrow_cmp);
    return r;
}

/* The rank of storey `name` in link k: its level's place (lowest first), else after them. */
static size_t link_storey_rank(const cc_link *k, cc_str name, const cc_level *lv, size_t nl) {
    for (size_t i = 0; i < nl && nl <= 4096; i++)
        if (!str_cmp(lv[i].name, name)) return i;
    (void)k;
    return (size_t)-1;
}

typedef struct {
    size_t from, to, rank;
} lgroup;

static int lgroup_cmp(const void *x, const void *y) {
    const lgroup *a = x, *b = y;
    if (a->rank != b->rank) return a->rank < b->rank ? -1 : 1;
    return (a->from > b->from) - (a->from < b->from);
}

static int oom_out(Buf *out) {
    buf_clear(out);
    buf_puts(out, "out of memory");
    return -1;
}

static const cc_link *link_named(cc_session *s, const char *name, size_t nn, Buf *out) {
    int li = cc_links_find(s, name, nn);
    if (li < 0) {
        buf_puts(out, "no linked model "), buf_append(out, name, nn);
        return NULL;
    }
    if (!s->links[li].kind) {
        buf_puts(out, "the linked model "), buf_append(out, name, nn), buf_puts(out, " is not loaded");
        return NULL;
    }
    return &s->links[li];
}

/* One link's tree object (the keys of cc_scene_tree_json, then "link"). */
static int tree_of(cc_session *s, const cc_link *k, JsonW *w, int with_classes) {
    size_t n;
    lrow *r = link_rows(k, &n);
    Arena a;
    arena_init(&a, 0);
    cc_level *lv = NULL;
    size_t nl = 0;
    lgroup *g = r ? malloc((n ? n : 1) * sizeof *g) : NULL;
    if (!g || link_own_levels(k, &a, &lv, &nl)) {
        free(r), free(g), arena_free(&a);
        return -1;
    }
    double c, sn, t[3];
    cc_link_placement(s, k, &c, &sn, t);
    size_t ng = 0;
    for (size_t i = 0; i < n; i++)
        if (!i || str_cmp(r[i].storey, r[i - 1].storey)) {
            if (ng) g[ng - 1].to = i;
            g[ng].from = i, g[ng].rank = link_storey_rank(k, r[i].storey, lv, nl), ng++;
        }
    if (ng) g[ng - 1].to = n;
    qsort(g, ng, sizeof *g, lgroup_cmp);
    jw_obj_begin(w);
    jw_key(w, "link"), jw_str(w, k->name);
    jw_key(w, "storeys"), jw_arr_begin(w);
    for (size_t q = 0; q < ng; q++) {
        jw_obj_begin(w);
        if (with_classes) {
            jw_key(w, "classes"), jw_arr_begin(w);
            for (size_t i = g[q].from; i < g[q].to;) {
                size_t j = i;
                while (j < g[q].to && !str_cmp(r[j].cls, r[i].cls)) j++;
                jw_obj_begin(w);
                jw_key(w, "class"), jw_str_n(w, r[i].cls.p ? r[i].cls.p : "", r[i].cls.n);
                jw_key(w, "count"), jw_u64(w, j - i);
                jw_obj_end(w);
                i = j;
            }
            jw_arr_end(w);
        }
        jw_key(w, "count"), jw_u64(w, g[q].to - g[q].from);
        jw_key(w, "elevation_mm");
        if (g[q].rank != (size_t)-1) jw_f64(w, lv[g[q].rank].elevation + t[2] + 0.0);
        else jw_null(w);
        cc_str nm = r[g[q].from].storey;
        jw_key(w, "name"), jw_str_n(w, nm.p ? nm.p : "", nm.n);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_key(w, "total"), jw_u64(w, n);
    jw_key(w, "visible"), jw_bool(w, k->visible);
    jw_obj_end(w);
    free(r), free(g), arena_free(&a);
    return 0;
}

int cc_link_tree_json(cc_session *s, const char *name, size_t nn, Buf *out) {
    const cc_link *k = link_named(s, name, nn, out);
    if (!k) return -1;
    JsonW w;
    jw_init(&w, out, 0);
    if (tree_of(s, k, &w, 1) || jw_finish(&w) != JW_OK || out->oom) return oom_out(out);
    return 0;
}

int cc_links_tree_json(cc_session *s, Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    jw_arr_begin(&w);
    for (size_t i = 0; i < s->nlinks; i++)
        if (s->links[i].kind && tree_of(s, &s->links[i], &w, 0)) return oom_out(out);
    jw_arr_end(&w);
    if (jw_finish(&w) != JW_OK || out->oom) return oom_out(out);
    return 0;
}

int cc_link_elements_json(cc_session *s, const char *name, size_t nn, const char *storey, size_t sn, const char *cls,
                          size_t cn, size_t limit, Buf *out) {
    const cc_link *k = link_named(s, name, nn, out);
    if (!k) return -1;
    size_t n;
    lrow *r = link_rows(k, &n);
    if (!r) return oom_out(out);
    JsonW w;
    jw_init(&w, out, 0);
    jw_arr_begin(&w);
    size_t q = 0;
    for (size_t i = 0; i < n && q < limit; i++) {
        if (!r[i].has_id || !str_is(r[i].storey, storey, sn) || (cls && !str_is(r[i].cls, cls, cn))) continue;
        jw_obj_begin(&w);
        jw_key(&w, "class"), jw_str_n(&w, r[i].cls.p ? r[i].cls.p : "", r[i].cls.n);
        jw_key(&w, "id"), jw_str_n(&w, r[i].id, 22);
        jw_key(&w, "link"), jw_str(&w, k->name);
        jw_key(&w, "name"), jw_str_n(&w, r[i].name.p ? r[i].name.p : "", r[i].name.n);
        jw_obj_end(&w);
        q++;
    }
    jw_arr_end(&w);
    free(r);
    if (jw_finish(&w) != JW_OK || out->oom) return oom_out(out);
    return 0;
}

int cc_link_levels_json(cc_session *s, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    cc_level *own = NULL, *lv = NULL;
    size_t nown = 0, n = 0, *fr = NULL;
    int rc = 0;
    if (s->open) rc = cc_levels_of_model(&s->model, &a, &own, &nown);
    else if (s->view)
        rc = cc_sv_scene_levels(s->view, s->view == CC_VIEW_IFC ? (const void *)s->ifc : (const void *)s->vf, &a, &own, &nown);
    if (rc || cc_link_levels(s, own, nown, &a, &lv, &n, &fr)) {
        arena_free(&a);
        return oom_out(out);
    }
    JsonW w;
    jw_init(&w, out, 0);
    jw_arr_begin(&w);
    for (size_t i = 0; i < n; i++) {
        jw_obj_begin(&w);
        jw_key(&w, "elevation_mm"), jw_f64(&w, lv[i].elevation);
        jw_key(&w, "key"), jw_str_n(&w, lv[i].key.p, 22);
        jw_key(&w, "link"), jw_str(&w, s->links[fr[i]].name);
        jw_key(&w, "name"), jw_str_n(&w, lv[i].name.p ? lv[i].name.p : "", lv[i].name.n);
        /* its floor plan: GlobalId::derived("bimble view plan down <key>") (view_effective.c) */
        char seed[22 + 22];
        memcpy(seed, "bimble view plan down ", 22);
        memcpy(seed + 22, lv[i].key.p, 22);
        cc_guid g = cc_guid_derived(seed, sizeof seed);
        jw_key(&w, "view"), jw_str_n(&w, g.c, 22);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    arena_free(&a);
    if (jw_finish(&w) != JW_OK || out->oom) return oom_out(out);
    return 0;
}

/* ---- a linked element's property sets (ifc_psets.h) ---- */

/* SIZE2: the packed table's reads (ifc_psets.h cc_psets_read_fn): a range of the cache's section. */
typedef struct {
    const cc_platform *pf;
    const char *path;
    uint64_t off, len;
} PackedRd;
static int packed_read(void *ctx, uint64_t off, size_t n, void *dst) {
    const PackedRd *r = ctx;
    size_t got = 0;
    if (off > r->len || n > r->len - off) return -1;
    return r->pf->read_range(r->pf->ctx, r->path, r->off + off, n, dst, &got) == CC_PF_OK && got == n ? 0 : -1;
}

int cc_link_psets_json(cc_session *s, const cc_link *k, size_t i, Buf *out) {
    const cc_scene *sc = cc_link_scene(k);
    if (i >= sc->nelements) return -1;
    size_t idx = k->kind == CC_LINK_VIEW ? sc->elements[i].model_index : i;
    const uint8_t *p = NULL;
    size_t n = 0;
    char *tmp = NULL;
    const char *why = NULL;
    if (k->kind == CC_LINK_IFC && k->psets) {
        if (cc_psets_entry((const uint8_t *)k->psets, k->npsets, idx, &p, &n)) why = "unreadable";
    } else if (k->kind == CC_LINK_VIEW && k->vf->has_psets && k->vf->psets_packed) { /* SIZE2: the packed table */
        const cc_platform *pf = s->pf;
        PackedRd r = {pf, k->cache && k->cache[0] ? k->cache : k->path, k->vf->psets_off, k->vf->psets_len};
        Buf e;
        buf_init(&e);
        int prc = !pf->read_range ? -3 : cc_psets_packed_entry(packed_read, &r, k->vf->psets_len, idx, &e);
        if (prc == -3) why = "not available yet (no file access)";
        else if (prc == -2) why = "out of memory";
        else if (prc) why = "unreadable (the cache changed)";
        else if (e.len) tmp = e.p, p = (const uint8_t *)e.p, n = e.len, e.p = NULL;
        buf_free(&e);
    } else if (k->kind == CC_LINK_VIEW && k->vf->has_psets) {
        const cc_platform *pf = s->pf;
        const char *path = k->cache && k->cache[0] ? k->cache : k->path;
        uint64_t ne = k->vf->nelements, off = k->vf->psets_off, head = cc_psets_head(ne);
        unsigned char e[8];
        size_t got = 0;
        if (!pf->read_range) why = "not available yet (no file access)";
        else if (idx >= ne) why = "unreadable";
        else if (pf->read_range(pf->ctx, path, off + 4 + 4 * (idx ? idx - 1 : 0), idx ? 8 : 4, (char *)e, &got) != CC_PF_OK ||
                 got != (idx ? 8u : 4u))
            why = "unreadable (the cache changed)";
        else {
            uint64_t a = idx ? ((uint64_t)e[0] | (uint64_t)e[1] << 8 | (uint64_t)e[2] << 16 | (uint64_t)e[3] << 24) : 0;
            const unsigned char *q = idx ? e + 4 : e;
            uint64_t b = (uint64_t)q[0] | (uint64_t)q[1] << 8 | (uint64_t)q[2] << 16 | (uint64_t)q[3] << 24;
            if (a > b || b - a > CC_PSETS_ENTRY || head + b > k->vf->psets_len) why = "unreadable (the cache changed)";
            else if (b > a) {
                tmp = malloc((size_t)(b - a));
                if (!tmp) why = "out of memory";
                else if (pf->read_range(pf->ctx, path, off + head + a, (size_t)(b - a), tmp, &got) != CC_PF_OK || got != b - a)
                    why = "unreadable (the cache changed)";
                else p = (const uint8_t *)tmp, n = (size_t)(b - a);
            }
        }
    } else why = k->kind == CC_LINK_MODEL ? "a .bimble link: its properties are the model's (not read here)" : "none read";
    if (!why && n) { /* untrusted: what goes out is JSON */
        Arena a;
        arena_init(&a, 0);
        JrError je;
        const JrVal *v = jr_parse(&a, (const char *)p, n, &je);
        if (!v || v->type != JR_ARR) why = "unreadable";
        arena_free(&a);
    }
    buf_puts(out, ",\"property_sets\":");
    if (!why) buf_append(out, n ? (const char *)p : "[]", n ? n : 2);
    else buf_puts(out, "[]");
    buf_puts(out, ",\"property_sets_note\":");
    if (why) json_escape(out, why, strlen(why));
    else buf_puts(out, "null");
    free(tmp);
    return 0;
}

/* ---- notes on linked elements (m1links.h) ---- */

static void note_free(cc_note *n) { free(n->link), free(n->text); }

void cc_notes_clear(cc_session *s) {
    for (size_t i = 0; i < s->nnotes; i++) note_free(&s->notes[i]);
    free(s->notes);
    s->notes = NULL, s->nnotes = s->capnotes = 0;
}

static char *dup_n(const char *p, size_t n) {
    char *q = malloc(n + 1);
    if (!q) return NULL;
    if (n) memcpy(q, p, n);
    q[n] = 0;
    return q;
}

static int note_push(cc_session *s, const char *link, size_t ln, const char *el, const double at[3], const char *text, size_t tn) {
    if (s->nnotes == s->capnotes) {
        size_t c = s->capnotes ? 2 * s->capnotes : 8;
        cc_note *p = realloc(s->notes, c * sizeof *p);
        if (!p) return -1;
        s->notes = p, s->capnotes = c;
    }
    cc_note *n = &s->notes[s->nnotes];
    n->link = dup_n(link, ln), n->text = dup_n(text, tn);
    if (!n->link || !n->text) return note_free(n), -1;
    memcpy(n->element, el, 22);
    for (int c = 0; c < 3; c++) n->at[c] = at[c] + 0.0;
    s->nnotes++;
    return 0;
}

int cc_notes_from_model(cc_session *s) {
    cc_notes_clear(s);
    for (size_t i = 0; s->open && i < s->model.nlinks; i++) {
        const cc_model_link *e = &s->model.links[i];
        for (size_t q = 0; q < e->nnotes; q++)
            if (note_push(s, e->name.p, e->name.n, e->notes[q].element.c, e->notes[q].at, e->notes[q].text.p ? e->notes[q].text.p : "",
                          e->notes[q].text.n))
                return -1;
    }
    return 0;
}

int cc_notes_to_model(cc_session *s, Arena *a, cc_model_link *l, const char *name) {
    size_t n = 0;
    for (size_t i = 0; i < s->nnotes; i++) n += !strcmp(s->notes[i].link, name);
    l->notes = NULL, l->nnotes = 0;
    if (!n) return 0;
    if (n > CC_MODEL_NOTES_MAX) n = CC_MODEL_NOTES_MAX;
    cc_model_note *v = arena_alloc(a, n * sizeof *v, sizeof(double));
    if (!v) return -1;
    size_t k = 0;
    for (size_t i = 0; i < s->nnotes && k < n; i++) {
        const cc_note *x = &s->notes[i];
        if (strcmp(x->link, name)) continue;
        size_t tn = strlen(x->text);
        char *t = arena_dup(a, tn ? x->text : "", tn ? tn : 1);
        if (!t) return -1;
        memcpy(v[k].element.c, x->element, 22);
        memcpy(v[k].at, x->at, sizeof v[k].at);
        v[k].text = (cc_str){t, tn};
        k++;
    }
    l->notes = v, l->nnotes = k;
    return 0;
}

void cc_notes_sig(const cc_session *s, Buf *b) {
    for (size_t i = 0; i < s->nnotes; i++) {
        const cc_note *n = &s->notes[i];
        buf_puts(b, n->link), buf_putc(b, 0), buf_append(b, n->element, 22), buf_append(b, n->at, sizeof n->at);
        buf_puts(b, n->text), buf_putc(b, '\n');
    }
}

/* The element's point (link world mm, as the link reads it): its box's centre, its top. 0 found. */
static int element_anchor(const cc_link *k, const char *id, double at[3]) {
    const cc_scene *sc = cc_link_raw_scene(k);
    char t[22];
    for (size_t i = 0; i < sc->nelements; i++) {
        const cc_scene_element *e = &sc->elements[i];
        if (k->kind == CC_LINK_IFC) {
            cc_str g = k->ifc->info[e->model_index].global_id;
            for (size_t j = 0; j < 22; j++) t[j] = j < g.n ? g.p[j] : '0';
        } else memcpy(t, e->id.c, 22);
        if (memcmp(t, id, 22)) continue;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        static const float zero[3] = {0, 0, 0};
        cc_vis_grow(sc, i, zero, 0.0f, lo, hi);
        if (lo[0] > hi[0]) return -1;
        for (int c = 0; c < 3; c++) at[c] = sc->origin[c] + (c < 2 ? ((double)lo[c] + (double)hi[c]) / 2.0 : (double)hi[c]);
        return 0;
    }
    return -1;
}

int cc_notes_op(cc_session *s, int add, const char *link, size_t ln, const JrVal *v, Buf *out) {
    if (!s->open) return buf_puts(out, "notes belong to an editable model (.bimble): File > New, then link"), -1;
    if (!add) {
        const JrVal *ix = jr_get(v, "index");
        size_t k = 0, want = ix && ix->type == JR_NUM && ix->num >= 0 && ix->num < 1e6 ? (size_t)ix->num : (size_t)-1;
        for (size_t i = 0; i < s->nnotes; i++) {
            if (strlen(s->notes[i].link) != ln || memcmp(s->notes[i].link, link, ln)) continue;
            if (k++ != want) continue;
            note_free(&s->notes[i]);
            memmove(s->notes + i, s->notes + i + 1, (s->nnotes - i - 1) * sizeof *s->notes);
            s->nnotes--;
            s->links_gen++;
            return 0;
        }
        return buf_puts(out, "index: the note's number among the link's notes (query link_notes)"), -1;
    }
    const JrVal *el = jr_get(v, "element"), *tx = jr_get(v, "text"), *at = jr_get(v, "at");
    if (!el || el->type != JR_STR || !cc_gid_valid(el->s, el->n)) return buf_puts(out, "element: the linked element's GlobalId"), -1;
    if (!tx || tx->type != JR_STR || !tx->n || tx->n > CC_MODEL_NOTE_TEXT || strlen(tx->s) != tx->n)
        return buf_puts(out, "text: 1 to 2000 bytes"), -1;
    size_t cnt = 0;
    for (size_t i = 0; i < s->nnotes; i++) cnt += strlen(s->notes[i].link) == ln && !memcmp(s->notes[i].link, link, ln);
    if (cnt >= CC_MODEL_NOTES_MAX) return buf_puts(out, "a link holds at most 256 notes"), -1;
    int li = cc_links_find(s, link, ln);
    double p[3];
    if (at) { /* the host's world (where the user clicked): back into the link's own */
        if (at->type != JR_ARR || at->n != 3) return buf_puts(out, "at: [x, y, z] in mm (the host's world)"), -1;
        double w[3];
        for (int c = 0; c < 3; c++) {
            w[c] = at->items[c].num;
            if (at->items[c].type != JR_NUM || !(w[c] > -1e12 && w[c] < 1e12)) return buf_puts(out, "at: finite numbers"), -1;
        }
        double c, sn, t[3];
        cc_link_placement(s, &s->links[li], &c, &sn, t);
        double x = w[0] - t[0], y = w[1] - t[1];
        p[0] = c * x + sn * y, p[1] = -sn * x + c * y, p[2] = w[2] - t[2];
    } else if (!s->links[li].kind || element_anchor(&s->links[li], el->s, p))
        return buf_puts(out, "no element with this id in the link (give \"at\")"), -1;
    if (note_push(s, link, ln, el->s, p, tx->s, tx->n)) return buf_puts(out, "out of memory"), -1;
    s->links_gen++;
    return 0;
}

int cc_notes_json(cc_session *s, Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    jw_arr_begin(&w);
    for (size_t i = 0; i < s->nnotes; i++) {
        const cc_note *n = &s->notes[i];
        int li = cc_links_find(s, n->link, strlen(n->link));
        const cc_link *k = li >= 0 ? &s->links[li] : NULL;
        size_t idx = 0;
        for (size_t j = 0; j < i; j++) idx += !strcmp(s->notes[j].link, n->link);
        double p[3] = {n->at[0], n->at[1], n->at[2]};
        if (k) {
            double c, sn, t[3];
            cc_link_placement(s, k, &c, &sn, t);
            p[0] = c * n->at[0] - sn * n->at[1] + t[0], p[1] = sn * n->at[0] + c * n->at[1] + t[1], p[2] = n->at[2] + t[2];
        }
        int orphan = !k || !k->kind || cc_link_find(k, n->element) < 0;
        jw_obj_begin(&w);
        jw_key(&w, "at"), jw_arr_begin(&w);
        for (int c = 0; c < 3; c++) jw_f64(&w, p[c] + 0.0);
        jw_arr_end(&w);
        jw_key(&w, "element"), jw_str_n(&w, n->element, 22);
        jw_key(&w, "index"), jw_u64(&w, idx);
        jw_key(&w, "link"), jw_str(&w, n->link);
        jw_key(&w, "orphan"), jw_bool(&w, orphan);
        jw_key(&w, "text"), jw_str(&w, n->text);
        jw_key(&w, "visible"), jw_bool(&w, k && k->visible);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    if (jw_finish(&w) != JW_OK || out->oom) return oom_out(out);
    return 0;
}

int cc_notes_into_view(cc_session *s, cc_view *v, Arena *a) {
    if (!s->nnotes || v->kind == CC_VIEW_THREE_D) return 0;
    double z[4];
    int plan = v->kind == CC_VIEW_PLAN && !cc_sv_plan_range(s, v, z);
    size_t k = v->nannotations;
    cc_annotation *an = arena_alloc(a, (k + s->nnotes) * sizeof *an, sizeof(double));
    if (!an) return -1;
    if (k) memcpy(an, v->annotations, k * sizeof *an);
    Buf nb;
    buf_init(&nb);
    if (cc_notes_json(s, &nb)) return buf_free(&nb), -1;
    JrError je;
    const JrVal *arr = jr_parse(a, nb.p, nb.len, &je);
    buf_free(&nb);
    for (size_t i = 0; arr && i < arr->n; i++) {
        const JrVal *x = &arr->items[i], *at = jr_get(x, "at"), *tx = jr_get(x, "text"), *vis = jr_get(x, "visible");
        if (!vis || !vis->b) continue;
        double p[3] = {at->items[0].num, at->items[1].num, at->items[2].num};
        if (plan && !(p[2] >= z[CC_RANGE_BOTTOM] - 1.0 && p[2] <= z[CC_RANGE_TOP] + 1.0)) continue; /* this storey's */
        cc_annotation *t = &an[k++];
        memset(t, 0, sizeof *t);
        t->type = CC_ANN_TEXT;
        cc_guid g = cc_guid_derived(tx->s, tx->n);
        memcpy(t->id.c, g.c, 22);
        t->text = (cc_str){tx->s, tx->n};
        memcpy(t->at, p, sizeof p);
        t->size_mm = 2.5;
    }
    v->annotations = an, v->nannotations = k;
    return 0;
}
