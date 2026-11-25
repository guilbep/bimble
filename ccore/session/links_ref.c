/* Reference plans in the session (ticket L1). See links_ref.h. */
#pragma STDC FP_CONTRACT OFF
#include "links_ref.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "georef.h"
#include "guid.h"
#include "ifc_math.h"
#include "json_r.h"
#include "model.h"
#include "ref_read.h"
#include "scene.h"
#include "session.h"
#include "view_io.h"
#include "views.h"
#include "visibility.h"

static double to_radians(double deg) { return deg * (3.141592653589793 / 180.0); } /* f64::to_radians */

/* f64::round: half away from zero. */
static double round_away(double x) {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x, f = x - t;
    if (f >= 0.5) t += 1.0;
    else if (f <= -0.5) t -= 1.0;
    return t == 0.0 ? 0.0 * x : t; /* keeps the sign of zero, as round does */
}

/* Reference::place */
static void place(const cc_reference *r, double s, double c, const double p[2], double out[2]) {
    out[0] = p[0] * c - p[1] * s + r->offset_mm[0];
    out[1] = p[0] * s + p[1] * c + r->offset_mm[1];
}

/* The placed lines' box. 0: no point. */
static int ref_box(const cc_reference *r, double lo[2], double hi[2]) {
    double t = to_radians(r->rotation_deg), s = cc_sin(t), c = cc_cos(t);
    lo[0] = lo[1] = 1.7976931348623157e308, hi[0] = hi[1] = -1.7976931348623157e308;
    int any = 0;
    for (size_t i = 0; i < r->npolylines; i++)
        for (size_t k = 0; k < r->polylines[i].n; k++) {
            double q[2];
            place(r, s, c, r->polylines[i].pts[k], q);
            for (int j = 0; j < 2; j++) {
                if (q[j] < lo[j]) lo[j] = q[j];
                if (q[j] > hi[j]) hi[j] = q[j];
            }
            any = 1;
        }
    return any;
}

/* viewing.rs model_in_view_plane, for a plan: the model's box (ClipBox::around), world x, y. */
static int model_box(const cc_view *v, const cc_scene *sc, double lo[2], double hi[2]) {
    if (!sc || v->kind != CC_VIEW_PLAN) return 0;
    cc_vis box;
    cc_vis_init(&box);
    cc_vis_fit(&box, sc, NULL);
    int ok = box.has_box;
    if (ok)
        for (int k = 0; k < 2; k++) lo[k] = (double)box.min[k] + sc->origin[k], hi[k] = (double)box.max[k] + sc->origin[k];
    cc_vis_free(&box);
    return ok;
}

/* reference_overlaps: -1 none (no model or no lines), 0 no, 1 yes. */
static int overlaps(const cc_view *v, const cc_scene *sc, const cc_reference *r) {
    double lo[2], hi[2], rl[2], rh[2];
    if (!model_box(v, sc, lo, hi) || !ref_box(r, rl, rh)) return -1;
    for (int k = 0; k < 2; k++) {
        double g = (hi[k] - lo[k]) * 0.5;
        if (!(rl[k] <= hi[k] + g && rh[k] >= lo[k] - g)) return 0;
    }
    return 1;
}

static void centre(const cc_view *v, const cc_scene *sc, cc_reference *r) {
    double lo[2], hi[2], rl[2], rh[2];
    if (!model_box(v, sc, lo, hi)) return;
    ref_box(r, rl, rh);
    for (int k = 0; k < 2; k++) r->offset_mm[k] = round_away(r->offset_mm[k] + (lo[k] + hi[k]) / 2.0 - (rl[k] + rh[k]) / 2.0);
}

enum { AS_IS, SURVEY, CENTRED };

