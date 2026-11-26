/* Sheets on paper: see draw_sheet.h. sheetdraw.rs:60-240. */
#include "draw_sheet.h"
#include <stdlib.h>
#include <string.h>
#include "draw_annot.h"
#include "draw_view.h"
#include "view_effective.h"
#include "view_io.h"

/* Sheet margin around the frame, mm (as on plans). */
#define MARGIN 10.0

static cc_str cstr(const char *s) {
    cc_str r = {s, strlen(s)};
    return r;
}

static cc_str arena_str(Arena *a, const char *p, size_t n) {
    cc_str r = {"", 0};
    char *q = arena_alloc(a, n + 1, 1);
    if (!q) {
        r.p = NULL;
        return r;
    }
    if (n) memcpy(q, p, n);
    q[n] = 0;
    r.p = q, r.n = n;
    return r;
}

/* The loops of an item, copied and moved by d (items may share their loops). */
static cc_ring *shifted_rings(Arena *a, const cc_ring *r, size_t n, const double d[2]) {
    cc_ring *o = arena_alloc(a, (n ? n : 1) * sizeof *o, _Alignof(cc_ring));
    if (!o) return NULL;
    for (size_t i = 0; i < n; i++) {
        o[i].n = r[i].n;
        o[i].p = arena_alloc(a, (r[i].n ? r[i].n : 1) * sizeof *o[i].p, _Alignof(double));
        if (!o[i].p) return NULL;
        for (size_t k = 0; k < r[i].n; k++) o[i].p[k][0] = r[i].p[k][0] + d[0], o[i].p[k][1] = r[i].p[k][1] + d[1];
    }
    return o;
}

typedef struct {
    double (*p)[2][2];
    size_t n, cap;
} Segs;

static int seg_push(Segs *s, const double a[2], const double b[2]) {
    if (s->n == s->cap) {
        size_t c = s->cap ? 2 * s->cap : 64;
        void *q = realloc(s->p, c * sizeof *s->p);
        if (!q) return -1;
        s->p = q, s->cap = c;
    }
    s->p[s->n][0][0] = a[0], s->p[s->n][0][1] = a[1];
    s->p[s->n][1][0] = b[0], s->p[s->n][1][1] = b[1];
    s->n++;
    return 0;
}

static int legend_eq(const cc_legend_entry *x, const cc_legend_entry *y) {
    return x->kind == y->kind && (x->kind == CC_SWATCH_ABOVE || x->cat == y->cat) && x->label.n == y->label.n &&
           (!x->label.n || !memcmp(x->label.p, y->label.p, x->label.n));
}

static int rank(cc_swatch_kind k) { return k == CC_SWATCH_CUT ? 0 : k == CC_SWATCH_SEEN ? 1 : 2; }

