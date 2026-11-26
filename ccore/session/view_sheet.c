/* The View, Sheets and Annotate tabs' tools (ticket VIEWSHEET): see view_sheet.h. */
#include "view_sheet.h"
#include <string.h>
#include "arena.h"
#include "json_r.h"

static const char *const NAMES[] = {"new_view", "draw_section", "sheet_place_view", "sheet_revision"};

int cc_vs_index(const char *name) {
    for (int i = 0; i < 4; i++)
        if (!strcmp(name, NAMES[i])) return i;
    return -1;
}

static int same(cc_str a, const char *b, size_t n) { return a.n == n && (!n || !memcmp(a.p, b, n)); }

const cc_text_type *cc_vs_text_type(const cc_paper_style *st, const char *kind) {
    size_t n = strlen(kind);
    for (size_t i = 0; n && i < st->ntext_types; i++)
        if (same(st->text_types[i].name, kind, n)) return &st->text_types[i];
    return NULL;
}

/* An f32 of the style as JSON: its shortest decimal (2.5f is 2.5). */
static void f32(JsonW *w, float x) { jw_f64(w, (double)x); }

static void kind_or_null(JsonW *w, const char *k) {
    if (*k) jw_str(w, k);
    else jw_null(w);
}

void cc_vs_types_json(const cc_viewsheet *v, const cc_paper_style *st, JsonW *w) {
    static const char *const UNIT[3] = {"m", "cm", "mm"};
    jw_obj_begin(w);
    jw_key(w, "dimension"), kind_or_null(w, v->dim_kind);
    jw_key(w, "dimension_types"), jw_arr_begin(w);
    for (size_t i = 0; i < st->ndim_types; i++) {
        const cc_dim_type *t = &st->dim_types[i];
        jw_obj_begin(w);
        jw_key(w, "decimals"), jw_f64(w, t->decimals);
        jw_key(w, "name"), jw_str_n(w, t->name.p, t->name.n);
        jw_key(w, "text_mm"), f32(w, t->text_mm);
        jw_key(w, "unit"), jw_str(w, UNIT[t->unit]);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_key(w, "text"), kind_or_null(w, v->text_kind);
    jw_key(w, "text_types"), jw_arr_begin(w);
    for (size_t i = 0; i < st->ntext_types; i++) {
        const cc_text_type *t = &st->text_types[i];
        jw_obj_begin(w);
        jw_key(w, "font"), jw_str_n(w, t->font.p, t->font.n);
        jw_key(w, "name"), jw_str_n(w, t->name.p, t->name.n);
        jw_key(w, "size_mm"), f32(w, t->size_mm);
        jw_obj_end(w);
    }
    jw_arr_end(w);
    jw_obj_end(w);
}

static int fail(Buf *out, const char *why) {
    buf_puts(out, "annotation_type: ");
    buf_puts(out, why);
    return -1;
}

int cc_vs_set_type(cc_viewsheet *v, const cc_paper_style *st, const char *body, size_t n, Buf *out) {
    Arena a;
    arena_init(&a, 0);
    JrError je;
    const JrVal *j = jr_parse(&a, body ? body : "", body ? n : 0, &je);
    int rc = -1;
    if (!j || j->type != JR_OBJ || j->n != 1) {
        fail(out, "expected {\"text\": a text type | null} or {\"dimension\": a dimension type | null}");
        goto end;
    }
    const JrMember *m = &j->members[0];
    int text = jr_key_is(m, "text");
    if (!text && !jr_key_is(m, "dimension")) {
        fail(out, "expected the key \"text\" or \"dimension\"");
        goto end;
    }
    char *dst = text ? v->text_kind : v->dim_kind;
    if (m->val.type == JR_NULL) { /* Default */
        dst[0] = 0;
        rc = 0;
        goto end;
    }
    if (m->val.type != JR_STR || !m->val.n || m->val.n > CC_VS_KIND_MAX) {
        fail(out, "expected a type's name (at most 200 bytes) or null");
        goto end;
    }
    int known = 0;
    if (text)
        for (size_t i = 0; i < st->ntext_types && !known; i++) known = same(st->text_types[i].name, m->val.s, m->val.n);
    else
        for (size_t i = 0; i < st->ndim_types && !known; i++) known = same(st->dim_types[i].name, m->val.s, m->val.n);
    if (!known) {
        fail(out, text ? "no such text type in the graphic standards" : "no such dimension type in the graphic standards");
        goto end;
    }
    memcpy(dst, m->val.s, m->val.n);
    dst[m->val.n] = 0;
    rc = 0;
end:
    arena_free(&a);
    return rc;
}
