/* Verbs of levels and grids (ADR 0056, ticket G1), C additions of the verb table (verb_table.c):
 * level_add, level_set, level_remove, grid_add, grid_axis_set, grid_remove. Each returns one
 * command (add_storey, set_storey, remove_storey, add_grid, set_grid, remove_grid), one history
 * step, or none when nothing changes. Arguments are untrusted: unknown fields are refused, sizes
 * and counts bounded. No Rust oracle (ccore/golden/DEVIATIONS.md). Layer L3 (api). */
#include <string.h>
#include "command.h"
#include "grid.h"
#include "guid.h"
#include "json_r.h"
#include "verbs.h"

#ifndef __TINYC__ /* tcc never contracts and warns on the pragma */
#pragma STDC FP_CONTRACT OFF
#endif

#define GRID_OVERHANG_MM 1000.0 /* grid_add {x, y}: axes run this far past the outer axes */

/* Prototypes for verb_table.c (the table's own handlers are declared in verb_table.h). */
int cc_vh_level_add(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_level_set(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_level_remove(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_grid_add(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_grid_axis_set(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_grid_remove(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);

static int finite_(double x) { return x - x == 0.0; }
static double fabs_(double x) { return x < 0 ? -x : x; }

/* ---- errors ---- */

static int fail(Buf *err, const char *verb, const char *a, cc_str s, const char *b) {
    buf_clear(err);
    buf_puts(err, verb);
    buf_puts(err, ": ");
    if (a) buf_puts(err, a);
    if (s.n) buf_append(err, s.p, s.n < 200 ? s.n : 200);
    if (b) buf_puts(err, b);
    return -1;
}

static const cc_str NONE = {NULL, 0};

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

/* ---- argument decoding ---- */

/* An object's members by name (the last of equal keys wins, as a Value map); an unknown one is
 * refused. A null value counts as absent. */
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

/* A text, trimmed, not blank. The bytes stay in the DOM (out's arena). */
static int a_text(const JrVal *v, const char *field, cc_str *out, Buf *err) {
    if (v->type != JR_STR) return bad_type(err, field, "a string");
    *out = cc_str_trim((cc_str){v->s, v->n});
    if (!out->n) return bad(err, "`", field, "` is blank");
    return 0;
}

static int a_mm(const JrVal *v, const char *field, double *out, Buf *err) {
    if (v->type != JR_NUM) return bad_type(err, field, "a number, mm");
    if (!finite_(v->num) || fabs_(v->num) > CC_MAX_MM) return bad(err, "`", field, "`: at most 1e7 mm");
    *out = v->num + 0.0;
    return 0;
}

static int a_pt(const JrVal *v, const char *field, double out[2], Buf *err) {
    if (v->type != JR_ARR || v->n != 2) return bad_type(err, field, "[x, y], mm");
    for (int k = 0; k < 2; k++)
        if (a_mm(&v->items[k], field, &out[k], err)) return -1;
    return 0;
}

static int a_bool(const JrVal *v, const char *field, int *out, Buf *err) {
    if (v->type != JR_BOOL) return bad_type(err, field, "a boolean");
    *out = v->b;
    return 0;
}

static size_t elements_on(const cc_model *m, const cc_gid *storey) {
    size_t k = 0;
    for (size_t i = 0; i < m->nelements; i++) k += !cc_gid_cmp(&m->elements[i].storey, storey);
    return k;
}

static int no_level(Buf *err, const char *verb, const cc_gid *id) {
    buf_clear(err);
    buf_puts(err, verb);
    buf_puts(err, ": no level ");
    cc_buf_gid(err, id);
    return -1;
}

static int holds(Buf *err, const char *verb, const cc_storey *s, size_t n, const char *tail) {
    buf_clear(err);
    buf_puts(err, verb);
    buf_puts(err, ": the level ");
    buf_append(err, s->name.p, s->name.n < 200 ? s->name.n : 200);
    buf_puts(err, " holds ");
    buf_u64(err, n);
    buf_puts(err, n == 1 ? " element" : " elements");
    buf_puts(err, tail);
    return -1;
}

static void new_gid(cc_gid *g) {
    cc_guid id = cc_guid_new();
    memcpy(g->c, id.c, 22);
}

/* ---- levels ---- */

int cc_vh_level_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    (void)m;
    static const char *const F[] = {"level_mm", "name", "storey"};
    const JrVal *f[3];
    cc_storey s;
    memset(&s, 0, sizeof s);
    int storey = 1;
    if (args_of(args, F, 3, f, err) || need(f[1], "name", err) || need(f[0], "level_mm", err) ||
        a_text(f[1], "name", &s.name, err) || a_mm(f[0], "level_mm", &s.elevation, err) ||
        (f[2] && a_bool(f[2], "storey", &storey, err)))
        return -1;
    s.not_storey = !storey;
    new_gid(&s.id);
    cc_command *c = cc_cmds_push(l, CC_CMD_ADD_STOREY);
    if (!c) return fail(err, "level_add", "out of memory", NONE, 0);
    c->storey = s;
    return 0;
}

int cc_vh_level_set(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"id", "level_mm", "name", "storey"};
    const JrVal *f[4];
    cc_gid id;
    if (args_of(args, F, 4, f, err) || need(f[0], "id", err) || a_gid(f[0], "id", &id, err)) return -1;
    const cc_storey *old = cc_model_storey(m, &id);
    if (!old) return no_level(err, "level_set", &id);
    cc_storey s = *old;
    int storey = !old->not_storey;
    if ((f[2] && a_text(f[2], "name", &s.name, err)) || (f[1] && a_mm(f[1], "level_mm", &s.elevation, err)) ||
        (f[3] && a_bool(f[3], "storey", &storey, err)))
        return -1;
    s.not_storey = !storey;
    if (s.not_storey && !old->not_storey) {
        size_t n = elements_on(m, &id);
        if (n) return holds(err, "level_set", old, n, ": it stays a storey");
    }
    int same = s.not_storey == old->not_storey && s.elevation == old->elevation && s.name.n == old->name.n &&
               (!s.name.n || !memcmp(s.name.p, old->name.p, s.name.n));
    if (same) return 0;
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_STOREY);
    if (!c) return fail(err, "level_set", "out of memory", NONE, 0);
    c->storey = s;
    return 0;
}

int cc_vh_level_remove(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"id"};
    const JrVal *f[1];
    cc_gid id;
    if (args_of(args, F, 1, f, err) || need(f[0], "id", err) || a_gid(f[0], "id", &id, err)) return -1;
    const cc_storey *s = cc_model_storey(m, &id);
    if (!s) return no_level(err, "level_remove", &id);
    size_t n = elements_on(m, &id);
    if (n) return holds(err, "level_remove", s, n, ": move or delete them first");
    cc_command *c = cc_cmds_push(l, CC_CMD_REMOVE_STOREY);
    if (!c) return fail(err, "level_remove", "out of memory", NONE, 0);
    c->id = id;
    return 0;
}

/* ---- grids ---- */

/* The grid checked as the command will check it (grid.h), so a dry run answers what applies. */
static int grid_check(const cc_model *m, const char *verb, const cc_grid *g, Buf *err) {
    Buf why;
    buf_init(&why);
    const cc_grid *old = cc_model_grid(m, &g->id);
    int rc = cc_grids_validate(m, g, 1, NULL, &why);
    /* The other grids' axes: ids unique across the table (an edited grid's own ids are its). */
    for (size_t i = 0; !rc && i < g->naxes; i++)
        for (size_t k = 0; !rc && k < m->ngrids; k++)
            if (&m->grids[k] != old && (cc_grid_axis_find(&m->grids[k], &g->axes[i].id) ||
                                        !cc_gid_cmp(&m->grids[k].id, &g->axes[i].id))) {
                buf_puts(&why, "the axis id ");
                cc_buf_gid(&why, &g->axes[i].id);
                buf_puts(&why, " is used by another grid");
                rc = -1;
            }
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

/* Tag of the k-th axis of a family: numbers "1", "2"… or letters "A"…"Z", "AA", "AB"… */
static cc_str auto_tag(Arena *a, size_t k, int letters) {
    char t[24];
    size_t n = 0;
    size_t v = k + 1;
    if (letters)
        while (v) t[n++] = (char)('A' + (v - 1) % 26), v = (v - 1) / 26;
    else
        while (v) t[n++] = (char)('0' + v % 10), v /= 10;
    char *p = arena_alloc(a, n, 1);
    if (!p) return NONE;
    for (size_t i = 0; i < n; i++) p[i] = t[n - 1 - i];
    return (cc_str){p, n};
}

static int a_coords(const JrVal *v, const char *field, double **out, size_t *n, Arena *a, Buf *err) {
    if (v->type != JR_ARR || v->n == 0) return bad_type(err, field, "a non-empty list of numbers, mm");
    if (v->n > CC_GRID_MAX_AXES) return bad(err, "`", field, "`: at most 1000 axes");
    double *x = arena_alloc(a, v->n * sizeof *x, 8);
    if (!x) return bad(err, "out of memory", 0, 0);
    for (size_t i = 0; i < v->n; i++)
        if (a_mm(&v->items[i], field, &x[i], err)) return -1;
    *out = x;
    *n = v->n;
    return 0;
}

int cc_vh_grid_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"axes", "name", "tags", "x", "y"};
    static const char *const A[] = {"end", "start", "tag"};
    const JrVal *f[5];
    cc_grid g;
    memset(&g, 0, sizeof g);
    if (args_of(args, F, 5, f, err) || need(f[1], "name", err) || a_text(f[1], "name", &g.name, err)) return -1;
    int has_xy = f[3] || f[4];
    if (f[0] && (has_xy || f[2])) return fail(err, "grid_add", "give axes, or x and y (with tags), not both", NONE, 0);
    if (!f[0] && !(f[3] && f[4]))
        return fail(err, "grid_add", "give axes, or x and y: a grid has axes in two directions", NONE, 0);
    new_gid(&g.id);
    if (f[0]) {
        const JrVal *ax = f[0];
        if (ax->type != JR_ARR || ax->n == 0) return bad_type(err, "axes", "a non-empty list of {tag, start, end}");
        if (ax->n > CC_GRID_MAX_AXES) return bad(err, "`axes`: at most 1000 axes", 0, 0);
        g.naxes = ax->n;
        g.axes = arena_alloc(&l->arena, g.naxes * sizeof *g.axes, 8);
        if (!g.axes) return bad(err, "out of memory", 0, 0);
        for (size_t i = 0; i < g.naxes; i++) {
            const JrVal *fa[3];
            cc_grid_axis *a = &g.axes[i];
            memset(a, 0, sizeof *a);
            if (args_of(&ax->items[i], A, 3, fa, err) || need(fa[2], "tag", err) || need(fa[1], "start", err) ||
                need(fa[0], "end", err) || a_text(fa[2], "tag", &a->tag, err) || a_pt(fa[1], "start", a->start, err) ||
                a_pt(fa[0], "end", a->end, err))
                return -1;
            new_gid(&a->id);
        }
    } else {
        double *x, *y;
        size_t nx, ny;
        if (a_coords(f[3], "x", &x, &nx, &l->arena, err) || a_coords(f[4], "y", &y, &ny, &l->arena, err)) return -1;
        if (nx + ny > CC_GRID_MAX_AXES) return fail(err, "grid_add", "at most 1000 axes", NONE, 0);
        if (f[2] && (f[2]->type != JR_ARR || f[2]->n != nx + ny))
            return bad_type(err, "tags", "a list of one tag per axis, the x axes first, then the y axes");
        double x0 = x[0], x1 = x[0], y0 = y[0], y1 = y[0];
        for (size_t i = 1; i < nx; i++) x0 = x[i] < x0 ? x[i] : x0, x1 = x[i] > x1 ? x[i] : x1;
        for (size_t i = 1; i < ny; i++) y0 = y[i] < y0 ? y[i] : y0, y1 = y[i] > y1 ? y[i] : y1;
        x0 -= GRID_OVERHANG_MM, x1 += GRID_OVERHANG_MM, y0 -= GRID_OVERHANG_MM, y1 += GRID_OVERHANG_MM;
        g.naxes = nx + ny;
        g.axes = arena_alloc(&l->arena, g.naxes * sizeof *g.axes, 8);
        if (!g.axes) return bad(err, "out of memory", 0, 0);
        for (size_t i = 0; i < g.naxes; i++) {
            cc_grid_axis *a = &g.axes[i];
            memset(a, 0, sizeof *a);
            if (i < nx) a->start[0] = a->end[0] = x[i], a->start[1] = y0, a->end[1] = y1;
            else a->start[0] = x0, a->end[0] = x1, a->start[1] = a->end[1] = y[i - nx];
            if (f[2]) {
                if (a_text(&f[2]->items[i], "tags", &a->tag, err)) return -1;
            } else if (!(a->tag = auto_tag(&l->arena, i < nx ? i : i - nx, i >= nx)).n)
                return bad(err, "out of memory", 0, 0);
            new_gid(&a->id);
        }
    }
    if (grid_check(m, "grid_add", &g, err)) return -1;
    cc_command *c = cc_cmds_push(l, CC_CMD_ADD_GRID);
    if (!c) return bad(err, "out of memory", 0, 0);
    c->grid = g;
    return 0;
}

