/* Suite grid_axis (ticket SLAB1): single grid axes, the verbs grid_axis_add {grid, tag, start,
 * end} and grid_axis_remove {grid, axis} (verbs_levels.c; C additions, no Rust oracle:
 * DEVIATIONS 30). On a copy of golden/frame.json with a rectangular grid: an axis added gets a new
 * GlobalId, the others keep theirs; one history step each, undone and redone; the IFC4 and IFC2x3
 * exports carry the new axis and read back with it; removing the last axis of a direction (IfcGrid
 * needs UAxes and VAxes, LIST [1:?] each) is refused; the schemas are JSON; errors are refused. */
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/command.h"
#include "../src/file.h"
#include "../src/grid.h"
#include "../src/guid.h"
#include "../src/history.h"
#include "../src/ifc_out.h"
#include "../src/json_r.h"
#include "../src/model.h"
#include "../src/verb_table.h"
#include "../src/verbs.h"
#include "check.h"

#define IFC_TIME "2026-01-01T00:00:00"

static void buf_printf(Buf *b, const char *fmt, ...) {
    char t[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    buf_puts(b, t);
}

/* Runs a verb and applies its commands as one step; 0 ok with the commands' count in *n. */
static int run(cc_history *h, cc_model *m, const char *verb, const char *args, Buf *err, size_t *n) {
    cc_cmds l;
    cc_cmds_init(&l);
    buf_clear(err);
    int rc = cc_verb_run(m, verb, args, strlen(args), &l, err);
    if (n) *n = l.n;
    if (!rc && l.n) rc = cc_history_apply(h, m, l.v, l.n, err);
    cc_cmds_free(&l);
    return rc;
}

static const cc_grid_axis *axis_tagged(const cc_grid *g, const char *tag) {
    for (size_t i = 0; g && i < g->naxes; i++)
        if (g->axes[i].tag.n == strlen(tag) && !memcmp(g->axes[i].tag.p, tag, g->axes[i].tag.n)) return &g->axes[i];
    return NULL;
}

int main(int argc, char **argv) {
    const char *floats = argc > 1 ? argv[1] : "ccore/golden/floats";
    char gdir[4096];
    snprintf(gdir, sizeof gdir, "%s/..", floats);
    size_t fn;
    char *base = slurp(gdir, "frame.json", &fn);
    cc_model m;
    cc_error e;
    cc_history h;
    Buf err, args, ifc;
    buf_init(&err), buf_init(&args), buf_init(&ifc);
    cc_history_init(&h);
    check_true("frame loads", cc_model_load(&m, base, fn, &e) == 0);
    cc_guid_seed(1);
    size_t n;
    check_true("grid_add 3 x 2", !run(&h, &m, "grid_add", "{\"name\":\"Main\",\"x\":[0,6000,12000],\"y\":[0,5000]}", &err, &n) && m.ngrids == 1);
    cc_grid g0 = m.grids[0];
    char gid[23], ids[5][23];
    memcpy(gid, g0.id.c, 22), gid[22] = 0;
    for (size_t i = 0; i < 5 && i < g0.naxes; i++) memcpy(ids[i], g0.axes[i].id.c, 22), ids[i][22] = 0;
    size_t steps0 = h.nundo;

    /* ---- grid_axis_add ---- */
    buf_clear(&args);
    buf_printf(&args, "{\"grid\":\"%s\",\"tag\":\"4\",\"start\":[18000,-1000],\"end\":[18000,6000]}", gid);
    int rc = run(&h, &m, "grid_axis_add", buf_cstr(&args), &err, &n);
    if (rc) printf("  %s\n", buf_cstr(&err));
    check_true("grid_axis_add: one set_grid, one history step", !rc && n == 1 && h.nundo == steps0 + 1 && m.grids[0].naxes == 6);
    const cc_grid_axis *a4 = axis_tagged(&m.grids[0], "4");
    check_true("the new axis: its tag, ends, a new GlobalId", a4 && a4->start[0] == 18000.0 && a4->start[1] == -1000.0 && a4->end[1] == 6000.0 &&
                                                                  cc_grid_uses_id(&m, &a4->id));
    int stable = 1;
    for (size_t i = 0; i < 5; i++) stable &= !memcmp(m.grids[0].axes[i].id.c, ids[i], 22);
    for (size_t i = 0; a4 && i < 5; i++) stable &= memcmp(a4->id.c, ids[i], 22) != 0;
    check_true("the other axes keep their GlobalIds; the new one is new", stable && memcmp(m.grids[0].id.c, gid, 22) == 0);
    check_true("export IFC4 with the new axis", cc_ifc_export(&m, "g.ifc", IFC_TIME, CC_IFC4, &ifc) == 0 && strstr(buf_cstr(&ifc), "IFCGRIDAXIS('4'"));
    {
        const char *t = buf_cstr(&ifc);
        size_t axes = 0;
        for (const char *p = strstr(t, "IFCGRIDAXIS("); p; p = strstr(p + 1, "IFCGRIDAXIS(")) axes++;
        check_true("IFC4: 6 IfcGridAxis", axes == 6);
        if (axes != 6) printf("  axes %zu\n", axes);
    }
    buf_clear(&ifc);
    check_true("export IFC2x3 with the new axis", cc_ifc_export(&m, "g.ifc", IFC_TIME, CC_IFC2X3, &ifc) == 0 && strstr(buf_cstr(&ifc), "IFCGRIDAXIS('4'"));
    check_true("undo: 5 axes", cc_history_undo(&h, &m) && m.grids[0].naxes == 5 && !axis_tagged(&m.grids[0], "4"));
    check_true("redo: 6 axes, the same new id", cc_history_redo(&h, &m) && m.grids[0].naxes == 6 && axis_tagged(&m.grids[0], "4") &&
                                                    !memcmp(axis_tagged(&m.grids[0], "4")->id.c, a4 ? a4->id.c : "", 22));
    buf_clear(&args);
    buf_printf(&args, "{\"grid\":\"%s\",\"tag\":\"4\",\"start\":[24000,-1000],\"end\":[24000,6000]}", gid);
    check_true("a tag twice: refused", run(&h, &m, "grid_axis_add", buf_cstr(&args), &err, &n) && strstr(buf_cstr(&err), "grid_axis_add: "));
    printf("  %s\n", buf_cstr(&err));
    check_true("no grid: refused", run(&h, &m, "grid_axis_add", "{\"grid\":\"00000000000000000000ZZ\",\"tag\":\"9\",\"start\":[0,0],\"end\":[1,0]}", &err, &n) &&
                                       !strcmp(buf_cstr(&err), "grid_axis_add: no grid 00000000000000000000ZZ"));
    buf_clear(&args);
    buf_printf(&args, "{\"grid\":\"%s\",\"tag\":\"Z\",\"start\":[0,0]}", gid);
    check_true("no end: refused", run(&h, &m, "grid_axis_add", buf_cstr(&args), &err, &n));
    buf_clear(&args);
    buf_printf(&args, "{\"grid\":\"%s\",\"tag\":\"Z\",\"start\":[0,0],\"end\":[0.5,0]}", gid);
    check_true("shorter than 1 mm: refused", run(&h, &m, "grid_axis_add", buf_cstr(&args), &err, &n));
    buf_clear(&args);
    buf_printf(&args, "{\"grid\":\"%s\",\"tag\":\"Z\",\"start\":[0,0],\"end\":[1,0],\"z\":1}", gid);
    check_true("unknown field: refused", run(&h, &m, "grid_axis_add", buf_cstr(&args), &err, &n) && strstr(buf_cstr(&err), "unknown field `z`"));

    /* ---- grid_axis_remove ---- */
    const cc_grid_axis *ax = axis_tagged(&m.grids[0], "A");
    char aid[23] = {0};
    if (ax) memcpy(aid, ax->id.c, 22);
    size_t s1 = h.nundo;
    buf_clear(&args);
    buf_printf(&args, "{\"grid\":\"%s\",\"axis\":\"%s\"}", gid, aid);
    rc = run(&h, &m, "grid_axis_remove", buf_cstr(&args), &err, &n);
    if (rc) printf("  %s\n", buf_cstr(&err));
    check_true("grid_axis_remove A: one step, 5 axes, the rest keep their ids", !rc && h.nundo == s1 + 1 && m.grids[0].naxes == 5 &&
                                                                                    !axis_tagged(&m.grids[0], "A") && axis_tagged(&m.grids[0], "B") &&
                                                                                    !memcmp(axis_tagged(&m.grids[0], "1")->id.c, ids[0], 22));
    const cc_grid_axis *bx = axis_tagged(&m.grids[0], "B");
    buf_clear(&args);
    buf_printf(&args, "{\"grid\":\"%s\",\"axis\":\"%.22s\"}", gid, bx ? bx->id.c : "");
    check_true("removing B, the last axis of its direction: refused (IfcGrid UAxes and VAxes)",
               run(&h, &m, "grid_axis_remove", buf_cstr(&args), &err, &n) &&
                   !strcmp(buf_cstr(&err), "grid_axis_remove: B is the last axis of its direction; an IfcGrid needs axes in two (UAxes and VAxes)"));
    printf("  %s\n", buf_cstr(&err));
    /* axis 0 ("1") goes: the U axes are now those parallel to "2"; still two directions */
    buf_clear(&args);
    buf_printf(&args, "{\"grid\":\"%s\",\"axis\":\"%s\"}", gid, ids[0]);
    check_true("removing axis 0 (\"1\"): allowed, 4 axes", !run(&h, &m, "grid_axis_remove", buf_cstr(&args), &err, &n) && m.grids[0].naxes == 4);
    buf_clear(&ifc);
    check_true("export IFC4 after the removals", cc_ifc_export(&m, "g.ifc", IFC_TIME, CC_IFC4, &ifc) == 0 && !strstr(buf_cstr(&ifc), "IFCGRIDAXIS('1'") &&
                                                     !strstr(buf_cstr(&ifc), "IFCGRIDAXIS('A'") && strstr(buf_cstr(&ifc), "IFCGRIDAXIS('B'"));
    buf_clear(&args);
    buf_printf(&args, "{\"grid\":\"%s\",\"axis\":\"00000000000000000000ZZ\"}", gid);
    check_true("no axis: refused", run(&h, &m, "grid_axis_remove", buf_cstr(&args), &err, &n) && strstr(buf_cstr(&err), "grid_axis_remove: no axis 00000000000000000000ZZ in the grid Main"));
    check_true("undo twice: A and 1 back, with their ids", cc_history_undo(&h, &m) && cc_history_undo(&h, &m) && m.grids[0].naxes == 6 &&
                                                               axis_tagged(&m.grids[0], "A") && !memcmp(axis_tagged(&m.grids[0], "A")->id.c, aid, 22) &&
                                                               !memcmp(axis_tagged(&m.grids[0], "1")->id.c, ids[0], 22));

    /* ---- the schemas ---- */
    for (int i = 0; i < 2; i++) {
        const cc_verb_def *d = cc_verb_find(i ? "grid_axis_remove" : "grid_axis_add");
        Arena a;
        arena_init(&a, 0);
        JrError je;
        const JrVal *v = d ? jr_parse(&a, d->schema, strlen(d->schema), &je) : NULL;
        check_true(i ? "grid_axis_remove: a verb, its schema JSON" : "grid_axis_add: a verb, its schema JSON", v && v->type == JR_OBJ);
        arena_free(&a);
    }
    buf_free(&err), buf_free(&args), buf_free(&ifc);
    cc_history_free(&h);
    cc_model_free(&m);
    free(base);
    return report("grid_axis");
}
