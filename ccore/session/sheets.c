/* Sheets in the session: see sheets.h. */
#include "sheets.h"
#include <string.h>
#include "calls.h"
#include "json_r.h"
#include "json_w.h"
#include "paper.h"
#include "session.h"
#include "view_io.h"
#include "views.h"
#include "visibility.h"

/* sheet.rs size_mm: width and height in the sheet's orientation; 0 for an unknown paper. */
static int paper_mm(const cc_sheet *s, double *w, double *h) {
    static const char *const P[] = {"A4", "A3", "A2", "A1", "A0"};
    static const double W[] = {297.0, 420.0, 594.0, 841.0, 1189.0}, H[] = {210.0, 297.0, 420.0, 594.0, 841.0};
    for (int i = 0; i < 5; i++)
        if (s->paper.n == 2 && !memcmp(s->paper.p, P[i], 2)) {
            *w = s->portrait ? H[i] : W[i], *h = s->portrait ? W[i] : H[i];
            return 1;
        }
    *w = *h = 0.0;
    return 0;
}

/* The scale a view is drawn at (ADR 0046, sheet.rs adopted_scales); as verbs_sheets.c. */
static uint32_t drawn_scale(const cc_model *m, const cc_view *v) {
    if (v->scale != 100) return v->scale;
    uint32_t first = 0;
    int placed = 0;
    for (size_t i = 0; i < m->nsheets; i++)
        for (size_t k = 0; k < m->sheets[i].nplaced; k++) {
            const cc_placed *p = &m->sheets[i].placed[k];
            if (cc_gid_cmp(&p->view, &v->id)) continue;
            if (placed && p->scale != first) return 100;
            first = p->scale, placed = 1;
        }
    return placed ? first : 100;
}

/* To 0.001 mm: the box is an estimate from the scene's f32 bounds. */
static double mm3(double x) {
    if (!(x > -1e12 && x < 1e12)) return x;
    double t = x * 1000.0;
    int64_t k = (int64_t)(t < 0 ? t - 0.5 : t + 0.5);
    return (double)k / 1000.0 + 0.0;
}

int cc_sheet_known(const struct cc_session *s, const cc_gid *id) {
    return s->open && cc_model_sheet(&s->model, id) != NULL;
}

