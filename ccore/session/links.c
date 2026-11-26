/* Linked models (ticket B5): see links.h. Loading is in session.c (cc_link_load: it reads through
 * the session's platform), the /mesh trailer in mesh.c (cc_links_bmsh), the calls in calls.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "links.h"

static const char *const FAMILY[CC_CAT_COUNT] = {"Concrete", "Steel", "Timber", "Masonry",
                                                 "Insulation", "Earth", "Glass", "Other"}; /* MaterialCategory::label */

void cc_link_free(cc_link *k) {
    if (k->ifc) cc_ifc_scene_free(k->ifc), free(k->ifc);
    if (k->vf) cc_scene_free(&k->scene), cc_viewfile_free(k->vf), free(k->vf);
    if (k->model) cc_scene_free(&k->scene), cc_model_free(k->model), free(k->model);
    free(k->path);
    memset(k, 0, sizeof *k);
}

const cc_scene *cc_link_scene(const cc_link *k) { return k->kind == CC_LINK_IFC ? &k->ifc->scene : &k->scene; }

void cc_link_id(const cc_link *k, size_t i, char id[22]) {
    const cc_scene *sc = cc_link_scene(k);
    if (k->kind == CC_LINK_IFC) { /* cc_ifc_scene_bmsh: the file's text, '0'-padded to 22 */
        cc_str g = k->ifc->info[sc->elements[i].model_index].global_id;
        for (size_t j = 0; j < 22; j++) id[j] = j < g.n ? g.p[j] : '0';
    } else memcpy(id, sc->elements[i].id.c, 22);
}

long cc_link_find(const cc_link *k, const char *id) {
    const cc_scene *sc = cc_link_scene(k);
    char t[22];
    for (size_t i = 0; i < sc->nelements; i++) {
        cc_link_id(k, i, t);
        if (!memcmp(t, id, 22)) return (long)i;
    }
    return -1;
}

int cc_links_find(const cc_session *s, const char *name, size_t n) {
    for (size_t i = 0; i < s->nlinks; i++)
        if (strlen(s->links[i].name) == n && !memcmp(s->links[i].name, name, n)) return (int)i;
    return -1;
}

int cc_links_add(cc_session *s, const char *path, cc_error *err) {
    cc_link k;
    if (cc_link_load(s, path, &k, err)) return -1;
    int at = cc_links_find(s, k.name, strlen(k.name)); /* the same file again: in its place */
    if (at >= 0) {
        k.visible = s->links[at].visible;
        if (s->link_sel == at) s->link_sel = -1;
        cc_link_free(&s->links[at]);
        s->links[at] = k;
    } else {
        if (s->nlinks >= CC_MAX_LINKS) {
            cc_link_free(&k);
            snprintf(err->msg, sizeof err->msg, "at most %d linked models", CC_MAX_LINKS);
            return -1;
        }
        if (s->nlinks == s->caplinks) {
            size_t c = s->caplinks ? 2 * s->caplinks : 4;
            cc_link *p = realloc(s->links, c * sizeof *p);
            if (!p) {
                cc_link_free(&k);
                snprintf(err->msg, sizeof err->msg, "out of memory");
                return -1;
            }
            s->links = p, s->caplinks = c;
        }
        s->links[s->nlinks++] = k;
    }
    const cc_scene *sc = cc_link_scene(&k);
    cc_log(s->pf, "bimble-ccore: linked %s: %zu elements", path, sc->nelements); /* lib.rs:1484 */
    s->links_gen++;
    return 0;
}

int cc_links_reload(cc_session *s, int i, cc_error *err) {
    cc_link k;
    if (cc_link_load(s, s->links[i].path, &k, err)) return -1;
    k.visible = s->links[i].visible;
    if (s->link_sel == i) s->link_sel = -1;
    cc_link_free(&s->links[i]);
    s->links[i] = k;
    s->links_gen++;
    return 0;
}

