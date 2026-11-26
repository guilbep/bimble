/* Suite measure (ticket T1): the measure tool (session/measure.h, op "tool" measure_*).
 * Golden: golden/measure.txt (ccore_golden, bimble_api::measure: the viewer's format_mm and
 * measurements_json): every format_mm line, the f32 distance of each pair, the measurements JSON.
 * Tool on golden/frame.json (columns 300 x 300, P1.00 at (0, 0), P1.10 at (6000, 0), level 0 to
 * 2780; orthographic rays, the ray_tol 24 mm beside: 2 mm per pixel): a ray along +x at y -148,
 * z 1000 meets the face x = -150 of P1.00 2 mm from its corner edge: an edge point; at z 1 near
 * its foot: the corner; at y 0: the face (its normal); Alt: the face. Two clicks make one
 * measurement (P1.00's edge to P1.10's: "6 000 mm"); no rev change, no history step; query
 * measurements; Shift (ortho), Ctrl (chain); Esc drops the first point, then the tool; clear; the
 * section box and hidden elements are not hit; a linked copy is; a read-only scene (frame.ifc)
 * measures too; the action's enabled and pressed states; errors. */
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/measure.h"
#include "../session/session.h"
#include "check.h"

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_init(out);
    return cc_call(s, op, name, body, strlen(body), out);
}

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

/* The text of `ask`, checked and freed. */
static void expect(const char *what, cc_session *s, const char *op, const char *name, const char *body, int want_st, const char *needle) {
    int st;
    char *t = ask(s, op, name, body, &st);
    int ok = st == want_st && (!needle || has(t, needle));
    if (!ok) printf("  %s: %d %s\n", what, st, t);
    check_true(what, ok);
    free(t);
}

static float parse_f32(const char *p, char **end) {
    if (!strncmp(p, "inf", 3)) return *end = (char *)p + 3, 1.0f / 0.0f;
    if (!strncmp(p, "-inf", 4)) return *end = (char *)p + 4, -1.0f / 0.0f;
    if (!strncmp(p, "NaN", 3)) return *end = (char *)p + 3, 0.0f / 0.0f;
    return (float)strtod(p, end);
}

static int same_bits(float a, float b) { return !memcmp(&a, &b, sizeof a); }

/* A ray in scene coordinates from world (x, y, z) along (dx, dy, dz), with ray_tol `gap` mm
 * beside it along (gx, gy, gz). */
static void ray_body(char *out, size_t cap, const double org[3], double x, double y, double z, const double d[3], const double g[3], const char *extra) {
    double ox = x - org[0], oy = y - org[1], oz = z - org[2];
    snprintf(out, cap, "{\"ray\":[%.17g,%.17g,%.17g,%g,%g,%g],\"ray_tol\":[%.17g,%.17g,%.17g,%g,%g,%g]%s}", ox, oy, oz, d[0], d[1], d[2],
             ox + g[0], oy + g[1], oz + g[2], d[0], d[1], d[2], extra);
}

static void golden(const char *dir) {
    size_t n;
    char *txt = slurp(dir, "../measure.txt", &n);
    cc_mdim dims[8];
    size_t nd = 0, nfmt = 0;
    for (char *line = txt; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *tab = strchr(line, '\t');
        if (!tab) {
            check_true("measure.txt: a tab in every line", 0);
            break;
        }
        *tab = 0;
        char *rest = tab + 1;
        if (!strcmp(line, "format_mm")) {
            char *e;
            float x = parse_f32(rest, &e);
            Buf b;
            buf_init(&b);
            cc_format_mm(&b, x);
            char what[96];
            snprintf(what, sizeof what, "format_mm(%.*s)", (int)(e - rest), rest);
            check_eq(what, b.p ? b.p : "", b.len, e + 1, strlen(e + 1));
            buf_free(&b);
            nfmt++;
        } else if (!strcmp(line, "pair") && nd < 8) {
            cc_mdim *d = &dims[nd++];
            memset(d, 0, sizeof *d);
            char *e = rest;
            for (int k = 0; k < 3; k++) d->a.p[k] = parse_f32(e, &e);
            for (int k = 0; k < 3; k++) d->b.p[k] = parse_f32(e, &e);
            d->a.kind = d->b.kind = CC_MSNAP_VERTEX;
            float want = parse_f32(e + 1, &e), got = cc_mdim_distance(d);
            if (!same_bits(got, want)) printf("  pair %zu: got %.9g want %.9g\n", nd, got, want);
            check_true("pair: the f32 distance, bit for bit", same_bits(got, want));
        } else if (!strcmp(line, "measurements")) {
            Buf b;
            buf_init(&b);
            JsonW w;
            jw_init(&w, &b, 0);
            cc_measurements_json(&w, dims, nd);
            jw_finish(&w);
            check_eq("measurements_json of the pairs", b.p ? b.p : "", b.len, rest, strlen(rest));
            buf_free(&b);
        } else check_true("measure.txt: a known line", 0);
        line = nl ? nl + 1 : NULL;
    }
    check_true("measure.txt: the format_mm lines and 3 pairs read", nfmt >= 30 && nd == 3);
    free(txt);
}

