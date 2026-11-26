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
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w); /* sorted keys */
    jw_key(&w, "class"), jw_str_n(&w, cls.p ? cls.p : "", cls.n);
    jw_key(&w, "family"), jw_str(&w, FAMILY[e->category < CC_CAT_COUNT ? e->category : CC_CAT_COUNT - 1]);
    jw_key(&w, "generator"), jw_str(&w, "");
    jw_key(&w, "generator_params"), jw_obj_begin(&w), jw_obj_end(&w);
    jw_key(&w, "id"), jw_str_n(&w, t, 22);
    jw_key(&w, "link"), jw_str(&w, k->name);
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
    jw_key(&w, "volume_m3"), jw_f64(&w, volume_mm3(sc, (size_t)i) / 1e9);
    jw_obj_end(&w);
    if (jw_finish(&w) != JW_OK || out->oom) {
        buf_clear(out);
        buf_puts(out, "out of memory");
        return -1;
    }
    return 0;
}
