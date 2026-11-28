/* Suite measure_geo (ticket MEAS2): the core's tools across links at national-grid coordinates.
 * A synthetic building (a slab 18,300 x 2,000 x 200 mm, 14 columns, two beams, a 12 m beam turned
 * 30 degrees; storeys at 0 and 3,200) is linked into an EMPTY open model (scene origin 0) twice, by
 * the "file" rule (both georeferenced, IfcMapConversion 30 degrees): once at 0 and once at
 * (2.6e9, 1.2e9, 5e5) mm. With the placement removed (in f64), each answer must equal the one at 0
 * within 0.001 mm:
 *   - measure: a corner, an edge point, a face point (the pick under one click), a distance of
 *     18,300 mm between two slab corners (the chained click);
 *   - the levels read from the links (a plan's cut height is level + its range: f64);
 *   - the section box fitted to a selected linked element (Box to selection) and to the links (the
 *     first section box), within one f32 step (MEAS3: the box in its own frame, cc_vis.base; 150.6 mm
 *     off before), and one face dragged 1,000 mm moves 1,000.000 mm.
 * Then the same building with its own coordinates at 2,600 km (the IFC itself, no placement: the
 * native test's case), the measure only. Hostile: a layer offset of 1e15 mm (measured in its own
 * frame: the corner to f64's 0.125 mm), NaN and infinite offsets (not hit, no crash), a map
 * conversion at 1e15 mm (refused: the link stays at its own coordinates).
 * Fails on the code before MEAS2: the link offset and the snap were f32 in the shown scene's frame
 * (at 2,600 km: no corner nor edge found, the face point 37.9 mm off, no distance; the file's own
 * coordinates: corner and edge missed, the face 120 mm off; a 1e15 mm layer: 1e15 mm off; a NaN
 * offset hit). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/links.h"
#include "../session/m1links.h"
#include "../session/measure.h"
#include "../session/session.h"
#include "dxf_in.h"
#include "ifc_math.h"
#include "check.h"

#define DIR "out/test-measure_geo"
#define TOL 0.001 /* mm */

static double dabs(double x) { return x < 0 ? -x : x; }

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_clear(out);
    out->oom = 0;
    return cc_call(s, op, name, body, strlen(body), out);
}

/* The building, its points offset by (ox, oy, oz) mm in the file; a map conversion (E, N, H in
 * metres, turned `deg`) when `map`. */