void cc_links_remove(cc_session *s, int i) { /* lib.rs:1535 unlink */
    cc_link_free(&s->links[i]);
    memmove(&s->links[i], &s->links[i + 1], (s->nlinks - (size_t)i - 1) * sizeof *s->links);
    s->nlinks--;
    s->link_sel = -1;
    s->links_gen++;
}

void cc_links_set_visible(cc_session *s, int i, int visible) {
    if (s->links[i].visible == !!visible) return;
    s->links[i].visible = !!visible;
    if (!visible && s->link_sel == i) s->link_sel = -1;
    s->links_gen++;
}

void cc_links_clear(cc_session *s) {
    free(s->links_saved), s->links_saved = NULL, s->links_saved_n = 0; /* LNK1 */
    if (s->nlinks) s->links_gen++;
    for (size_t i = 0; i < s->nlinks; i++) cc_link_free(&s->links[i]);
    s->nlinks = 0;
    s->link_sel = -1;
}

void cc_links_json(const cc_session *s, JsonW *w) { /* state.rs:199-216, json! sorted */
    jw_arr_begin(w);
    for (size_t i = 0; i < s->nlinks; i++) {
        const cc_link *k = &s->links[i];
        const char *dot = strrchr(k->name, '.');
        char ext[32] = "";
        if (dot && dot != k->name && strlen(dot + 1) < sizeof ext) /* Path::extension, lowercased */
            for (size_t j = 0; dot[1 + j]; j++) {
                char c = dot[1 + j];
                ext[j] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c), ext[j + 1] = 0;
            }
        jw_obj_begin(w);
        jw_key(w, "id"), jw_str(w, k->name);
        jw_key(w, "kind"), jw_str(w, ext);
        jw_key(w, "name"), jw_str(w, k->name);
        jw_key(w, "visible"), jw_bool(w, k->visible);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

/* Mesh::volume of the element in scene coordinates (divergence theorem), f64. */
static double volume_mm3(const cc_scene *sc, size_t i) {
    Arena a;
    arena_init(&a, 0);
    cc_mesh m;
    double v = 0;
    if (cc_scene_world_mesh(sc, i, &a, &m) == 0)
        for (uint32_t t = 0; t + 2 < m.ni; t += 3) {
            if (m.idx[t] >= m.nv || m.idx[t + 1] >= m.nv || m.idx[t + 2] >= m.nv) continue;
            const float *p = m.pos[m.idx[t]], *q = m.pos[m.idx[t + 1]], *r = m.pos[m.idx[t + 2]];
            double cx = (double)q[1] * r[2] - (double)q[2] * r[1];
            double cy = (double)q[2] * r[0] - (double)q[0] * r[2];
            double cz = (double)q[0] * r[1] - (double)q[1] * r[0];
            v += ((double)p[0] * cx + (double)p[1] * cy + (double)p[2] * cz) / 6.0;
        }
    arena_free(&a);
    return v < 0 ? -v : v;
}

static void str_or_null(JsonW *w, cc_str t) {
    if (t.n) jw_str_n(w, t.p, t.n);
    else jw_null(w);
}

/* The read-only element detail (lib.rs:4508 link_properties, :4570 properties): query element's
 * shape plus family, read_only, triangles and volume_m3; "link" only for a linked model. */
static int ro_element_json(const cc_scene *sc, size_t i, cc_str cls, cc_str nm, cc_str storey, cc_str material,
                           const char t[22], const char *link, Buf *out) {
    const cc_scene_element *e = &sc->elements[i];
    const cc_mesh *lm = cc_scene_local_mesh(sc, i);
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w); /* sorted keys */
    jw_key(&w, "class"), jw_str_n(&w, cls.p ? cls.p : "", cls.n);
    jw_key(&w, "family"), jw_str(&w, FAMILY[e->category < CC_CAT_COUNT ? e->category : CC_CAT_COUNT - 1]);
    jw_key(&w, "generator"), jw_str(&w, "");
    jw_key(&w, "generator_params"), jw_obj_begin(&w), jw_obj_end(&w);
    jw_key(&w, "id"), jw_str_n(&w, t, 22);
    if (link) jw_key(&w, "link"), jw_str(&w, link);
    jw_key(&w, "material"), jw_str(&w, "");
    jw_key(&w, "material_name"), str_or_null(&w, material);
    jw_key(&w, "name"), jw_str_n(&w, nm.p ? nm.p : "", nm.n);
    jw_key(&w, "openings"), jw_u64(&w, 0);
    jw_key(&w, "placement"), jw_obj_begin(&w);
    static const double O[3] = {0, 0, 0}, X[3] = {1, 0, 0}, Z[3] = {0, 0, 1};
    jw_key(&w, "origin"), jw_arr_begin(&w);
    for (int j = 0; j < 3; j++) jw_f64(&w, O[j]);
    jw_arr_end(&w);
    jw_key(&w, "x_axis"), jw_arr_begin(&w);
    for (int j = 0; j < 3; j++) jw_f64(&w, X[j]);
    jw_arr_end(&w);
    jw_key(&w, "z_axis"), jw_arr_begin(&w);
    for (int j = 0; j < 3; j++) jw_f64(&w, Z[j]);
    jw_arr_end(&w);
    jw_obj_end(&w);
    jw_key(&w, "properties"), jw_obj_begin(&w), jw_obj_end(&w);
    jw_key(&w, "read_only"), jw_bool(&w, 1);
    jw_key(&w, "storey"), jw_str(&w, "");
    jw_key(&w, "storey_name"), str_or_null(&w, storey);
    if (!link) jw_key(&w, "triangles"), jw_u64(&w, lm ? lm->ni / 3 : 0); /* lib.rs:4590 "Triangles" */
    jw_key(&w, "volume_m3"), jw_f64(&w, volume_mm3(sc, i) / 1e9);
    jw_obj_end(&w);
    if (jw_finish(&w) != JW_OK || out->oom) {
        buf_clear(out);
        buf_puts(out, "out of memory");
        return -1;
    }
    return 0;
}

