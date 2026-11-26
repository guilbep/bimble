/* Verbs of sheets (ticket SH1), C additions of the verb table (verb_table.c): sheet_place,
 * sheet_move, sheet_unplace, sheet_remove. Rust's dispatch has add_sheet and set_sheet only
 * (verbs.c); nobody could place a view. Each verb returns one command (set_sheet, or remove_sheet
 * for sheet_remove), one history step, or none when nothing changes. The sheet is checked as
 * Command::SetSheet checks it (sheet.rs validate: at most 200 placed, on the paper, scale 1:1 to
 * 1:5000, the view in the model), with Rust's texts. The file a sheet makes stays a plain `Sheet`:
 * Rust reads it. Arguments are untrusted: unknown fields refused, sizes bounded. No Rust oracle
 * (ccore/golden/DEVIATIONS.md, golden/sheets/). Layer L3 (api). */
#include <string.h>
#include "command.h"
#include "guid.h"
#include "json_r.h"
#include "verbs.h"
#include "view_io.h"

#ifndef __TINYC__ /* tcc never contracts and warns on the pragma */
#pragma STDC FP_CONTRACT OFF
#endif

/* Prototypes for verb_table.c (the table's own handlers are declared in verb_table.h). */
int cc_vh_sheet_place(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_sheet_move(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_sheet_unplace(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_sheet_remove(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);

static int finite_(double x) { return x - x == 0.0; }
static double fabs_(double x) { return x < 0 ? -x : x; }

/* ---- errors and argument decoding (the shape of verbs_levels.c) ---- */

static int bad(Buf *err, const char *a, const char *b, const char *c) {
    buf_clear(err);
    buf_puts(err, "bad arguments: ");
    if (a) buf_puts(err, a);
    if (b) buf_puts(err, b);
    if (c) buf_puts(err, c);
    return -1;
}

static int bad_type(Buf *err, const char *field, const char *expected) {
    bad(err, "`", field, "`: expected ");
    buf_puts(err, expected);
    return -1;
}

static int oom(Buf *err, const char *verb) {
    buf_clear(err);
    buf_puts(err, verb);
    buf_puts(err, ": out of memory");
    return -1;
}

/* An object's members by name (the last of equal keys wins); an unknown one is refused. A null
 * value counts as absent. */
static int args_of(const JrVal *v, const char *const *F, int n, const JrVal **f, Buf *err) {
    for (int i = 0; i < n; i++) f[i] = NULL;
    if (v->type != JR_OBJ) return bad(err, "expected an object", 0, 0);
    for (size_t j = 0; j < v->n; j++) {
        const JrMember *mb = &v->members[j];
        int i = 0;
        while (i < n && !jr_key_is(mb, F[i])) i++;
        if (i == n) {
            buf_clear(err);
            buf_puts(err, "bad arguments: unknown field `");
            buf_append(err, mb->key, mb->key_n < 200 ? mb->key_n : 200);
            buf_puts(err, "`, expected one of");
            for (int k = 0; k < n; k++) buf_puts(err, k ? ", `" : " `"), buf_puts(err, F[k]), buf_putc(err, '`');
            return -1;
        }
        f[i] = mb->val.type == JR_NULL ? NULL : &mb->val;
    }
    return 0;
}

static int need(const JrVal *f, const char *name, Buf *err) {
    return f ? 0 : bad(err, "missing field `", name, "`");
}

static int a_gid(const JrVal *v, const char *field, cc_gid *out, Buf *err) {
    if (v->type != JR_STR) return bad_type(err, field, "a GlobalId");
    if (!cc_gid_valid(v->s, v->n)) {
        bad(err, "invalid IFC GlobalId: ", 0, 0);
        cc_buf_debug_str(err, v->s, v->n < 200 ? v->n : 200);
        return -1;
    }
    memcpy(out->c, v->s, 22);
    return 0;
}

/* [x, y], paper mm from the sheet's top-left corner; on the paper is the sheet's check. */
static int a_at(const JrVal *v, double out[2], Buf *err) {
    if (v->type != JR_ARR || v->n != 2) return bad_type(err, "at_mm", "[x, y], paper mm");
    for (int k = 0; k < 2; k++) {
        const JrVal *x = &v->items[k];
        if (x->type != JR_NUM) return bad_type(err, "at_mm", "[x, y], paper mm");
        if (!finite_(x->num) || fabs_(x->num) > CC_MAX_MM) return bad(err, "`at_mm`: at most 1e7 mm", 0, 0);
        out[k] = x->num + 0.0;
    }
    return 0;
}

/* 1:N, N a whole number 1 to 5000 (sheet.rs validate). */
static int a_scale(const JrVal *v, uint32_t *out, Buf *err) {
    if (v->type != JR_NUM) return bad_type(err, "scale", "a whole number N of 1:N");
    if (!(v->num >= 1.0 && v->num <= 5000.0)) return bad(err, "`scale`: 1:1 to 1:5000", 0, 0);
    if (v->num != (double)(uint32_t)v->num) return bad_type(err, "scale", "a whole number N of 1:N");
    *out = (uint32_t)v->num;
    return 0;
}

static const cc_sheet *sheet_of(const cc_model *m, const cc_gid *id, Buf *err) {
    const cc_sheet *s = cc_model_sheet(m, id);
    if (!s) { /* set_sheet's text */
        buf_clear(err);
        buf_puts(err, "no sheet GlobalId(");
        cc_buf_gid(err, id);
        buf_putc(err, ')');
    }
    return s;
}

/* The placed view `id` on `s`, its index, or -1 with the error. */
static long placed_of(const cc_sheet *s, const cc_gid *id, const char *verb, Buf *err) {
    for (size_t i = 0; i < s->nplaced; i++)
        if (!cc_gid_cmp(&s->placed[i].id, id)) return (long)i;
    buf_clear(err);
    buf_puts(err, verb);
    buf_puts(err, ": no placed view ");
    cc_buf_gid(err, id);
    buf_puts(err, " on the sheet ");
    buf_append(err, s->number.p, s->number.n < 200 ? s->number.n : 200);
    return -1;
}

/* The sheet as Command::SetSheet will check it (its number is unchanged: no clash to test). */
static int check(const cc_model *m, const char *verb, const cc_sheet *s, Buf *err) {
    Buf why;
    buf_init(&why);
    int rc = cc_sheet_validate(m, s, &why);
    if (rc) {
        buf_clear(err);
        buf_puts(err, verb);
        buf_puts(err, ": ");
        if (why.oom) buf_puts(err, "out of memory");
        else buf_append(err, why.p, why.len);
    }
    buf_free(&why);
    return rc;
}

/* One set_sheet of `s`, its placed views `p[0..n)` (in out's arena). */
static int push_set(cc_cmds *l, const cc_sheet *s, cc_placed *p, size_t n, const char *verb, Buf *err) {
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_SHEET);
    if (!c) return oom(err, verb);
    c->sheet = *s;
    c->sheet.placed = n ? p : NULL;
    c->sheet.nplaced = n;
    return 0;
}

/* The scale a view is drawn at (ADR 0046, sheet.rs adopted_scales): its own when said (not 100);
 * else the one scale all its placements share; else 100. `*placed` gets 1 when it is on a sheet. */
static uint32_t drawn_scale(const cc_model *m, const cc_gid *view, int *placed) {
    const cc_view *v = cc_model_view(m, view); /* a default view is not stored: 1:100 */
    uint32_t own = v ? v->scale : 100, first = 0;
    int same = 1;
    *placed = 0;
    for (size_t i = 0; i < m->nsheets; i++)
        for (size_t k = 0; k < m->sheets[i].nplaced; k++) {
            const cc_placed *p = &m->sheets[i].placed[k];
            if (cc_gid_cmp(&p->view, view)) continue;
            if (!*placed) first = p->scale;
            else if (p->scale != first) same = 0;
            *placed = 1;
        }
    if (own != 100) return own;
    return *placed && same ? first : 100;
}

/* ---- the verbs ---- */

int cc_vh_sheet_place(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"at_mm", "scale", "sheet", "view"};
    const JrVal *f[4];
    cc_gid sid, vid;
    double at[2];
    uint32_t scale = 0;
    if (args_of(args, F, 4, f, err) || need(f[2], "sheet", err) || need(f[3], "view", err) ||
        need(f[0], "at_mm", err) || a_gid(f[2], "sheet", &sid, err) || a_gid(f[3], "view", &vid, err) ||
        a_at(f[0], at, err) || (f[1] && a_scale(f[1], &scale, err)))
        return -1;
    const cc_sheet *s = sheet_of(m, &sid, err);
    if (!s) return -1;
    int placed;
    uint32_t drawn = drawn_scale(m, &vid, &placed);
    const cc_view *v = cc_model_view(m, &vid);
    /* The first placement of a view whose scale is not said sets it (adopted_scales); after that
     * a view has one scale on every sheet, the view's (set_view changes it). */
    if (scale && scale != drawn && (placed || (v && v->scale != 100))) {
        buf_clear(err);
        buf_puts(err, "sheet_place: the view ");
        if (v) buf_append(err, v->name.p, v->name.n < 200 ? v->name.n : 200);
        else cc_buf_gid(err, &vid);
        buf_puts(err, " is drawn at 1:");
        buf_u64(err, drawn);
        buf_puts(err, ": a view has one scale on every sheet (set_view changes it)");
        return -1;
    }
    cc_placed *p = arena_alloc(&l->arena, (s->nplaced + 1) * sizeof *p, 8);
    if (!p) return oom(err, "sheet_place");
    if (s->nplaced) memcpy(p, s->placed, s->nplaced * sizeof *p);
    cc_placed *n = &p[s->nplaced];
    memset(n, 0, sizeof *n);
    n->view = vid;
    n->scale = scale ? scale : drawn;
    n->at_mm[0] = at[0], n->at_mm[1] = at[1];
    cc_sheet t = *s;
    t.placed = p, t.nplaced = s->nplaced + 1;
    if (check(m, "sheet_place", &t, err)) return -1;
    cc_guid g = cc_guid_new(); /* a refused placement uses none */
    memcpy(n->id.c, g.c, 22);
    return push_set(l, s, p, t.nplaced, "sheet_place", err);
}

int cc_vh_sheet_move(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"at_mm", "placed", "sheet"};
    const JrVal *f[3];
    cc_gid sid, pid;
    double at[2];
    if (args_of(args, F, 3, f, err) || need(f[2], "sheet", err) || need(f[1], "placed", err) ||
        need(f[0], "at_mm", err) || a_gid(f[2], "sheet", &sid, err) || a_gid(f[1], "placed", &pid, err) ||
        a_at(f[0], at, err))
        return -1;
    const cc_sheet *s = sheet_of(m, &sid, err);
    if (!s) return -1;
    long k = placed_of(s, &pid, "sheet_move", err);
    if (k < 0) return -1;
    if (s->placed[k].at_mm[0] == at[0] && s->placed[k].at_mm[1] == at[1]) return 0;
    cc_placed *p = arena_alloc(&l->arena, s->nplaced * sizeof *p, 8);
    if (!p) return oom(err, "sheet_move");
    memcpy(p, s->placed, s->nplaced * sizeof *p);
    p[k].at_mm[0] = at[0], p[k].at_mm[1] = at[1];
    cc_sheet t = *s;
    t.placed = p;
    if (check(m, "sheet_move", &t, err)) return -1;
    return push_set(l, s, p, s->nplaced, "sheet_move", err);
}

