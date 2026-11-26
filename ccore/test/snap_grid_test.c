/* Suite snap_grid (ticket SLAB1): the place tool snaps to the model's grid axes (snap.rs
 * `targets`: `scene.grids` first, the axes through the columns only without a grid). snap.rs's
 * test `model_grids_are_used_and_cross` on an empty scene; then on golden/frame.json with a grid
 * added by `grid_add`: a column clicked 42 mm from a crossing lands exactly on it, the crossing
 * is the strongest target, the column axes are gone (a column centre is a "column centre" now);
 * removing the grid brings the column axes back (the targets follow the model's revision). */
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "../session/tool.h"
#include "check.h"

static char *ask(cc_session *s, const char *op, const char *name, const char *body, int *st) {
    Buf b;
    buf_init(&b);
    int r = cc_call(s, op, name, body, strlen(body), &b);
    if (st) *st = r;
    const char *t = buf_cstr(&b);
    char *z = malloc(strlen(t) + 1);
    strcpy(z, t);
    buf_free(&b);
    return z;
}
static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

int main(int argc, char **argv) {
    const char *floats = argc > 1 ? argv[1] : "ccore/golden/floats";
    char path[4096];
    snprintf(path, sizeof path, "%s/../frame.json", floats);

    /* ---- snap.rs model_grids_are_used_and_cross ---- */
    {
        cc_scene empty;
        memset(&empty, 0, sizeof empty);
        const double grid[2][4] = {{0, -1000, 0, 9000}, {-1000, 2500, 9000, 2500}};
        cc_snap_targets t;
        memset(&t, 0, sizeof t);
        check_true("empty scene + 2 axes: targets", !cc_snap_targets_build_grid(&t, &empty, 0.0, 3000.0, grid, 2));
        double q[2];
        int k = cc_snap(&t, (double[]){20, 2480}, 50, q);
        check_true("snap.rs: (20, 2480) -> the crossing (0, 2500)", k == CC_SNAP_AXIS_CROSSING && q[0] == 0.0 && q[1] == 2500.0);
        check_true("1 crossing, 2 axis lines", t.npoints == 1 && t.nlines == 2);
        k = cc_snap(&t, (double[]){4004, 2530}, 50, q);
        check_true("near one axis: along it, rounded", k == CC_SNAP_AXIS && q[0] == 4000.0 && q[1] == 2500.0);
        const double tiny[1][4] = {{0, 0, 0.5, 0}};
        check_true("an axis of 1 mm or less is no axis", !cc_snap_targets_build_grid(&t, &empty, 0.0, 3000.0, tiny, 1) && t.nlines == 0);
        cc_snap_targets_free(&t);
    }

    /* ---- the session: frame.json + a grid ---- */
    cc_session *s = cc_session_new(cc_platform_native());
    cc_error err;
    if (!s || cc_session_open(s, path, &err)) {
        printf("open: %s\n", s ? err.msg : "oom");
        return 1;
    }
    int st;
    char *a = ask(s, "set_placing", "-", "{\"kind\":\"column\"}", &st);
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[1542,2470],\"tol_mm\":100}", &st);
    check_true("no grid: (1542, 2470) is near nothing, rounded", has(a, "\"at\":[1540.0,2470.0],\"snap\":null"));
    free(a);
    a = ask(s, "verb", "grid_add", "{\"name\":\"G\",\"x\":[1500,7500],\"y\":[2500,7500]}", &st);
    check_true("grid_add", st == CC_OK);
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[1542,2470],\"tol_mm\":100}", &st);
    check_true("the grid: the pointer 52 mm off snaps to the crossing (1500, 2500)", has(a, "\"at\":[1500.0,2500.0],\"snap\":\"axis crossing\""));
    if (!has(a, "\"at\":[1500.0,2500.0]")) printf("  %s\n", a);
    free(a);
    size_t n0 = cc_session_model(s)->nelements;
    a = ask(s, "tool", "click", "{\"at\":[1530,2458],\"tol_mm\":100}", &st);
    check_true("a column clicked near the crossing: placed", st == CC_OK && cc_session_model(s)->nelements == n0 + 1);
    {
        const char *pl = strstr(a, "\"placed\":\"");
        cc_gid id;
        const cc_element *e = NULL;
        if (pl) memcpy(id.c, pl + 10, 22), e = cc_model_element(cc_session_model(s), &id);
        check_true("its origin is the crossing, exactly", e && e->placement.origin[0] == 1500.0 && e->placement.origin[1] == 2500.0);
    }
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[6037,4958],\"tol_mm\":100}", &st);
    check_true("with a grid the column axes are gone: (6037, 4958) is a column centre", has(a, "\"at\":[6000.0,5000.0],\"snap\":\"column centre\""));
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[4000,7530],\"tol_mm\":50}", &st);
    check_true("near the grid axis y = 7500: on it", has(a, "\"at\":[4000.0,7500.0],\"snap\":\"grid axis\""));
    free(a);
    free(ask(s, "undo", "-", "", &st)); /* the column */
    free(ask(s, "undo", "-", "", &st)); /* the grid */
    a = ask(s, "tool", "pointer", "{\"at\":[6037,4958],\"tol_mm\":100}", &st);
    check_true("grid undone: the column axes again (an axis crossing)", has(a, "\"at\":[6000.0,5000.0],\"snap\":\"axis crossing\""));
    free(a);
    cc_session_free(s);
    return report("snap_grid");
}
