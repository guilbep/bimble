/* Sheets on paper in the session: see sheet_svg.h. */
#include "sheet_svg.h"
#include "toggles.h" /* KEY1 */
#include <stdlib.h>
#include <string.h>
#include "calls.h"
#include "json_r.h"
#include "json_w.h"
#include "session.h"
#include "view_io.h"
#include "view_only.h"
#ifndef CC_VIEW_ONLY
#include "draw_plan.h"
#include "draw_sheet.h"
#include "draw_annot.h" /* cc_section_marks (D7) */
#include "draw_style.h"
#include "ifc_out.h" /* cc_iso_utc, CC_IFC_VERSION */
#include "pdf_out.h"
#include "view_effective.h" /* D7 */
#include "paper_scene.h" /* LNK1 */
#include "views.h"       /* LNK1: cc_sv_list */
#endif

void cc_paper_cache_free(struct cc_session *s) {
    buf_free(&s->paper_svg);
    buf_free(&s->paper_key);
    free(s->paper_ids), free(s->paper_rects);
    s->paper_ids = NULL, s->paper_rects = NULL, s->npaper = 0;
}

/* ---- D7: the fonts and the version of the PDF export (both builds: the carriers call them) ---- */
void cc_paper_fonts_free(struct cc_session *s) {
    for (int k = 0; k < 2; k++) free(s->pdf_font[k]), s->pdf_font[k] = NULL, s->pdf_font_n[k] = 0;
    free(s->pdf_font_dir), s->pdf_font_dir = NULL;
    free(s->paper_version), s->paper_version = NULL;
}

static char *zcopy(const char *p, size_t n) {
    char *z = malloc(n + 1);
    if (z) memcpy(z, p, n), z[n] = 0;
    return z;
}

int cc_paper_set_fonts(struct cc_session *s, const unsigned char *regular, size_t nr, const unsigned char *bold,
                       size_t nb) {
    const unsigned char *f[2] = {regular, bold};
    size_t n[2] = {nr, nb};
    int rc = 0;
    for (int k = 0; k < 2; k++) {
        free(s->pdf_font[k]), s->pdf_font[k] = NULL, s->pdf_font_n[k] = 0;
        if (!f[k] || !n[k]) continue;
        s->pdf_font[k] = (unsigned char *)zcopy((const char *)f[k], n[k]);
        if (s->pdf_font[k]) s->pdf_font_n[k] = n[k];
        else rc = -1;
    }
    return rc;
}

int cc_paper_set_font_dir(struct cc_session *s, const char *dir) {
    free(s->pdf_font_dir), s->pdf_font_dir = NULL;
    if (dir && !(s->pdf_font_dir = zcopy(dir, strlen(dir)))) return -1;
    return 0;
}

int cc_paper_set_version(struct cc_session *s, const char *version) {
    free(s->paper_version), s->paper_version = NULL;
    if (version && !(s->paper_version = zcopy(version, strlen(version)))) return -1;
    return 0;
}

#ifdef CC_VIEW_ONLY
int cc_paper_pdf_query(struct cc_session *s, const cc_scene *sc, int view, const char *body, size_t n, Buf *out) {
    (void)s, (void)sc, (void)view, (void)body, (void)n;
    buf_puts(out, CC_VIEW_ONLY_TEXT);
    return CC_ERR;
}
int cc_paper_fonts_query(struct cc_session *s, Buf *out) { /* SIZE1 */
    (void)s;
    buf_puts(out, CC_VIEW_ONLY_TEXT);
    return CC_ERR;
}
int cc_paper_sheet_svg(struct cc_session *s, const cc_scene *sc, const char *body, size_t n, Buf *out) {
    (void)s, (void)sc, (void)body, (void)n;
    buf_puts(out, CC_VIEW_ONLY_TEXT);
    return CC_ERR;
}
int cc_paper_placed_box(struct cc_session *s, const cc_scene *sc, const cc_sheet *sh, const cc_gid *placed,
                        double out[4]) {
    (void)s, (void)sc, (void)sh, (void)placed, (void)out;
    return 0;
}
int cc_paper_sheet_pdf(struct cc_session *s, const cc_scene *sc, const cc_sheet *sh, const unsigned char *const fonts[2],
                       const size_t font_len[2], Buf *pdf, Buf *err) {
    (void)s, (void)sc, (void)sh, (void)fonts, (void)font_len, (void)pdf;
    buf_puts(err, CC_VIEW_ONLY_TEXT);
    return -1;
}
/* ---- PAPER1: the viewer build draws nothing on paper ---- */
int cc_paper_view_svg(struct cc_session *s, const cc_scene *sc, const char *body, size_t n, Buf *out) {
    (void)s, (void)sc, (void)body, (void)n;
    buf_puts(out, CC_VIEW_ONLY_TEXT);
    return CC_ERR;
}
int cc_paper_sheets_pdf(struct cc_session *s, const cc_scene *sc, const char *body, size_t n, Buf *out) {
    (void)s, (void)sc, (void)body, (void)n;
    buf_puts(out, CC_VIEW_ONLY_TEXT);
    return CC_ERR;
}
/* ---- end PAPER1 ---- */
#else
/* Path::file_stem of the session's path: the file name without its last extension (a name that
 * starts with its only dot keeps it). */