int cc_link_element_json(cc_session *s, const char *name, size_t nn, const char *id, size_t idn, Buf *out) {
    int li = cc_links_find(s, name, nn);
    if (li < 0) {
        buf_puts(out, "no linked model "), buf_append(out, name, nn);
        return -1;
    }
    const cc_link *k = &s->links[li];
    long i = idn == 22 ? cc_link_find(k, id) : -1;
    if (i < 0) {
        buf_puts(out, "no element with this id");
        return -1;
    }
    const cc_scene *sc = cc_link_scene(k);
    const cc_scene_element *e = &sc->elements[i];
    cc_str cls = e->class_name, nm = {0}, storey = {0}, material = {0};
    if (k->kind == CC_LINK_IFC) {
        const cc_ifc_info *f = &k->ifc->info[e->model_index];
        cls = f->class_name, nm = f->name, storey = f->storey, material = f->material;
    } else if (k->kind == CC_LINK_VIEW) {
        const cc_view_element *f = &k->vf->elements[e->model_index];
        cls = f->class_name, nm = f->name, storey = f->storey, material = f->material;
    } else {
        const cc_element *f = &k->model->elements[e->model_index];
        const cc_storey *st = cc_model_storey(k->model, &f->storey);
        const cc_material *mt = cc_model_material(k->model, &f->material);
        nm = f->name;
        if (st) storey = st->name;
        if (mt) material = mt->name;
    }
    char t[22];
    cc_link_id(k, (size_t)i, t);
    return ro_element_json(sc, (size_t)i, cls, nm, storey, material, t, k->name, out);
}

