/* Linked models (ticket B5): see links.h. Loading is in session.c (cc_link_load: it reads through
 * the session's platform), the /mesh trailer in mesh.c (cc_links_bmsh), the calls in calls.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "links.h"
#include "m1links.h"
#include "link_cache.h" /* LINKF */
#include "jobs.h"       /* LINKF: cc_atomic_load */
#include "ifc_math.h"   /* LINKF: cc_sin, cc_cos */
#include "json_r.h"

static const char *const FAMILY[CC_CAT_COUNT] = {"Concrete", "Steel", "Timber", "Masonry",
                                                 "Insulation", "Earth", "Glass", "Other"}; /* MaterialCategory::label */

static void keep_settings(cc_link *k, const cc_link *old);
static int link_rows_view(cc_link *k); /* SIZE3 */
static void cc_links_undo_free(cc_session *s);

/* SIZE3: link k's decoded meshes dropped (a lazy link: its view file keeps their counts and places). */
static void link_evict(cc_link *k) {
    if (!k->resident) return;
    if (k->placed) cc_scene_free(&k->pscene), k->placed = 0;
    cc_viewfile_dematerialize(k->vf);
    link_rows_view(k); /* empty meshes again (the counts stay in the view file) */
    arena_free(&k->marena);
    k->resident = 0, k->mbytes = 0;
}

/* What link k holds in memory (its scene), freed; the link itself stays. */
static void link_drop_data(cc_link *k) {
    if (k->ifc) cc_ifc_scene_free(k->ifc), free(k->ifc), k->ifc = NULL;
    if (k->vf) link_evict(k);
    if (k->vf) cc_scene_free(&k->scene), cc_viewfile_free(k->vf), free(k->vf), k->vf = NULL;
    if (k->map) k->map_pf->unmap_file(k->map_pf->ctx, k->map), k->map = NULL; /* SIZE3: after the view file over it */
    k->pwant = 0;
    if (k->model) cc_scene_free(&k->scene), cc_model_free(k->model), free(k->model), k->model = NULL;
    memset(&k->scene, 0, sizeof k->scene);
    if (k->placed) cc_scene_free(&k->pscene), k->placed = 0; /* LINKF */
    free(k->psets), k->psets = NULL, k->npsets = 0; /* M1LINKS */
    k->kind = 0;
}

void cc_link_free(cc_link *k) {
    link_drop_data(k);
    free(k->path);
    free(k->msg), free(k->cache), free(k->log); /* LINKF */
    memset(k, 0, sizeof *k);
}

static int place_scene(cc_scene *out, const cc_scene *in, double c, double sn, const double t[3]);
static uint64_t link_use_tick; /* SIZE3: the order links' meshes were last asked (least recently used first) */

const cc_scene *cc_link_rows(const cc_link *k) { return k->ifc ? &k->ifc->scene : &k->scene; }

/* SIZE3: a link read in place and not decoded shows a scene of EMPTY meshes (counts 0, arrays NULL; its shapes a
 * zeroed array of the scene's own): no structure reachable through a cc_scene ever holds counts without arrays, so
 * code that reads geometry from a scene it was not meant to have sees nothing, never a NULL array. The counts live
 * in the view file only (cc_link_triangles, mesh.c's served blocks). */
static int link_rows_view(cc_link *k) {
    if (!k->zshapes && k->vf->nshapes) {
        k->zshapes = arena_alloc(&k->scene.arena, k->vf->nshapes * sizeof *k->zshapes, 8);
        if (!k->zshapes) return -1;
    }
    if (k->zshapes) memset(k->zshapes, 0, k->vf->nshapes * sizeof *k->zshapes);
    k->scene.shapes = k->zshapes;
    for (size_t i = 0; i < k->scene.nelements; i++) memset(&k->scene.elements[i].mesh, 0, sizeof(cc_mesh));
    return 0;
}

uint64_t cc_link_triangles(const cc_link *k) {
    uint64_t tris = 0;
    if (k->vf && k->vf->lazy) { /* the counts the mapped read kept */
        for (size_t e = 0; e < k->vf->nelements; e++) tris += k->vf->elements[e].shape < 0 ? k->vf->elements[e].mesh.ni / 3 : 0;
        for (size_t e = 0; e < k->vf->nshapes; e++) tris += k->vf->shapes[e].ni / 3;
        return tris;
    }
    const cc_scene *sc = cc_link_rows(k);
    for (size_t e = 0; e < sc->nelements; e++) tris += sc->elements[e].shape < 0 ? sc->elements[e].mesh.ni / 3 : 0;
    for (size_t e = 0; e < sc->nshapes; e++) tris += sc->shapes[e].ni / 3; /* each shared shape once */
    return tris;
}

int cc_link_resident(cc_link *k) {
    if (!k->vf || !k->vf->lazy) return 0;
    k->mused = ++link_use_tick;
    if (!k->resident) {
        arena_init(&k->marena, 0);
        uint64_t b = 0;
        const cc_platform *pf = k->map_pf;
        double t0 = pf ? pf->now_ms(pf->ctx) : 0;
        if (cc_viewfile_materialize(k->vf, &k->marena, &b)) { /* out of memory, or the cache changed under its mapping */
            arena_free(&k->marena);
            for (size_t i = 0; i < k->vf->nelements; i++) memset(&k->vf->elements[i].mesh, 0, sizeof(cc_mesh));
            for (size_t i = 0; i < k->vf->nshapes; i++) memset(&k->vf->shapes[i], 0, sizeof(cc_mesh));
            link_rows_view(k);
            return -1; /* draws nothing from now on: its counts are 0 */
        }
        for (size_t i = 0; i < k->scene.nelements && i < k->vf->nelements; i++) k->scene.elements[i].mesh = k->vf->elements[i].mesh;
        k->scene.shapes = k->vf->shapes; /* decoded */
        k->resident = 1, k->mbytes = b;
        if (pf) cc_log(pf, "bimble-ccore: link %s: its meshes decoded (%.1f MB in %.0f ms)", k->path, (double)b / 1e6, pf->now_ms(pf->ctx) - t0);
    }
    if (k->pwant && !k->placed) {
        if (place_scene(&k->pscene, &k->scene, k->pc, k->ps, k->pt)) cc_scene_free(&k->pscene);
        else k->placed = 1;
    }
    return 0;
}

int cc_link_box_lazy(const cc_link *k, const double org[3], double lo[3], double hi[3]) {
    if (!k->kind || !k->vf || !k->vf->lazy || k->resident || !(k->vf->box[0] <= k->vf->box[3])) return 0;
    const double *O = k->vf->origin;
    double c = k->pwant ? k->pc : 1.0, sn = k->pwant ? k->ps : 0.0, t[3] = {0, 0, 0};
    if (k->pwant) memcpy(t, k->pt, sizeof t);
    double P[3] = {c * O[0] - sn * O[1] + t[0], sn * O[0] + c * O[1] + t[1], O[2] + t[2]}; /* place_scene's origin */
    for (int q = 0; q < 3; q++) lo[q] = 1.0 / 0.0, hi[q] = -1.0 / 0.0;
    for (int i = 0; i < 4; i++) {
        double x = i & 1 ? k->vf->box[3] : k->vf->box[0], y = i & 2 ? k->vf->box[4] : k->vf->box[1];
        double w[2] = {c * x - sn * y + P[0] - org[0], sn * x + c * y + P[1] - org[1]};
        for (int q = 0; q < 2; q++) lo[q] = w[q] < lo[q] ? w[q] : lo[q], hi[q] = w[q] > hi[q] ? w[q] : hi[q];
    }
    lo[2] = k->vf->box[2] + P[2] - org[2], hi[2] = k->vf->box[5] + P[2] - org[2];
    return 1;
}