static cc_str file_stem(const char *path) {
    const char *f = cc_path_last_sep(path);
    f = f ? f + 1 : path;
    size_t n = strlen(f);
    const char *dot = strrchr(f, '.');
    if (dot && dot != f) n = (size_t)(dot - f);
    cc_str r = {f, n};
    return r;
}

static void debug_str(Buf *b, const char *p, size_t n) { /* {:?} of a String (ASCII ids) */
    buf_putc(b, '"');
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '"' || p[i] == '\\') buf_putc(b, '\\');
        buf_putc(b, p[i]);
    }
    buf_putc(b, '"');
}

/* Draws `sh` into the session's cache unless it holds it already. 0 ok; else CC_ERR's text in
 * `err` (the drawing's error, or out of memory). */
static int ensure(struct cc_session *s, const cc_scene *sc, const cc_sheet *sh, Buf *err) {
    char date[32];
    cc_iso_utc(s->pf->unix_s ? s->pf->unix_s(s->pf->ctx) : 0, date);
    date[10] = 0; /* YYYY-MM-DD */
    Buf key;
    buf_init(&key);
    buf_u64(&key, s->rev);
    buf_putc(&key, ' ');
    buf_append(&key, sh->id.c, 22);
    buf_putc(&key, ' ');
    buf_puts(&key, date);
    buf_putc(&key, ' ');
    buf_puts(&key, s->path);
    buf_putc(&key, ' '); /* KEY1: the plan annotation toggles drawn */
    buf_u64(&key, s->plan_off);
    buf_putc(&key, ' '); /* LNK1: the linked models drawn */
    buf_u64(&key, s->links_gen);
    if (key.oom) return buf_free(&key), buf_puts(err, "out of memory"), -1;
    if (s->paper_svg.len && key.len == s->paper_key.len && !memcmp(key.p, s->paper_key.p, key.len))
        return buf_free(&key), 0;
    cc_paper_cache_free(s);
    Arena a;
    arena_init(&a, 0);
    int rc = -1;
    cc_style style;
    cc_plan_template tpl;
    Buf derr, svg;
    buf_init(&derr), buf_init(&svg);
    if (cc_style_default(&style, &a) || cc_plan_template_default(&tpl, &a) ||
        cc_plan_template_with_project(&tpl, s->model.project, s->model.nproject, &a)) {
        buf_puts(err, "out of memory");
        goto end;
    }
    cc_toggles_apply(s, &tpl); /* KEY1: the plan annotation toggles (toggles.h) */
    cc_sheet_options o = {&style, &tpl, file_stem(s->path), {date, strlen(date)},
                          {CC_IFC_VERSION, strlen(CC_IFC_VERSION)}};
    cc_paper paper;
    cc_sheet_info info;
    cc_paper_src src; /* LNK1: the model with its visible links (paper_scene.h) */
    if (cc_paper_src_get(s, sc, NULL, &src, err)) goto end;
    int r = cc_sheet_draw(src.scene, src.model, sh, &o, &a, &paper, &info, &derr);
    cc_paper_src_free(&src);
    if (r) {
        if (r < 0 || derr.oom) buf_puts(err, "out of memory");
        else buf_append(err, derr.p, derr.len);
        goto end;
    }
    if (cc_paper_svg(&paper, &svg)) {
        buf_puts(err, "out of memory");
        goto end;
    }
    JsonW jw;
    jw_init(&jw, &s->paper_svg, 0);
    jw_obj_begin(&jw);
    jw_key(&jw, "svg"), jw_str_n(&jw, svg.p, svg.len);
    jw_obj_end(&jw);
    size_t np = info.nplaced;
    s->paper_ids = malloc((np ? np : 1) * sizeof *s->paper_ids);
    s->paper_rects = malloc((np ? np : 1) * sizeof *s->paper_rects);
    buf_append(&s->paper_key, key.p, key.len);
    if (jw_finish(&jw) != JW_OK || s->paper_svg.oom || s->paper_key.oom || !s->paper_ids || !s->paper_rects) {
        cc_paper_cache_free(s);
        buf_puts(err, "out of memory");
        goto end;
    }
    for (size_t i = 0; i < np; i++) {
        s->paper_ids[i] = info.placed[i].id;
        memcpy(s->paper_rects[i], info.placed[i].rect, sizeof s->paper_rects[i]);
    }
    s->npaper = np;
    rc = 0;
end:
    buf_free(&key), buf_free(&derr), buf_free(&svg);
    arena_free(&a);
    return rc;
}

