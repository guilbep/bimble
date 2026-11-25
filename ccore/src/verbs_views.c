/* Verbs of views (ticket SEC1): see verbs_views.h. No Rust oracle for the verbs themselves
 * (ccore/golden/DEVIATIONS.md section 21); the views they make are Rust's (views.rs new_plan,
 * new_section), stored as Rust stores them. Layer L3 (api). */
#include <stdlib.h>
#include <string.h>
#include "command.h"
#include "guid.h"
#include "json_r.h"
#include "verbs.h"
#include "verbs_views.h"
#include "view_effective.h"
#include "view_io.h"

#define MAX_LIST 100000 /* entries in one visibility list */

static int finite_(double x) { return x - x == 0.0; }
static double fabs_(double x) { return x < 0 ? -x : x; }

static int bad(Buf *err, const char *a, const char *b, const char *c) {
    buf_clear(err);
    buf_puts(err, "bad arguments: ");
    if (a) buf_puts(err, a);
    if (b) buf_puts(err, b);
    if (c) buf_puts(err, c);
    return -1;
}

static int oom(Buf *err) {
    buf_clear(err);
    buf_puts(err, "out of memory");
    return -1;
}

/* An object's members by name; an unknown one is refused; null counts as absent. */
static int args_of(const JrVal *v, const char *const *F, int n, const JrVal **f, Buf *err) {
    for (int i = 0; i < n; i++) f[i] = NULL;
    if (!v || v->type != JR_OBJ) return bad(err, "expected an object", 0, 0);
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

static int need(const JrVal *f, const char *name, Buf *err) { return f ? 0 : bad(err, "missing field `", name, "`"); }

static int a_gid(const JrVal *v, const char *field, cc_gid *out, Buf *err) {
    if (v->type != JR_STR) return bad(err, "`", field, "`: expected a GlobalId");
    if (!cc_gid_valid(v->s, v->n)) {
        bad(err, "invalid IFC GlobalId: ", 0, 0);
        cc_buf_debug_str(err, v->s, v->n < 200 ? v->n : 200);
        return -1;
    }
    memcpy(out->c, v->s, 22);
    return 0;
}

static int a_text(const JrVal *v, const char *field, cc_str *out, Buf *err) {
    if (v->type != JR_STR) return bad(err, "`", field, "`: expected a string");
    *out = cc_str_trim((cc_str){v->s, v->n});
    if (!out->n) return bad(err, "`", field, "` is blank");
    if (out->n > 800) return bad(err, "`", field, "`: at most 200 characters");
    return 0;
}

static int a_mm(const JrVal *v, const char *field, double *out, Buf *err) {
    if (v->type != JR_NUM) return bad(err, "`", field, "`: expected a number, mm");
    if (!finite_(v->num) || fabs_(v->num) > CC_MAX_MM) return bad(err, "`", field, "`: at most 1e7 mm");
    *out = v->num + 0.0;
    return 0;
}

static int a_pt(const JrVal *v, const char *field, double out[2], Buf *err) {
    if (v->type != JR_ARR || v->n != 2) return bad(err, "`", field, "`: expected [x, y], mm");
    for (int k = 0; k < 2; k++)
        if (a_mm(&v->items[k], field, &out[k], err)) return -1;
    return 0;
}

/* One of `names` (n of them): its index, or -1 with the error. */
static int a_enum(const JrVal *v, const char *field, const char *const *names, int n, Buf *err) {
    if (v->type == JR_STR)
        for (int i = 0; i < n; i++)
            if (strlen(names[i]) == v->n && !memcmp(names[i], v->s, v->n)) return i;
    bad(err, "`", field, "`: expected one of");
    for (int i = 0; i < n; i++) buf_puts(err, i ? ", " : " "), buf_puts(err, names[i]);
    return -1;
}

static void new_gid(cc_gid *g) {
    cc_guid id = cc_guid_new();
    memcpy(g->c, id.c, 22);
}

static cc_str cat(Arena *a, const char *x, cc_str y, const char *z) {
    size_t nx = strlen(x), nz = strlen(z);
    char *p = arena_alloc(a, nx + y.n + nz + 1, 1);
    if (!p) return (cc_str){NULL, 0};
    memcpy(p, x, nx);
    if (y.n) memcpy(p + nx, y.p, y.n);
    memcpy(p + nx + y.n, z, nz);
    p[nx + y.n + nz] = 0;
    return (cc_str){p, nx + y.n + nz};
}

/* The view `id` as view::effective gives it (a stored one, or a default), copied into `a`.
 * 1 found, 0 not, -1 out of memory. */
static int effective(const cc_model *m, const cc_gid *id, Arena *a, cc_view *out, size_t *nviews) {
    cc_view *v;
    size_t n;
    if (cc_views_effective(m, a, &v, &n)) return -1;
    if (nviews) *nviews = n;
    for (size_t i = 0; i < n; i++)
        if (!cc_gid_cmp(&v[i].id, id)) {
            *out = v[i];
            return cc_view_dup(a, out) ? -1 : 1;
        }
    return 0;
}

static int no_view(Buf *err, const cc_gid *id) {
    buf_clear(err);
    buf_puts(err, "no view GlobalId(");
    cc_buf_gid(err, id);
    buf_putc(err, ')');
    return -1;
}

/* Validate as Command::AddView / SetView will, then push it. */
static int push_view(const cc_model *m, cc_view *v, int add, cc_cmds *l, Buf *err) {
    cc_view_canonical(v);
    Buf why;
    buf_init(&why);
    if (cc_view_validate(m, v, &why)) {
        buf_clear(err);
        if (why.oom) buf_puts(err, "out of memory");
        else buf_append(err, why.p, why.len);
        buf_free(&why);
        return -1;
    }
    buf_free(&why);
    cc_command *c = cc_cmds_push(l, add ? CC_CMD_ADD_VIEW : CC_CMD_SET_VIEW);
    if (!c) return oom(err);
    c->view = *v;
    return 0;
}

/* ---- view_add ---- */

static const char *const KINDS[] = {"plan", "ceiling", "section", "three_d"};
static const char *const FACINGS[] = {"north", "south", "east", "west"}; /* cc_facing order */

int cc_vh_view_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"kind", "storey", "facing", "origin", "from", "name"};
    const JrVal *f[6];
    if (args_of(args, F, 6, f, err)) return -1;
    Arena *a = &l->arena;
    cc_view v;
    memset(&v, 0, sizeof v);
    v.scale = 100;
    if (f[4]) { /* Duplicate (viewing.rs:781): the view with a new id, "<name> (copy)" */
        if (f[0] || f[1] || f[2] || f[3]) return bad(err, "`from` duplicates a view: give no kind, storey, facing or origin", 0, 0);
        cc_gid from;
        if (a_gid(f[4], "from", &from, err)) return -1;
        int r = effective(m, &from, a, &v, NULL);
        if (r < 0) return oom(err);
        if (!r) return no_view(err, &from);
        v.name = cat(a, "", v.name, " (copy)");
    } else {
        if (need(f[0], "kind", err)) return -1;
        int k = a_enum(f[0], "kind", KINDS, 4, err);
        if (k < 0) return -1;
        if (k <= 1) { /* views.rs new_plan */
            if (f[2] || f[3]) return bad(err, "a plan has no `facing` or `origin`", 0, 0);
            if (need(f[1], "storey", err)) return -1;
            if (f[1]->type != JR_STR) return bad(err, "`storey`: expected the level's GlobalId", 0, 0);
            Arena t;
            arena_init(&t, 0);
            cc_level *lv;
            size_t n;
            const cc_level *hit = NULL;
            if (cc_levels_of_model(m, &t, &lv, &n)) return arena_free(&t), oom(err);
            for (size_t i = 0; i < n && !hit; i++)
                if (lv[i].key.n == f[1]->n && !memcmp(lv[i].key.p, f[1]->s, f[1]->n)) hit = &lv[i];
            if (!hit) {
                arena_free(&t);
                bad(err, "no level ", 0, 0);
                cc_buf_debug_str(err, f[1]->s, f[1]->n < 200 ? f[1]->n : 200);
                return -1;
            }
            v.kind = CC_VIEW_PLAN;
            v.look = k == 1 ? CC_LOOK_UP : CC_LOOK_DOWN;
            v.name = cat(a, k == 1 ? "Ceiling " : "Plan ", hit->name, " (copy)");
            v.storey = cat(a, "", hit->key, "");
            arena_free(&t);
            /* ViewRange::default_for */
            v.range[CC_RANGE_TOP] = (cc_range_plane){CC_LEVEL_ABOVE, 0.0};
            v.range[CC_RANGE_CUT] = (cc_range_plane){CC_LEVEL_ASSOCIATED, k == 1 ? 2300.0 : 1200.0};
            v.range[CC_RANGE_BOTTOM] = (cc_range_plane){CC_LEVEL_ASSOCIATED, 0.0};
            v.range[CC_RANGE_DEPTH] = (cc_range_plane){k == 1 ? CC_LEVEL_ABOVE : CC_LEVEL_ASSOCIATED, 0.0};
        } else if (k == 2) { /* views.rs new_section */
            if (f[1]) return bad(err, "a section has no `storey`", 0, 0);
            if (need(f[2], "facing", err) || need(f[3], "origin", err)) return -1;
            int fc = a_enum(f[2], "facing", FACINGS, 4, err);
            if (fc < 0 || a_pt(f[3], "origin", v.origin, err)) return -1;
            v.kind = CC_VIEW_SECTION;
            v.facing = (cc_facing)fc;
            size_t nsec = 1;
            for (size_t i = 0; i < m->nviews; i++) nsec += m->views[i].kind == CC_VIEW_SECTION;
            char num[48];
            size_t q = 0;
            char d[24];
            size_t nd = 0;
            do d[nd++] = (char)('0' + nsec % 10), nsec /= 10;
            while (nsec);
            while (nd) num[q++] = d[--nd];
            num[q] = 0;
            cc_str looking = {FACINGS[fc], strlen(FACINGS[fc])};
            cc_str n1 = cat(a, "Section ", (cc_str){num, q}, " (looking ");
            v.name = n1.p ? cat(a, n1.p, looking, ")") : n1;
        } else { /* viewing.rs:836: "3D <views + 1>" */
            if (f[1] || f[2] || f[3]) return bad(err, "a 3D view has no `storey`, `facing` or `origin`", 0, 0);
            Arena t;
            arena_init(&t, 0);
            cc_view *all;
            size_t n;
            if (cc_views_effective(m, &t, &all, &n)) return arena_free(&t), oom(err);
            arena_free(&t);
            char num[24], d[24];
            size_t q = 0, nd = 0;
            n += 1;
            do d[nd++] = (char)('0' + n % 10), n /= 10;
            while (n);
            while (nd) num[q++] = d[--nd];
            v.kind = CC_VIEW_THREE_D;
            v.name = cat(a, "3D ", (cc_str){num, q}, "");
        }
    }
    if (!v.name.p) return oom(err);
    if (f[5] && a_text(f[5], "name", &v.name, err)) return -1;
    new_gid(&v.id);
    return push_view(m, &v, 1, l, err);
}