static void write_building(const char *path, double ox, double oy, double oz, int map, double e, double n, double h, double deg) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    double c = cc_cos(deg * 3.14159265358979323846 / 180.0), s = cc_sin(deg * 3.14159265358979323846 / 180.0);
    fprintf(f, "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('x'),'2;1');\nFILE_NAME('x','t',(''),(''),'','','');\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n"
               "#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);\n#2=IFCUNITASSIGNMENT((#1));\n#3=IFCCARTESIANPOINT((0.,0.,0.));\n"
               "#4=IFCAXIS2PLACEMENT3D(#3,$,$);\n#5=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-05,#4,$);\n"
               "#6=IFCPROJECT('0YvctVUKr0kugbFTf53O9L',$,'p',$,$,$,$,(#5),#2);\n#10=IFCCARTESIANPOINT((0.,0.));\n#11=IFCAXIS2PLACEMENT2D(#10,$);\n"
               "#12=IFCDIRECTION((0.,0.,1.));\n"
               "#20=IFCLOCALPLACEMENT($,#4);\n#21=IFCSITE('1Geo0000VUKr0kugbFTf5S',$,'site',$,$,#20,$,$,.ELEMENT.,$,$,$,$,$);\n"
               "#22=IFCLOCALPLACEMENT(#20,#4);\n#23=IFCBUILDING('1Geo0001VUKr0kugbFTf5B',$,'bldg',$,$,#22,$,$,.ELEMENT.,$,$,$);\n"
               "#24=IFCLOCALPLACEMENT(#22,#4);\n#25=IFCBUILDINGSTOREY('1Geo0002VUKr0kugbFTf5L',$,'L0',$,$,#24,$,$,.ELEMENT.,0.);\n"
               "#26=IFCCARTESIANPOINT((0.,0.,3200.));\n#27=IFCAXIS2PLACEMENT3D(#26,$,$);\n#28=IFCLOCALPLACEMENT(#22,#27);\n"
               "#29=IFCBUILDINGSTOREY('1Geo0003VUKr0kugbFTf5M',$,'L1',$,$,#28,$,$,.ELEMENT.,3200.);\n"
               "#30=IFCRELAGGREGATES('1Geo0004VUKr0kugbFTf5A',$,$,$,#6,(#21));\n#31=IFCRELAGGREGATES('1Geo0005VUKr0kugbFTf5A',$,$,$,#21,(#23));\n"
               "#32=IFCRELAGGREGATES('1Geo0006VUKr0kugbFTf5A',$,$,$,#23,(#25,#29));\n");
    if (map)
        fprintf(f, "#40=IFCPROJECTEDCRS('EPSG:2056',$,$,$,$,$,$);\n#41=IFCMAPCONVERSION(#5,#40,%.6f,%.6f,%.6f,%.17g,%.17g,$);\n", e, n, h, c, s);
    int id = 100, k = 0, els[24], ne = 0;
    typedef struct {
        double x, y, z, w, d, hh, ang;
    } Box;
    Box E[18];
    E[k++] = (Box){9150, 1000, 0, 18300, 2000, 200, 0}; /* the slab: 18,300 mm along x */
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 2; j++) E[k++] = (Box){1500 + i * 2500, 600 + j * 800, 200, 300, 300, 3000, 0};
    for (int j = 0; j < 2; j++) E[k++] = (Box){9150, 600 + j * 800, 3200, 15600, 300, 400, 0};
    E[k++] = (Box){9000, 1000, 3600, 12000, 200, 300, 30};
    for (int q = 0; q < k; q++) {
        double ca = cc_cos(E[q].ang * 3.14159265358979323846 / 180.0), sa = cc_sin(E[q].ang * 3.14159265358979323846 / 180.0);
        int p = id++, dx = id++, ap = id++, lp = id++, pr = id++, ex = id++, sr = id++, ps = id++, el = id++;
        fprintf(f, "#%d=IFCCARTESIANPOINT((%.1f,%.1f,%.1f));\n#%d=IFCDIRECTION((%.6f,%.6f,0.));\n#%d=IFCAXIS2PLACEMENT3D(#%d,#12,#%d);\n#%d=IFCLOCALPLACEMENT($,#%d);\n",
                p, ox + E[q].x, oy + E[q].y, oz + E[q].z, dx, ca, sa, ap, p, dx, lp, ap);
        fprintf(f, "#%d=IFCRECTANGLEPROFILEDEF(.AREA.,$,#11,%.1f,%.1f);\n#%d=IFCEXTRUDEDAREASOLID(#%d,#4,#12,%.1f);\n", pr, E[q].w, E[q].d, ex, pr, E[q].hh);
        fprintf(f, "#%d=IFCSHAPEREPRESENTATION(#5,'Body','SweptSolid',(#%d));\n#%d=IFCPRODUCTDEFINITIONSHAPE($,$,(#%d));\n", sr, ex, ps, sr);
        fprintf(f, "#%d=IFCBUILDINGELEMENTPROXY('2Geo%04dVUKr0kugbFTf53O',$,'e%d',$,$,#%d,#%d,$,$);\n", el, q, q, lp, ps);
        els[ne++] = el;
    }
    fprintf(f, "#%d=IFCRELCONTAINEDINSPATIALSTRUCTURE('1Geo0007VUKr0kugbFTf5C',$,$,$,(", id++);
    for (int q = 0; q < ne; q++) fprintf(f, "%s#%d", q ? "," : "", els[q]);
    fprintf(f, "),#25);\nENDSEC;\nEND-ISO-10303-21;\n");
    fclose(f);
}

/* The JSON number list after `key` ("\"at\":["): n numbers into v; 0 when absent. */
static int nums_after(const char *t, const char *key, double *v, int n) {
    const char *p = strstr(t, key);
    if (!p) return 0;
    p += strlen(key);
    for (int i = 0; i < n; i++) {
        char *e;
        v[i] = strtod(p, &e);
        if (e == p) return 0;
        p = e + 1;
    }
    return 1;
}

typedef struct {
    double c, s, t[3]; /* the link's placement: world = R(c, s) w + t */
} Place;

static void to_world(const Place *P, const double w[3], double out[3]) {
    double x = P->c * w[0] - P->s * w[1], y = P->s * w[0] + P->c * w[1];
    out[0] = x + P->t[0], out[1] = y + P->t[1], out[2] = w[2] + P->t[2];
}

static void from_world(const Place *P, const double h[3], double out[3]) { /* the placement removed, in f64 */
    double d[3] = {h[0] - P->t[0], h[1] - P->t[1], h[2] - P->t[2]};
    out[0] = P->c * d[0] + P->s * d[1], out[1] = -P->s * d[0] + P->c * d[1], out[2] = d[2];
}

/* A pointer ray straight down onto the link's point w (link world), the ray_tol 24 mm beside along
 * the host's x (12 px: 2 mm per pixel), as the page sends it (shown scene coordinates). */