int cc_paper_placed_box(struct cc_session *s, const cc_scene *sc, const cc_sheet *sh, const cc_gid *placed,
                        double out[4]) {
    if (!s->open || !sc) return 0;
    Buf err;
    buf_init(&err);
    int bad = ensure(s, sc, sh, &err);
    buf_free(&err);
    if (bad) return 0;
    for (size_t i = 0; i < s->npaper; i++)
        if (!cc_gid_cmp(&s->paper_ids[i], placed)) return memcpy(out, s->paper_rects[i], 4 * sizeof *out), 1;
    return 0;
}

int cc_paper_sheet_svg(struct cc_session *s, const cc_scene *sc, const char *body, size_t n, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    int rc = CC_ERR;
    JrError je;
    const JrVal *v = jr_parse(&a, body, n, &je), *idv = v && v->type == JR_OBJ ? jr_get(v, "id") : NULL;
    if (!v) {
        buf_puts(out, je.msg);
        goto end;
    }
    if (!idv || idv->type != JR_STR) {
        buf_puts(out, idv ? "id: a string" : "missing field `id`");
        goto end;
    }
    const cc_sheet *sh = NULL;
    if (s->open && idv->n == 22) {
        cc_gid id;
        memcpy(id.c, idv->s, 22);
        sh = cc_model_sheet(&s->model, &id);
    }
    if (!sh) { /* v.sheets() is empty without a model: the same text */
        buf_puts(out, "no sheet with id ");
        debug_str(out, idv->s, idv->n);
        goto end;
    }
    if (!sc) {
        buf_puts(out, "no model open");
        goto end;
    }
    if (ensure(s, sc, sh, out)) goto end;
    buf_append(out, s->paper_svg.p, s->paper_svg.len);
    rc = out->oom ? CC_ERR : CC_OK;
end:
    arena_free(&a);
    return rc;
}
/* The PDF: the drawing of ensure() (its options), not kept; written by cc_paper_pdf. */
int cc_paper_sheet_pdf(struct cc_session *s, const cc_scene *sc, const cc_sheet *sh, const unsigned char *const fonts[2],
                       const size_t font_len[2], Buf *pdf, Buf *err) {
    if (!s->open || !sc) return buf_puts(err, "no model open"), -1;
    char date[32];
    cc_iso_utc(s->pf->unix_s ? s->pf->unix_s(s->pf->ctx) : 0, date);
    date[10] = 0; /* YYYY-MM-DD */
    Arena a;
    arena_init(&a, 0);
    int rc = -1;
    cc_style style;
    cc_plan_template tpl;
    Buf derr;
    buf_init(&derr);
    if (cc_style_default(&style, &a) || cc_plan_template_default(&tpl, &a) ||
        cc_plan_template_with_project(&tpl, s->model.project, s->model.nproject, &a)) {
        buf_puts(err, "out of memory");
        goto end;
    }
    cc_toggles_apply(s, &tpl); /* KEY1: the plan annotation toggles (toggles.h) */
    cc_sheet_options o = {&style, &tpl, file_stem(s->path), {date, strlen(date)},
                          {CC_IFC_VERSION, strlen(CC_IFC_VERSION)}};
    cc_paper paper;
    cc_sheet_info info;
    cc_paper_src src; /* LNK1: the model with its visible links (paper_scene.h) */
    if (cc_paper_src_get(s, sc, NULL, &src, err)) goto end;
    int r = cc_sheet_draw(src.scene, src.model, sh, &o, &a, &paper, &info, &derr);
    cc_paper_src_free(&src);
    if (r) {
        if (r < 0 || derr.oom) buf_puts(err, "out of memory");
        else buf_append(err, derr.p, derr.len);
        goto end;
    }
    cc_pdf_options po;
    memset(&po, 0, sizeof po);
    po.version = CC_IFC_VERSION;
    po.version_len = strlen(CC_IFC_VERSION);
    for (int k = 0; k < 2 && fonts; k++) po.roboto[k] = fonts[k], po.roboto_len[k] = font_len[k];
    r = cc_paper_pdf(&paper, &po, pdf);
    if (r == CC_PDF_ERR_FONT) buf_puts(err, "the Roboto font files are needed (Roboto-Regular.ttf, Roboto-Bold.ttf)");
    else if (r == CC_PDF_ERR_PEN) buf_puts(err, "a pen of negative width");
    else if (r) buf_puts(err, "out of memory");
    else rc = 0;
end:
    buf_free(&derr);
    arena_free(&a);
    return rc;
}