size_t cc_sheet_scales(const cc_sheet *def, const cc_view *views, size_t nviews, uint32_t *out) {
    size_t n = 0;
    for (size_t i = 0; i < def->nplaced; i++) {
        const cc_view *v = NULL;
        for (size_t k = 0; k < nviews && !v; k++)
            if (!cc_gid_cmp(&views[k].id, &def->placed[i].view)) v = &views[k];
        if (!v) continue;
        size_t j = n++; /* sort_unstable then dedup: an insertion sort, equal ones dropped below */
        while (j > 0 && out[j - 1] > v->scale) out[j] = out[j - 1], j--;
        out[j] = v->scale;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (!k || out[k - 1] != out[i]) out[k++] = out[i];
    return k;
}

/* fields = template fields, then the sheet's over them (BTreeMap extend): sorted by key bytes. */
static cc_kv *merged_fields(Arena *a, const cc_kv *t, size_t nt, const cc_kv *s, size_t ns, size_t *n) {
    cc_kv *o = arena_alloc(a, (nt + ns ? nt + ns : 1) * sizeof *o, _Alignof(cc_kv));
    if (!o) return NULL;
    size_t i = 0, j = 0, k = 0;
    while (i < nt || j < ns) {
        int c;
        if (i == nt) c = 1;
        else if (j == ns) c = -1;
        else {
            size_t m = t[i].key.n < s[j].key.n ? t[i].key.n : s[j].key.n;
            c = m ? memcmp(t[i].key.p, s[j].key.p, m) : 0;
            if (!c) c = t[i].key.n < s[j].key.n ? -1 : t[i].key.n > s[j].key.n;
        }
        if (c < 0) o[k++] = t[i++];
        else if (c > 0) o[k++] = s[j++];
        else o[k++] = s[j++], i++;
    }
    *n = k;
    return o;
}

static int body(const cc_scene *s, const cc_model *m, const cc_sheet *def, const cc_sheet_options *o, Arena *a,
                cc_paper *out, cc_sheet_info *info, Buf *err, cc_item_list *items, Segs *segs, Buf *tmp) {
    Buf why;
    buf_init(&why);
    if (cc_sheet_validate(m, def, &why)) {
        int oom = why.oom;
        buf_append(err, why.p, why.len);
        buf_free(&why);
        return oom ? -1 : 1;
    }
    buf_free(&why);
    int pi = cc_paper_size_parse(def->paper.p, def->paper.n);
    if (pi < 0) {
        buf_puts(err, "unknown paper");
        return 1;
    }
    double w, h;
    cc_paper_size_mm((cc_paper_size)pi, &w, &h);
    if (def->portrait) {
        double t = w;
        w = h, h = t;
    }
    cc_view *views;
    size_t nv;
    cc_section_mark *marks;
    size_t nm;
    if (cc_views_effective(m, a, &views, &nv) || cc_section_marks(views, nv, a, &marks, &nm)) return -1;
    const cc_style *style = o->style;
    cc_rgb ink = style->edges;
    size_t np = def->nplaced;
    info->placed = arena_alloc(a, (np ? np : 1) * sizeof *info->placed, _Alignof(cc_placed_info));
    info->problems = arena_alloc(a, (np ? np : 1) * sizeof *info->problems, _Alignof(cc_str));
    /* The legend entries, each once, in the order the views give them. */
    size_t nleg = 0, capleg = 0;
    cc_legend_entry *legend = NULL;
    if (!info->placed || !info->problems) return -1;
    int rc = -1;
    for (size_t i = 0; i < np; i++) {
        const cc_placed *p = &def->placed[i];
        const cc_view *v = NULL;
        for (size_t k = 0; k < nv && !v; k++)
            if (!cc_gid_cmp(&views[k].id, &p->view)) v = &views[k];
        if (!v) continue;
        cc_plan_options po;
        cc_plan_options_init(&po);
        /* The view's scale (ADR 0046): the same on every sheet, as on screen. */
        po.scale = v->scale;
        po.has_paper = 1;
        po.paper = (cc_paper_size)pi;
        po.style = style;
        po.tpl = o->tpl;
        po.project = o->project;
        po.date = o->date;
        po.version = o->version;
        po.view = v;
        po.bare = 1;
        po.section_marks = marks;
        po.nsection_marks = nm;
        cc_paper drawn;
        cc_plan_info pinfo;
        buf_clear(tmp);
        int r = cc_plan(s, m, &po, a, &drawn, &pinfo, tmp);
        if (r < 0 || tmp->oom) goto end;
        if (r == 1) { /* "{name}: {error}", a note where the view is placed */
            Buf msg;
            buf_init(&msg);
            buf_append(&msg, v->name.p, v->name.n);
            buf_puts(&msg, ": ");
            buf_append(&msg, tmp->p, tmp->len);
            cc_str t = msg.oom ? (cc_str){NULL, 0} : arena_str(a, msg.p, msg.len);
            buf_free(&msg);
            cc_item *it = t.p ? cc_item_list_push(items) : NULL;
            if (!it) goto end;
            it->kind = CC_ITEM_TEXT;
            it->pos[0] = p->at_mm[0], it->pos[1] = p->at_mm[1];
            it->size_mm = 3.0;
            it->text = t.p, it->text_len = t.n;
            it->color = ink;
            it->anchor = CC_ANCHOR_MIDDLE;
            it->angle_deg = 0.0;
            it->bold = 0;
            info->problems[info->nproblems++] = t;
            continue;
        }
        double d[2] = {p->at_mm[0] - drawn.width_mm / 2.0, p->at_mm[1] - drawn.height_mm / 2.0};
        segs->n = 0;
        for (size_t k = 0; k < drawn.nitems; k++) {
            cc_item it = drawn.items[k];
            if (it.kind == CC_ITEM_TEXT) {
                it.pos[0] += d[0], it.pos[1] += d[1];
            } else {
                cc_ring *r2 = shifted_rings(a, it.rings, it.nrings, d);
                if (!r2) goto end;
                it.rings = r2;
                for (size_t l = 0; l < it.nrings; l++) {
                    const cc_ring *g = &r2[l];
                    if (it.kind == CC_ITEM_AREA && !it.has_pen) break;
                    for (size_t q = 0; q + 1 < g->n; q++)
                        if (seg_push(segs, g->p[q], g->p[q + 1])) goto end;
                    if (it.kind == CC_ITEM_AREA && g->n && seg_push(segs, g->p[g->n - 1], g->p[0])) goto end;
                }
            }
            cc_item *dst = cc_item_list_push(items);
            if (!dst) goto end;
            *dst = it;
        }
        cc_placed_info *pl = &info->placed[info->nplaced++];
        pl->id = p->id;
        pl->view = p->view;
        pl->rect[0] = d[0], pl->rect[1] = d[1];
        pl->rect[2] = d[0] + drawn.width_mm, pl->rect[3] = d[1] + drawn.height_mm;
        pl->frame = cc_view_frame_shifted(pinfo.frame, d);
        pl->nsegments = segs->n;
        pl->segments = arena_alloc(a, (segs->n ? segs->n : 1) * sizeof *pl->segments, _Alignof(double));
        if (!pl->segments) goto end;
        if (segs->n) memcpy(pl->segments, segs->p, segs->n * sizeof *segs->p);
        for (size_t k = 0; k < pinfo.nlegend; k++) {
            int seen = 0;
            for (size_t q = 0; q < nleg && !seen; q++) seen = legend_eq(&legend[q], &pinfo.legend[k]);
            if (seen) continue;
            if (nleg == capleg) {
                size_t c = capleg ? 2 * capleg : 16;
                void *x = realloc(legend, c * sizeof *legend);
                if (!x) goto end;
                legend = x, capleg = c;
            }
            legend[nleg++] = pinfo.legend[k];
        }
        info->views_drawn++;
    }
    /* The sheet's texts, one item per line. */
    for (size_t i = 0; i < def->ntexts; i++) {
        const cc_sheet_text *t = &def->texts[i];
        const cc_text_type *tt = NULL;
        for (size_t j = 0; t->has_kind && j < style->paper.ntext_types && !tt; j++) {
            const cc_text_type *x = &style->paper.text_types[j];
            if (x->name.n == t->kind.n && (!x->name.n || !memcmp(x->name.p, t->kind.p, x->name.n))) tt = x;
        }
        double size = tt ? (double)tt->size_mm : t->size_mm;
        cc_rgb color = tt ? tt->color : ink;
        size_t from = 0, line = 0;
        for (size_t k = 0; k <= t->text.n; k++) {
            if (k < t->text.n && t->text.p[k] != '\n') continue;
            cc_str l = arena_str(a, t->text.p + from, k - from);
            cc_item *it = l.p ? cc_item_list_push(items) : NULL;
            if (!it) goto end;
            it->kind = CC_ITEM_TEXT;
            it->pos[0] = t->at_mm[0], it->pos[1] = t->at_mm[1] + (double)line * size * 1.25;
            it->size_mm = size;
            it->text = l.p, it->text_len = l.n;
            it->color = color;
            it->anchor = CC_ANCHOR_START;
            it->angle_deg = t->angle_deg;
            it->bold = 0;
            from = k + 1, line++;
        }
    }
    /* Cut swatches first, then seen, then above, as on a plan (a stable sort). */
    for (size_t i = 1; i < nleg; i++) {
        cc_legend_entry e = legend[i];
        size_t j = i;
        while (j > 0 && rank(legend[j - 1].kind) > rank(e.kind)) legend[j] = legend[j - 1], j--;
        legend[j] = e;
    }
    cc_annot_ctx ac;
    memset(&ac, 0, sizeof ac);
    ac.a = a;
    ac.items = items;
    ac.style = style;
    ac.tpl = o->tpl;
    ac.scale = 100;
    ac.k = 1.0;
    if (cc_draw_legend(&ac, MARGIN, h - MARGIN - o->tpl->title_block.height_mm, legend, nleg)) goto end;

    size_t nf;
    cc_kv *fields = merged_fields(a, o->tpl->fields, o->tpl->nfields, def->fields, def->nfields, &nf);
    uint32_t *sc = arena_alloc(a, (np ? np : 1) * sizeof *sc, _Alignof(uint32_t));
    if (!fields || !sc) goto end;
    size_t nsc = cc_sheet_scales(def, views, nv, sc);
    Buf scales, paper;
    buf_init(&scales), buf_init(&paper);
    for (size_t i = 0; i < nsc; i++) {
        if (i) buf_puts(&scales, ", ");
        buf_puts(&scales, "1:");
        buf_u64(&scales, sc[i]);
    }
    buf_puts(&paper, cc_paper_size_name((cc_paper_size)pi));
    if (def->portrait) buf_puts(&paper, " portrait");
    const cc_str values[13][2] = {
        {cstr("project"), o->project},
        {cstr("number"), def->number},
        {cstr("sheet"), def->name},
        {cstr("drawing"), def->name},
        {cstr("drawing_no"), def->number},
        {cstr("scale"), {scales.p ? scales.p : "", scales.len}},
        {cstr("paper"), {paper.p ? paper.p : "", paper.len}},
        {cstr("date"), o->date},
        /* env!("CARGO_PKG_VERSION") in Rust. */
        {cstr("bimble"), o->version},
        /* Plan-only values: empty on a sheet of several views. */
        {cstr("storey"), cstr("")},
        {cstr("level"), cstr("")},
        {cstr("view"), cstr("")},
        {cstr("n"), cstr("")},
    };
    cc_fill_values fv = {values, 13, fields, nf};
    int bad = scales.oom || paper.oom || cc_frame_and_title_block(&ac, w, h, &fv, o->version);
    buf_free(&scales), buf_free(&paper);
    if (bad) goto end;

    /* The title: "{number} {name}", after "{project} – " when there is a project. */
    Buf title;
    buf_init(&title);
    if (o->project.n) {
        buf_append(&title, o->project.p, o->project.n);
        buf_puts(&title, " \xe2\x80\x93 ");
    }
    buf_append(&title, def->number.p, def->number.n);
    buf_putc(&title, ' ');
    buf_append(&title, def->name.p, def->name.n);
    cc_str ts = title.oom ? (cc_str){NULL, 0} : arena_str(a, title.p, title.len);
    buf_free(&title);
    cc_item *all = arena_alloc(a, (items->n ? items->n : 1) * sizeof *all, _Alignof(cc_item));
    if (!ts.p || !all) goto end;
    if (items->n) memcpy(all, items->p, items->n * sizeof *all);
    out->width_mm = w;
    out->height_mm = h;
    out->title = ts.p;
    out->title_len = ts.n;
    out->items = all;
    out->nitems = items->n;
    out->font = style->paper.font;
    rc = 0;
end:
    free(legend);
    return rc;
}

int cc_sheet_draw(const cc_scene *s, const cc_model *m, const cc_sheet *def, const cc_sheet_options *o, Arena *a,
                  cc_paper *out, cc_sheet_info *info, Buf *err) {
    memset(out, 0, sizeof *out);
    memset(info, 0, sizeof *info);
    cc_item_list items = {0};
    Segs segs = {0};
    Buf tmp;
    buf_init(&tmp);
    int rc = body(s, m, def, o, a, out, info, err, &items, &segs, &tmp);
    free(items.p);
    free(segs.p);
    buf_free(&tmp);
    if (rc) memset(out, 0, sizeof *out);
    return rc;
}