static void ray_down(char *out, size_t cap, const Place *P, const double org[3], const double w[3], const char *extra) {
    double h[3];
    to_world(P, w, h);
    double o[3] = {h[0] - org[0], h[1] - org[1], h[2] + 10000 - org[2]};
    snprintf(out, cap, "{\"ray\":[%.17g,%.17g,%.17g,0,0,-1],\"ray_tol\":[%.17g,%.17g,%.17g,0,0,-1]%s}", o[0], o[1], o[2], o[0] + 24, o[1], o[2], extra);
}

typedef struct {
    double corner[3], edge[3], face[3], dist, box_sel[6], box_links[6], level1;
    double drag; /* MEAS3: how far the box's +x face moved for a drag of 1,000 mm */
    int kinds_ok, face_hit, nlevels, box_sel_ok, box_links_ok, drag_ok;
} Answers;

/* The answers on link `name` (the only one shown), in the link's own frame (placement removed). */
static void answers(cc_session *s, const char *name, const double shift[3], Answers *A) {
    memset(A, 0, sizeof *A);
    Buf o;
    buf_init(&o);
    int li = cc_links_find(s, name, strlen(name));
    Place P = {1, 0, {0, 0, 0}};
    cc_links_sync_placement(s);
    cc_link_placement(s, &s->links[li], &P.c, &P.s, P.t);
    const cc_scene *hs = cc_session_scene(s);
    double org[3] = {0, 0, 0}, at[3];
    if (hs) memcpy(org, hs->origin, sizeof org);
    char body[512];
    call(s, "action", "measure", "", &o);
    double CORNER[3] = {12, 9, 200}, EDGE[3] = {5000, 7, 200}, FACE[3] = {5000, 1000, 200}, CORNER2[3] = {18289, 11, 200};
    for (int k = 0; k < 3; k++) CORNER[k] += shift[k], EDGE[k] += shift[k], FACE[k] += shift[k], CORNER2[k] += shift[k]; /* the file's own coordinates */
    ray_down(body, sizeof body, &P, org, CORNER, "");
    call(s, "tool", "measure_pointer", body, &o);
    A->kinds_ok = strstr(buf_cstr(&o), "\"snap\":\"corner\"") != NULL;
    if (nums_after(buf_cstr(&o), "\"at\":[", at, 3)) from_world(&P, at, A->corner);
    if (!A->kinds_ok) printf("  %s corner: %s\n", name, buf_cstr(&o));
    ray_down(body, sizeof body, &P, org, EDGE, "");
    call(s, "tool", "measure_pointer", body, &o);
    int ek = strstr(buf_cstr(&o), "\"snap\":\"edge\"") != NULL;
    if (!ek) printf("  %s edge: %s\n", name, buf_cstr(&o));
    A->kinds_ok &= ek;
    if (nums_after(buf_cstr(&o), "\"at\":[", at, 3)) from_world(&P, at, A->edge);
    ray_down(body, sizeof body, &P, org, FACE, "");
    call(s, "tool", "measure_click", body, &o); /* one click on the slab: the pick */
    A->face_hit = strstr(buf_cstr(&o), "\"snap\":\"face\"") && strstr(buf_cstr(&o), "\"first\":[");
    if (nums_after(buf_cstr(&o), "\"first\":[", at, 3)) from_world(&P, at, A->face);
    call(s, "tool", "measure_cancel", "", &o);
    ray_down(body, sizeof body, &P, org, CORNER, "");
    call(s, "tool", "measure_click", body, &o);
    ray_down(body, sizeof body, &P, org, CORNER2, "");
    call(s, "tool", "measure_click", body, &o);
    double v = 0;
    if (nums_after(buf_cstr(&o), "\"value_mm\":", &v, 1)) A->dist = v;
    call(s, "tool", "measure_clear", "", &o);
    call(s, "tool", "measure_cancel", "", &o), call(s, "tool", "measure_cancel", "", &o);
    /* the levels read from the links (M1LINKS): the second storey, placement removed */
    Arena a;
    arena_init(&a, 0);
    cc_level *lv;
    size_t n;
    if (!cc_link_levels(s, NULL, 0, &a, &lv, &n, NULL)) {
        A->nlevels = (int)n; /* every link's (merged within 200 mm): this link's second storey among them */
        for (size_t i = 0; i < n; i++)
            if (dabs(lv[i].elevation - P.t[2] - shift[2] - 3200) < 1) A->level1 = lv[i].elevation - P.t[2] - shift[2];
    }
    arena_free(&a);
    /* Box to selection around the slab, then the first section box over the links */
    char id[23];
    cc_link_id(&s->links[li], 0, id);
    id[22] = 0;
    snprintf(body, sizeof body, "{\"link\":\"%s\",\"ids\":[\"%s\"]}", name, id);
    call(s, "select", "-", body, &o);
    call(s, "action", "box_to_selection", "", &o);
    call(s, "query", "visibility", "{}", &o);
    double mn[3], mx[3];
    if (nums_after(buf_cstr(&o), "\"min\":[", mn, 3) && nums_after(buf_cstr(&o), "\"max\":[", mx, 3)) {
        A->box_sel_ok = 1; /* axis-aligned in the host frame: the lower corner moved back by t only */
        for (int k = 0; k < 3; k++) A->box_sel[k] = mn[k] + org[k] - P.t[k], A->box_sel[3 + k] = mx[k] + org[k] - P.t[k];
    }
    /* MEAS3: drag the box's +x face by 1,000 mm (box_press on its handle, box_release 1,000 mm further along x) */
    if (A->box_sel_ok) {
        double c[3] = {mx[0], 0.5 * (mn[1] + mx[1]), 0.5 * (mn[2] + mx[2])}; /* the face's centre, scene coordinates */
        for (int step = 0; step < 2; step++) {
            double x = c[0] + (step ? 1000.0 : 0.0), y = c[1] - 10000.0;
            snprintf(body, sizeof body, "{\"ray\":[%.17g,%.17g,%.17g,0,1,0],\"ray_tol\":[%.17g,%.17g,%.17g,0,1,0]}", x, y, c[2], x, y, c[2] + 24);
            int st = call(s, "tool", step ? "box_release" : "box_press", body, &o);
            if (!step) A->drag_ok = st == CC_OK && strstr(buf_cstr(&o), "\"face\":1") != NULL;
        }
        call(s, "query", "visibility", "{}", &o);
        double mx2[3];
        if (A->drag_ok && nums_after(buf_cstr(&o), "\"max\":[", mx2, 3)) A->drag = mx2[0] - mx[0];
        else printf("  %s: the drag: %s\n", name, buf_cstr(&o));
    }
    call(s, "select", "-", "[]", &o);
    call(s, "action", "section_box", "", &o); /* off */
    s->vis.has_box = 0;
    call(s, "action", "section_box", "", &o); /* on: fitted to the links */
    call(s, "query", "visibility", "{}", &o);
    if (nums_after(buf_cstr(&o), "\"min\":[", mn, 3) && nums_after(buf_cstr(&o), "\"max\":[", mx, 3)) {
        A->box_links_ok = 1;
        for (int k = 0; k < 3; k++) A->box_links[k] = mn[k] + org[k] - P.t[k], A->box_links[3 + k] = mx[k] + org[k] - P.t[k];
    }
    call(s, "action", "section_box", "", &o);
    s->vis.has_box = 0;
    buf_free(&o);
}