int cc_scene_element_json(cc_session *s, const char *id, size_t idn, Buf *out) {
    if (s->open || (s->view != CC_VIEW_IFC && s->view != CC_VIEW_FILE)) {
        buf_puts(out, s->open ? "scene_element: the file is editable: query element" : "no model open");
        return -1;
    }
    const cc_scene *sc = s->view == CC_VIEW_IFC ? &s->ifc->scene : &s->vscene;
    for (size_t i = 0; idn == 22 && i < sc->nelements; i++) {
        const cc_scene_element *e = &sc->elements[i];
        if (!e->id.c[0] || memcmp(e->id.c, id, 22)) continue;
        cc_str cls = e->class_name, nm = {0}, storey = {0}, material = {0};
        if (s->view == CC_VIEW_IFC) {
            const cc_ifc_info *f = &s->ifc->info[e->model_index];
            cls = f->class_name, nm = f->name, storey = f->storey, material = f->material;
        } else {
            const cc_view_element *f = &s->vf->elements[e->model_index];
            cls = f->class_name, nm = f->name, storey = f->storey, material = f->material;
        }
        return ro_element_json(sc, i, cls, nm, storey, material, e->id.c, NULL, out);
    }
    buf_puts(out, "no element with this id");
    return -1;
}

/* ---- FIXUI: the Navigator's object tree of a read-only scene (links.h) ---- */

typedef struct {
    cc_str storey, cls, name;
    const char *id; /* 22 chars, or NULL: no valid GlobalId */
    size_t at;      /* scene order */
} ro_row;

static int str_cmp(cc_str a, cc_str b) {
    size_t n = a.n < b.n ? a.n : b.n;
    int c = n ? memcmp(a.p, b.p, n) : 0;
    return c ? c : (a.n > b.n) - (a.n < b.n);
}
static int str_eq(cc_str a, const char *p, size_t n) { return a.n == n && (!n || !memcmp(a.p, p, n)); }

static int row_cmp(const void *x, const void *y) {
    const ro_row *a = x, *b = y;
    int c = str_cmp(a->storey, b->storey);
    if (!c) c = str_cmp(a->cls, b->cls);
    return c ? c : (a->at > b->at) - (a->at < b->at);
}

/* The read-only scene's rows, sorted by (storey, class, scene order). NULL: no read-only scene or
 * out of memory (`*oom`). */
static ro_row *ro_rows(cc_session *s, size_t *n, int *oom) {
    *n = 0, *oom = 0;
    if (s->open || (s->view != CC_VIEW_IFC && s->view != CC_VIEW_FILE)) return NULL;
    const cc_scene *sc = s->view == CC_VIEW_IFC ? &s->ifc->scene : &s->vscene;
    ro_row *r = malloc((sc->nelements ? sc->nelements : 1) * sizeof *r);
    if (!r) return *oom = 1, NULL;
    for (size_t i = 0; i < sc->nelements; i++) {
        const cc_scene_element *e = &sc->elements[i];
        ro_row *w = &r[i];
        if (s->view == CC_VIEW_IFC) {
            const cc_ifc_info *f = &s->ifc->info[e->model_index];
            w->storey = f->storey, w->cls = f->class_name, w->name = f->name;
        } else {
            const cc_view_element *f = &s->vf->elements[e->model_index];
            w->storey = f->storey, w->cls = f->class_name, w->name = f->name;
        }
        if (!w->cls.n) w->cls = e->class_name;
        w->id = e->id.c[0] ? e->id.c : NULL;
        w->at = i;
    }
    *n = sc->nelements;
    qsort(r, *n, sizeof *r, row_cmp);
    return r;
}

/* A storey group's rank: its place among the IFC's storeys (lowest first), else after them. */
static size_t storey_rank(const cc_session *s, cc_str name) {
    if (s->view == CC_VIEW_IFC && s->ifc->nstoreys <= 4096) /* a bound: groups x storeys */
        for (size_t k = 0; k < s->ifc->nstoreys; k++)
            if (!str_cmp(s->ifc->storeys[k].name, name)) return k;
    return (size_t)-1;
}

typedef struct {
    size_t from, to, rank;
} ro_group;

static int group_cmp(const void *x, const void *y) {
    const ro_group *a = x, *b = y;
    if (a->rank != b->rank) return a->rank < b->rank ? -1 : 1;
    return (a->from > b->from) - (a->from < b->from);
}