const cc_scene *cc_link_raw_scene(const cc_link *k) {
    if (k->vf && k->vf->lazy) cc_link_resident((cc_link *)k); /* SIZE3: decoded on demand (the session's calls are serial) */
    return cc_link_rows(k);
}
const cc_scene *cc_link_scene(const cc_link *k) {
    const cc_scene *raw = cc_link_raw_scene(k);
    return k->placed ? &k->pscene : raw; /* LINKF */
}

void cc_link_id(const cc_link *k, size_t i, char id[22]) {
    const cc_scene *sc = cc_link_rows(k); /* SIZE3: ids only (the placed scene's rows are the same) */
    if (k->kind == CC_LINK_IFC) { /* cc_ifc_scene_bmsh: the file's text, '0'-padded to 22 */
        cc_str g = k->ifc->info[sc->elements[i].model_index].global_id;
        for (size_t j = 0; j < 22; j++) id[j] = j < g.n ? g.p[j] : '0';
    } else memcpy(id, sc->elements[i].id.c, 22);
}

long cc_link_find(const cc_link *k, const char *id) {
    const cc_scene *sc = cc_link_rows(k);
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

static int links_add_queued(cc_session *s, const char *path, int *at, cc_error *err); /* LINKF */
static int queued_kind(const char *path);

int cc_links_add(cc_session *s, const char *path, cc_error *err) {
    if (queued_kind(path)) return links_add_queued(s, path, NULL, err); /* LINKF */
    cc_link k;
    if (cc_link_load(s, path, &k, err)) return -1;
    int at = cc_links_find(s, k.name, strlen(k.name)); /* the same file again: in its place */
    if (at >= 0) {
        k.visible = s->links[at].visible;
        if (s->link_sel == at) s->link_sel = -1;
        keep_settings(&k, &s->links[at]); /* LINKF */
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
        k.uid = ++s->link_uid_next, k.pinned = 1, k.colour = -1; /* LINKF */
        s->links[s->nlinks++] = k;
    }
    const cc_scene *sc = cc_link_rows(&k);
    cc_log(s->pf, "bimble-ccore: linked %s: %zu elements", path, sc->nelements); /* lib.rs:1484 */
    s->links_gen++, s->links_mesh_gen++; /* LNK3D: the geometry changed */
    return 0;
}

int cc_links_reload(cc_session *s, int i, cc_error *err) {
    if (s->links[i].kind != CC_LINK_MODEL && queued_kind(s->links[i].path)) { /* LINKF: its cache, else read again */
        int at = i;
        char *p = malloc(strlen(s->links[i].path) + 1);
        if (!p) return snprintf(err->msg, sizeof err->msg, "out of memory"), -1;
        strcpy(p, s->links[i].path);
        int rc = links_add_queued(s, p, &at, err);
        free(p);
        return rc;
    }
    cc_link k;
    if (cc_link_load(s, s->links[i].path, &k, err)) return -1;
    k.visible = s->links[i].visible;
    if (s->link_sel == i) s->link_sel = -1;
    keep_settings(&k, &s->links[i]); /* LINKF */
    cc_link_free(&s->links[i]);
    s->links[i] = k;
    s->links_gen++, s->links_mesh_gen++;
    return 0;
}

void cc_links_remove(cc_session *s, int i) { /* lib.rs:1535 unlink */
    if (s->link_job && s->link_job_uid == s->links[i].uid) /* LINKF: its conversion stops, its result is dropped */
        cc_atomic_store(&s->link_job->pg.cancel, 1), s->link_job_uid = 0;
    cc_link_free(&s->links[i]);
    memmove(&s->links[i], &s->links[i + 1], (s->nlinks - (size_t)i - 1) * sizeof *s->links);
    s->nlinks--;
    s->link_sel = -1;
    s->links_gen++, s->links_mesh_gen++;
}

void cc_links_set_visible(cc_session *s, int i, int visible) {
    if (s->links[i].visible == !!visible) return;
    s->links[i].visible = !!visible;
    if (!visible && s->link_sel == i) s->link_sel = -1;
    s->links_gen++;
}

void cc_links_clear(cc_session *s) {
    free(s->links_saved), s->links_saved = NULL, s->links_saved_n = 0; /* LNK1 */
    if (s->link_job) cc_atomic_store(&s->link_job->pg.cancel, 1), s->link_job_uid = 0; /* LINKF */
    cc_links_undo_free(s);
    if (s->nlinks) s->links_gen++, s->links_mesh_gen++;
    for (size_t i = 0; i < s->nlinks; i++) cc_link_free(&s->links[i]);
    s->nlinks = 0;
    s->link_sel = -1;
    cc_notes_clear(s); /* M1LINKS: the notes go with the links */
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

/* LNK3D: query link_draw (links.h). */
void cc_links_draw_json(cc_session *s, JsonW *w) {
    jw_arr_begin(w);
    for (size_t i = 0; i < s->nlinks; i++) {
        const cc_link *k = &s->links[i];
        double c, sn, t[3], o[3];
        cc_link_placement(s, k, &c, &sn, t);
        const double *lo = k->kind ? cc_link_rows(k)->origin /* SIZE3: no mesh decoded */ : (const double[3]){0, 0, 0};
        o[0] = c * lo[0] - sn * lo[1] + t[0], o[1] = sn * lo[0] + c * lo[1] + t[1], o[2] = lo[2] + t[2];
        jw_obj_begin(w);
        jw_key(w, "colour"), jw_i64(w, k->colour);
        jw_key(w, "cos"), jw_f64(w, c + 0.0);
        jw_key(w, "name"), jw_str(w, k->name);
        jw_key(w, "origin"), jw_arr_begin(w);
        for (int q = 0; q < 3; q++) jw_f64(w, o[q] + 0.0); /* + 0.0: never -0.0 */
        jw_arr_end(w);
        jw_key(w, "sin"), jw_f64(w, sn + 0.0);
        jw_key(w, "visible"), jw_bool(w, k->visible && k->kind);
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
    const cc_scene *sc = cc_link_scene(k); /* SIZE3: decoded (its volume below reads its mesh) */
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
    size_t at = out->len;
    if (ro_element_json(sc, (size_t)i, cls, nm, storey, material, t, k->name, out)) return -1;
    if (out->len > at && out->p[out->len - 1] == '}') { /* M1LINKS: its property sets (m1links.h) */
        out->len--;
        cc_link_psets_json(s, k, (size_t)i, out);
        buf_putc(out, '}');
    }
    return out->oom ? -1 : 0;
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
        buf_putc(b, k->visible ? '1' : '0');
        /* LINKF: the placement, pin and colour are saved too */
        buf_append(b, &k->origin, sizeof k->origin), buf_append(b, k->move_mm, sizeof k->move_mm);
        buf_append(b, &k->rotation_deg, sizeof k->rotation_deg), buf_putc(b, k->pinned ? 'p' : 'u');
        buf_append(b, &k->colour, sizeof k->colour), buf_putc(b, '\n');
    }
    cc_notes_sig(s, b); /* M1LINKS: a note added or removed is an unsaved change */
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
    k.state = CC_LS_MISSING, k.uid = ++s->link_uid_next, k.pinned = 1, k.colour = -1; /* LINKF */
    s->links[s->nlinks++] = k;
    s->links_gen++, s->links_mesh_gen++;
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
        if (s->nlinks > before || ok) { /* LINKF: the saved placement, pin and colour */
            const char *nm = cc_path_last_sep(abs);
            nm = nm ? nm + 1 : abs;
            int li = cc_links_find(s, nm, strlen(nm));
            if (li < 0 && s->nlinks > before) li = (int)s->nlinks - 1;
            if (li >= 0) {
                cc_link *k = &s->links[li];
                k->origin = e->origin, k->rotation_deg = e->rotation_deg, k->pinned = !e->unpinned, k->colour = e->has_colour ? e->colour : -1;
                memcpy(k->move_mm, e->move_mm, sizeof k->move_mm);
            }
        }
        if (!ok) {
            cc_log(s->pf, "bimble-ccore: linked model %s not found (%s): kept as missing", abs,
                   s->pf->files ? err.msg : "no files here");
            if (add_missing(s, abs, !e->hidden)) {
                free(abs), free(rel);
                return -1;
            }
            cc_link *k = &s->links[s->nlinks - 1]; /* LINKF: kept as saved */
            k->origin = e->origin, k->rotation_deg = e->rotation_deg, k->pinned = !e->unpinned, k->colour = e->has_colour ? e->colour : -1;
            memcpy(k->move_mm, e->move_mm, sizeof k->move_mm);
        }
        free(abs), free(rel);
    }
    if (cc_notes_from_model(s)) return -1; /* M1LINKS: the notes the file keeps on linked elements */
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
        /* LINKF: its placement, pin and colour (model.h; each left out of the file at its default) */
        l[i].origin = k->origin, l[i].rotation_deg = k->rotation_deg, l[i].unpinned = !k->pinned;
        l[i].has_colour = k->colour >= 0, l[i].colour = k->colour >= 0 ? k->colour : 0;
        memcpy(l[i].move_mm, k->move_mm, sizeof l[i].move_mm);
        if (cc_notes_to_model(s, &m->arena, &l[i], k->name)) return -1; /* M1LINKS */
    }
    m->links = l, m->nlinks = s->nlinks;
    return 0;
}