/* ---- D7: sheet_pdf and view_pdf (sheet_svg.h cc_paper_pdf_query) ---- */

#define FONT_MAX (4u << 20) /* a font file read from the folder: the shipped ones are 350 KB */

/* The fonts on the first export without files handed over: read from the folder, kept only when
 * each is the shipped file of its weight (else the writer would refuse it anyway). */
static void read_fonts(struct cc_session *s) {
    static const char *const NAMES[2] = {"Roboto-Regular.ttf", "Roboto-Bold.ttf"};
    if (!s->pdf_font_dir || (s->pdf_font[0] && s->pdf_font[1])) return;
    for (int k = 0; k < 2; k++) {
        if (s->pdf_font[k]) continue;
        Buf path;
        buf_init(&path);
        buf_puts(&path, s->pdf_font_dir);
        buf_putc(&path, '/');
        buf_puts(&path, NAMES[k]);
        char *p = NULL;
        size_t n = 0;
        if (!path.oom && s->pf->read_file(s->pf->ctx, buf_cstr(&path), FONT_MAX, &p, &n) == CC_PF_OK) {
            if (cc_pdf_roboto_is_shipped((const unsigned char *)p, n, k))
                s->pdf_font[k] = (unsigned char *)p, s->pdf_font_n[k] = n;
            else free(p), cc_log(s->pf, "bimble-ccore: %s is not the shipped Roboto file", buf_cstr(&path));
        }
        buf_free(&path);
    }
    /* SIZE1: the folder is kept (not "tried once"): files put there later are found by the next
     * `pdf_fonts` or Roboto export. A missing file costs one failed open. */
}

/* SIZE1: `query pdf_fonts` -> {"helvetica":true,"roboto":bool}: roboto when both shipped files are
 * here (handed over, or read from the folder now). */
int cc_paper_fonts_query(struct cc_session *s, Buf *out) {
    read_fonts(s);
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "helvetica"), jw_bool(&w, 1);
    jw_key(&w, "roboto"), jw_bool(&w, s->pdf_font[0] && s->pdf_font[1] && cc_pdf_roboto_is_shipped(s->pdf_font[0], s->pdf_font_n[0], 0) &&
                                          cc_pdf_roboto_is_shipped(s->pdf_font[1], s->pdf_font_n[1], 1));
    jw_obj_end(&w);
    return jw_finish(&w) == JW_OK && !out->oom ? CC_OK : CC_ERR;
}

/* The sheet or the view on paper with `font`, into `a`. 0 ok; else -1 with the text in `err`.
 * PAPER1: `bare` (a view only: paper.rs refresh_paper's On paper picture), `pinfo` (a view's
 * PlanInfo, or NULL). Note for KEY1: Rust draws with the viewer's plan template (self.plan.template,
 * which the actions section_labels, openings, section_marks, slab_levels toggle); this takes the
 * default one, so once the session keeps those toggles they belong here too. */
static int draw_paper_ex(struct cc_session *s, const cc_scene *sc, const cc_sheet *sh, const cc_view *v,
                         const cc_view *views, size_t nv, cc_sheet_font font, int bare, cc_plan_info *pinfo_out,
                         Arena *a, cc_paper *paper, Buf *err);