int cc_scene_tree_json(cc_session *s, Buf *out) {
    size_t n;
    int oom;
    ro_row *r = ro_rows(s, &n, &oom);
    if (!r && !oom) { /* no read-only scene (no model, or an editable one: query storeys): the empty
                         * tree, as `sheets` answers [] without a model; the page may ask while a
                         * file opens or closes */
        buf_puts(out, "{\"storeys\":[],\"total\":0}");
        return 0;
    }
    if (!r) {
        buf_puts(out, "out of memory");
        return -1;
    }
    size_t ng = 0;
    for (size_t i = 0; i < n; i++) ng += !i || str_cmp(r[i].storey, r[i - 1].storey);
    ro_group *g = malloc((ng ? ng : 1) * sizeof *g);
    if (!g) {
        free(r);
        buf_puts(out, "out of memory");
        return -1;
    }
    ng = 0;
    for (size_t i = 0; i < n; i++)
        if (!i || str_cmp(r[i].storey, r[i - 1].storey)) {
            if (ng) g[ng - 1].to = i;
            g[ng].from = i, g[ng].rank = storey_rank(s, r[i].storey), ng++;
        }
    if (ng) g[ng - 1].to = n;
    qsort(g, ng, sizeof *g, group_cmp);
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "storeys"), jw_arr_begin(&w);
    for (size_t k = 0; k < ng; k++) {
        jw_obj_begin(&w);
        jw_key(&w, "classes"), jw_arr_begin(&w);
        for (size_t i = g[k].from; i < g[k].to;) {
            size_t j = i;
            while (j < g[k].to && !str_cmp(r[j].cls, r[i].cls)) j++;
            jw_obj_begin(&w);
            jw_key(&w, "class"), jw_str_n(&w, r[i].cls.p ? r[i].cls.p : "", r[i].cls.n);
            jw_key(&w, "count"), jw_u64(&w, j - i);
            jw_obj_end(&w);
            i = j;
        }
        jw_arr_end(&w);
        jw_key(&w, "count"), jw_u64(&w, g[k].to - g[k].from);
        cc_str nm = r[g[k].from].storey;
        jw_key(&w, "name"), jw_str_n(&w, nm.p ? nm.p : "", nm.n);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "total"), jw_u64(&w, n);
    jw_obj_end(&w);
    free(g), free(r);
    if (jw_finish(&w) != JW_OK || out->oom) {
        buf_clear(out);
        buf_puts(out, "out of memory");
        return -1;
    }
    return 0;
}

int cc_scene_elements_json(cc_session *s, const char *storey, size_t sn, const char *cls, size_t cn, size_t limit,
                           Buf *out) {
    size_t n;
    int oom;
    ro_row *r = ro_rows(s, &n, &oom);
    if (!r) {
        buf_puts(out, oom ? "out of memory" : s->open ? "scene_elements: the file is editable: query elements" : "no model open");
        return -1;
    }
    JsonW w;
    jw_init(&w, out, 0);
    jw_arr_begin(&w);
    size_t k = 0;
    for (size_t i = 0; i < n && k < limit; i++) {
        if (!r[i].id || !str_eq(r[i].storey, storey, sn) || (cls && !str_eq(r[i].cls, cls, cn))) continue;
        jw_obj_begin(&w);
        jw_key(&w, "class"), jw_str_n(&w, r[i].cls.p ? r[i].cls.p : "", r[i].cls.n);
        jw_key(&w, "id"), jw_str_n(&w, r[i].id, 22);
        jw_key(&w, "name"), jw_str_n(&w, r[i].name.p ? r[i].name.p : "", r[i].name.n);
        jw_obj_end(&w);
        k++;
    }
    jw_arr_end(&w);
    free(r);
    if (jw_finish(&w) != JW_OK || out->oom) {
        buf_clear(out);
        buf_puts(out, "out of memory");
        return -1;
    }
    return 0;
}

/* ---- LNK1: links kept in the .bimble (links.h, DEVIATIONS 29) ---- */

static int is_abs(const char *p) {
    return p[0] == '/' || p[0] == '\\' || (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':');
}

/* The next component of p at *i ('/' or '\\' separated): its start and length, 0 at the end. */
static size_t comp(const char *p, size_t *i, const char **c) {
    while (p[*i] == '/' || p[*i] == '\\') (*i)++;
    *c = p + *i;
    size_t k = 0;
    while (p[*i] && p[*i] != '/' && p[*i] != '\\') (*i)++, k++;
    return k;
}