int cc_links_locate(cc_session *s, int i, const char *path, cc_error *err) {
    if (queued_kind(path)) { /* LINKF: in its place, through the queue */
        int at = i;
        return links_add_queued(s, path, &at, err);
    }
    cc_link k;
    if (cc_link_load(s, path, &k, err)) return -1;
    k.visible = s->links[i].visible;
    if (s->link_sel == i) s->link_sel = -1;
    keep_settings(&k, &s->links[i]); /* LINKF */
    cc_link_free(&s->links[i]);
    s->links[i] = k;
    s->links_gen++, s->links_mesh_gen++;
    cc_log(s->pf, "bimble-ccore: linked %s: %zu elements", path, cc_link_rows(&k)->nelements);
    return 0;
}
/* ---- end LNK1 ---- */

/* ---- LINKF: conversions in turn, link a folder, its undo step (links.h, link_cache.h) ---- */

const char *const CC_LINK_STATE_NAME[] = {"ready", "missing", "queued", "converting", "failed", "refused", "unloaded"};
const char *const CC_LINK_ORIGIN_NAME[] = {"file", "origin", "base_point", "survey_point"};

static int ends_ci(const char *p, const char *suffix) {
    size_t n = strlen(p), k = strlen(suffix);
    if (n < k) return 0;
    for (size_t i = 0; i < k; i++) {
        char c = p[n - k + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suffix[i]) return 0;
    }
    return 1;
}

/* The files a conversion job reads (link_cache.h): an IFC, a .bimbleview. */
static int queued_kind(const char *path) { return ends_ci(path, ".ifc") || ends_ci(path, ".bimbleview"); }

static char *dup_z(const char *p) {
    if (!p || !*p) return NULL;
    char *q = malloc(strlen(p) + 1);
    if (q) strcpy(q, p);
    return q;
}

/* What the user set on a link, kept when it is read again in its place. */
static void keep_settings(cc_link *k, const cc_link *old) {
    k->uid = old->uid, k->origin = old->origin, k->rotation_deg = old->rotation_deg, k->pinned = old->pinned;
    k->colour = old->colour;
    memcpy(k->move_mm, old->move_mm, sizeof k->move_mm);
}

static int link_by_uid(const cc_session *s, uint32_t uid) {
    for (size_t i = 0; uid && i < s->nlinks; i++)
        if (s->links[i].uid == uid) return (int)i;
    return -1;
}

static void link_geo(cc_link *k);

/* The finished job's results into link k (its data was dropped when it was queued). */
static void take(cc_session *s, cc_link *k, cc_link_job *j) {
    free(k->msg), free(k->cache), free(k->log);
    k->msg = k->cache = k->log = NULL;
    k->progress = 1000;
    k->bytes = j->bytes;
    k->load_ms = j->ms;
    if (j->rc != CC_JOB_OK) {
        k->state = j->rc == CC_JOB_REFUSED ? CC_LS_REFUSED : CC_LS_FAILED;
        k->msg = dup_z(j->rc == CC_JOB_CANCELLED ? "cancelled" : j->err.msg);
        cc_log(s->pf, "bimble-ccore: link %s: %s", k->path, j->rc == CC_JOB_CANCELLED ? "cancelled" : j->err.msg);
        return;
    }
    cc_error err;
    if (j->vf) {
        if (cc_view_scene(&k->scene, j->vf, &err)) {
            cc_scene_free(&k->scene);
            memset(&k->scene, 0, sizeof k->scene);
            k->state = CC_LS_FAILED, k->msg = dup_z("out of memory");
            return;
        }
        k->vf = j->vf, j->vf = NULL, k->kind = CC_LINK_VIEW;
        k->map = j->map, k->map_pf = j->pf, j->map = NULL; /* SIZE3: read in place over its mapped cache */
        k->zshapes = NULL;
        if (k->vf->lazy && link_rows_view(k)) { /* SIZE3: its scene empty until decoded */
            k->state = CC_LS_FAILED, k->msg = dup_z("out of memory");
            return;
        }
    } else k->ifc = j->ifc, j->ifc = NULL, k->kind = CC_LINK_IFC;
    k->psets = j->psets, k->npsets = j->npsets, j->psets = NULL, j->npsets = 0; /* M1LINKS: a small IFC's */
    k->state = CC_LS_READY;
    link_geo(k); /* LINKF: where the file says it is */
    k->cache = dup_z(j->cache), k->log = dup_z(j->log), k->msg = dup_z(j->note);
    k->cache_mtime_s = j->cache_mtime_s, k->from_cache = j->from_cache;
    cc_log(s->pf, "bimble-ccore: linked %s: %zu elements (%s, %.0f ms)", k->path, cc_link_rows(k)->nelements,
           j->from_cache ? "its cache" : j->cache[0] ? "read, cache written" : "read", j->ms);
}