static int by_coordinates(const cc_model *m, const cc_view *v, const cc_scene *sc, cc_reference *r) {
    if (v->kind == CC_VIEW_PLAN && cc_georef_has_map(&m->georef) && r->rotation_deg == 0.0 && r->offset_mm[0] == 0.0 &&
        r->offset_mm[1] == 0.0) {
        cc_reference t = *r;
        cc_georef_survey_plan_to_world(&m->georef, &t.rotation_deg, t.offset_mm);
        if (overlaps(v, sc, &t) == 1) {
            *r = t;
            return SURVEY;
        }
    }
    int o = overlaps(v, sc, r);
    if (o != 0) return AS_IS;
    centre(v, sc, r);
    return CENTRED;
}

/* viewing.rs place_reference */
static void place_reference(const cc_model *m, const cc_view *v, const cc_scene *sc, cc_reference *r) {
    if (by_coordinates(m, v, sc, r) != CENTRED) return;
    if (v->kind == CC_VIEW_PLAN) {
        double deg = cc_georef_plan_turn(&m->georef, v->true_north) * (180.0 / 3.141592653589793); /* to_degrees */
        r->rotation_deg = round_away(deg * 1e6) / 1e6 + 0.0;
    }
    by_coordinates(m, v, sc, r);
}

static int fail(Buf *out, const char *a, const char *b) {
    buf_clear(out);
    buf_puts(out, a);
    if (b) buf_puts(out, b);
    return -1;
}