/* ---- view_remove ---- */

int cc_vh_view_remove(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"id"};
    const JrVal *f[1];
    cc_gid id;
    if (args_of(args, F, 1, f, err) || need(f[0], "id", err) || a_gid(f[0], "id", &id, err)) return -1;
    if (!cc_model_view(m, &id)) {
        buf_clear(err);
        buf_puts(err, "view_remove: the view ");
        cc_buf_gid(err, &id);
        buf_puts(err, cc_view_exists(m, &id) ? " is a default view, unchanged: nothing to remove" : " is not a view of the model");
        return -1;
    }
    cc_command *c = cc_cmds_push(l, CC_CMD_REMOVE_VIEW);
    if (!c) return oom(err);
    c->id = id;
    return 0;
}

/* ---- view_edit ---- */

static int str_cmp(const void *x, const void *y) {
    const cc_str *a = x, *b = y;
    size_t n = a->n < b->n ? a->n : b->n;
    int c = n ? memcmp(a->p, b->p, n) : 0;
    return c ? c : (a->n > b->n) - (a->n < b->n);
}

static int strs_sort(Arena *a, cc_strs *s) {
    if (!s->n) return 0;
    cc_str *v = arena_alloc(a, s->n * sizeof *v, _Alignof(cc_str));
    if (!v) return -1;
    memcpy(v, s->v, s->n * sizeof *v);
    qsort(v, s->n, sizeof *v, str_cmp);
    size_t k = 0;
    for (size_t i = 0; i < s->n; i++)
        if (!k || str_cmp(&v[k - 1], &v[i])) v[k++] = v[i];
    s->v = v, s->n = k;
    return 0;
}