/* Starts link i's conversion: on the carrier's thread, else here. */
static void start(cc_session *s, int i) {
    cc_link *k = &s->links[i];
    cc_link_job *j = cc_link_job_new(s->pf, k->path);
    if (!j) {
        k->state = CC_LS_FAILED, free(k->msg), k->msg = dup_z("out of memory");
        return;
    }
    j->cache_min = s->link_cache_min, j->no_beside = s->link_no_beside;
    k->state = CC_LS_CONVERTING, k->progress = 0;
    s->link_job = j, s->link_job_uid = k->uid;
    if (!s->pf->spawn || s->pf->spawn(s->pf->ctx, cc_link_job_run, j)) cc_link_job_run(j);
}

int cc_links_busy(const cc_session *s) {
    if (s->link_job) return 1;
    for (size_t i = 0; i < s->nlinks; i++)
        if (s->links[i].state == CC_LS_QUEUED) return 1;
    return 0;
}

int cc_links_wants_tick(const cc_session *s) {
    if (cc_links_busy(s)) return 1;
    for (size_t i = 0; i < s->nlinks; i++) /* SIZE3 (F): a link refused for memory, retried once by itself */
        if (s->links[i].state == CC_LS_REFUSED && !s->links[i].auto_retried && s->pf->mem_available) return 1;
    uint64_t total = 0; /* SIZE3: decoded meshes past the budget, dropped after CC_LINK_KEEP_MS */
    for (size_t i = 0; i < s->nlinks; i++) total += s->links[i].resident ? s->links[i].mbytes : 0;
    return total > s->link_mesh_budget;
}

/* SIZE3: decoded link meshes past the budget dropped, the least recently used first, none used in the
 * last CC_LINK_KEEP_MS (a measure over every link keeps them while it runs). Called at each tick. */
static void links_trim(cc_session *s) {
    double now = s->pf->now_ms(s->pf->ctx);
    uint64_t total = 0;
    int trimmed = 0;
    for (size_t i = 0; i < s->nlinks; i++) {
        cc_link *k = &s->links[i];
        if (!k->resident) continue;
        if (k->mused != (uint64_t)k->mused_ms_tick) k->mused_ms_tick = k->mused, k->mused_ms = now; /* used since the last tick */
        total += k->mbytes;
    }
    while (total > s->link_mesh_budget) {
        cc_link *old = NULL;
        for (size_t i = 0; i < s->nlinks; i++) {
            cc_link *k = &s->links[i];
            if (k->resident && now - k->mused_ms >= s->link_keep_ms && (!old || k->mused < old->mused)) old = k;
        }
        if (!old) break;
        total -= old->mbytes;
        cc_log(s->pf, "bimble-ccore: link %s: its meshes dropped (%.1f MB; decoded again when needed)", old->path, (double)old->mbytes / 1e6);
        link_evict(old);
        trimmed = 1;
    }
    if (trimmed && s->pf->trim) s->pf->trim(s->pf->ctx);
}

int cc_links_tick(cc_session *s) {
    int ch = 0;
    links_trim(s);
    cc_link_job *j = s->link_job;
    if (j) {
        int at = link_by_uid(s, s->link_job_uid);
        if (!cc_link_job_done(j)) {
            /* the progress is served by query links_panel (the page polls it while a link converts):
             * no change published for it, so the page's full refresh runs once per finished link */
            if (at >= 0) s->links[at].progress = (unsigned)cc_atomic_load(&j->pg.done);
            return 0;
        } else {
            if (at >= 0) take(s, &s->links[at], j);
            cc_link_job_free(j);
            s->link_job = NULL, s->link_job_uid = 0;
            ch = 1;
        }
    }
    if (!s->link_job && s->pf->mem_available && s->pf->stat_path) { /* SIZE2: a link refused for memory, queued again once
                                                                     * when the memory it needs is free now (checked every 2 s) */
        double now = s->pf->now_ms(s->pf->ctx);
        if (now - s->links_retry_at >= 2000.0) {
            s->links_retry_at = now;
            for (size_t i = 0; i < s->nlinks; i++) {
                cc_link *k = &s->links[i];
                cc_pf_stat st;
                cc_error e;
                if (k->state != CC_LS_REFUSED || k->auto_retried || !k->path || s->pf->stat_path(s->pf->ctx, k->path, 1, &st) != CC_PF_OK) continue;
                if (cc_session_fits(s->pf, k->path, st.size, &e) == 0) {
                    k->auto_retried = 1, k->state = CC_LS_QUEUED, k->bytes = st.size;
                    cc_log(s->pf, "bimble-ccore: link %s: memory free now, converted again", k->path);
                    ch = 1;
                    break;
                }
            }
        }
    }
    if (!s->link_job) { /* the next: the smallest queued, so something appears soon */
        int best = -1;
        for (size_t i = 0; i < s->nlinks; i++)
            if (s->links[i].state == CC_LS_QUEUED && (best < 0 || s->links[i].bytes < s->links[best].bytes)) best = (int)i;
        if (best >= 0) start(s, best); /* "converting": no change published (above) */
        if (s->link_job && cc_link_job_done(s->link_job)) /* ran here (no thread): take it now */
            return cc_links_tick(s), 1;
    }
    if (ch) s->links_gen++, s->links_mesh_gen++;
    return ch;
}

/* Queues `path` as a link: at *at (in its place) when at is given, else in the place of a link of
 * the same name, else at the end. Small files and carriers without a thread: read now, and on a
 * failure nothing changes (-1 with the reader's error), as before. */