int cc_vh_grid_axis_set(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"axis", "end", "grid", "start", "tag"};
    const JrVal *f[5];
    cc_gid gid, aid;
    if (args_of(args, F, 5, f, err) || need(f[2], "grid", err) || need(f[0], "axis", err) ||
        a_gid(f[2], "grid", &gid, err) || a_gid(f[0], "axis", &aid, err))
        return -1;
    const cc_grid *old = cc_model_grid(m, &gid);
    if (!old) {
        buf_clear(err);
        buf_puts(err, "grid_axis_set: no grid ");
        cc_buf_gid(err, &gid);
        return -1;
    }
    const cc_grid_axis *oa = cc_grid_axis_find(old, &aid);
    if (!oa) {
        buf_clear(err);
        buf_puts(err, "grid_axis_set: no axis ");
        cc_buf_gid(err, &aid);
        buf_puts(err, " in the grid ");
        buf_append(err, old->name.p, old->name.n < 200 ? old->name.n : 200);
        return -1;
    }
    cc_grid_axis a = *oa;
    if ((f[4] && a_text(f[4], "tag", &a.tag, err)) || (f[3] && a_pt(f[3], "start", a.start, err)) ||
        (f[1] && a_pt(f[1], "end", a.end, err)))
        return -1;
    if (a.tag.n == oa->tag.n && !memcmp(a.tag.p, oa->tag.p, a.tag.n) && !memcmp(a.start, oa->start, sizeof a.start) &&
        !memcmp(a.end, oa->end, sizeof a.end))
        return 0;
    cc_grid g = *old;
    g.axes = arena_alloc(&l->arena, g.naxes * sizeof *g.axes, 8);
    if (!g.axes) return bad(err, "out of memory", 0, 0);
    memcpy(g.axes, old->axes, g.naxes * sizeof *g.axes);
    g.axes[oa - old->axes] = a;
    if (grid_check(m, "grid_axis_set", &g, err)) return -1;
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_GRID);
    if (!c) return bad(err, "out of memory", 0, 0);
    c->grid = g;
    return 0;
}