static double gap3(const double *a, const double *b, int n) {
    double g = 0;
    for (int k = 0; k < n; k++)
        if (!(dabs(a[k] - b[k]) <= g)) g = dabs(a[k] - b[k]);
    return g;
}

static void show(cc_session *s, const char *name, int on) {
    char body[256];
    Buf o;
    buf_init(&o);
    snprintf(body, sizeof body, "{\"op\":\"%s\",\"name\":\"%s\"}", on ? "show" : "hide", name);
    call(s, "action", "manage_links", body, &o);
    buf_free(&o);
}

/* MEAS3: paper through links. An empty open model with link `path` alone; the plan named `plan`: its DXF's
 * points (model space, world x y) into `pts` (n points), its SVG into `svg`. 0 when there is none. The plan of
 * the link's L0: at 0 it merges with the host's Level 0 (within 200 mm), far it is a plan from links, "Plan L0". */
static size_t paper_case(const char *path, const char *plan, double **pts, Buf *svg, char *first_views, size_t cap) {
    cc_session *s = cc_session_new(cc_platform_native());
    cc_error err;
    size_t n = 0;
    *pts = NULL;
    if (!s || cc_session_new_model(s, &err)) return 0;
    cc_georef *g = &s->model.georef;
    g->map_rotation_deg = 0, g->east_m = 1000, g->north_m = 1000, g->height_m = 0;
    g->has_crs = 1, g->crs = (cc_str){"EPSG:2056", 9};
    Buf o;
    buf_init(&o);
    char body[512];
    snprintf(body, sizeof body, "{\"path\":\"%s\"}", path);
    call(s, "action", "link_model", body, &o);
    call(s, "query", "open_views", "", &o);
    snprintf(first_views, cap, "%.*s", (int)(o.len < cap - 1 ? o.len : cap - 1), buf_cstr(&o));
    char key[96];
    snprintf(key, sizeof key, "\"name\":\"%s\"", plan);
    const char *pv = strstr(buf_cstr(&o), key), *idp = NULL;
    for (const char *q = buf_cstr(&o); pv && (q = strstr(q, "\"id\":\"")) && q < pv; q += 6) idp = q + 6;
    if (idp) {
        char id[23];
        memcpy(id, idp, 22), id[22] = 0;
        snprintf(body, sizeof body, "{\"view\":\"%s\"}", id);
        Buf d;
        buf_init(&d);
        int st = call(s, "query", "view_dxf", body, &d);
        if (st != CC_OK) printf("  %s: view_dxf %d: %.200s\n", path, st, buf_cstr(&d));
        if (st == CC_OK && getenv("MEAS3_DUMP")) { /* debugging: the DXF beside the test */
            char fn[256];
            snprintf(fn, sizeof fn, "%s.dxf", path);
            FILE *f = fopen(fn, "wb");
            if (f) fwrite(d.p, 1, d.len, f), fclose(f);
        }
        if (st == CC_OK) {
            cc_dxf x;
            Buf e;
            buf_init(&e);
            if (!cc_dxf_read((const uint8_t *)d.p, d.len, &x, &e)) {
                *pts = malloc((x.npts ? x.npts : 1) * 2 * sizeof **pts);
                for (size_t k = 0; *pts && k < x.npolylines; k++) { /* the cut and seen elements (a plan from links also
                                                                     * draws what is above up to the next link level) */
                    const char *ln = x.layer[k] < x.nlayers ? x.layers[x.layer[k]].name : "";
                    if (strstr(ln, "-above")) continue;
                    for (size_t i = x.start[k]; i < x.start[k + 1]; i++) (*pts)[2 * n] = x.xy[2 * i], (*pts)[2 * n + 1] = x.xy[2 * i + 1], n++;
                }
                cc_dxf_free(&x);
            }
            buf_free(&e);
        }
        buf_free(&d);
        snprintf(body, sizeof body, "{\"view\":\"%s\",\"bare\":true}", id);
        call(s, "query", "view_svg", body, svg);
    }
    buf_free(&o);
    cc_session_free(s);
    return n;
}