static int links_add_queued(cc_session *s, const char *path, int *at, cc_error *err) {
    err->offset = CC_NO_OFFSET;
    err->msg[0] = 0;
    uint64_t bytes = 0;
    if (s->pf->files && s->pf->stat_path) {
        cc_pf_stat st;
        if (s->pf->stat_path(s->pf->ctx, path, 1, &st) != CC_PF_OK || st.kind != CC_PF_FILE) {
            snprintf(err->msg, sizeof err->msg, "%s: cannot open", path);
            return -1;
        }
        bytes = st.size;
    }
    int now = !s->pf->spawn || bytes < s->link_cache_min || !s->pf->files;
    if (now) { /* read here: as cc_link_load, through the job (the cache when it is worth one) */
        cc_link_job *j = cc_link_job_new(s->pf, path);
        if (!j) return snprintf(err->msg, sizeof err->msg, "out of memory"), -1;
        j->cache_min = s->link_cache_min, j->no_beside = s->link_no_beside;
        cc_link_job_run(j);
        if (j->rc != CC_JOB_OK) {
            *err = j->err;
            if (j->rc == CC_JOB_CANCELLED) snprintf(err->msg, sizeof err->msg, "cancelled");
            cc_link_job_free(j);
            return -1;
        }
        cc_link k;
        memset(&k, 0, sizeof k);
        k.path = dup_z(path);
        if (!k.path) return cc_link_job_free(j), snprintf(err->msg, sizeof err->msg, "out of memory"), -1;
        const char *sl = cc_path_last_sep(k.path);
        k.name = sl ? sl + 1 : k.path;
        k.visible = 1, k.pinned = 1, k.colour = -1;
        take(s, &k, j);
        cc_link_job_free(j);
        if (k.state != CC_LS_READY) {
            snprintf(err->msg, sizeof err->msg, "%s", k.msg ? k.msg : "failed");
            cc_link_free(&k);
            return -1;
        }
        int i = at ? *at : cc_links_find(s, k.name, strlen(k.name));
        if (i >= 0) {
            k.visible = s->links[i].visible;
            if (s->link_sel == i) s->link_sel = -1;
            if (s->link_job && s->link_job_uid == s->links[i].uid) cc_atomic_store(&s->link_job->pg.cancel, 1), s->link_job_uid = 0;
            keep_settings(&k, &s->links[i]);
            cc_link_free(&s->links[i]);
            s->links[i] = k;
        } else {
            if (s->nlinks >= CC_MAX_LINKS) {
                cc_link_free(&k);
                snprintf(err->msg, sizeof err->msg, "at most %d linked models", CC_MAX_LINKS);
                return -1;
            }
            if (s->nlinks == s->caplinks) {
                size_t c = s->caplinks ? 2 * s->caplinks : 4;
                cc_link *p = realloc(s->links, c * sizeof *p);
                if (!p) return cc_link_free(&k), snprintf(err->msg, sizeof err->msg, "out of memory"), -1;
                s->links = p, s->caplinks = c;
            }
            k.uid = ++s->link_uid_next;
            s->links[s->nlinks++] = k;
        }
        s->links_gen++, s->links_mesh_gen++;
        return 0;
    }
    /* queued: the row now, the scene when its turn came */
    int i = at ? *at : -1;
    if (i < 0) {
        const char *sl = cc_path_last_sep(path);
        const char *nm = sl ? sl + 1 : path;
        i = cc_links_find(s, nm, strlen(nm));
    }
    char *p = dup_z(path);
    if (!p) return snprintf(err->msg, sizeof err->msg, "out of memory"), -1;
    if (i < 0) {
        if (s->nlinks >= CC_MAX_LINKS) {
            free(p);
            snprintf(err->msg, sizeof err->msg, "at most %d linked models", CC_MAX_LINKS);
            return -1;
        }
        if (s->nlinks == s->caplinks) {
            size_t c = s->caplinks ? 2 * s->caplinks : 4;
            cc_link *q = realloc(s->links, c * sizeof *q);
            if (!q) return free(p), snprintf(err->msg, sizeof err->msg, "out of memory"), -1;
            s->links = q, s->caplinks = c;
        }
        i = (int)s->nlinks++;
        memset(&s->links[i], 0, sizeof s->links[i]);
        s->links[i].uid = ++s->link_uid_next, s->links[i].visible = 1, s->links[i].pinned = 1, s->links[i].colour = -1;
    } else {
        if (s->link_sel == i) s->link_sel = -1;
        if (s->link_job && s->link_job_uid == s->links[i].uid) cc_atomic_store(&s->link_job->pg.cancel, 1), s->link_job_uid = 0;
        link_drop_data(&s->links[i]);
        free(s->links[i].path), free(s->links[i].msg), free(s->links[i].cache), free(s->links[i].log);
        s->links[i].msg = s->links[i].cache = s->links[i].log = NULL;
    }
    cc_link *k = &s->links[i];
    k->path = p;
    const char *sl = cc_path_last_sep(k->path);
    k->name = sl ? sl + 1 : k->path;
    k->state = CC_LS_QUEUED, k->progress = 0, k->bytes = bytes;
    s->links_gen++, s->links_mesh_gen++;
    cc_links_tick(s); /* starts it when nothing runs */
    return 0;
}

void cc_links_cancel(cc_session *s, int i) {
    cc_link *k = &s->links[i];
    if (k->state == CC_LS_QUEUED) {
        k->state = CC_LS_FAILED, free(k->msg), k->msg = dup_z("cancelled");
        s->links_gen++, s->links_mesh_gen++;
    } else if (k->state == CC_LS_CONVERTING && s->link_job && s->link_job_uid == k->uid)
        cc_atomic_store(&s->link_job->pg.cancel, 1); /* the job ends "cancelled" (a batch later) */
}

void cc_links_unload(cc_session *s, int i) {
    cc_link *k = &s->links[i];
    if (k->state == CC_LS_MISSING) return;
    if (k->state == CC_LS_CONVERTING && s->link_job && s->link_job_uid == k->uid)
        cc_atomic_store(&s->link_job->pg.cancel, 1), s->link_job_uid = 0;
    if (s->link_sel == i) s->link_sel = -1;
    link_drop_data(k);
    k->state = CC_LS_UNLOADED;
    s->links_gen++, s->links_mesh_gen++;
}

int cc_links_load(cc_session *s, int i, cc_error *err) {
    if (s->links[i].state == CC_LS_READY) return 0;
    if (s->links[i].state == CC_LS_QUEUED || s->links[i].state == CC_LS_CONVERTING) return 0;
    return cc_links_reload(s, i, err);
}

/* ---- link_folder: one undo step ---- */
struct cc_links_undo {
    char *paths; /* NUL-separated */
    size_t len, npaths;
    size_t hist_nundo; /* the model's undo steps when it was taken: undone only when it is the last step */
};

static void undo_entry_free(struct cc_links_undo *u) { free(u->paths); }

static void cc_links_undo_free(cc_session *s) {
    for (size_t i = 0; i < s->nlinks_undo; i++) undo_entry_free(&s->links_undo[i]);
    for (size_t i = 0; i < s->nlinks_redo; i++) undo_entry_free(&s->links_redo[i]);
    s->nlinks_undo = s->nlinks_redo = 0;
}

void cc_links_session_free(cc_session *s) {
    if (s->link_job) cc_link_job_orphan(s->link_job), s->link_job = NULL;
    cc_links_undo_free(s);
    free(s->links_undo), free(s->links_redo);
    s->links_undo = s->links_redo = NULL;
}

void cc_links_undo_clear_redo(cc_session *s) {
    for (size_t i = 0; i < s->nlinks_redo; i++) undo_entry_free(&s->links_redo[i]);
    s->nlinks_redo = 0;
}

static int push_entry(struct cc_links_undo **v, size_t *n, size_t *cap, struct cc_links_undo e) {
    if (*n == *cap) {
        size_t c = *cap ? 2 * *cap : 4;
        struct cc_links_undo *p = realloc(*v, c * sizeof *p);
        if (!p) return -1;
        *v = p, *cap = c;
    }
    (*v)[(*n)++] = e;
    return 0;
}

int cc_links_can_undo(const cc_session *s, int undo) {
    size_t hn = s->open ? s->hist.nundo : 0;
    if (undo) return s->nlinks_undo && s->links_undo[s->nlinks_undo - 1].hist_nundo == hn;
    return s->nlinks_redo && s->links_redo[s->nlinks_redo - 1].hist_nundo == hn;
}

int cc_links_undo_step(cc_session *s, int undo) {
    if (!cc_links_can_undo(s, undo)) return 0;
    struct cc_links_undo e = undo ? s->links_undo[--s->nlinks_undo] : s->links_redo[--s->nlinks_redo];
    const char *p = e.paths;
    for (size_t k = 0; k < e.npaths; k++, p += strlen(p) + 1) {
        if (undo) {
            for (size_t i = 0; i < s->nlinks; i++)
                if (s->links[i].path && !strcmp(s->links[i].path, p)) {
                    cc_links_remove(s, (int)i);
                    break;
                }
        } else {
            cc_error err;
            if (cc_links_add(s, p, &err)) cc_log(s->pf, "bimble-ccore: redo link %s: %s", p, err.msg);
        }
    }
    if (push_entry(undo ? &s->links_redo : &s->links_undo, undo ? &s->nlinks_redo : &s->nlinks_undo,
                   undo ? &s->caplinks_redo : &s->caplinks_undo, e))
        undo_entry_free(&e);
    s->links_gen++, s->links_mesh_gen++;
    return 1;
}