int cc_visibility_set(Arena *a, cc_visibility *v) {
    cc_strs *l[7] = {&v->hidden_families,  &v->hidden_classes,  &v->hidden_storeys,    &v->hidden_links,
                     &v->hidden_materials, &v->hidden_elements, &v->isolated_elements};
    for (int i = 0; i < 7; i++)
        if (strs_sort(a, l[i])) return -1;
    return 0;
}

static int strs_eq(const cc_strs *a, const cc_strs *b) {
    if (a->n != b->n) return 0;
    for (size_t i = 0; i < a->n; i++)
        if (str_cmp(&a->v[i], &b->v[i])) return 0;
    return 1;
}

static const char *const VIS[] = {"hidden_families",  "hidden_classes",  "hidden_storeys",   "hidden_links",
                                  "hidden_materials", "hidden_elements", "isolated_elements"};

static cc_strs *vis_list(cc_visibility *v, int i) {
    cc_strs *l[7] = {&v->hidden_families,  &v->hidden_classes,  &v->hidden_storeys,    &v->hidden_links,
                     &v->hidden_materials, &v->hidden_elements, &v->isolated_elements};
    return l[i];
}

static int a_list(const JrVal *x, const char *field, int gids, Arena *a, cc_strs *out, Buf *err) {
    if (x->type != JR_ARR) return bad(err, "`", field, "`: expected a list of strings");
    if (x->n > MAX_LIST) return bad(err, "`", field, "`: at most 100000 entries");
    cc_str *v = arena_alloc(a, (x->n ? x->n : 1) * sizeof *v, _Alignof(cc_str));
    if (!v) return oom(err);
    for (size_t i = 0; i < x->n; i++) {
        const JrVal *e = &x->items[i];
        if (e->type != JR_STR || e->n > 800) return bad(err, "`", field, "`: expected a list of strings");
        if (gids && !cc_gid_valid(e->s, e->n)) return bad(err, "`", field, "`: expected GlobalIds");
        char *p = arena_alloc(a, e->n + 1, 1);
        if (!p) return oom(err);
        if (e->n) memcpy(p, e->s, e->n);
        p[e->n] = 0;
        v[i] = (cc_str){p, e->n};
    }
    out->v = v, out->n = x->n;
    return strs_sort(a, out) ? oom(err) : 0;
}