static int cmp_xy(const void *a, const void *b) {
    const double *p = a, *q = b;
    return p[0] != q[0] ? (p[0] < q[0] ? -1 : 1) : p[1] < q[1] ? -1 : p[1] > q[1];
}

/* The point of `pts` nearest to (x, y). */
static void nearest(const double *pts, size_t n, double x, double y, double out[2]) {
    double best = 1e300;
    for (size_t i = 0; i < n; i++) {
        double dx = pts[2 * i] - x, dy = pts[2 * i + 1] - y, d = dx * dx + dy * dy;
        if (d < best) best = d, out[0] = pts[2 * i], out[1] = pts[2 * i + 1];
    }
}

/* MEAS2, unit: one layer (a scene) moved by `off`, a ray down onto its point w: the snap. */
static int layer_snap(const cc_scene *sc, const double off[3], const double aim[3], const double w[3], cc_msnap *out) {
    cc_mlayer L;
    memset(&L, 0, sizeof L);
    L.sc = sc;
    memcpy(L.off, off, sizeof L.off);
    double org[3] = {0, 0, 0};
    cc_mview v;
    memset(&v, 0, sizeof v);
    v.layers = &L, v.nlayers = 1, v.org = org;
    double p[3] = {w[0] - sc->origin[0] + aim[0], w[1] - sc->origin[1] + aim[1], w[2] - sc->origin[2] + aim[2]}; /* the ray: as if moved by `aim` */
    double ray[6] = {p[0], p[1], p[2] + 10000, 0, 0, -1}, tol[6] = {p[0] + 24, p[1], p[2] + 10000, 0, 0, -1};
    memset(out, 0, sizeof *out);
    return cc_measure_snap(&v, ray, tol, 12.0, 0, out);
}