static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* The files of `dir` to link, sorted: every *.ifc, and each *.bimbleview that is not an IFC's cache
 * (<x>.ifc.bimbleview with <x>.ifc there). Into `names` (malloc'd), recursion bounded. */
static int folder_files(cc_session *s, const char *dir, int depth, int recursive, char ***names, size_t *n, size_t *cap) {
    char *list = NULL;
    size_t ln = 0;
    if (!s->pf->list_dir || s->pf->list_dir(s->pf->ctx, dir, (size_t)16 << 20, &list, &ln) != CC_PF_OK) return -1;
    int rc = 0;
    for (size_t o = 0; o < ln && !rc; o += strlen(list + o) + 1) {
        const char *e = list + o + 1;
        int is_dir = list[o] == 'd';
        if (e[0] == '.') continue; /* hidden */
        size_t need = strlen(dir) + 1 + strlen(e) + 1;
        char *full = malloc(need);
        if (!full) { rc = -1; break; }
        snprintf(full, need, "%s/%s", dir, e);
        if (is_dir) {
            if (recursive && depth < 8) rc = folder_files(s, full, depth + 1, recursive, names, n, cap);
            free(full);
            continue;
        }
        int take_it = ends_ci(e, ".ifc");
        if (!take_it && ends_ci(e, ".bimbleview")) {
            size_t el = strlen(full);
            take_it = 1;
            if (el > 15 && ends_ci(full + el - 15, ".ifc.bimbleview")) { /* a cache: its IFC there? */
                full[el - 11] = 0;
                take_it = !s->pf->exists(s->pf->ctx, full);
                full[el - 11] = '.';
            }
        }
        if (!take_it) { free(full); continue; }
        if (*n == *cap) {
            size_t c = *cap ? 2 * *cap : 16;
            char **p = realloc(*names, c * sizeof *p);
            if (!p) { free(full); rc = -1; break; }
            *names = p, *cap = c;
        }
        (*names)[(*n)++] = full;
    }
    free(list);
    return rc;
}

int cc_links_add_folder(cc_session *s, const char *dir, int recursive, size_t *added, cc_error *err) {
    err->offset = CC_NO_OFFSET;
    err->msg[0] = 0;
    *added = 0;
    char **names = NULL;
    size_t n = 0, cap = 0;
    char real[4096];
    if (s->pf->real_path && !s->pf->real_path(s->pf->ctx, dir, real, sizeof real)) dir = real;
    if (folder_files(s, dir, 0, recursive, &names, &n, &cap)) {
        for (size_t i = 0; i < n; i++) free(names[i]);
        free(names);
        snprintf(err->msg, sizeof err->msg, "%s: not a folder that can be listed", dir);
        return -1;
    }
    if (n) qsort(names, n, sizeof *names, name_cmp);
    Buf done;
    buf_init(&done);
    size_t nd = 0;
    int rc = 0;
    if (!n) snprintf(err->msg, sizeof err->msg, "%s: no .ifc or .bimbleview file in this folder", dir), rc = -1;
    for (size_t i = 0; i < n && !rc; i++) {
        cc_error e;
        int before = cc_links_find(s, cc_path_last_sep(names[i]) + 1, strlen(cc_path_last_sep(names[i]) + 1));
        if (before < 0 && s->nlinks >= CC_MAX_LINKS) {
            snprintf(err->msg, sizeof err->msg, "at most %d linked models: %zu of the folder's %zu files linked", CC_MAX_LINKS, nd, n);
            break;
        }
        if (cc_links_add(s, names[i], &e)) { /* a small file read now that failed: a failed row, the others go on */
            cc_log(s->pf, "bimble-ccore: link_folder %s: %s", names[i], e.msg);
            continue;
        }
        buf_puts(&done, names[i]), buf_putc(&done, 0), nd++;
    }
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
    if (nd && !done.oom) {
        struct cc_links_undo u = {malloc(done.len), done.len, nd, s->open ? s->hist.nundo : 0};
        if (u.paths) {
            memcpy(u.paths, done.p, done.len);
            if (push_entry(&s->links_undo, &s->nlinks_undo, &s->caplinks_undo, u)) free(u.paths);
            else cc_links_undo_clear_redo(s);
        }
    }
    buf_free(&done);
    *added = nd;
    return nd ? 0 : -1;
}
/* ---- end LINKF ---- */

/* ---- LINKF: positioning (links.h cc_link_placement) ---- */

static const double PI = 3.14159265358979323846;

/* The georeference of a view file's GEOR (georef.h's file form) into k's geo fields. */
static void geo_from_json(cc_link *k, const char *p, size_t n) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = jr_parse(&a, p, n, &je);
    if (v && v->type == JR_OBJ) {
        const JrVal *e = jr_get(v, "east_m"), *no = jr_get(v, "north_m"), *h = jr_get(v, "height_m"), *r = jr_get(v, "map_rotation_deg");
        const JrVal *c = jr_get(v, "crs"), *sp = jr_get(v, "survey_point_mm");
        if (e && e->type == JR_NUM) k->geo_e = e->num;
        if (no && no->type == JR_NUM) k->geo_n = no->num;
        if (h && h->type == JR_NUM) k->geo_h = h->num;
        if (r && r->type == JR_NUM) k->geo_rot = r->num;
        if (c && c->type == JR_STR && c->n < sizeof k->geo_crs) memcpy(k->geo_crs, c->s, c->n), k->geo_crs[c->n] = 0;
        if (sp && sp->type == JR_ARR && sp->n == 3)
            for (int i = 0; i < 3; i++)
                if (sp->items[i].type == JR_NUM) k->survey_mm[i] = sp->items[i].num;
    }
    arena_free(&a);
}

static void geo_from(cc_link *k, const cc_georef *g) {
    k->geo_e = g->east_m, k->geo_n = g->north_m, k->geo_h = g->height_m, k->geo_rot = g->map_rotation_deg;
    if (g->has_crs && g->crs.n < sizeof k->geo_crs) memcpy(k->geo_crs, g->crs.p, g->crs.n), k->geo_crs[g->crs.n] = 0;
    if (g->has_survey_point) memcpy(k->survey_mm, g->survey_point_mm, sizeof k->survey_mm);
}

static int finite_all(const double *v, int n) {
    for (int i = 0; i < n; i++)
        if (!(v[i] > -1e15 && v[i] < 1e15)) return 0;
    return 1;
}

static void link_geo(cc_link *k) {
    k->geo_map = 0, k->geo_e = k->geo_n = k->geo_h = k->geo_rot = 0, k->geo_crs[0] = 0;
    memset(k->base_mm, 0, sizeof k->base_mm), memset(k->survey_mm, 0, sizeof k->survey_mm);
    if (k->ifc) geo_from(k, &k->ifc->georef), memcpy(k->base_mm, k->ifc->base_point_mm, sizeof k->base_mm);
    else if (k->vf) {
        if (k->vf->has_georef) geo_from_json(k, k->vf->georef.p, k->vf->georef.n);
        if (k->vf->has_source) cc_link_cache_base_point(k->vf->source.p, k->vf->source.n, k->base_mm);
    } else if (k->model) geo_from(k, &k->model->georef);
    double g[4] = {k->geo_e, k->geo_n, k->geo_h, k->geo_rot};
    if (!finite_all(g, 4) || !finite_all(k->base_mm, 3) || !finite_all(k->survey_mm, 3)) { /* untrusted: never a NaN placement */
        k->geo_e = k->geo_n = k->geo_h = k->geo_rot = 0;
        memset(k->base_mm, 0, sizeof k->base_mm), memset(k->survey_mm, 0, sizeof k->survey_mm);
    }
    k->geo_map = k->geo_e != 0.0 || k->geo_n != 0.0 || k->geo_rot != 0.0;
}

