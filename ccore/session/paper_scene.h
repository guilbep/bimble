/* What the paper drawing draws (ticket LNK1): the scene and the model cc_plan / cc_sheet_draw take,
 * for whatever the session shows.
 *
 * Rust's plan() draws from a Scene alone (bimble-drawing plan.rs; paper.rs:119 `l.scene`), so it
 * draws a read-only scene (an IFC opened as it is, a .bimbleview) as it draws a model's. The C
 * drawing reads the storeys, materials, grids, georeference and an element's storey and material
 * names from a cc_model through each scene element's model_index (cc_plan takes scene + model).
 * This file gives it one:
 *   - an editable model without visible links: the model and its scene as they are (the D4
 *     goldens draw this way: nothing changes for them);
 *   - a read-only scene: a model made from what the scene read (ifc_read.h / viewfile.h): one
 *     storey per level (its id the level's key when that is a GlobalId's 22 characters, else derived
 *     from the name: cc_paper_src_storey_key), one material per material name, one element per
 *     scene element (same index: model_index needs no change), the grids and the georeference as
 *     read; no project information;
 *   - with visible linked models (a C addition, DEVIATIONS 29; Rust's paper draws the host scene
 *     only): one scene, the host's elements then each visible link's, its meshes moved by
 *     (link origin - host origin); a link's elements take the host storey of the same name (none
 *     else) and their own materials. A link hidden in the session, or in the drawn view's
 *     hidden_links, is left out.
 * Everything lives in the source; free it with cc_paper_src_free whatever the result. */
#ifndef CCORE_SESSION_PAPER_SCENE_H
#define CCORE_SESSION_PAPER_SCENE_H
#include "buf.h"
#include "model.h"
#include "scene.h"

struct cc_session;

typedef struct {
    const cc_scene *scene;
    const cc_model *model;
    cc_model own_model; /* the made model (arena), when model == &own_model */
    cc_scene own_scene; /* the merged scene, when scene == &own_scene */
    int made_model, made_scene;
    /* DXF1: where each drawn link's elements start (model_index), in order, and its name (the
     * session's link name, borrowed): the DXF export's layer prefix. */
    size_t *link_first;
    const char **link_name;
    size_t nlinks;
} cc_paper_src;

/* `sc`: the scene shown (calls.c scene_now), `view`: the drawn view (its hidden_links) or NULL.
 * 0 ok; -1 with the error in `err` ("no model open", out of memory). */
int cc_paper_src_get(struct cc_session *s, const cc_scene *sc, const cc_view *view, cc_paper_src *src, Buf *err);
void cc_paper_src_free(cc_paper_src *src);
/* The made model's storey id for a level key (a read-only scene's views name their storey by
 * key): the key itself when it is 22 bytes, else an id derived from it. */
cc_gid cc_paper_src_storey_key(cc_str key);
/* A read-only scene's views (cc_sv_list) with their storey keys turned into the made model's
 * storey ids (in place; new strings in `a`). Nothing to do for an editable model. 0, -1 out of
 * memory. */
int cc_paper_src_views(struct cc_session *s, cc_view *v, size_t n, Arena *a);
#endif
