/* Suite tool (ticket B4): the place tool (session/tool.h, op "tool" in calls.h).
 * Snap: the cases of crates/bimble-viewer/src/snap.rs tests on golden/frame.json (the demo
 * concrete frame: columns 6 x 5 m apart, no grid), level 0 up to 3000; crossing, rounding, ortho,
 * the ray on the storey plane. State machine through cc_call on a session: a column is one click
 * (one history step, at the snapped point, history label as the verb's); a beam is two clicks
 * (the first adds nothing, the ghost follows the pointer, the second adds one beam: one step);
 * Esc (tool cancel) stops placing and drops the points; free (no snap); ortho; errors. */
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "../session/tool.h"
#include "check.h"

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_init(out);
    return cc_call(s, op, name, body, strlen(body), out);
}

/* The answer as a heap string, the status in *st. */
static char *ask(cc_session *s, const char *op, const char *name, const char *body, int *st) {
    Buf b;
    int r = call(s, op, name, body, &b);
    if (st) *st = r;
    const char *t = buf_cstr(&b);
    char *z = malloc(strlen(t) + 1);
    strcpy(z, t);
    buf_free(&b);
    return z;
}

static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

static size_t count(const char *hay, const char *needle) {
    size_t n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

static void snap_is(const char *what, const cc_snap_targets *t, double x, double y, double tol, double wx, double wy, int wk) {
    double p[2] = {x, y}, q[2];
    int k = cc_snap(t, p, tol, q);
    int ok = q[0] == wx && q[1] == wy && k == wk;
    if (!ok) printf("  %s: got (%.17g, %.17g) kind %d, want (%g, %g) kind %d\n", what, q[0], q[1], k, wx, wy, wk);
    check_true(what, ok);
}

int main(int argc, char **argv) {
    char path[4096];
    const char *floats = argc > 1 ? argv[1] : "ccore/golden/floats";
    snprintf(path, sizeof path, "%s/../frame.json", floats);

    /* ---- pure helpers ---- */
    check_true("round10 half away from zero", cc_round10(2535.0) == 2540.0 && cc_round10(-2535.0) == -2540.0 &&
                                                  cc_round10(2534.9) == 2530.0 && cc_round10(4.9) == 0.0);
    double z = cc_round10(-4.0);
    check_true("round10 gives no -0.0", z == 0.0 && 1.0 / z > 0.0);
    double o[2];
    cc_ortho((double[]){0, 0}, (double[]){5000, 300}, o);
    check_true("ortho along x", o[0] == 5000 && o[1] == 0);
    cc_ortho((double[]){100, 100}, (double[]){-200, 4000}, o);
    check_true("ortho along y", o[0] == 100 && o[1] == 4000);
    double h[2];
    check_true("ray down meets the plane", !cc_ray_plane((double[]){10, 20, 1000, 0.5, 0, -1}, 0, h) && h[0] == 510 && h[1] == 20);
    check_true("ray up misses it", cc_ray_plane((double[]){10, 20, 1000, 0, 0, 1}, 0, h) == -1);
    check_true("ray along it misses it", cc_ray_plane((double[]){10, 20, 1000, 1, 0, 0}, 0, h) == -1);
    check_true("labels", !strcmp(cc_snap_label(CC_SNAP_AXIS), "grid axis") && cc_snap_label(CC_SNAP_NONE) == NULL);

    /* ---- snap.rs tests on the frame ---- */
    const cc_platform *pf = cc_platform_native();
    cc_session *s = cc_session_new(pf);
    cc_error err;
    if (!s || cc_session_open(s, path, &err)) {
        printf("open: %s\n", s ? err.msg : "oom");
        return 1;
    }
    const cc_scene *sc = cc_session_scene(s);
    check_true("scene built", sc != NULL);
    cc_snap_targets t;
    memset(&t, 0, sizeof t);
    check_true("targets built", sc && !cc_snap_targets_build(&t, sc, 0.0, 3000.0));
    printf("  targets: %zu points, %zu lines\n", t.npoints, t.nlines);
    size_t crossings = 0, centres = 0;
    for (size_t i = 0; i < t.npoints; i++) crossings += t.points[i].kind == CC_SNAP_AXIS_CROSSING, centres += t.points[i].kind == CC_SNAP_COLUMN_CENTRE;
    /* The band reaches the next level: the columns standing on it count too (snap.rs). */
    check_true("4 x 3 axis crossings, the 24 column centres of the band", crossings == 12 && centres == 24);
    snap_is("40 mm off a column centre: the axis crossing", &t, 6037, 4958, 100, 6000, 5000, CC_SNAP_AXIS_CROSSING);
    snap_is("near a corner column: its crossing", &t, 12031, -22, 100, 12000, 0, CC_SNAP_AXIS_CROSSING);
    snap_is("near an axis only: slides along it, rounded", &t, 3004, 5030, 50, 3000, 5000, CC_SNAP_AXIS);
    snap_is("far from all: rounded to 10 mm", &t, 3004, 2533, 50, 3000, 2530, CC_SNAP_NONE);
    snap_is("tolerance 0: rounded", &t, 6037, 4958, 0, 6040, 4960, CC_SNAP_NONE);
    /* a column corner (150 mm from the centre) within 40 mm, the crossing 200+ away: a corner */
    snap_is("near a column corner: the corner", &t, 6160, 5170, 40, 6150, 5150, CC_SNAP_CORNER);
    cc_snap_targets_free(&t);
    cc_snap_targets t2;
    memset(&t2, 0, sizeof t2);
    check_true("a level with nothing on it: no targets", !cc_snap_targets_build(&t2, sc, 50000.0, 54000.0) && t2.npoints == 0 && t2.nlines == 0);
    cc_snap_targets_free(&t2);

    /* ---- the state machine ---- */
    int st;
    char *a = ask(s, "tool", "click", "{\"at\":[1,2]}", &st);
    check_true("click while not placing: 400", st == CC_ERR && has(a, "nothing is being placed"));
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[1,2]}", &st);
    check_true("pointer while not placing: 200, inactive, no hover, no ghost",
               st == CC_OK && has(a, "\"active\":false") && has(a, "\"hover\":null") && has(a, "\"ghost\":null"));
    free(a);
    a = ask(s, "tool", "nope", "", &st);
    check_true("unknown tool call: 400", st == CC_ERR && has(a, "unknown tool call \"nope\""));
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[1]}", &st);
    check_true("bad at: 400", st == CC_ERR && has(a, "at: expected"));
    free(a);
    a = ask(s, "tool", "pointer", "{\"where\":1}", &st);
    check_true("unknown field: 400", st == CC_ERR && has(a, "unknown field: where"));
    free(a);

    size_t n0 = cc_session_model(s)->nelements;
    a = ask(s, "set_placing", "-", "{\"kind\":\"column\"}", &st);
    check_true("set_placing column", st == CC_OK);
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[3004,2533],\"tol_mm\":50}", &st);
    check_true("column: pointer -> hover rounded, a ghost (8 vertices of a box)",
               st == CC_OK && has(a, "\"hover\":{\"at\":[3000.0,2530.0],\"snap\":null,\"z\":0.0}") && has(a, "\"ghost\":{\"edges\":[") &&
                   has(a, "\"ghost_error\":null"));
    if (st != CC_OK) printf("  %s\n", a);
    free(a);
    a = ask(s, "query", "ghost", "", &st);
    check_true("query ghost = the pointer's answer", st == CC_OK && has(a, "\"ghost\":{") && has(a, "\"placed\":null"));
    free(a);
    check_true("the pointer changes nothing", cc_session_model(s)->nelements == n0 && cc_session_rev(s) == 1);
    a = ask(s, "tool", "click", "{\"at\":[6037,4958],\"tol_mm\":100}", &st);
    check_true("column: one click adds one, at the snapped point", st == CC_OK && cc_session_model(s)->nelements == n0 + 1 && has(a, "\"placed\":\""));
    cc_gid placed;
    memset(&placed, 0, sizeof placed);
    if (has(a, "\"placed\":\"")) memcpy(placed.c, strstr(a, "\"placed\":\"") + 10, 22);
    free(a);
    a = ask(s, "query", "history", "", &st);
    check_true("one history step", count(a, "\"") > 0 && has(a, "\"undo\":[\"") && !has(a, "\",\""));
    printf("  history after the column: %s\n", a);
    free(a);
    {
        const cc_element *e = cc_model_element(cc_session_model(s), &placed);
        check_true("the new column is on the crossing (6000, 5000) of the placing storey",
                   e && e->cls == CC_IFC_COLUMN && e->placement.origin[0] == 6000.0 && e->placement.origin[1] == 5000.0 &&
                       !memcmp(e->storey.c, s->storey.c, 22));
    }
    a = ask(s, "query", "placing", "", &st);
    check_true("still placing a column after the click (Rust keeps placing)", has(a, "\"active\":true") && has(a, "\"kind\":\"column\""));
    free(a);

    /* beam: two clicks, one step */
    size_t n1 = cc_session_model(s)->nelements;
    a = ask(s, "set_placing", "-", "{\"kind\":\"beam\"}", &st);
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[3000,7000],\"tol_mm\":10}", &st);
    check_true("beam, no point yet: a hover, no ghost", st == CC_OK && has(a, "\"ghost\":null") && has(a, "\"hover\":{"));
    free(a);
    a = ask(s, "tool", "click", "", &st);
    check_true("beam: a click without a position uses the pointer", st == CC_OK && has(a, "\"points\":[[3000.0,7000.0]]") &&
                                                                         cc_session_model(s)->nelements == n1);
    free(a);
    a = ask(s, "query", "placing", "", &st);
    check_true("placing shows the point and the next hint", has(a, "\"points\":[[3000.0,7000.0]]") && has(a, "Click the end point."));
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[9000,7300],\"tol_mm\":10,\"ortho\":true}", &st);
    check_true("ortho: the end along x", has(a, "\"at\":[9000.0,7000.0]") && has(a, "\"ghost\":{"));
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[3003,7004],\"tol_mm\":0}", &st);
    check_true("too close: no ghost, the verb's error", has(a, "\"ghost\":null") && has(a, "the two points are too close"));
    free(a);
    a = ask(s, "tool", "click", "{\"at\":[9000,7300],\"tol_mm\":10,\"ortho\":true}", &st);
    check_true("beam: the second click adds one beam", st == CC_OK && cc_session_model(s)->nelements == n1 + 1 && has(a, "\"points\":[]"));
    free(a);
    a = ask(s, "query", "history", "", &st);
    check_true("two history steps in all (one per gesture)", count(a, "\",\"") == 1);
    printf("  history after the beam: %s\n", a);
    free(a);
    {
        const cc_model *m = cc_session_model(s);
        int ok = 0;
        for (size_t i = 0; i < m->nelements; i++) {
            const cc_element *e = &m->elements[i];
            if (e->cls == CC_IFC_BEAM && e->placement.origin[0] == 3000.0 && e->placement.origin[1] == 7000.0 &&
                e->generator.kind == CC_GEN_RECT_BEAM && e->generator.u.beam.length == 6000.0)
                ok++;
        }
        check_true("the beam runs from (3000, 7000), 6000 long", ok == 1);
    }
    a = ask(s, "undo", "-", "", &st);
    free(a);
    check_true("one undo takes the beam back", cc_session_model(s)->nelements == n1);

    /* Esc */
    a = ask(s, "set_placing", "-", "{\"kind\":\"wall\"}", &st);
    free(a);
    a = ask(s, "tool", "click", "{\"at\":[0,8000],\"free\":true}", &st);
    check_true("wall: first point", has(a, "\"points\":[[0.0,8000.0]]"));
    free(a);
    a = ask(s, "tool", "cancel", "", &st);
    check_true("cancel: inactive, no points, nothing added",
               st == CC_OK && has(a, "\"active\":false") && has(a, "\"points\":[]") && cc_session_model(s)->nelements == n1);
    free(a);
    a = ask(s, "query", "placing", "", &st);
    check_true("placing off after cancel", has(a, "\"active\":false"));
    free(a);

    /* free: no snap near a column */
    a = ask(s, "set_placing", "-", "{\"kind\":\"column\"}", &st);
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[6037,4958],\"tol_mm\":100,\"free\":true}", &st);
    check_true("free: rounded, no snap", has(a, "\"at\":[6040.0,4960.0],\"snap\":null"));
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[6037,4958],\"tol_mm\":100}", &st);
    check_true("snap label", has(a, "\"snap\":\"axis crossing\""));
    free(a);
    /* a ray from above, scene coordinates */
    {
        const cc_scene *sc2 = cc_session_scene(s);
        char body[256];
        snprintf(body, sizeof body, "{\"ray\":[%.17g,%.17g,%.17g,0,0,-1],\"tol_mm\":100}", 6030.0 - sc2->origin[0], 5020.0 - sc2->origin[1],
                 10000.0 - sc2->origin[2]);
        a = ask(s, "tool", "pointer", body, &st);
        check_true("a ray straight down snaps like the point", has(a, "\"at\":[6000.0,5000.0],\"snap\":\"axis crossing\""));
        if (!has(a, "\"at\":[6000.0,5000.0]")) printf("  %s\n", a);
        free(a);
    }
    /* slab: rectangle; polygon refused */
    a = ask(s, "set_placing", "-", "{\"kind\":\"slab\",\"top_offset_mm\":-20}", &st);
    free(a);
    size_t n2 = cc_session_model(s)->nelements;
    a = ask(s, "tool", "click", "{\"at\":[20000,0],\"free\":true}", &st);
    free(a);
    a = ask(s, "tool", "click", "{\"at\":[23000,2000],\"free\":true}", &st);
    check_true("slab rectangle: two clicks, one slab", st == CC_OK && cc_session_model(s)->nelements == n2 + 1);
    {
        const cc_model *m = cc_session_model(s);
        int ok = 0;
        for (size_t i = 0; i < m->nelements; i++) {
            const cc_element *e = &m->elements[i];
            if (e->cls == CC_IFC_SLAB && e->placement.origin[0] == 20000.0 && e->placement.origin[2] == 3000.0 - 250.0 - 20.0) ok++;
        }
        check_true("the slab gets top_offset_mm (edit.rs click)", ok == 1);
    }
    free(a);
    a = ask(s, "set_placing", "-", "{\"shape\":\"polygon\"}", &st);
    free(a);
    a = ask(s, "tool", "click", "{\"at\":[0,0]}", &st);
    check_true("polygon slab: refused (not ported)", st == CC_ERR && has(a, "polygon"));
    free(a);
    cc_session_free(s);
    return report("tool");
}
