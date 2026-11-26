/* Managing a reference plan (links_dxf.h, ticket L2). */
#pragma STDC FP_CONTRACT OFF
#include "links_dxf.h"
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "ref_read.h"
#include "session.h"
#include "view_io.h"
#include "view_only.h"


static const char *base_name(const char *p, size_t n, size_t *bn) {
    size_t i = n;
    while (i > 0 && p[i - 1] != '/' && p[i - 1] != '\\') i--;
    *bn = n - i;
    return p + i;
}

#ifndef CC_VIEW_ONLY
/* The file at path[0..n) read again as a reference (cc_ref_read: by its extension), its lines in
 * `a`: name, format, source and polylines of `r` replaced (views.rs:263). 0 ok, -1 with the text. */
static int reread(struct cc_session *s, const char *path, size_t n, double pdf_scale, Arena *a, cc_reference *r, Buf *out) {
    size_t bn;
    const char *name = base_name(path, n, &bn);
    char *bytes = NULL;
    size_t nb = 0;
    int st = s->pf->read_file(s->pf->ctx, path, CC_REF_MAX_FILE_BYTES, &bytes, &nb);
    if (st != CC_PF_OK) {
        buf_append(out, path, n);
        buf_puts(out, st == CC_PF_NOT_FOUND ? ": not found" : st == CC_PF_TOO_LARGE ? ": larger than 256 MB" : ": cannot be read");
        return -1;
    }
    cc_refread rd;
    int rc = cc_ref_read(name, bn, (const unsigned char *)bytes, nb, pdf_scale, &rd, out);
    free(bytes);
    if (rc) return -1;
    cc_loop *lines = arena_alloc(a, (rd.nlines ? rd.nlines : 1) * sizeof *lines, 8);
    double(*pts)[2] = arena_alloc(a, (rd.npts ? rd.npts : 1) * sizeof *pts, 8);
    char *nm = arena_dup(a, name, bn);
    char *src = arena_dup(a, path, n);
    if (!lines || !pts || !nm || !src) {
        cc_refread_free(&rd);
        return buf_puts(out, "out of memory"), -1;
    }
    if (rd.npts) memcpy(pts, rd.xy, rd.npts * sizeof *pts);
    for (size_t i = 0; i < rd.nlines; i++) lines[i].pts = pts + rd.start[i], lines[i].n = rd.start[i + 1] - rd.start[i];
    r->polylines = lines, r->npolylines = rd.nlines;
    cc_refread_free(&rd);
    r->name.p = nm, r->name.n = bn;
    const char *dot = NULL; /* format: the extension, lowercased (viewing.rs:1490) */
    for (size_t i = 0; i < bn; i++)
        if (name[i] == '.') dot = name + i;
    char *f = arena_alloc(a, 8, 1);
    size_t fn = 0;
    if (f && dot)
        for (const char *q = dot + 1; q < name + bn && fn < 7; q++) f[fn++] = (char)(*q >= 'A' && *q <= 'Z' ? *q - 'A' + 'a' : *q);
    if (f) r->format.p = f, r->format.n = fn;
    if (s->pf->files) r->has_source = 1, r->source.p = src, r->source.n = n; /* new.source.or(old) */
    return 0;
}
#endif

/* The command setting `v` (a stored view: set_view, a default one: add_view). */
static int push_view(struct cc_session *s, cc_cmds *l, const cc_view *v, Buf *out) {
    cc_command *c = cc_cmds_push(l, cc_model_view(&s->model, &v->id) ? CC_CMD_SET_VIEW : CC_CMD_ADD_VIEW);
    if (!c) return buf_puts(out, "out of memory"), -1;
    c->view = *v;
    return 0;
}

/* The stored view holding reference `id` and its index. */
static const cc_view *find_ref(const struct cc_session *s, const cc_gid *id, size_t *k) {
    if (!s->open) return NULL;
    for (size_t i = 0; i < s->model.nviews; i++)
        for (size_t j = 0; j < s->model.views[i].nreferences; j++)
            if (!cc_gid_cmp(&s->model.views[i].references[j].id, id)) return *k = j, &s->model.views[i];
    return NULL;
}