static const double PX[3] = {1, 0, 0}, PY[3] = {0, 1, 0}, NX[3] = {-1, 0, 0};
static const double GY[3] = {0, 24, 0}, GX[3] = {24, 0, 0}, GZ[3] = {0, 0, 24};

int main(int argc, char **argv) {
    const char *floats = argc > 1 ? argv[1] : "ccore/golden/floats";
    golden(floats);

    /* ---- pure helpers ---- */
    cc_mdim slab = {{{0, 0, 250}, {0}, CC_MSNAP_FACE, 1, {0, 0, 1}}, {{3000, 4000, 0}, {0}, CC_MSNAP_FACE, 1, {0, 0, -1}}};
    float perp = 0;
    check_true("perpendicular: top and bottom of a 250 mm slab", cc_mdim_perpendicular(&slab, &perp) && perp == 250.0f);
    slab.b.has_n = 0;
    check_true("perpendicular: none from a corner", !cc_mdim_perpendicular(&slab, &perp));
    cc_msnap from = {{100, 200, 300}, {0}, CC_MSNAP_VERTEX, 0, {0}}, to = {{1100, 250, 280}, {0}, CC_MSNAP_VERTEX, 1, {0, 0, 1}};
    cc_measure_orthogonal(&from, &to);
    check_true("orthogonal: along x, no normal", to.p[0] == 1100 && to.p[1] == 200 && to.p[2] == 300 && !to.has_n);
    to = (cc_msnap){{90, -800, 310}, {0}, CC_MSNAP_EDGE, 0, {0}};
    cc_measure_orthogonal(&from, &to);
    check_true("orthogonal: along y", to.p[0] == 100 && to.p[1] == -800 && to.p[2] == 300 && to.kind == CC_MSNAP_EDGE);
    to = (cc_msnap){{140, 230, 2300}, {0}, CC_MSNAP_EDGE, 0, {0}};
    cc_measure_orthogonal(&from, &to);
    check_true("orthogonal: along z", to.p[0] == 100 && to.p[1] == 200 && to.p[2] == 2300);

    /* ---- the tool on the frame ---- */
    char path[4096], body[512];
    snprintf(path, sizeof path, "%s/../frame.json", floats);
    const cc_platform *pf = cc_platform_native();
    cc_session *s = cc_session_new(pf);
    cc_error err;
    if (!s || cc_session_open(s, path, &err)) {
        printf("open: %s\n", s ? err.msg : "oom");
        return 1;
    }
    const cc_scene *sc = cc_session_scene(s);
    check_true("scene built", sc != NULL);
    double org[3];
    memcpy(org, sc->origin, sizeof org);
    uint64_t rev0 = s->rev;
    size_t undo0 = s->hist.nundo;

    expect("query measurements: [] at first", s, "query", "measurements", "{}", CC_OK, "[]");
    expect("actions: measure enabled, not pressed", s, "query", "actions", "{}", CC_OK,
           "{\"enabled\":true,\"label\":\"Measure\",\"name\":\"measure\",\"pressed\":false");
    ray_body(body, sizeof body, org, -10150, -148, 1000, PX, GY, "");
    expect("pointer while off: inactive, no hover", s, "tool", "measure_pointer", body, CC_OK, "{\"active\":false,\"first\":null,\"hover\":null,\"items\":[]");
    expect("click while off: 400", s, "tool", "measure_click", body, CC_ERR, "the measure tool is off");
    expect("action measure", s, "action", "measure", "", CC_OK, NULL);
    expect("actions: measure pressed", s, "query", "actions", "{}", CC_OK, "\"name\":\"measure\",\"pressed\":true");

    /* the edge of P1.00 (world (-150, -150, 1000)) */
    expect("pointer near P1.00's corner edge: an edge point", s, "tool", "measure_pointer", body, CC_OK,
           "\"hover\":{\"at\":[-150.0,-150.0,1000.0],\"snap\":\"edge\"}");
    ray_body(body, sizeof body, org, -10150, -149, 1, PX, GY, "");
    expect("pointer near P1.00's foot: the corner", s, "tool", "measure_pointer", body, CC_OK,
           "\"hover\":{\"at\":[-150.0,-150.0,0.0],\"snap\":\"corner\"}");
    ray_body(body, sizeof body, org, -10150, 0, 1000, PX, GY, "");
    expect("pointer on the middle of P1.00's face: the face", s, "tool", "measure_pointer", body, CC_OK,
           "\"hover\":{\"at\":[-150.0,0.0,1000.0],\"snap\":\"face\"}");
    ray_body(body, sizeof body, org, -10150, -148, 1000, PX, GY, ",\"free\":true");
    expect("Alt near the edge: the face point", s, "tool", "measure_pointer", body, CC_OK,
           "\"hover\":{\"at\":[-150.0,-148.0,1000.0],\"snap\":\"face\"}");
    ray_body(body, sizeof body, org, -10150, -148, 1000, PX, GY, "");
    expect("click 1: the first point", s, "tool", "measure_click", body, CC_OK, "\"first\":[-150.0,-150.0,1000.0]");
    /* P1.10's edge (world (5850, -150, 1000)), from -y */
    ray_body(body, sizeof body, org, 5852, -10150, 1000, PY, GX, "");
    expect("pointer at P1.10's edge: the preview 6 000 mm", s, "tool", "measure_pointer", body, CC_OK,
           "\"preview\":{\"text\":\"6\xe2\x80\x89" "000 mm\",\"value_mm\":6000.0}");
    expect("click 2: one measurement", s, "tool", "measure_click", body, CC_OK,
           "\"first\":null,\"hover\":{\"at\":[5850.0,-150.0,1000.0],\"snap\":\"edge\"},\"items\":[{\"a\":[-150.0,-150.0,1000.0],\"b\":[5850.0,-150.0,1000.0],"
           "\"id\":0,\"label\":\"1. 6\xe2\x80\x89" "000 mm\",\"text\":\"6\xe2\x80\x89" "000 mm\",\"value_mm\":6000.0}]");
    expect("query measurements: one row", s, "query", "measurements", "{}", CC_OK,
           "[{\"id\":0,\"label\":\"1. 6\xe2\x80\x89" "000 mm\",\"value_mm\":6000.0}]");
    check_true("no rev change, no history step", s->rev == rev0 && s->hist.nundo == undo0 && !cc_session_unsaved(s));

    /* Shift: the second point on an axis through the first; Ctrl: go on from there */
    ray_body(body, sizeof body, org, -10150, -148, 1000, PX, GY, "");
    expect("click 1 again", s, "tool", "measure_click", body, CC_OK, "\"first\":[-150.0,-150.0,1000.0]");
    ray_body(body, sizeof body, org, -10150, -148, 1, PX, GY, "");
    expect("pointer at the foot (an edge point)", s, "tool", "measure_pointer", body, CC_OK, "\"snap\":\"corner\"");
    ray_body(body, sizeof body, org, 6000, -10150, 700, PY, GZ, ",\"ortho\":true");
    expect("Shift on P1.10's -y face: kept on x through the first", s, "tool", "measure_pointer", body, CC_OK, "\"hover\":{\"at\":[6000.0,-150.0,1000.0],\"snap\":\"face\"}");
    ray_body(body, sizeof body, org, 5852, -10150, 1000, PY, GX, ",\"chain\":true");
    expect("Ctrl + click: measurement 2, the first point stays (chain)", s, "tool", "measure_click", body, CC_OK,
           "\"first\":[5850.0,-150.0,1000.0]");
    expect("query measurements: two rows", s, "query", "measurements", "{}", CC_OK, "\"id\":1,\"label\":\"2. 6\xe2\x80\x89" "000 mm\"");
    expect("Esc drops the first point, the tool stays", s, "tool", "measure_cancel", "", CC_OK, "{\"active\":true,\"first\":null");
    expect("a second Esc leaves the tool", s, "tool", "measure_cancel", "", CC_OK, "{\"active\":false,");
    expect("actions: measure not pressed", s, "query", "actions", "{}", CC_OK, "\"name\":\"measure\",\"pressed\":false");
    expect("the measurements stay", s, "query", "measurements", "{}", CC_OK, "\"id\":1");
    expect("clear", s, "tool", "measure_clear", "", CC_OK, "\"items\":[]");
    expect("query measurements: [] after clear", s, "query", "measurements", "{}", CC_OK, "[]");

    /* the section box and hidden elements are not hit */
    expect("action measure (on again)", s, "action", "measure", "", CC_OK, NULL);
    snprintf(body, sizeof body, "{\"min\":[%.17g,%.17g,%.17g],\"max\":[%.17g,%.17g,%.17g],\"on\":true}", 3000 - org[0], -1000 - org[1], -1000 - org[2],
             20000 - org[0], 11000 - org[1], 7000 - org[2]);
    expect("box_set: x from 3000", s, "tool", "box_set", body, CC_OK, "\"on\":true");
    ray_body(body, sizeof body, org, -10150, -148, 1000, PX, GY, "");
    expect("with the box: the ray goes on to P1.10", s, "tool", "measure_pointer", body, CC_OK, "\"hover\":{\"at\":[5850.0,-150.0,1000.0],\"snap\":\"edge\"}");
    expect("section box off", s, "action", "section_box", "", CC_OK, NULL);
    expect("without the box: P1.00 again", s, "tool", "measure_pointer", body, CC_OK, "\"hover\":{\"at\":[-150.0,-150.0,1000.0]");
    expect("select P1.00", s, "select", "-", "[\"2P97ZHH3v3XutfaphHA_Tu\"]", CC_OK, NULL);
    expect("hide it for now", s, "action", "hide_now", "", CC_OK, NULL);
    expect("hidden: the ray goes on to P1.10", s, "tool", "measure_pointer", body, CC_OK, "\"hover\":{\"at\":[5850.0,-150.0,1000.0]");

    /* a linked copy of the frame is hit where P1.00 is hidden */
    snprintf(body, sizeof body, "{\"path\":\"%s\"}", path);
    expect("link a copy of the frame", s, "action", "link_model", body, CC_OK, NULL);
    ray_body(body, sizeof body, org, -10150, -148, 1000, PX, GY, "");
    expect("the linked P1.00 is measured", s, "tool", "measure_pointer", body, CC_OK, "\"hover\":{\"at\":[-150.0,-150.0,1000.0],\"snap\":\"edge\"}");
    expect("show all hidden for now", s, "action", "clear_temporary", "", CC_OK, NULL);

    /* errors */
    expect("a bad body: 400", s, "tool", "measure_pointer", "{\"ray\":[1,2]}", CC_ERR, "ray: expected");
    expect("an unknown field: 400", s, "tool", "measure_pointer", "{\"at\":[1,2]}", CC_ERR, "unknown field: at");
    expect("tol_px out of range: 400", s, "tool", "measure_pointer", "{\"tol_px\":0}", CC_ERR, "tol_px");
    ray_body(body, sizeof body, org, -10150, -148, 1000, NX, GY, "");
    expect("a ray that meets nothing: no hover", s, "tool", "measure_pointer", body, CC_OK, "\"hover\":null");
    expect("a click without a hover adds nothing", s, "tool", "measure_click", "", CC_OK, "\"first\":null");
    check_true("still no rev change", s->rev == rev0 && s->hist.nundo == undo0);

    /* placing turns measuring off (ribbon.rs:687); another model drops the measurements */
    ray_body(body, sizeof body, org, -10150, 0, 1000, PX, GY, "");
    expect("click 1", s, "tool", "measure_click", body, CC_OK, NULL);
    expect("click 2 (the same face point: 0.0 mm)", s, "tool", "measure_click", body, CC_OK, "\"label\":\"1. 0.0 mm\"");
    expect("place a column", s, "action", "place_column", "", CC_OK, NULL);
    expect("measure is off", s, "query", "measure", "{}", CC_OK, "{\"active\":false");
    snprintf(path, sizeof path, "%s/../ifc/frame.ifc", floats);
    if (cc_session_open_file(s, path, &err)) printf("open %s: %s\n", path, err.msg);
    expect("a read-only scene: the measurements went", s, "query", "measurements", "{}", CC_OK, "[]");
    expect("read-only: action measure is enabled", s, "action", "measure", "", CC_OK, NULL);
    sc = cc_session_scene(s);
    memcpy(org, sc ? sc->origin : org, sizeof org);
    ray_body(body, sizeof body, org, -10150, -148, 1000, PX, GY, "");
    expect("read-only: click 1 on P1.00's edge", s, "tool", "measure_click", body, CC_OK, "\"first\":[-150.0,-150.0,1000.0]");
    ray_body(body, sizeof body, org, 5852, -10150, 1000, PY, GX, "");
    expect("read-only: click 2: 6 000 mm", s, "tool", "measure_click", body, CC_OK, "\"label\":\"1. 6\xe2\x80\x89" "000 mm\"");
    cc_session_close(s);
    expect("nothing open: no hover, no measurement", s, "tool", "measure_pointer", body, CC_OK, "\"hover\":null,\"items\":[]");
    cc_session_free(s);
    return report("measure");
}