int cc_refs_link(cc_session *s, const char *format, const char *body, size_t n, const cc_scene *sc, Arena *a, cc_cmds *cmds,
                 Buf *out) {
    JrError je;
    const JrVal *v = n ? jr_parse(a, body, n, &je) : NULL;
    if (n && !v) return fail(out, je.msg, NULL);
    const JrVal *p = v && v->type == JR_OBJ ? jr_get(v, "path") : NULL;
    if (!p || p->type != JR_STR)
        return fail(out, "link_", NULL), buf_puts(out, format),
               buf_puts(out, ": the C core has no file dialog: send {\"path\": \"<file>\", \"scale\"?: N}"), -1;
    if (!p->n || strlen(p->s) < p->n) return fail(out, "path: empty or with a NUL", NULL);
    if (!s->open) return fail(out, "reference plans go in a model's 2D views: open a model (.bimble) to link one", NULL);
    cc_view view;
    int r = cc_sv_active(s, a, &view);
    if (r < 0) return fail(out, "out of memory", NULL);
    if (!r || !s->tab_open || view.kind == CC_VIEW_THREE_D)
        return fail(out, "reference plans go in 2D views: open a plan or a section first", NULL);
    double scale = view.scale ? (double)view.scale : 100.0;
    const JrVal *sv = jr_get(v, "scale");
    if (sv && sv->type != JR_NULL) {
        if (sv->type != JR_NUM) return fail(out, "scale: a number (the plan's 1:N)", NULL);
        scale = sv->num;
    }
    int has_place = 0;
    double off[2] = {0, 0}, rot = 0;
    const JrVal *ov = jr_get(v, "offset_mm"), *rv = jr_get(v, "rotation_deg");
    if (ov && ov->type != JR_NULL) {
        if (ov->type != JR_ARR || ov->n != 2 || ov->items[0].type != JR_NUM || ov->items[1].type != JR_NUM)
            return fail(out, "offset_mm: [x, y] in mm", NULL);
        off[0] = ov->items[0].num, off[1] = ov->items[1].num, has_place = 1;
    }
    if (rv && rv->type != JR_NULL) {
        if (rv->type != JR_NUM) return fail(out, "rotation_deg: a number", NULL);
        rot = rv->num, has_place = 1;
    }
    /* the file */
    char *bytes = NULL;
    size_t nb = 0;
    int st = s->pf->read_file(s->pf->ctx, p->s, CC_REF_MAX_FILE_BYTES, &bytes, &nb);
    if (st != CC_PF_OK) {
        fail(out, p->s, NULL);
        buf_puts(out, st == CC_PF_NOT_FOUND ? ": not found" : st == CC_PF_TOO_LARGE ? ": larger than 256 MB" : ": cannot be read");
        return -1;
    }
    const char *name = p->s;
    for (const char *q = p->s; *q; q++)
        if (*q == '/' || *q == '\\') name = q + 1;
    cc_refread rd;
    Buf err;
    buf_init(&err);
#ifdef CC_VIEW_ONLY /* build.sh VIEW_DENY: no reader (view_only.c refuses link_pdf before this) */
    (void)scale, (void)nb, memset(&rd, 0, sizeof rd);
    r = -1;
    buf_puts(&err, "this build is view-only");
#else
    r = cc_ref_read(name, strlen(name), (const unsigned char *)bytes, nb, scale, &rd, &err);
#endif
    free(bytes);
    if (r) {
        buf_clear(out);
        buf_append(out, err.p, err.len);
        buf_free(&err);
        return -1;
    }
    buf_free(&err);
    /* the Reference (viewing.rs:1487-1500) */
    cc_reference *refs = arena_alloc(a, (view.nreferences + 1) * sizeof *refs, 8);
    cc_loop *lines = arena_alloc(a, (rd.nlines ? rd.nlines : 1) * sizeof *lines, 8);
    double (*pts)[2] = arena_alloc(a, (rd.npts ? rd.npts : 1) * sizeof *pts, 8);
    char *nm = arena_dup(a, name, strlen(name) + 1);
    char *src = s->pf->files ? arena_dup(a, p->s, p->n + 1) : NULL;
    if (!refs || !lines || !pts || !nm || (s->pf->files && !src)) {
#ifndef CC_VIEW_ONLY
        cc_refread_free(&rd);
#endif
        return fail(out, "out of memory", NULL);
    }
    if (view.nreferences) memcpy(refs, view.references, view.nreferences * sizeof *refs);
    if (rd.npts) memcpy(pts, rd.xy, rd.npts * sizeof *pts);
    for (size_t i = 0; i < rd.nlines; i++) lines[i].pts = pts + rd.start[i], lines[i].n = rd.start[i + 1] - rd.start[i];
    cc_reference *x = &refs[view.nreferences];
    memset(x, 0, sizeof *x);
    cc_guid g = cc_guid_new();
    memcpy(x->id.c, g.c, 22);
    x->name.p = nm, x->name.n = strlen(nm);
    x->format.p = format, x->format.n = strlen(format);
    if (src) x->has_source = 1, x->source.p = src, x->source.n = p->n;
    x->visible = 1;
    x->polylines = lines, x->npolylines = rd.nlines;
#ifndef CC_VIEW_ONLY
    cc_refread_free(&rd);
#endif
    if (has_place) x->offset_mm[0] = off[0], x->offset_mm[1] = off[1], x->rotation_deg = rot;
    else place_reference(&s->model, &view, sc, x);
    view.references = refs, view.nreferences++;
    Buf why;
    buf_init(&why);
    if (cc_view_validate(&s->model, &view, &why)) {
        buf_clear(out);
        buf_append(out, why.p, why.len);
        buf_free(&why);
        return -1;
    }
    buf_free(&why);
    cc_command *c = cc_cmds_push(cmds, cc_model_view(&s->model, &view.id) ? CC_CMD_SET_VIEW : CC_CMD_ADD_VIEW);
    if (!c) return fail(out, "out of memory", NULL);
    c->view = view;
    return 0;
}

/* The views to look in: the model's stored views (default views hold no references). */
void cc_refs_links_append(cc_session *s, Buf *out) {
    if (!s->open || out->len < 1 || out->p[out->len - 1] != ']') return;
    const cc_model *m = &s->model;
    Buf rows;
    buf_init(&rows);
    JsonW w;
    jw_init(&w, &rows, 0);
    jw_arr_begin(&w);
    for (size_t i = 0; i < m->nviews; i++)
        for (size_t k = 0; k < m->views[i].nreferences; k++) {
            const cc_reference *r = &m->views[i].references[k];
            jw_obj_begin(&w);
            jw_key(&w, "id"), jw_str_n(&w, r->id.c, 22);
            jw_key(&w, "kind"), jw_str_n(&w, r->format.p, r->format.n);
            jw_key(&w, "name"), jw_str_n(&w, r->name.p, r->name.n);
            jw_key(&w, "visible"), jw_bool(&w, r->visible);
            jw_obj_end(&w);
        }
    jw_arr_end(&w);
    jw_finish(&w);
    if (rows.len > 2) { /* "[…]": splice the rows in */
        out->len--;
        if (out->len > 1) buf_putc(out, ',');
        buf_append(out, rows.p + 1, rows.len - 1);
    }
    buf_free(&rows);
}