static int num(const JrVal *v, double *x) {
    if (!v || v->type != JR_NUM || !(v->num == v->num) || v->num - v->num != 0.0) return -1;
    *x = v->num + 0.0; /* no -0.0 */
    return 0;
}

int cc_ref_manage(struct cc_session *s, const JrVal *body, Arena *a, cc_cmds *l, Buf *out) {
    const JrVal *op = jr_get(body, "op"), *idv = jr_get(body, "id");
    if (!op || op->type != JR_STR) return buf_puts(out, "op: a string"), -1;
    if (!idv || idv->type != JR_STR || idv->n != 22) return buf_puts(out, "id: a reference plan's GlobalId (query links)"), -1;
    cc_gid id;
    memcpy(id.c, idv->s, 22);
    size_t k;
    const cc_view *sv = find_ref(s, &id, &k);
    if (!sv) return buf_puts(out, "no reference plan GlobalId("), buf_append(out, idv->s, 22), buf_puts(out, ")"), -1;
    cc_view v = *sv;
    cc_reference *refs = arena_alloc(a, v.nreferences * sizeof *refs, 8);
    if (!refs) return buf_puts(out, "out of memory"), -1;
    memcpy(refs, v.references, v.nreferences * sizeof *refs);
    v.references = refs;
    cc_reference *r = &refs[k];
#define OP(x) (op->n == strlen(x) && !memcmp(op->s, x, op->n))
    if (OP("remove")) {
        memmove(refs + k, refs + k + 1, (v.nreferences - k - 1) * sizeof *refs);
        v.nreferences--;
    } else if (OP("show") || OP("hide")) {
        if (r->visible == OP("show")) return 0;
        r->visible = OP("show");
    } else if (OP("move")) {
        const JrVal *o = jr_get(body, "offset_mm");
        double x, y;
        if (!o || o->type != JR_ARR || o->n != 2 || num(&o->items[0], &x) || num(&o->items[1], &y))
            return buf_puts(out, "offset_mm: [x, y], finite mm"), -1;
        if (r->offset_mm[0] == x && r->offset_mm[1] == y) return 0;
        r->offset_mm[0] = x, r->offset_mm[1] = y;
    } else if (OP("rotate")) {
        double d;
        if (num(jr_get(body, "rotation_deg"), &d)) return buf_puts(out, "rotation_deg: finite degrees"), -1;
        if (r->rotation_deg == d) return 0;
        r->rotation_deg = d;
    } else if (OP("reload")) { /* views.rs:263 reload_reference: lines, name, format; the place kept */
#ifdef CC_VIEW_ONLY
        return buf_puts(out, CC_VIEW_ONLY_TEXT), -1;
#else
        const JrVal *p = jr_get(body, "path");
        const char *path = NULL;
        size_t pn = 0;
        if (p && p->type == JR_STR) path = p->s, pn = p->n;
        else if (r->has_source && s->pf->files) {
            char *z = arena_alloc(a, r->source.n + 1, 1); /* the model's strings are not NUL-terminated */
            if (z) memcpy(z, r->source.p, r->source.n), z[r->source.n] = 0;
            path = z, pn = r->source.n;
        }
        else return buf_puts(out, "where it was read from isn't known here: reload it with {\"path\"}"), -1;
        if (!path) return buf_puts(out, "out of memory"), -1;
        if (!pn || strlen(path) < pn) return buf_puts(out, "path: empty or with a NUL"), -1;
        if (reread(s, path, pn, v.scale ? (double)v.scale : 100.0, a, r, out)) return -1;
#endif
    } else return buf_puts(out, "op: one of list, remove, reload, show, hide, move, rotate"), -1;
#undef OP
    return push_view(s, l, &v, out);
}