static int draw_paper(struct cc_session *s, const cc_scene *sc, const cc_sheet *sh, const cc_view *v,
                      const cc_view *views, size_t nv, cc_sheet_font font, Arena *a, cc_paper *paper, Buf *err) {
    return draw_paper_ex(s, sc, sh, v, views, nv, font, 0, NULL, a, paper, err);
}
static int draw_paper_ex(struct cc_session *s, const cc_scene *sc, const cc_sheet *sh, const cc_view *v,
                         const cc_view *views, size_t nv, cc_sheet_font font, int bare, cc_plan_info *pinfo_out,
                         Arena *a, cc_paper *paper, Buf *err) {
    char date[32];
    cc_iso_utc(s->pf->unix_s ? s->pf->unix_s(s->pf->ctx) : 0, date);
    date[10] = 0; /* YYYY-MM-DD */
    const char *ver = s->paper_version ? s->paper_version : CC_IFC_VERSION;
    cc_style *style = arena_alloc(a, sizeof *style, _Alignof(cc_style));
    cc_plan_template *tpl = arena_alloc(a, sizeof *tpl, _Alignof(cc_plan_template));
    char *d = arena_alloc(a, sizeof date, 1);
    if (!style || !tpl || !d || cc_style_default(style, a) || cc_plan_template_default(tpl, a) ||
        cc_plan_template_with_project(tpl, s->open ? s->model.project : NULL, s->open ? s->model.nproject : 0, a))
        return buf_puts(err, "out of memory"), -1;
    cc_toggles_apply(s, tpl); /* KEY1: the plan annotation toggles (toggles.h) */
    memcpy(d, date, sizeof date);
    style->paper.font = font;
    cc_paper_src src; /* LNK1: a read-only scene's made model, the visible links (paper_scene.h) */
    if (cc_paper_src_get(s, sc, sh ? NULL : v, &src, err)) return -1;
    Buf derr;
    buf_init(&derr);
    int r;
    if (sh) {
        cc_sheet_options o = {style, tpl, file_stem(s->path), {d, strlen(d)}, {ver, strlen(ver)}};
        cc_sheet_info info;
        r = cc_sheet_draw(src.scene, src.model, sh, &o, a, paper, &info, &derr);
    } else {
        cc_section_mark *marks;
        size_t nm;
        if (cc_section_marks(views, nv, a, &marks, &nm))
            return cc_paper_src_free(&src), buf_free(&derr), buf_puts(err, "out of memory"), -1;
        cc_plan_options po; /* lib.rs plan_options with the view */
        cc_plan_options_init(&po);
        po.scale = v->scale;
        po.style = style;
        po.tpl = tpl;
        po.project = file_stem(s->path);
        po.date = (cc_str){d, strlen(d)};
        po.version = (cc_str){ver, strlen(ver)};
        po.view = v;
        po.section_marks = marks;
        po.nsection_marks = nm;
        po.bare = bare; /* PAPER1 */
        cc_plan_info pinfo;
        r = cc_plan(src.scene, src.model, &po, a, paper, &pinfo, &derr);
        if (!r && pinfo_out) *pinfo_out = pinfo;
    }
    if (r) {
        if (r < 0 || derr.oom) buf_puts(err, "out of memory");
        else buf_append(err, derr.p, derr.len);
    }
    buf_free(&derr);
    cc_paper_src_free(&src);
    return r ? -1 : 0;
}