int cc_vh_grid_remove(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"id"};
    const JrVal *f[1];
    cc_gid id;
    if (args_of(args, F, 1, f, err) || need(f[0], "id", err) || a_gid(f[0], "id", &id, err)) return -1;
    if (!cc_model_grid(m, &id)) {
        buf_clear(err);
        buf_puts(err, "grid_remove: no grid ");
        cc_buf_gid(err, &id);
        return -1;
    }
    cc_command *c = cc_cmds_push(l, CC_CMD_REMOVE_GRID);
    if (!c) return bad(err, "out of memory", 0, 0);
    c->id = id;
    return 0;
}

/* ---- SLAB1: single grid axes ---- */

int cc_vh_grid_axis_add(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_grid_axis_remove(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);

static const cc_grid *grid_of(const cc_model *m, const char *verb, const cc_gid *gid, Buf *err) {
    const cc_grid *g = cc_model_grid(m, gid);
    if (!g) {
        buf_clear(err);
        buf_puts(err, verb);
        buf_puts(err, ": no grid ");
        cc_buf_gid(err, gid);
    }
    return g;
}

/* grid_axis_add {grid, tag, start, end}: one axis more, with a new GlobalId (the others keep
 * theirs); set_grid, one history step. */
int cc_vh_grid_axis_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"end", "grid", "start", "tag"};
    const JrVal *f[4];
    cc_gid gid;
    cc_grid_axis a;
    memset(&a, 0, sizeof a);
    if (args_of(args, F, 4, f, err) || need(f[1], "grid", err) || need(f[3], "tag", err) || need(f[2], "start", err) ||
        need(f[0], "end", err) || a_gid(f[1], "grid", &gid, err) || a_text(f[3], "tag", &a.tag, err) ||
        a_pt(f[2], "start", a.start, err) || a_pt(f[0], "end", a.end, err))
        return -1;
    const cc_grid *old = grid_of(m, "grid_axis_add", &gid, err);
    if (!old) return -1;
    if (old->naxes >= CC_GRID_MAX_AXES) return fail(err, "grid_axis_add", "a grid has at most 1000 axes", NONE, 0);
    cc_grid g = *old;
    g.axes = arena_alloc(&l->arena, (g.naxes + 1) * sizeof *g.axes, 8);
    if (!g.axes) return bad(err, "out of memory", 0, 0);
    memcpy(g.axes, old->axes, g.naxes * sizeof *g.axes);
    new_gid(&a.id);
    g.axes[g.naxes++] = a;
    if (grid_check(m, "grid_axis_add", &g, err)) return -1;
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_GRID);
    if (!c) return bad(err, "out of memory", 0, 0);
    c->grid = g;
    return 0;
}

