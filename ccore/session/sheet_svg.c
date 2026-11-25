/* Sheets on paper in the session: see sheet_svg.h. */
#include "sheet_svg.h"
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
#include "draw_style.h"
#include "ifc_out.h" /* cc_iso_utc, CC_IFC_VERSION */
#include "pdf_out.h"
#endif

void cc_paper_cache_free(struct cc_session *s) {
    buf_free(&s->paper_svg);
    buf_free(&s->paper_key);
    free(s->paper_ids), free(s->paper_rects);
    s->paper_ids = NULL, s->paper_rects = NULL, s->npaper = 0;
}

#ifdef CC_VIEW_ONLY
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
#else
/* Path::file_stem of the session's path: the file name without its last extension (a name that
 * starts with its only dot keeps it). */
static cc_str file_stem(const char *path) {
    const char *f = strrchr(path, '/');
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
    cc_sheet_options o = {&style, &tpl, file_stem(s->path), {date, strlen(date)},
                          {CC_IFC_VERSION, strlen(CC_IFC_VERSION)}};
    cc_paper paper;
    cc_sheet_info info;
    int r = cc_sheet_draw(sc, &s->model, sh, &o, &a, &paper, &info, &derr);
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
    cc_sheet_options o = {&style, &tpl, file_stem(s->path), {date, strlen(date)},
                          {CC_IFC_VERSION, strlen(CC_IFC_VERSION)}};
    cc_paper paper;
    cc_sheet_info info;
    int r = cc_sheet_draw(sc, &s->model, sh, &o, &a, &paper, &info, &derr);
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
#endif