int cc_paper_pdf_query(struct cc_session *s, const cc_scene *sc, int view, const char *body, size_t n, Buf *out) {
    const char *key = view ? "view" : "sheet";
    Arena a;
    arena_init(&a, 0);
    int rc = CC_ERR;
    Buf pdf;
    buf_init(&pdf);
    JrError je;
    const JrVal *v = jr_parse(&a, body, n, &je);
    const JrVal *idv = v && v->type == JR_OBJ ? jr_get(v, key) : NULL;
    const JrVal *pv = v && v->type == JR_OBJ ? jr_get(v, "path") : NULL;
    if (!v) {
        buf_puts(out, je.msg);
        goto end;
    }
    if (!idv || idv->type != JR_STR) {
        buf_puts(out, idv ? "" : "missing field `"), buf_puts(out, key), buf_puts(out, idv ? ": a string" : "`");
        goto end;
    }
    if (pv && pv->type != JR_STR && pv->type != JR_NULL) {
        buf_puts(out, "path: a string");
        goto end;
    }
    /* SIZE1: "font": "helvetica" (the default: no file needed) or "roboto" (when the files are here). */
    const JrVal *fv = v->type == JR_OBJ ? jr_get(v, "font") : NULL;
    int want_roboto = 0;
    if (fv && fv->type != JR_NULL) {
        if (fv->type == JR_STR && fv->n == 6 && !memcmp(fv->s, "roboto", 6)) want_roboto = 1;
        else if (!(fv->type == JR_STR && fv->n == 9 && !memcmp(fv->s, "helvetica", 9))) {
            buf_puts(out, "font: \"helvetica\" or \"roboto\"");
            goto end;
        }
    }
    const char *path = pv && pv->type == JR_STR ? pv->s : NULL;
    if (path && (!pv->n || strlen(path) < pv->n)) {
        buf_puts(out, "path: empty or with a NUL");
        goto end;
    }
    const cc_sheet *sh = NULL;
    const cc_view *vw = NULL;
    cc_view *views = NULL;
    size_t nv = 0;
    if ((s->open || (view && s->view)) && idv->n == 22) { /* LNK1: a read-only scene's views too */
        cc_gid id;
        memcpy(id.c, idv->s, 22);
        if (!view) sh = cc_model_sheet(&s->model, &id);
        else {
            if (s->open ? cc_views_effective(&s->model, &a, &views, &nv)
                        : cc_sv_list(s, &a, &views, &nv) || cc_paper_src_views(s, views, nv, &a)) {
                buf_puts(out, "out of memory");
                goto end;
            }
            for (size_t k = 0; k < nv && !vw; k++)
                if (!cc_gid_cmp(&views[k].id, &id)) vw = &views[k];
        }
    }
    if (!sh && !vw) {
        buf_puts(out, view ? "no view with id " : "no sheet with id ");
        debug_str(out, idv->s, idv->n);
        goto end;
    }
    if (!sc) {
        buf_puts(out, "no model open");
        goto end;
    }
    if (want_roboto) read_fonts(s); /* SIZE1: only when asked for */
    cc_pdf_options po;
    memset(&po, 0, sizeof po);
    po.version = s->paper_version ? s->paper_version : CC_IFC_VERSION;
    po.version_len = strlen(po.version);
    for (int k = 0; k < 2; k++) po.roboto[k] = s->pdf_font[k], po.roboto_len[k] = s->pdf_font_n[k];
    /* SIZE1: Helvetica by default; Roboto when asked for and both files are there; else, or when the
     * writer refuses them, Helvetica (laid out with Helvetica's metrics: Rust's Helvetica file). */
    int r = CC_PDF_ERR_FONT;
    if (want_roboto && s->pdf_font[0] && s->pdf_font[1]) {
        cc_paper paper;
        if (draw_paper(s, sc, sh, vw, views, nv, CC_FONT_ROBOTO, &a, &paper, out)) goto end;
        r = cc_paper_pdf(&paper, &po, &pdf);
    }
    if (r == CC_PDF_ERR_FONT) {
        if (want_roboto)
            cc_log(s->pf, "bimble-ccore: PDF: the Roboto font files (Roboto-Regular.ttf, Roboto-Bold.ttf) are missing "
                          "or not the shipped ones: the text is set in Helvetica");
        buf_clear(&pdf);
        pdf.oom = 0;
        cc_paper paper;
        if (draw_paper(s, sc, sh, vw, views, nv, CC_FONT_HELVETICA, &a, &paper, out)) goto end;
        r = cc_paper_pdf(&paper, &po, &pdf);
    }
    if (r == CC_PDF_ERR_PEN) {
        buf_puts(out, "a pen of negative width");
        goto end;
    }
    if (r || pdf.oom) {
        buf_puts(out, "out of memory");
        goto end;
    }
    if (path && s->pf->files) {
        if (s->pf->write_file(s->pf->ctx, path, pdf.p, pdf.len)) {
            buf_puts(out, path), buf_puts(out, ": cannot write");
            goto end;
        }
        cc_log(s->pf, "bimble-ccore: Saved %s", path);
        JsonW w;
        jw_init(&w, out, 0);
        jw_obj_begin(&w);
        jw_key(&w, "bytes"), jw_u64(&w, pdf.len);
        jw_key(&w, "path"), jw_str(&w, path);
        jw_obj_end(&w);
        rc = jw_finish(&w) == JW_OK && !out->oom ? CC_OK : CC_ERR;
        goto end;
    }
    buf_append(out, pdf.p, pdf.len);
    rc = out->oom ? CC_ERR : CC_OK;
end:
    buf_free(&pdf);
    arena_free(&a);
    return rc;
}
/* ---- PAPER1: view_svg (a view's paper as SVG) and sheets_pdf (several sheets, one PDF) ---- */