/* grid_axis_remove {grid, axis}: the axis goes, the others keep their GlobalIds. IFC4 IfcGrid has
 * UAxes and VAxes, each LIST [1:?] (WAxes optional): the last axis of a direction stays. */
int cc_vh_grid_axis_remove(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"axis", "grid"};
    const JrVal *f[2];
    cc_gid gid, aid;
    if (args_of(args, F, 2, f, err) || need(f[1], "grid", err) || need(f[0], "axis", err) ||
        a_gid(f[1], "grid", &gid, err) || a_gid(f[0], "axis", &aid, err))
        return -1;
    const cc_grid *old = grid_of(m, "grid_axis_remove", &gid, err);
    if (!old) return -1;
    const cc_grid_axis *oa = cc_grid_axis_find(old, &aid);
    if (!oa) {
        buf_clear(err);
        buf_puts(err, "grid_axis_remove: no axis ");
        cc_buf_gid(err, &aid);
        buf_puts(err, " in the grid ");
        buf_append(err, old->name.p, old->name.n < 200 ? old->name.n : 200);
        return -1;
    }
    cc_grid g = *old;
    g.axes = arena_alloc(&l->arena, (g.naxes ? g.naxes : 1) * sizeof *g.axes, 8);
    if (!g.axes) return bad(err, "out of memory", 0, 0);
    size_t k = (size_t)(oa - old->axes);
    memcpy(g.axes, old->axes, k * sizeof *g.axes);
    memcpy(g.axes + k, old->axes + k + 1, (old->naxes - k - 1) * sizeof *g.axes);
    g.naxes--;
    int two = 0;
    for (size_t i = 1; i < g.naxes && !two; i++) two = !cc_grid_axis_is_u(&g, i);
    if (!two) {
        buf_clear(err);
        buf_puts(err, "grid_axis_remove: ");
        buf_append(err, oa->tag.p, oa->tag.n < 200 ? oa->tag.n : 200);
        buf_puts(err, " is the last axis of its direction; an IfcGrid needs axes in two (UAxes and VAxes)");
        return -1;
    }
    if (grid_check(m, "grid_axis_remove", &g, err)) return -1;
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_GRID);
    if (!c) return bad(err, "out of memory", 0, 0);
    c->grid = g;
    return 0;
}
/* ---- end SLAB1 ---- */