int cc_vh_sheet_unplace(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"placed", "sheet"};
    const JrVal *f[2];
    cc_gid sid, pid;
    if (args_of(args, F, 2, f, err) || need(f[1], "sheet", err) || need(f[0], "placed", err) ||
        a_gid(f[1], "sheet", &sid, err) || a_gid(f[0], "placed", &pid, err))
        return -1;
    const cc_sheet *s = sheet_of(m, &sid, err);
    if (!s) return -1;
    long k = placed_of(s, &pid, "sheet_unplace", err);
    if (k < 0) return -1;
    cc_placed *p = s->nplaced > 1 ? arena_alloc(&l->arena, (s->nplaced - 1) * sizeof *p, 8) : NULL;
    if (s->nplaced > 1 && !p) return oom(err, "sheet_unplace");
    size_t n = 0;
    for (size_t i = 0; i < s->nplaced; i++)
        if ((long)i != k) p[n++] = s->placed[i];
    return push_set(l, s, p, n, "sheet_unplace", err);
}

int cc_vh_sheet_remove(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"id"};
    const JrVal *f[1];
    cc_gid id;
    if (args_of(args, F, 1, f, err) || need(f[0], "id", err) || a_gid(f[0], "id", &id, err)) return -1;
    if (!sheet_of(m, &id, err)) return -1;
    cc_command *c = cc_cmds_push(l, CC_CMD_REMOVE_SHEET);
    if (!c) return oom(err, "sheet_remove");
    c->id = id;
    return 0;
}