/* "font": absent or null -> `dflt`, "roboto" or "helvetica"; else -1 with the error in `out`. */
static int font_of(const JrVal *v, int dflt, Buf *out) {
    const JrVal *fv = v && v->type == JR_OBJ ? jr_get(v, "font") : NULL;
    if (!fv || fv->type == JR_NULL) return dflt;
    if (fv->type == JR_STR && fv->n == 6 && !memcmp(fv->s, "roboto", 6)) return 1;
    if (fv->type == JR_STR && fv->n == 9 && !memcmp(fv->s, "helvetica", 9)) return 0;
    buf_puts(out, "font: \"helvetica\" or \"roboto\"");
    return -1;
}

int cc_paper_view_svg(struct cc_session *s, const cc_scene *sc, const char *body, size_t n, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    int rc = CC_ERR;
    Buf svg;
    buf_init(&svg);
    JrError je;
    const JrVal *v = jr_parse(&a, body, n, &je);
    const JrVal *idv = v && v->type == JR_OBJ ? jr_get(v, "view") : NULL;
    const JrVal *bv = v && v->type == JR_OBJ ? jr_get(v, "bare") : NULL;
    if (!v) {
        buf_puts(out, je.msg);
        goto end;
    }
    if (!idv || idv->type != JR_STR) {
        buf_puts(out, idv ? "view: a string" : "missing field `view`");
        goto end;
    }
    if (bv && bv->type != JR_BOOL && bv->type != JR_NULL) {
        buf_puts(out, "bare: true or false");
        goto end;
    }
    int roboto = font_of(v, 1, out); /* the SVG names its font: the style's (Roboto) by default */
    if (roboto < 0) goto end;
    const cc_view *vw = NULL;
    cc_view *views = NULL;
    size_t nv = 0;
    if ((s->open || s->view) && idv->n == 22) { /* LNK1: a read-only scene's views too */
        cc_gid id;
        memcpy(id.c, idv->s, 22);
        if (s->open ? cc_views_effective(&s->model, &a, &views, &nv)
                    : cc_sv_list(s, &a, &views, &nv) || cc_paper_src_views(s, views, nv, &a)) {
            buf_puts(out, "out of memory");
            goto end;
        }
        for (size_t k = 0; k < nv && !vw; k++)
            if (!cc_gid_cmp(&views[k].id, &id)) vw = &views[k];
    }
    if (!vw) {
        buf_puts(out, "no view with id ");
        debug_str(out, idv->s, idv->n);
        goto end;
    }
    if (!sc) {
        buf_puts(out, "no model open");
        goto end;
    }
    cc_paper paper;
    cc_plan_info info;
    if (draw_paper_ex(s, sc, NULL, vw, views, nv, roboto ? CC_FONT_ROBOTO : CC_FONT_HELVETICA,
                      bv && bv->type == JR_BOOL && bv->b, &info, &a, &paper, out))
        goto end;
    if (cc_paper_svg(&paper, &svg)) {
        buf_puts(out, "out of memory");
        goto end;
    }
    JsonW w;
    jw_init(&w, out, 0);
    jw_obj_begin(&w);
    jw_key(&w, "height_mm"), jw_f64(&w, paper.height_mm);
    jw_key(&w, "paper"), jw_str(&w, cc_paper_size_name(info.paper));
    jw_key(&w, "portrait"), jw_bool(&w, info.portrait);
    jw_key(&w, "scale"), jw_u64(&w, vw->scale);
    jw_key(&w, "svg"), jw_str_n(&w, svg.p, svg.len);
    jw_key(&w, "width_mm"), jw_f64(&w, paper.width_mm);
    jw_obj_end(&w);
    rc = jw_finish(&w) == JW_OK && !out->oom ? CC_OK : CC_ERR;
end:
    buf_free(&svg);
    arena_free(&a);
    return rc;
}