/* The host's: the open model's georeference (base point: its world origin, ADR 0075; survey point:
 * the georeference's, else the world origin), or the read-only IFC's (base point: its building). */
static void host_geo(const cc_session *s, cc_link *h) {
    memset(h, 0, sizeof *h);
    if (s->open) geo_from(h, &s->model.georef);
    else if (s->view == CC_VIEW_IFC && s->ifc) geo_from(h, &s->ifc->georef), memcpy(h->base_mm, s->ifc->base_point_mm, sizeof h->base_mm);
    else if (s->view == CC_VIEW_FILE && s->vf && s->vf->has_georef) geo_from_json(h, s->vf->georef.p, s->vf->georef.n);
    h->geo_map = h->geo_e != 0.0 || h->geo_n != 0.0 || h->geo_rot != 0.0;
}

static void rot2(double c, double sn, const double v[2], double out[2]) {
    double x = c * v[0] - sn * v[1], y = sn * v[0] + c * v[1];
    out[0] = x, out[1] = y;
}

void cc_link_placement(const cc_session *s, const cc_link *k, double *c, double *sn, double t[3]) {
    cc_link h;
    host_geo(s, &h);
    double ang = 0, tr[3] = {0, 0, 0}; /* the rule: w -> R(ang) w + tr */
    int rule = k->origin;
    if (rule == CC_LO_FILE && k->geo_map && h.geo_map && (!k->geo_crs[0] || !h.geo_crs[0] || !strcmp(k->geo_crs, h.geo_crs))) {
        /* survey_l(w) = R(a_l) w + S_l (mm); the host point with the same survey coordinates:
         * h = R(-a_h) (R(a_l) w + S_l - S_h) */
        double ah = h.geo_rot * PI / 180.0;
        ang = (k->geo_rot - h.geo_rot) * PI / 180.0;
        double d[2] = {(k->geo_e - h.geo_e) * 1000.0, (k->geo_n - h.geo_n) * 1000.0};
        rot2(cc_cos(-ah), cc_sin(-ah), d, tr);
        tr[2] = (k->geo_h - h.geo_h) * 1000.0;
    } else if (rule == CC_LO_BASE_POINT)
        for (int i = 0; i < 3; i++) tr[i] = h.base_mm[i] - k->base_mm[i];
    else if (rule == CC_LO_SURVEY_POINT)
        for (int i = 0; i < 3; i++) tr[i] = h.survey_mm[i] - k->survey_mm[i];
    /* then the manual turn about the host's Z and the offset */
    double am = k->rotation_deg * PI / 180.0, cm = cc_cos(am), sm = cc_sin(am);
    double a = ang + am;
    *c = cc_cos(a), *sn = cc_sin(a);
    if (a == 0.0) *c = 1.0, *sn = 0.0;
    rot2(cm, sm, tr, t);
    if (am == 0.0) t[0] = tr[0], t[1] = tr[1];
    t[0] += k->move_mm[0], t[1] += k->move_mm[1], t[2] = tr[2] + k->move_mm[2];
    for (int i = 0; i < 3; i++) t[i] += 0.0;
}

/* `in` placed: origin R o + t, own meshes and placements turned by R (shapes borrowed). */
static int place_scene(cc_scene *out, const cc_scene *in, double c, double sn, const double t[3]) {
    memset(out, 0, sizeof *out);
    arena_init(&out->arena, 0);
    double o[2];
    rot2(c, sn, in->origin, o);
    out->origin[0] = o[0] + t[0], out->origin[1] = o[1] + t[1], out->origin[2] = in->origin[2] + t[2];
    out->shapes = in->shapes, out->nshapes = in->nshapes;
    out->warnings = in->warnings, out->nwarnings = in->nwarnings;
    if (!in->nelements) return 0;
    out->elements = arena_alloc(&out->arena, in->nelements * sizeof *out->elements, 8);
    if (!out->elements) return -1;
    float cf = (float)c, sf = (float)sn;
    for (size_t i = 0; i < in->nelements; i++) {
        cc_scene_element *e = &out->elements[i];
        *e = in->elements[i];
        if (e->shape >= 0) {
            float *ax[4] = {e->to_scene.x, e->to_scene.y, e->to_scene.z, e->to_scene.t};
            for (int q = 0; q < 4; q++) {
                float x = ax[q][0], y = ax[q][1];
                ax[q][0] = cf * x - sf * y, ax[q][1] = sf * x + cf * y;
            }
            continue;
        }
        const cc_mesh *m = &in->elements[i].mesh;
        cc_mesh *d = &e->mesh;
        if (m->nv) {
            d->pos = arena_alloc(&out->arena, (size_t)m->nv * sizeof *d->pos, 4);
            if (!d->pos) return -1;
            for (uint32_t v = 0; v < m->nv; v++) {
                float x = m->pos[v][0], y = m->pos[v][1];
                d->pos[v][0] = cf * x - sf * y, d->pos[v][1] = sf * x + cf * y, d->pos[v][2] = m->pos[v][2];
            }
        }
        if (m->ne) {
            d->edges = arena_alloc(&out->arena, (size_t)m->ne * sizeof *d->edges, 4);
            if (!d->edges) return -1;
            for (uint32_t v = 0; v < m->ne; v++)
                for (int q = 0; q < 6; q += 3) {
                    float x = m->edges[v][q], y = m->edges[v][q + 1];
                    d->edges[v][q] = cf * x - sf * y, d->edges[v][q + 1] = sf * x + cf * y, d->edges[v][q + 2] = m->edges[v][q + 2];
                }
        }
        if (m->nn) {
            d->normals = arena_alloc(&out->arena, (size_t)m->nn * sizeof *d->normals, 1);
            if (!d->normals) return -1;
            for (uint32_t v = 0; v < m->nn; v++)
                for (int q = 0; q < 8; q += 4) {
                    float x = m->normals[v][q], y = m->normals[v][q + 1];
                    float rx = cf * x - sf * y, ry = sf * x + cf * y;
                    d->normals[v][q] = (int8_t)(rx < 0 ? rx - 0.5f : rx + 0.5f), d->normals[v][q + 1] = (int8_t)(ry < 0 ? ry - 0.5f : ry + 0.5f);
                    d->normals[v][q + 2] = m->normals[v][q + 2], d->normals[v][q + 3] = m->normals[v][q + 3];
                }
        }
    }
    out->nelements = in->nelements;
    return 0;
}