int cc_sheet_layout(struct cc_session *s, const cc_scene *sc, const char *body, size_t n, Buf *out) {
    if (!s->open) {
        buf_puts(out, s->view ? "this file is read only: import it to edit" : "no model open");
        return CC_ERR;
    }
    const cc_model *m = &s->model;
    Arena a;
    arena_init(&a, 0);
    int rc = CC_ERR;
    JrError je;
    const JrVal *v = jr_parse(&a, body, n, &je), *idv = v && v->type == JR_OBJ ? jr_get(v, "id") : NULL;
    cc_gid id;
    if (!v) {
        buf_puts(out, je.msg);
        goto end;
    }
    if (!idv || idv->type != JR_STR || !cc_gid_valid(idv->s, idv->n)) {
        buf_puts(out, idv ? "id: a GlobalId" : "missing field `id`");
        goto end;
    }
    memcpy(id.c, idv->s, 22);
    const cc_sheet *sh = cc_model_sheet(m, &id);
    if (!sh) {
        buf_puts(out, "no sheet GlobalId(");
        cc_buf_gid(out, &id);
        buf_putc(out, ')');
        goto end;
    }
    cc_view *views;
    size_t nv;
    if (cc_sv_list(s, &a, &views, &nv)) {
        buf_puts(out, "out of memory");
        goto end;
    }
    /* The model's box, world mm (the scene's bounds, as view_clip). */
    double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    cc_vis box;
    cc_vis_init(&box);
    if (sc) cc_vis_fit(&box, sc, NULL);
    if (sc && box.has_box)
        for (int k = 0; k < 3; k++) lo[k] = sc->origin[k] + box.min[k], hi[k] = sc->origin[k] + box.max[k];
    cc_vis_free(&box);
    double w, h;
    paper_mm(sh, &w, &h);
    JsonW jw;
    jw_init(&jw, out, 0);
    jw_obj_begin(&jw);
    jw_key(&jw, "height_mm"), jw_f64(&jw, h);
    jw_key(&jw, "id"), jw_str_n(&jw, sh->id.c, 22);
    jw_key(&jw, "name"), jw_str_n(&jw, sh->name.p, sh->name.n);
    jw_key(&jw, "number"), jw_str_n(&jw, sh->number.p, sh->number.n);
    jw_key(&jw, "paper"), jw_str_n(&jw, sh->paper.p, sh->paper.n);
    jw_key(&jw, "placed"), jw_arr_begin(&jw);
    for (size_t i = 0; i < sh->nplaced; i++) {
        const cc_placed *p = &sh->placed[i];
        const cc_view *vw = NULL;
        for (size_t k = 0; k < nv && !vw; k++)
            if (!cc_gid_cmp(&views[k].id, &p->view)) vw = &views[k];
        uint32_t scale = vw ? drawn_scale(m, vw) : p->scale;
        double bw = 0.0, bh = 0.0; /* world mm */
        if (vw && vw->has_crop) {
            bw = vw->crop_max[0] - vw->crop_min[0], bh = vw->crop_max[1] - vw->crop_min[1];
        } else if (vw && vw->kind == CC_VIEW_SECTION) {
            int ns = vw->facing == CC_FACING_NORTH || vw->facing == CC_FACING_SOUTH;
            bw = ns ? hi[0] - lo[0] : hi[1] - lo[1], bh = hi[2] - lo[2];
        } else {
            bw = hi[0] - lo[0], bh = hi[1] - lo[1];
        }
        bw = (bw < 0 ? -bw : bw) / scale, bh = (bh < 0 ? -bh : bh) / scale;
        /* The drawing's own box when the view is drawn (D5, paper.h), else the estimate. */
        double box[4];
        int drawn = sc && cc_paper_placed_box(s, sc, sh, &p->id, box);
        if (!drawn) {
            box[0] = mm3(p->at_mm[0] - bw / 2), box[1] = mm3(p->at_mm[1] - bh / 2);
            box[2] = mm3(p->at_mm[0] + bw / 2), box[3] = mm3(p->at_mm[1] + bh / 2);
        }
        jw_obj_begin(&jw);
        jw_key(&jw, "at_mm"), jw_arr_begin(&jw), jw_f64(&jw, p->at_mm[0]), jw_f64(&jw, p->at_mm[1]), jw_arr_end(&jw);
        jw_key(&jw, "box"), jw_str(&jw, drawn ? "drawing" : "estimate");
        jw_key(&jw, "box_mm"), jw_arr_begin(&jw);
        for (int k = 0; k < 4; k++) jw_f64(&jw, box[k]);
        jw_arr_end(&jw);
        jw_key(&jw, "id"), jw_str_n(&jw, p->id.c, 22);
        jw_key(&jw, "kind");
        if (vw) jw_str(&jw, cc_view_kind_name(vw->kind));
        else jw_null(&jw);
        jw_key(&jw, "scale"), jw_u64(&jw, scale);
        jw_key(&jw, "view"), jw_str_n(&jw, p->view.c, 22);
        jw_key(&jw, "view_name");
        if (vw) jw_str_n(&jw, vw->name.p, vw->name.n);
        else jw_null(&jw);
        jw_obj_end(&jw);
    }
    jw_arr_end(&jw);
    jw_key(&jw, "portrait"), jw_bool(&jw, sh->portrait);
    jw_key(&jw, "width_mm"), jw_f64(&jw, w);
    jw_obj_end(&jw);
    rc = jw_finish(&jw) == JW_OK ? CC_OK : (out->oom = 1, CC_ERR);
end:
    arena_free(&a);
    return rc;
}