int cc_vh_view_edit(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const F[] = {"id", "name", "origin", "facing", "depth_mm", "levels", "visibility"};
    static const char *const PLANES[] = {"top", "cut", "bottom", "depth"}; /* CC_RANGE_* order */
    static const char *const REFS[] = {"associated", "above", "below", "unlimited"};
    const JrVal *f[7];
    cc_gid id;
    if (args_of(args, F, 7, f, err) || need(f[0], "id", err) || a_gid(f[0], "id", &id, err)) return -1;
    Arena *a = &l->arena;
    cc_view old, v;
    int r = effective(m, &id, a, &old, NULL);
    if (r < 0) return oom(err);
    if (!r) return no_view(err, &id);
    v = old;
    if (f[1] && a_text(f[1], "name", &v.name, err)) return -1;
    if ((f[2] || f[3] || f[4]) && v.kind != CC_VIEW_SECTION)
        return bad(err, "`origin`, `facing` and `depth_mm` belong to a section", 0, 0);
    if (f[2] && a_pt(f[2], "origin", v.origin, err)) return -1;
    if (f[3]) {
        int fc = a_enum(f[3], "facing", FACINGS, 4, err);
        if (fc < 0) return -1;
        v.facing = (cc_facing)fc;
    }
    if (f[4]) {
        if (a_mm(f[4], "depth_mm", &v.depth_mm, err)) return -1;
        if (v.depth_mm < 0) return bad(err, "`depth_mm`: 0 (all) or more", 0, 0);
    }
    if (f[5]) {
        if (v.kind != CC_VIEW_PLAN) return bad(err, "`levels`: only a plan has a view range", 0, 0);
        const JrVal *p[4];
        if (args_of(f[5], PLANES, 4, p, err)) return -1;
        for (int k = 0; k < 4; k++)
            if (p[k]) {
                int lr = a_enum(p[k], PLANES[k], REFS, 4, err);
                if (lr < 0) return -1;
                v.range[k].level = (cc_level_ref)lr;
            }
    }
    if (f[6]) {
        const JrVal *p[7];
        if (args_of(f[6], VIS, 7, p, err)) return -1;
        for (int k = 0; k < 7; k++)
            if (p[k] && a_list(p[k], VIS[k], k >= 5, a, vis_list(&v.visibility, k), err)) return -1;
    }
    int same = v.name.n == old.name.n && !memcmp(v.name.p, old.name.p, v.name.n) && v.origin[0] == old.origin[0] &&
               v.origin[1] == old.origin[1] && v.facing == old.facing && v.depth_mm == old.depth_mm;
    for (int k = 0; k < 4 && same; k++) same = v.range[k].level == old.range[k].level;
    for (int k = 0; k < 7 && same; k++) same = strs_eq(vis_list(&v.visibility, k), vis_list(&old.visibility, k));
    if (same) return 0; /* no command: no empty undo step */
    return push_view(m, &v, !cc_model_view(m, &id), l, err);
}
