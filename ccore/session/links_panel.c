/* The Links panel's rows (ticket LNK1): see links_panel.h. */
#include "links_panel.h"
#include <string.h>
#include "json_w.h"
#include "links.h"
#include "session.h"

/* Path::extension of a file name, lowercased (state.rs links_json's "kind"). */
static void ext_of(const char *name, char ext[32]) {
    ext[0] = 0;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || strlen(dot + 1) >= 32) return;
    size_t j = 0;
    for (; dot[1 + j]; j++) {
        char c = dot[1 + j];
        ext[j] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    ext[j] = 0;
}

int cc_links_panel_json(struct cc_session *s, Buf *out) {
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "models"), jw_arr_begin(&w);
    for (size_t i = 0; s->open || s->view ? i < s->nlinks : 0; i++) {
        const cc_link *k = &s->links[i];
        char ext[32];
        ext_of(k->name, ext);
        /* LINKF: the conversion's state and the link's numbers (links.h) */
        const cc_scene *sc = cc_link_rows(k); /* SIZE3: counts only (a link read in place is not decoded for it) */
        uint64_t tris = 0;
        for (size_t e = 0; e < sc->nelements; e++) {
            const cc_mesh *lm = cc_scene_local_mesh(sc, e);
            if (lm && (sc->elements[e].shape < 0)) tris += lm->ni / 3;
        }
        for (size_t e = 0; e < sc->nshapes; e++) tris += sc->shapes[e].ni / 3; /* each shared shape once */
        jw_obj_begin(&w);
        jw_key(&w, "bytes"), jw_u64(&w, k->bytes);
        jw_key(&w, "cache"), k->cache ? jw_str(&w, k->cache) : jw_null(&w);
        jw_key(&w, "cache_mtime_s"), jw_i64(&w, k->cache ? k->cache_mtime_s : 0);
        jw_key(&w, "colour"), k->colour >= 0 ? jw_u64(&w, (uint64_t)k->colour) : jw_null(&w);
        jw_key(&w, "elements"), jw_u64(&w, sc->nelements);
        jw_key(&w, "from_cache"), jw_bool(&w, k->from_cache);
        jw_key(&w, "kind"), jw_str(&w, ext);
        jw_key(&w, "log"), k->log ? jw_str(&w, k->log) : jw_null(&w);
        jw_key(&w, "message"), k->msg ? jw_str(&w, k->msg) : jw_null(&w);
        jw_key(&w, "missing"), jw_bool(&w, k->state == CC_LS_MISSING);
        jw_key(&w, "move_mm"), jw_arr_begin(&w);
        for (int c = 0; c < 3; c++) jw_f64(&w, k->move_mm[c]);
        jw_arr_end(&w);
        jw_key(&w, "name"), jw_str(&w, k->name);
        jw_key(&w, "origin"), jw_str(&w, CC_LINK_ORIGIN_NAME[k->origin >= 0 && k->origin < 4 ? k->origin : 0]);
        jw_key(&w, "path"), jw_str(&w, k->path ? k->path : "");
        jw_key(&w, "pinned"), jw_bool(&w, k->pinned);
        jw_key(&w, "progress"), jw_u64(&w, k->state == CC_LS_CONVERTING ? k->progress / 10 : k->state == CC_LS_READY ? 100 : 0);
        jw_key(&w, "rotation_deg"), jw_f64(&w, k->rotation_deg);
        jw_key(&w, "state"), jw_str(&w, CC_LINK_STATE_NAME[k->state >= 0 && k->state <= CC_LS_UNLOADED ? k->state : 0]);
        jw_key(&w, "triangles"), jw_u64(&w, tris);
        jw_key(&w, "visible"), jw_bool(&w, k->visible);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "references"), jw_arr_begin(&w);
    const cc_model *m = &s->model;
    for (size_t i = 0; s->open ? i < m->nviews : 0; i++) {
        const cc_view *v = &m->views[i];
        for (size_t k = 0; k < v->nreferences; k++) {
            const cc_reference *r = &v->references[k];
            jw_obj_begin(&w);
            jw_key(&w, "id"), jw_str_n(&w, r->id.c, 22);
            jw_key(&w, "kind"), jw_str_n(&w, r->format.p, r->format.n);
            jw_key(&w, "name"), jw_str_n(&w, r->name.p, r->name.n);
            jw_key(&w, "offset_mm"), jw_arr_begin(&w), jw_f64(&w, r->offset_mm[0]), jw_f64(&w, r->offset_mm[1]),
                jw_arr_end(&w);
            jw_key(&w, "rotation_deg"), jw_f64(&w, r->rotation_deg);
            jw_key(&w, "source");
            if (r->has_source) jw_str_n(&w, r->source.p, r->source.n);
            else jw_null(&w);
            jw_key(&w, "view"), jw_str_n(&w, v->id.c, 22);
            jw_key(&w, "view_name"), jw_str_n(&w, v->name.p, v->name.n);
            jw_key(&w, "visible"), jw_bool(&w, r->visible);
            jw_obj_end(&w);
        }
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    return jw_finish(&w) == JW_OK && !out->oom ? 0 : -1;
}