int cc_links_relative(const char *dir, const char *file, char *out, size_t n) {
    out[0] = 0;
    if (!is_abs(dir) || !is_abs(file)) return 0;
    size_t i = 0, j = 0, common = 0;
    for (;;) { /* the common folders */
        size_t si = i, sj = j;
        const char *a, *b;
        size_t na = comp(dir, &i, &a), nb = comp(file, &j, &b);
        if (!na || !nb || na != nb || memcmp(a, b, na)) {
            i = si, j = sj;
            break;
        }
        common++;
    }
    if (!common) return 0; /* nothing in common (another drive, another top folder): the absolute path serves */
    size_t k = 0;
    const char *c;
    for (size_t t = i; comp(dir, &t, &c);) { /* up out of what is left of dir */
        if (k + 4 > n) return -1;
        memcpy(out + k, "../", 3), k += 3;
    }
    const char *rest = file + j;
    while (*rest == '/' || *rest == '\\') rest++;
    size_t rn = strlen(rest);
    if (!rn || k + rn + 1 > n) return out[0] = 0, rn ? -1 : 0;
    memcpy(out + k, rest, rn + 1);
    return 0;
}

/* The folder part of `path` ("" for a bare name). */
static size_t dir_len(const char *path) {
    const char *sl = cc_path_last_sep(path);
    return sl ? (size_t)(sl - path) : 0;
}

static void sig(const cc_session *s, Buf *b) {
    for (size_t i = 0; i < s->nlinks; i++) {
        const cc_link *k = &s->links[i];
        buf_puts(b, k->name), buf_putc(b, 0), buf_puts(b, k->path ? k->path : ""), buf_putc(b, 0);
        buf_putc(b, k->visible ? '1' : '0'), buf_putc(b, '\n');
    }
}

void cc_links_saved(cc_session *s) {
    Buf b;
    buf_init(&b);
    sig(s, &b);
    free(s->links_saved), s->links_saved = NULL, s->links_saved_n = 0;
    if (!b.oom && b.len) {
        s->links_saved = malloc(b.len);
        if (s->links_saved) memcpy(s->links_saved, b.p, b.len), s->links_saved_n = b.len;
    }
    buf_free(&b);
}

int cc_links_changed(const cc_session *s) {
    if (!s->nlinks) return s->links_saved_n != 0;
    Buf b;
    buf_init(&b);
    sig(s, &b);
    int ch = b.oom || b.len != s->links_saved_n || (b.len && memcmp(b.p, s->links_saved, b.len));
    buf_free(&b);
    return ch;
}

/* A missing link at the end: `path` kept, nothing drawn. */
static int add_missing(cc_session *s, const char *path, int visible) {
    if (s->nlinks >= CC_MAX_LINKS) return -1;
    if (s->nlinks == s->caplinks) {
        size_t c = s->caplinks ? 2 * s->caplinks : 4;
        cc_link *p = realloc(s->links, c * sizeof *p);
        if (!p) return -1;
        s->links = p, s->caplinks = c;
    }
    cc_link k;
    memset(&k, 0, sizeof k);
    k.path = malloc(strlen(path) + 1);
    if (!k.path) return -1;
    strcpy(k.path, path);
    const char *sl = cc_path_last_sep(k.path);
    k.name = sl ? sl + 1 : k.path;
    k.visible = visible;
    s->links[s->nlinks++] = k;
    s->links_gen++;
    return 0;
}