int main(int argc, char **argv) {
    (void)argc, (void)argv;
    mkdir("out", 0755);
    mkdir(DIR, 0755);
    /* the host: georeferenced (E 1000 m, N 1000 m, no turn); the links: 30 degrees, at 0 and at (2.6e9, 1.2e9, 5e5) mm */
    write_building(DIR "/a_zero.ifc", 0, 0, 0, 1, 1000, 1000, 0, 30);
    write_building(DIR "/b_far.ifc", 0, 0, 0, 1, 2601000, 1201000, 500, 30);
    write_building(DIR "/c_own.ifc", 2600000000.0, 1200000000.0, 470000.0, 0, 0, 0, 0, 0); /* its own coordinates at 2,600 km */
    write_building(DIR "/d_ref.ifc", 0, 0, 0, 0, 0, 0, 0, 0);                              /* the same at 0, no placement */
    write_building(DIR "/e_huge.ifc", 0, 0, 0, 1, 1e12, 1000, 0, 30);                    /* a map conversion at 1e15 mm */
    cc_session *s = cc_session_new(cc_platform_native());
    cc_error err;
    int ok = s && !cc_session_new_model(s, &err);
    check_true("an empty open model", ok);
    if (!ok) return report("measure_geo");
    cc_georef *g = &s->model.georef;
    g->map_rotation_deg = 0, g->east_m = 1000, g->north_m = 1000, g->height_m = 0;
    g->has_crs = 1, g->crs = (cc_str){"EPSG:2056", 9};
    Buf o;
    buf_init(&o);
    const char *L[5] = {DIR "/a_zero.ifc", DIR "/b_far.ifc", DIR "/c_own.ifc", DIR "/d_ref.ifc", DIR "/e_huge.ifc"};
    const char *N[5] = {"a_zero.ifc", "b_far.ifc", "c_own.ifc", "d_ref.ifc", "e_huge.ifc"};
    int linked = 0;
    for (int i = 0; i < 5; i++) {
        char body[512];
        snprintf(body, sizeof body, "{\"path\":\"%s\"}", L[i]);
        linked += call(s, "action", "link_model", body, &o) == CC_OK;
    }
    check_true("five links", linked == 5 && s->nlinks == 5);
    const cc_scene *hs = cc_session_scene(s);
    check_true("the open model is empty: its scene origin is 0", hs && hs->nelements == 0 && hs->origin[0] == 0 && hs->origin[1] == 0 && hs->origin[2] == 0);
    cc_links_sync_placement(s);
    Place P0, PF;
    cc_link_placement(s, &s->links[0], &P0.c, &P0.s, P0.t);
    cc_link_placement(s, &s->links[1], &PF.c, &PF.s, PF.t);
    printf("  a_zero placed by (%.17g, %.17g, %.17g), turned (%.17g, %.17g); b_far by (%.17g, %.17g, %.17g)\n", P0.t[0], P0.t[1], P0.t[2], P0.c, P0.s,
           PF.t[0], PF.t[1], PF.t[2]);
    check_true("the far link: placed 30 degrees, at (2.6e9, 1.2e9, 5e5) mm from the one at 0",
               P0.c == PF.c && P0.s == PF.s && dabs(P0.s - 0.5) < 1e-12 && PF.t[0] - P0.t[0] == 2.6e9 && PF.t[1] - P0.t[1] == 1.2e9 && PF.t[2] - P0.t[2] == 5e5);

    Answers A0, AF, AO, AR;
    for (int i = 0; i < 5; i++) show(s, N[i], i == 0);
    static const double NO[3] = {0, 0, 0}, SH[3] = {2600000000.0, 1200000000.0, 470000.0};
    answers(s, N[0], NO, &A0);
    for (int i = 0; i < 5; i++) show(s, N[i], i == 1);
    answers(s, N[1], NO, &AF);
    for (int i = 0; i < 5; i++) show(s, N[i], i == 2);
    answers(s, N[2], SH, &AO);
    for (int i = 0; i < 5; i++) show(s, N[i], i == 3);
    answers(s, N[3], NO, &AR);
    printf("  at 0:   corner (%.6f, %.6f, %.6f) edge (%.6f, %.6f, %.6f) face (%.6f, %.6f, %.6f) %.6f mm\n", A0.corner[0], A0.corner[1], A0.corner[2], A0.edge[0],
           A0.edge[1], A0.edge[2], A0.face[0], A0.face[1], A0.face[2], A0.dist);
    printf("  far:    corner (%.6f, %.6f, %.6f) edge (%.6f, %.6f, %.6f) face (%.6f, %.6f, %.6f) %.6f mm\n", AF.corner[0], AF.corner[1], AF.corner[2], AF.edge[0],
           AF.edge[1], AF.edge[2], AF.face[0], AF.face[1], AF.face[2], AF.dist);
    check_true("at 0: a corner, an edge point, a face (one click hits the slab)", A0.kinds_ok && A0.face_hit);
    check_true("at 0: the slab's corner (0, 0, 200) within 0.01 mm, 18,300 mm within 0.01 mm",
               dabs(A0.corner[0]) < 0.01 && dabs(A0.corner[1]) < 0.01 && dabs(A0.corner[2] - 200) < 0.01 && dabs(A0.dist - 18300) < 0.01);
    check_true("far: a corner, an edge point, a face (one click hits the slab)", AF.kinds_ok && AF.face_hit);
    double gc = gap3(A0.corner, AF.corner, 3), ge = gap3(A0.edge, AF.edge, 3), gf = gap3(A0.face, AF.face, 3), gd = dabs(A0.dist - AF.dist);
    printf("  far - at 0 (placement removed): corner %.6g mm, edge %.6g, face %.6g, distance %.6g\n", gc, ge, gf, gd);
    check_true("far: the corner as at 0 within 0.001 mm", gc <= TOL);
    check_true("far: the edge point as at 0 within 0.001 mm", ge <= TOL);
    check_true("far: the face point (the pick) as at 0 within 0.001 mm", gf <= TOL);
    check_true("far: 18,300 mm as at 0 within 0.001 mm", gd <= TOL);
    check_true("far: the levels read from the link as at 0 (f64)", dabs(A0.level1 - 3200) <= TOL && dabs(AF.level1 - 3200) <= TOL);
    double gb = gap3(A0.box_sel, AF.box_sel, 6), gl = gap3(A0.box_links, AF.box_links, 6);
    printf("  section box: Box to selection far - at 0 %.6g mm; fitted to the links %.6g mm\n", gb, gl);
    check_true("the section box: answered at 0 and far", A0.box_sel_ok && AF.box_sel_ok && A0.box_links_ok && AF.box_links_ok);
    /* MEAS3: the box in its own frame (cc_vis.base). Its corners stay f32 (Rust's ClipBox), relative to the scene
     * origin at 0 and to the box's base far away: each is within half an f32 step of the true corner (0.001 mm at
     * 18 m), so the two agree within one step (0.002 mm). Before MEAS3: 150.6 mm. */
    check_true("far: Box to selection as at 0 within one f32 step at 18 m (0.002 mm)", gb <= 0.002);
    check_true("far: the section box fitted to the links as at 0 within 0.002 mm", gl <= 0.002);
    printf("  drag of 1,000 mm: the +x face moved %.6f mm at 0, %.6f mm far\n", A0.drag, AF.drag);
    check_true("far: dragging the box's face 1,000 mm moves it 1,000.000 mm (within 0.001), as at 0",
               A0.drag_ok && AF.drag_ok && dabs(A0.drag - 1000) <= TOL && dabs(AF.drag - 1000) <= TOL);
    /* the building with its own coordinates at 2,600 km (no placement) against the same at 0 */
    for (int k = 0; k < 3; k++) AO.corner[k] -= SH[k], AO.edge[k] -= SH[k], AO.face[k] -= SH[k];
    double oc = gap3(AR.corner, AO.corner, 3), oe = gap3(AR.edge, AO.edge, 3), of = gap3(AR.face, AO.face, 3), od = dabs(AR.dist - AO.dist);
    printf("  own coordinates at 2,600 km - at 0: corner %.6g mm, edge %.6g, face %.6g, distance %.6g (%.6f mm)\n", oc, oe, of, od, AO.dist);
    check_true("own coordinates at 2,600 km: corner, edge, face, 18,300 mm as at 0 within 0.001 mm",
               AO.kinds_ok && AO.face_hit && oc <= TOL && oe <= TOL && of <= TOL && od <= TOL && dabs(AR.dist - 18300) < 0.01);
    /* hostile: a link placed 1e15 mm away (a map conversion at 1e12 m): placed, its tools answer without a crash; a
     * pointer ray out there is refused (the measure body's numbers stay within 1e12 mm) */
    for (int i = 0; i < 5; i++) show(s, N[i], i == 4);
    Place PH;
    cc_link_placement(s, &s->links[4], &PH.c, &PH.s, PH.t);
    Answers AH;
    answers(s, N[4], NO, &AH);
    char hb[512];
    ray_down(hb, sizeof hb, &PH, hs ? hs->origin : NO, NO, "");
    call(s, "action", "measure", "", &o);
    int st = call(s, "tool", "measure_pointer", hb, &o);
    int refused = st == CC_ERR && strstr(buf_cstr(&o), "ray: expected") != NULL;
    printf("  a link at %.6g mm: the pointer answers %d %s\n", PH.t[0], st, buf_cstr(&o));
    call(s, "action", "measure", "", &o);
    check_true("hostile: a link 1e15 mm away: no crash, a pointer ray out there refused (400)", PH.t[0] > 9e14 && refused);

    /* MEAS3: paper through links: the plan from the link at 0 and from the one at 2,600 km, alone in an empty model */
    {
        double *p0 = NULL, *pf = NULL;
        Buf s0, sf;
        buf_init(&s0), buf_init(&sf);
        char v0[4096], vf[4096];
        size_t n0 = paper_case(L[0], "Plan Level 0", &p0, &s0, v0, sizeof v0), nf = paper_case(L[1], "Plan L0", &pf, &sf, vf, sizeof vf);
        if (!n0 || !nf || getenv("MEAS3_VIEWS")) printf("  views at 0: %s\n  views far: %s\n", v0, vf);
        check_true("paper: a plan from the link at 0 and from the far one, as DXF", n0 > 20 && nf > 20);
        double gp = n0 == nf ? 0 : 1e300;
        for (size_t i = 0; i < nf; i++) pf[2 * i] -= PF.t[0], pf[2 * i + 1] -= PF.t[1]; /* the placement removed (no turn: the host's north) */
        if (n0 == nf) {
            qsort(p0, n0, 2 * sizeof *p0, cmp_xy), qsort(pf, nf, 2 * sizeof *pf, cmp_xy);
            for (size_t i = 0; i < 2 * n0; i++)
                if (dabs(p0[i] - pf[i]) > gp) gp = dabs(p0[i] - pf[i]);
        }
        /* the slab's 18,300 mm edge: its two corners R(30)(0, 0) and R(30)(18300, 0) */
        double c0[2] = {0, 0}, c1[2] = {0, 0}, f0[2] = {0, 0}, f1[2] = {0, 0}, e[2] = {P0.c * 18300, P0.s * 18300};
        nearest(p0, n0, 0, 0, c0), nearest(p0, n0, e[0], e[1], c1), nearest(pf, nf, 0, 0, f0), nearest(pf, nf, e[0], e[1], f1);
        double span0 = cc_sqrt((c1[0] - c0[0]) * (c1[0] - c0[0]) + (c1[1] - c0[1]) * (c1[1] - c0[1]));
        double spanf = cc_sqrt((f1[0] - f0[0]) * (f1[0] - f0[0]) + (f1[1] - f0[1]) * (f1[1] - f0[1]));
        printf("  paper: DXF %zu / %zu points (cut and seen), far - at 0 %.6g mm; the slab's edge %.4f mm at 0, %.4f mm far; SVG %zu / %zu bytes\n", n0, nf,
               gp, span0, spanf, s0.len, sf.len);
        check_true("paper: the far plan's DXF points are the plan at 0's, moved by the placement, within 0.002 mm", gp <= 0.002);
        check_true("paper: the slab's edge is 18,300 mm on the far plan (within 0.002 mm), as at 0", dabs(spanf - 18300) <= 0.002 && dabs(span0 - 18300) <= 0.002);
        check_true("paper: the far plan's DXF is in national coordinates (x about 2.6e9 mm)", nf && pf[0] + PF.t[0] > 2.5e9);
        free(p0), free(pf), buf_free(&s0), buf_free(&sf);
    }

    /* unit: one layer moved by 1e15 mm, NaN, infinity */
    const cc_scene *ref = cc_link_scene(&s->links[3]);
    double w[3] = {12, 9, 200}, z[3] = {0, 0, 0}, big[3] = {1e15, -1e15, 1e12}, nan3[3] = {0.0 / 0.0, 0, 0}, inf3[3] = {0, 1.0 / 0.0, 0};
    cc_msnap a, b;
    int r0 = layer_snap(ref, z, z, w, &a), r1 = layer_snap(ref, big, big, w, &b);
    double ga = 0;
    for (int k = 0; k < 3; k++) {
        double pa = (double)a.p[k] + a.org[k], pb = (double)b.p[k] + b.org[k] - big[k];
        if (dabs(pa - pb) > ga) ga = dabs(pa - pb);
    }
    printf("  a layer 1e15 mm away: the corner %.6g mm from the one at 0 (f64 steps 0.125 mm there)\n", ga);
    check_true("hostile: a layer 1e15 mm away: its corner, to f64's step there (0.125 mm)", r0 == 1 && r1 == 1 && a.kind == CC_MSNAP_VERTEX && b.kind == CC_MSNAP_VERTEX && ga <= 0.125);
    cc_mdim d = {a, b};
    float far_mm = cc_mdim_distance(&d);
    check_true("hostile: the distance across 1e15 mm is finite", far_mm == far_mm && far_mm > 1e14f && far_mm < 1e16f);
    cc_msnap c;
    check_true("hostile: a NaN or infinite offset: not hit, no crash", layer_snap(ref, nan3, z, w, &c) == 0 && layer_snap(ref, inf3, z, w, &c) == 0);

    buf_free(&o);
    cc_session_free(s);
    for (int i = 0; i < 5; i++) remove(L[i]);
    if (!getenv("MEAS3_DUMP")) rmdir(DIR);
    return report("measure_geo");
}