int cc_links_sync_placement(cc_session *s) {
    int ch = 0, rc = 0;
    for (size_t i = 0; i < s->nlinks; i++) {
        cc_link *k = &s->links[i];
        if (!k->kind) continue;
        double c, sn, t[3];
        cc_link_placement(s, k, &c, &sn, t);
        int ident = c == 1.0 && sn == 0.0 && t[0] == 0.0 && t[1] == 0.0 && t[2] == 0.0;
        if (k->vf && k->vf->lazy) { /* SIZE3: the placed scene is made when its meshes are decoded */
            if (ident ? !k->pwant : k->pwant && k->pc == c && k->ps == sn && !memcmp(k->pt, t, sizeof t)) continue;
            if (k->placed) cc_scene_free(&k->pscene), k->placed = 0;
            k->pwant = !ident, k->pc = c, k->ps = sn, memcpy(k->pt, t, sizeof t);
            if (k->pwant && k->resident && place_scene(&k->pscene, &k->scene, c, sn, t) == 0) k->placed = 1;
            else if (k->pwant && k->resident) cc_scene_free(&k->pscene), rc = -1;
            ch = 1;
            continue;
        }
        if (ident) {
            if (k->placed) cc_scene_free(&k->pscene), k->placed = 0, ch = 1;
            continue;
        }
        if (k->placed && k->pc == c && k->ps == sn && !memcmp(k->pt, t, sizeof t)) continue;
        if (k->placed) cc_scene_free(&k->pscene), k->placed = 0;
        if (place_scene(&k->pscene, cc_link_raw_scene(k), c, sn, t)) {
            cc_scene_free(&k->pscene);
            rc = -1;
        } else k->placed = 1, k->pc = c, k->ps = sn, memcpy(k->pt, t, sizeof t);
        ch = 1;
    }
    if (ch) s->links_gen++;
    return rc ? -1 : ch;
}
/* ---- end LINKF positioning ---- */

/* ---- LINKF: query link_log {name}: the conversion report of a link, read from the path the
 * session wrote it to (never a path the caller gives); at most 4 MiB. ---- */
int cc_link_log_text(cc_session *s, const char *body, size_t n, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *v = n ? jr_parse(&a, body, n, &je) : NULL, *nm = v && v->type == JR_OBJ ? jr_get(v, "name") : NULL;
    int rc = -1;
    if (!nm || nm->type != JR_STR) buf_puts(out, "link_log: send {\"name\": \"<linked file name>\"}");
    else {
        int li = cc_links_find(s, nm->s, nm->n);
        if (li < 0) buf_puts(out, "no linked model "), buf_append(out, nm->s, nm->n);
        else if (!s->links[li].log) buf_puts(out, "this link has no conversion report (read as it is: under 1 MiB, or not an IFC)");
        else {
            char *p = NULL;
            size_t k = 0;
            int st = s->pf->read_file(s->pf->ctx, s->links[li].log, (size_t)4 << 20, &p, &k);
            if (st != CC_PF_OK) buf_puts(out, "the report cannot be read: "), buf_puts(out, s->links[li].log);
            else if (jr_utf8_check(p, k) != -1) buf_puts(out, "the report is not UTF-8 text");
            else { /* {"name", "path", "text"} */
                JsonW w;
                jw_init(&w, out, 0);
                jw_obj_begin(&w);
                jw_key(&w, "name"), jw_str(&w, s->links[li].name);
                jw_key(&w, "path"), jw_str(&w, s->links[li].log);
                jw_key(&w, "text"), jw_str_n(&w, p, k);
                jw_obj_end(&w);
                rc = jw_finish(&w) == JW_OK && !out->oom ? 0 : -1;
            }
            free(p);
        }
    }
    arena_free(&a);
    return rc;
}

/* ---- LINKF: link_model {paths}: several files in one call, one undo step (links.h) ---- */

/* A row that says why a path of the list was not linked (state "refused", nothing drawn). */
static int add_refused(cc_session *s, const char *path, const char *why) {
    if (add_missing(s, path, 1)) return -1;
    cc_link *k = &s->links[s->nlinks - 1];
    k->state = CC_LS_REFUSED;
    k->msg = dup_z(why);
    return 0;
}

static int linked_already(const cc_session *s, const char *path) {
    char a[4096], b[4096];
    int ra = s->pf->real_path && !s->pf->real_path(s->pf->ctx, path, a, sizeof a);
    for (size_t i = 0; i < s->nlinks; i++) {
        const char *p = s->links[i].path;
        if (!p) continue;
        if (!strcmp(p, path)) return 1;
        if (ra && s->pf->real_path && !s->pf->real_path(s->pf->ctx, p, b, sizeof b) && !strcmp(a, b)) return 1;
    }
    return 0;
}

int cc_links_add_list(cc_session *s, const char *const *paths, size_t n, Buf *answer, cc_error *err) {
    err->offset = CC_NO_OFFSET;
    err->msg[0] = 0;
    Buf done, refused, skipped;
    buf_init(&done), buf_init(&refused), buf_init(&skipped);
    JsonW wr, ws;
    jw_init(&wr, &refused, 0), jw_init(&ws, &skipped, 0);
    jw_arr_begin(&wr), jw_arr_begin(&ws);
    size_t nd = 0;
    for (size_t i = 0; i < n; i++) {
        const char *p = paths[i];
        const char *why = NULL;
        cc_error e;
        e.msg[0] = 0;
        if (linked_already(s, p)) { /* a duplicate: skipped, said */
            jw_obj_begin(&ws), jw_key(&ws, "note"), jw_str(&ws, "already linked"), jw_key(&ws, "path"), jw_str(&ws, p), jw_obj_end(&ws);
            continue;
        }
        if (s->pf->path_allowed && !s->pf->path_allowed(s->pf->ctx, p))
            why = "outside the folders Bimble lists (query fs_roots)";
        else if (s->nlinks >= CC_MAX_LINKS) why = "at most 64 linked models";
        else if (cc_links_add(s, p, &e)) why = e.msg;
        if (why) {
            jw_obj_begin(&wr), jw_key(&wr, "path"), jw_str(&wr, p), jw_key(&wr, "reason"), jw_str(&wr, why), jw_obj_end(&wr);
            if (s->nlinks < CC_MAX_LINKS && !add_refused(s, p, why)) buf_puts(&done, p), buf_putc(&done, 0), nd++;
            continue;
        }
        buf_puts(&done, p), buf_putc(&done, 0), nd++;
    }
    jw_arr_end(&wr), jw_arr_end(&ws);
    jw_finish(&wr), jw_finish(&ws);
    int rc = 0;
    if (nd && !done.oom) { /* one undo step: what this call added (refused rows too) */
        struct cc_links_undo u = {malloc(done.len), done.len, nd, s->open ? s->hist.nundo : 0};
        if (u.paths) {
            memcpy(u.paths, done.p, done.len);
            if (push_entry(&s->links_undo, &s->nlinks_undo, &s->caplinks_undo, u)) free(u.paths);
            else cc_links_undo_clear_redo(s);
        }
    }
    /* the answer: the rows as they are now (queued, ready, refused), and what was skipped */
    buf_puts(answer, "{\"added\":"), buf_u64(answer, nd), buf_puts(answer, ",\"refused\":");
    buf_append(answer, refused.p, refused.len), buf_puts(answer, ",\"skipped\":"), buf_append(answer, skipped.p, skipped.len);
    buf_putc(answer, '}');
    if (answer->oom || done.oom || refused.oom || skipped.oom) rc = -1, snprintf(err->msg, sizeof err->msg, "out of memory");
    buf_free(&done), buf_free(&refused), buf_free(&skipped);
    return rc;
}