int cc_links_from_model(cc_session *s, const char *path) {
    if (!s->open) return 0;
    const cc_model *m = &s->model;
    size_t dl = dir_len(path);
    for (size_t i = 0; i < m->nlinks; i++) {
        const cc_model_link *e = &m->links[i];
        char *abs = malloc(e->path.n + 1), *rel = malloc(dl + 1 + e->relative.n + 1);
        if (!abs || !rel) {
            free(abs), free(rel);
            return -1;
        }
        memcpy(abs, e->path.p, e->path.n), abs[e->path.n] = 0;
        rel[0] = 0;
        if (e->relative.n && !is_abs(e->relative.p)) {
            memcpy(rel, path, dl);
            size_t k = dl;
            if (dl) rel[k++] = '/';
            memcpy(rel + k, e->relative.p, e->relative.n), rel[k + e->relative.n] = 0;
        }
        cc_error err;
        err.msg[0] = 0;
        size_t before = s->nlinks;
        int ok = 0;
        if (s->pf->files) { /* the module has no files: its read would answer the open's own bytes */
            char real[4096];
            if (rel[0] && s->pf->real_path && !s->pf->real_path(s->pf->ctx, rel, real, sizeof real) &&
                cc_links_add(s, real, &err) == 0)
                ok = 1; /* "out/../models/a.ifc" kept as the absolute path it names */
            else if (rel[0] && cc_links_add(s, rel, &err) == 0) ok = 1;
            else if (cc_links_add(s, abs, &err) == 0) ok = 1;
        }
        if (ok && s->nlinks > before && e->hidden) s->links[s->nlinks - 1].visible = 0;
        if (!ok) {
            cc_log(s->pf, "bimble-ccore: linked model %s not found (%s): kept as missing", abs,
                   s->pf->files ? err.msg : "no files here");
            if (add_missing(s, abs, !e->hidden)) {
                free(abs), free(rel);
                return -1;
            }
        }
        free(abs), free(rel);
    }
    cc_links_saved(s);
    return 0;
}

int cc_links_to_model(cc_session *s, const char *path) {
    if (!s->open) return 0;
    cc_model *m = &s->model;
    m->links = NULL, m->nlinks = 0;
    if (!s->nlinks) return 0;
    cc_model_link *l = arena_alloc(&m->arena, s->nlinks * sizeof *l, sizeof(void *));
    if (!l) return -1;
    char dir[4096], absdir[4096], abs[4096], rel[4096];
    size_t dl = dir_len(path);
    if (dl >= sizeof dir) dl = 0;
    memcpy(dir, path, dl), dir[dl] = 0;
    int real = s->pf->files && s->pf->real_path;
    if (!real || s->pf->real_path(s->pf->ctx, dl ? dir : ".", absdir, sizeof absdir)) absdir[0] = 0;
    for (size_t i = 0; i < s->nlinks; i++) {
        const cc_link *k = &s->links[i];
        const char *p = k->path ? k->path : k->name;
        if (real && k->kind && !s->pf->real_path(s->pf->ctx, p, abs, sizeof abs)) p = abs;
        rel[0] = 0;
        if (absdir[0]) cc_links_relative(absdir, p, rel, sizeof rel);
        else if (!s->pf->files && !cc_path_last_sep(p)) snprintf(rel, sizeof rel, "%s", p); /* the module: a file name */
        size_t pn = strlen(p), rn = strlen(rel), nn = strlen(k->name);
        char *pp = arena_dup(&m->arena, p, pn ? pn : 1), *rp = rn ? arena_dup(&m->arena, rel, rn) : NULL;
        char *np = arena_dup(&m->arena, k->name, nn ? nn : 1);
        if (!pp || (rn && !rp) || !np) return -1;
        l[i].name = (cc_str){np, nn}, l[i].path = (cc_str){pp, pn}, l[i].relative = (cc_str){rp, rn};
        l[i].hidden = !k->visible;
    }
    m->links = l, m->nlinks = s->nlinks;
    return 0;
}

int cc_links_locate(cc_session *s, int i, const char *path, cc_error *err) {
    cc_link k;
    if (cc_link_load(s, path, &k, err)) return -1;
    k.visible = s->links[i].visible;
    if (s->link_sel == i) s->link_sel = -1;
    cc_link_free(&s->links[i]);
    s->links[i] = k;
    s->links_gen++;
    cc_log(s->pf, "bimble-ccore: linked %s: %zu elements", path, cc_link_scene(&k)->nelements);
    return 0;
}
/* ---- end LNK1 ---- */