int cc_paper_sheets_pdf(struct cc_session *s, const cc_scene *sc, const char *body, size_t n, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    int rc = CC_ERR;
    Buf pdf;
    buf_init(&pdf);
    const cc_sheet **sh = NULL;
    cc_paper *papers = NULL;
    const cc_paper **pp = NULL;
    JrError je;
    const JrVal *v = jr_parse(&a, body, n, &je);
    const JrVal *lv = v && v->type == JR_OBJ ? jr_get(v, "sheets") : NULL;
    const JrVal *pv = v && v->type == JR_OBJ ? jr_get(v, "path") : NULL;
    if (!v) {
        buf_puts(out, je.msg);
        goto end;
    }
    if (!lv || lv->type != JR_ARR) {
        buf_puts(out, lv ? "sheets: an array of sheet ids" : "missing field `sheets`");
        goto end;
    }
    if (!lv->n || lv->n > CC_PDF_MAX_PAGES) {
        buf_puts(out, lv->n ? "sheets: at most 1000" : "sheets: at least one sheet");
        goto end;
    }
    if (pv && pv->type != JR_STR && pv->type != JR_NULL) {
        buf_puts(out, "path: a string");
        goto end;
    }
    int want_roboto = font_of(v, 0, out); /* the PDF: Helvetica by default (SIZE1) */
    if (want_roboto < 0) goto end;
    const char *path = pv && pv->type == JR_STR ? pv->s : NULL;
    if (path && (!pv->n || strlen(path) < pv->n)) {
        buf_puts(out, "path: empty or with a NUL");
        goto end;
    }
    size_t ns = lv->n;
    sh = calloc(ns, sizeof *sh);
    papers = calloc(ns, sizeof *papers);
    pp = calloc(ns, sizeof *pp);
    if (!sh || !papers || !pp) {
        buf_puts(out, "out of memory");
        goto end;
    }
    for (size_t k = 0; k < ns; k++) {
        const JrVal *x = &lv->items[k];
        if (x->type != JR_STR) {
            buf_puts(out, "sheets: an array of sheet ids");
            goto end;
        }
        if (s->open && x->n == 22) {
            cc_gid id;
            memcpy(id.c, x->s, 22);
            sh[k] = cc_model_sheet(&s->model, &id);
        }
        if (!sh[k]) {
            buf_puts(out, "no sheet with id ");
            debug_str(out, x->s, x->n);
            goto end;
        }
        pp[k] = &papers[k];
    }
    if (!sc) {
        buf_puts(out, "no model open");
        goto end;
    }
    if (want_roboto) read_fonts(s);
    cc_pdf_options po;
    memset(&po, 0, sizeof po);
    po.version = s->paper_version ? s->paper_version : CC_IFC_VERSION;
    po.version_len = strlen(po.version);
    for (int k = 0; k < 2; k++) po.roboto[k] = s->pdf_font[k], po.roboto_len[k] = s->pdf_font_n[k];
    int r = CC_PDF_ERR_FONT; /* as cc_paper_pdf_query: Roboto when asked for and there, else Helvetica */
    for (int pass = want_roboto && s->pdf_font[0] && s->pdf_font[1] ? 0 : 1; pass < 2 && r == CC_PDF_ERR_FONT; pass++) {
        if (pass == 1 && want_roboto)
            cc_log(s->pf, "bimble-ccore: PDF: the Roboto font files (Roboto-Regular.ttf, Roboto-Bold.ttf) are missing "
                          "or not the shipped ones: the text is set in Helvetica");
        buf_clear(&pdf);
        pdf.oom = 0;
        for (size_t k = 0; k < ns; k++)
            if (draw_paper(s, sc, sh[k], NULL, NULL, 0, pass ? CC_FONT_HELVETICA : CC_FONT_ROBOTO, &a, &papers[k], out))
                goto end;
        r = cc_paper_pdf_pages(pp, ns, &po, &pdf);
    }
    if (r == CC_PDF_ERR_PEN) {
        buf_puts(out, "a pen of negative width");
        goto end;
    }
    if (r || pdf.oom) {
        buf_puts(out, "out of memory");
        goto end;
    }
    if (path && s->pf->files) {
        if (s->pf->write_file(s->pf->ctx, path, pdf.p, pdf.len)) {
            buf_puts(out, path), buf_puts(out, ": cannot write");
            goto end;
        }
        cc_log(s->pf, "bimble-ccore: Saved %s", path);
        JsonW w;
        jw_init(&w, out, 0);
        jw_obj_begin(&w);
        jw_key(&w, "bytes"), jw_u64(&w, pdf.len);
        jw_key(&w, "pages"), jw_u64(&w, ns);
        jw_key(&w, "path"), jw_str(&w, path);
        jw_obj_end(&w);
        rc = jw_finish(&w) == JW_OK && !out->oom ? CC_OK : CC_ERR;
        goto end;
    }
    buf_append(out, pdf.p, pdf.len);
    rc = out->oom ? CC_ERR : CC_OK;
end:
    free(sh), free(papers), free(pp);
    buf_free(&pdf);
    arena_free(&a);
    return rc;
}
/* ---- end PAPER1 ---- */
#endif