int cc_refs_view_references(cc_session *s, const char *body, size_t n, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    int rc = -1;
    JrError je;
    size_t i = 0;
    while (i < n && (body[i] == ' ' || body[i] == '\n' || body[i] == '\t' || body[i] == '\r')) i++;
    const JrVal *v = i < n ? jr_parse(&a, body, n, &je) : NULL;
    if (i < n && !v) {
        fail(out, je.msg, NULL);
        goto end;
    }
    const JrVal *idv = v && v->type == JR_OBJ ? jr_get(v, "view") : NULL;
    cc_view view;
    int found = 0;
    if (idv && idv->type == JR_STR) {
        cc_view *vs;
        size_t nv;
        if (cc_sv_list(s, &a, &vs, &nv)) {
            fail(out, "out of memory", NULL);
            goto end;
        }
        for (size_t k = 0; k < nv && !found; k++)
            if (idv->n == 22 && !memcmp(vs[k].id.c, idv->s, 22)) view = vs[k], found = 1;
        if (!found) {
            fail(out, "no view ", NULL);
            buf_append(out, idv->s, idv->n);
            goto end;
        }
    } else {
        int r = s->has_active ? cc_sv_active(s, &a, &view) : 0;
        if (r < 0) {
            fail(out, "out of memory", NULL);
            goto end;
        }
        found = r;
    }
    if (!found) {
        buf_clear(out);
        buf_puts(out, "null");
        rc = 0;
        goto end;
    }
    size_t all = 0, shown = 0;
    for (size_t k = 0; k < view.nreferences; k++)
        for (size_t j = 0; j < view.references[k].npolylines; j++)
            if (view.references[k].polylines[j].n > 1) all += view.references[k].polylines[j].n - 1;
    buf_clear(out);
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "references"), jw_arr_begin(&w);
    for (size_t k = 0; k < view.nreferences; k++) {
        const cc_reference *r = &view.references[k];
        double t = to_radians(r->rotation_deg), sn = cc_sin(t), cs = cc_cos(t);
        size_t segs = 0;
        jw_obj_begin(&w);
        jw_key(&w, "id"), jw_str_n(&w, r->id.c, 22);
        jw_key(&w, "name"), jw_str_n(&w, r->name.p, r->name.n);
        jw_key(&w, "polylines"), jw_arr_begin(&w);
        for (size_t j = 0; r->visible && j < r->npolylines; j++) {
            const cc_loop *l = &r->polylines[j];
            if (l->n < 2) continue;
            segs += l->n - 1;
            if (shown + l->n - 1 > CC_REF_SHOWN_MAX) continue;
            shown += l->n - 1;
            jw_arr_begin(&w);
            for (size_t q = 0; q < l->n; q++) {
                double pt[2];
                place(r, sn, cs, l->pts[q], pt);
                jw_f64(&w, pt[0]), jw_f64(&w, pt[1]);
            }
            jw_arr_end(&w);
        }
        jw_arr_end(&w);
        jw_key(&w, "segments"), jw_u64(&w, segs);
        jw_key(&w, "visible"), jw_bool(&w, r->visible);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "segments"), jw_u64(&w, all);
    jw_key(&w, "shown"), jw_u64(&w, shown);
    jw_key(&w, "view"), jw_str_n(&w, view.id.c, 22);
    jw_obj_end(&w);
    if (jw_finish(&w) != JW_OK || out->oom) {
        fail(out, "out of memory", NULL);
        goto end;
    }
    rc = 0;
end:
    arena_free(&a);
    return rc;
}
