/* LINKF (session/links.h, ADR 0079, DEVIATIONS 37): where a linked model lands, by hand.
 *   - "file" with both georeferenced: a link with IfcMapConversion (30 deg, E 2600000, N 1200000,
 *     H 400) on a host at 90 deg, E 2600010, N 1200020, H 395: the link's (1000, 0, 0) lands at
 *     (-19500, 9133.975, 5000) (worked in the comment below); two IFC of one coordinate system
 *     (the second's site moved, its map conversion moved back) come out on top of each other;
 *   - base point to base point (the IFC's building at (5000, 2000, 0), the host's base point its
 *     world origin, ADR 0075), survey point to survey point (the host's survey point), origin to
 *     origin with a turn of 90 deg and an offset, base point with a turn and an offset;
 *   - the placed scene is the read one turned and moved (each vertex), the pin refuses a move;
 *   - across links: the measure tool hits a link where it was moved, Box to selection boxes a
 *     placed linked element, a hidden link is not hit, query element of each of three links;
 *   - paper: a plan with a moved link differs, moved back it is the same bytes;
 *   - /mesh: the LNKS trailer stays version 1 unplaced, version 2 with a placement or a colour.
 * Files written under out/test-link_place (removed at the end). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/links.h"
#include "../session/mesh.h"
#include "../session/session.h"
#include "check.h"

#define DIR "out/test-link_place"

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_clear(out);
    out->oom = 0;
    return cc_call(s, op, name, body, strlen(body), out);
}

/* frame.ifc with `before` replaced by `after` and `extra` entities added before the data's end. */
static void write_ifc(const char *frame, const char *to, const char *before, const char *after, const char *extra) {
    Buf b;
    buf_init(&b);
    const char *at = before ? strstr(frame, before) : NULL;
    const char *end = strstr(frame, "ENDSEC;\nEND-ISO");
    if (!end) end = frame + strlen(frame);
    if (at) buf_append(&b, frame, (size_t)(at - frame)), buf_puts(&b, after), at += strlen(before);
    else at = frame;
    buf_append(&b, at, (size_t)(end - at));
    buf_puts(&b, extra);
    buf_puts(&b, end);
    FILE *f = fopen(to, "wb");
    check_true(to, f && fwrite(b.p, 1, b.len, f) == b.len);
    if (f) fclose(f);
    buf_free(&b);
}

static double dabs(double x) { return x < 0 ? -x : x; }
static int near(double a, double b, double tol) { return dabs(a - b) <= tol; }

/* Link k's placement applied to w. */
static void place(cc_session *s, int k, const double w[3], double out[3]) {
    double c, sn, t[3];
    cc_link_placement(s, &s->links[k], &c, &sn, t);
    out[0] = c * w[0] - sn * w[1] + t[0], out[1] = sn * w[0] + c * w[1] + t[1], out[2] = w[2] + t[2];
}

static int place_is(cc_session *s, int k, double x, double y, double z, double ex, double ey, double ez) {
    double w[3] = {x, y, z}, o[3];
    place(s, k, w, o);
    int ok = near(o[0], ex, 1e-6) && near(o[1], ey, 1e-6) && near(o[2], ez, 1e-6);
    if (!ok) printf("  placed (%g, %g, %g) -> (%.9g, %.9g, %.9g), want (%.9g, %.9g, %.9g)\n", x, y, z, o[0], o[1], o[2], ex, ey, ez);
    return ok;
}

/* The world point of vertex v of element e of a scene (its origin + the placed mesh). */
static int world_vertex(const cc_scene *sc, size_t e, uint32_t v, double out[3]) {
    Arena a;
    arena_init(&a, 0);
    cc_mesh m;
    int ok = cc_scene_world_mesh(sc, e, &a, &m) == 0 && v < m.nv;
    if (ok)
        for (int q = 0; q < 3; q++) out[q] = sc->origin[q] + m.pos[v][q];
    arena_free(&a);
    return ok;
}

static int find_link(cc_session *s, const char *name) { return cc_links_find(s, name, strlen(name)); }

int main(int argc, char **argv) {
    const char *floats = argc > 1 ? argv[1] : "ccore/golden/floats";
    char gold[4096], path[4096];
    snprintf(gold, sizeof gold, "%s/..", floats);
    mkdir("out", 0755);
    mkdir(DIR, 0755);
    size_t fn;
    char *frame = slurp(gold, "ifc/frame.ifc", &fn);
    check_true("frame.ifc read", frame != NULL);
    if (!frame) return report("link_place");
    /* A: georeferenced, 30 deg (cos 30, sin 30), E 2600000, N 1200000, H 400 (metres, IFC4) */
    const char *mapA = "#9001=IFCPROJECTEDCRS('EPSG:2056',$,$,$,$,$,$);\n"
                       "#9002=IFCMAPCONVERSION(#14,#9001,2600000.,1200000.,400.,0.86602540378443865,0.5,$);\n";
    write_ifc(frame, DIR "/a_geo.ifc", NULL, NULL, mapA);
    /* B: the same building, its site moved by (-3000, -1000) and its map conversion moved back by
     * R(30)(3, 1) m = (2.098076211, 2.366025404): the same survey coordinates for every element */
    write_ifc(frame, DIR "/b_geo.ifc", "#17=IFCLOCALPLACEMENT($,#13);", "#17=IFCLOCALPLACEMENT($,#9011);",
              "#9010=IFCCARTESIANPOINT((-3000.,-1000.,0.));\n#9011=IFCAXIS2PLACEMENT3D(#9010,$,$);\n"
              "#9001=IFCPROJECTEDCRS('EPSG:2056',$,$,$,$,$,$);\n"
              "#9002=IFCMAPCONVERSION(#14,#9001,2600002.0980762114,1200002.3660254038,400.,0.86602540378443865,0.5,$);\n");
    /* C: no georeference, its building (base point) at (5000, 2000, 0) */
    write_ifc(frame, DIR "/c_base.ifc", "#19=IFCLOCALPLACEMENT(#17,#13);", "#19=IFCLOCALPLACEMENT(#17,#9021);",
              "#9020=IFCCARTESIANPOINT((5000.,2000.,0.));\n#9021=IFCAXIS2PLACEMENT3D(#9020,$,$);\n");
    write_ifc(frame, DIR "/d_copy.ifc", NULL, NULL, "");
    free(frame);

    cc_session *s = cc_session_new(cc_platform_native());
    cc_error err;
    snprintf(path, sizeof path, "%s/frame.json", gold);
    if (!s || cc_session_open(s, path, &err)) {
        check_true("frame.json opens", 0);
        return report("link_place");
    }
    /* the host: 90 deg, E 2600010, N 1200020, H 395; its survey point at (12000, 8000, 300) */
    cc_georef *g = &s->model.georef;
    g->map_rotation_deg = 90, g->east_m = 2600010, g->north_m = 1200020, g->height_m = 395;
    g->has_crs = 1, g->crs = (cc_str){"EPSG:2056", 9};
    g->has_survey_point = 1, g->survey_point_mm[0] = 12000, g->survey_point_mm[1] = 8000, g->survey_point_mm[2] = 300;

    Buf o;
    buf_init(&o);
    const char *L[4] = {DIR "/a_geo.ifc", DIR "/b_geo.ifc", DIR "/c_base.ifc", DIR "/d_copy.ifc"};
    for (int i = 0; i < 4; i++) {
        char body[512];
        snprintf(body, sizeof body, "{\"path\":\"%s\"}", L[i]);
        check_true("link_model", call(s, "action", "link_model", body, &o) == CC_OK);
    }
    int A = find_link(s, "a_geo.ifc"), B = find_link(s, "b_geo.ifc"), C = find_link(s, "c_base.ifc"), D = find_link(s, "d_copy.ifc");
    check_true("four links", A >= 0 && B >= 0 && C >= 0 && D >= 0 && s->nlinks == 4);
    if (A < 0 || B < 0 || C < 0 || D < 0) return report("link_place");

    /* "file", both georeferenced. survey_l(w) = R(30) w + S_l, survey_h(h) = R(90) h + S_h, so
     * h = R(-90) (R(30) w + S_l - S_h). w = (1000, 0, 0): R(30) w = (866.0254038, 500);
     * S_l - S_h = (-10000, -20000) mm: (-9133.9745962, -19500); R(-90) (x, y) = (y, -x):
     * (-19500, 9133.9745962); z: 0 + (400 - 395) m = 5000. Checked back: survey_h of it =
     * R(90)(-19.5, 9.1339746) m + S_h = (2600000.8660254, 1200000.5) = survey_l(w). */
    check_true("file: a georeferenced link lands through the survey coordinates",
               place_is(s, A, 1000, 0, 0, -19500, 9133.974596215561, 5000));
    check_true("file: its origin (0, 0, 0)", place_is(s, A, 0, 0, 0, -20000, 10000, 5000));
    /* two of one coordinate system: element k of each at the same host point */
    cc_links_sync_placement(s);
    const cc_scene *sa = cc_link_scene(&s->links[A]), *sb = cc_link_scene(&s->links[B]);
    int aligned = sa->nelements == sb->nelements && sa->nelements > 0;
    double worst = 0;
    for (size_t e = 0; aligned && e < sa->nelements; e += 7) {
        double pa[3], pb[3];
        aligned = world_vertex(sa, e, 0, pa) && world_vertex(sb, e, 0, pb);
        for (int q = 0; aligned && q < 3; q++) if (dabs(pa[q] - pb[q]) > worst) worst = dabs(pa[q] - pb[q]);
    }
    if (worst > 0.05) printf("  A and B apart by %g mm\n", worst);
    check_true("file: two IFC of one coordinate system come out aligned (within 0.05 mm)", aligned && worst <= 0.05);
    /* the placed scene is the read one, each vertex turned and moved */
    const cc_scene *ra = cc_link_raw_scene(&s->links[A]);
    double rw[3], pw[3], want[3];
    check_true("placed scene: a vertex of the read scene placed by hand", world_vertex(ra, 3, 1, rw) && world_vertex(sa, 3, 1, pw));
    place(s, A, rw, want);
    check_true("placed scene: the same point (f32: 0.05 mm)", near(pw[0], want[0], 0.05) && near(pw[1], want[1], 0.05) && near(pw[2], want[2], 0.05));

    /* C: not georeferenced: "file" is the identity; the base point and the survey point */
    check_true("file: a link without a georeference stays", place_is(s, C, 1000, 0, 0, 1000, 0, 0));
    check_true("pinned: place refused", call(s, "action", "manage_links", "{\"op\":\"place\",\"name\":\"c_base.ifc\",\"origin\":\"base_point\"}", &o) == CC_ERR &&
                                            strstr(buf_cstr(&o), "pinned"));
    call(s, "action", "manage_links", "{\"op\":\"unpin\",\"name\":\"c_base.ifc\"}", &o);
    check_true("base point to base point", call(s, "action", "manage_links", "{\"op\":\"place\",\"name\":\"c_base.ifc\",\"origin\":\"base_point\"}", &o) == CC_OK &&
                                               place_is(s, C, 1000, 0, 0, 1000 - 5000, -2000, 0));
    check_true("survey point to survey point", call(s, "action", "manage_links", "{\"op\":\"place\",\"name\":\"c_base.ifc\",\"origin\":\"survey_point\"}", &o) == CC_OK &&
                                                   place_is(s, C, 1000, 0, 0, 13000, 8000, 300));
    /* origin to origin, turned 90 deg about Z, then (1000, 0, 0): (1000, 0, 0) -> (0, 1000) + (1000, 0) */
    check_true("origin, 90 deg and an offset", call(s, "action", "manage_links", "{\"op\":\"place\",\"name\":\"c_base.ifc\",\"origin\":\"origin\",\"rotation_deg\":90,\"move_mm\":[1000,0,0]}", &o) == CC_OK &&
                                                   place_is(s, C, 1000, 0, 0, 1000, 1000, 0));
    /* base point, 90 deg, (1000, 0, 0): R90((1000, 0) - (5000, 2000)) + (1000, 0) = R90(-4000, -2000) + (1000, 0) = (3000, -4000) */
    check_true("base point, 90 deg and an offset", call(s, "action", "manage_links", "{\"op\":\"place\",\"name\":\"c_base.ifc\",\"origin\":\"base_point\"}", &o) == CC_OK &&
                                                       place_is(s, C, 1000, 0, 0, 3000, -4000, 0));
    /* "file" on A with a manual turn and offset on top: R90(-19500, 9133.97) + (0, 0, 100) */
    call(s, "action", "manage_links", "{\"op\":\"unpin\",\"name\":\"a_geo.ifc\"}", &o);
    check_true("file, then 90 deg and an offset", call(s, "action", "manage_links", "{\"op\":\"place\",\"name\":\"a_geo.ifc\",\"rotation_deg\":90,\"move_mm\":[0,0,100]}", &o) == CC_OK &&
                                                      place_is(s, A, 1000, 0, 0, -9133.974596215561, -19500, 5100));

    /* /mesh: version 2 with placements; the colour */
    const char *mp;
    size_t mn;
    uint32_t rev;
    double ms;
    check_true("mesh", cc_mesh_get_form(s, 3, &mp, &mn, &rev, &ms) == 200);
    const char *lk = NULL;
    for (size_t i = 0; mp && i + 8 <= mn; i += 4)
        if (!memcmp(mp + i, "LNKS", 4)) lk = mp + i;
    uint32_t ver = 0;
    if (lk) memcpy(&ver, lk + 4, 4);
    check_true("LNKS version 2 with placed links", ver == 2);

    /* across links: the measure tool on a copy moved up 20 m (the host frame is below 7 m) */
    call(s, "action", "manage_links", "{\"op\":\"hide\",\"name\":\"a_geo.ifc\"}", &o);
    call(s, "action", "manage_links", "{\"op\":\"hide\",\"name\":\"b_geo.ifc\"}", &o);
    call(s, "action", "manage_links", "{\"op\":\"hide\",\"name\":\"c_base.ifc\"}", &o);
    call(s, "action", "manage_links", "{\"op\":\"unpin\",\"name\":\"d_copy.ifc\"}", &o);
    check_true("move the copy up 20 m", call(s, "action", "manage_links", "{\"op\":\"place\",\"name\":\"d_copy.ifc\",\"move_mm\":[0,0,20000]}", &o) == CC_OK);
    const cc_scene *hs = cc_session_scene(s);
    double org[3] = {0, 0, 0};
    if (hs) memcpy(org, hs->origin, sizeof org);
    char body[512];
    check_true("measure on", call(s, "action", "measure", "", &o) == CC_OK);
    snprintf(body, sizeof body, "{\"ray\":[%.17g,%.17g,%.17g,1,0,0],\"ray_tol\":[%.17g,%.17g,%.17g,1,0,0]}", -10150 - org[0], -148 - org[1],
             21000 - org[2], -10150 - org[0], -148 - org[1] + 1, 21000 - org[2]);
    call(s, "tool", "measure_pointer", body, &o);
    int hit = strstr(buf_cstr(&o), "\"at\":[-150.0,") && strstr(buf_cstr(&o), ",21000.0]");
    if (!hit) printf("  %s\n", buf_cstr(&o));
    check_true("measure: the moved link is hit where it was moved (x -150, z 21000)", hit);
    call(s, "action", "manage_links", "{\"op\":\"hide\",\"name\":\"d_copy.ifc\"}", &o);
    call(s, "tool", "measure_pointer", body, &o);
    check_true("measure: hidden, not hit", strstr(buf_cstr(&o), "\"hover\":null") != NULL);
    call(s, "action", "manage_links", "{\"op\":\"show\",\"name\":\"d_copy.ifc\"}", &o);
    call(s, "tool", "measure_cancel", "", &o), call(s, "tool", "measure_cancel", "", &o);

    /* Box to selection around a placed linked element */
    char id[23];
    cc_link_id(&s->links[D], 0, id);
    id[22] = 0;
    snprintf(body, sizeof body, "{\"link\":\"d_copy.ifc\",\"ids\":[\"%s\"]}", id);
    check_true("select a linked element", call(s, "select", "-", body, &o) == CC_OK);
    check_true("box to selection", call(s, "action", "box_to_selection", "", &o) == CC_OK);
    call(s, "query", "visibility", "{}", &o);
    const char *mnp = strstr(buf_cstr(&o), "\"min\":[");
    double zmin = -1e9;
    if (mnp) {
        char *e2;
        strtod(mnp + 7, &e2), strtod(e2 + 1, &e2);
        zmin = strtod(e2 + 1, NULL) + org[2];
    }
    check_true("box to selection: around the element where the link put it (above 19 m)", zmin > 19000);

    /* query element of each of three links */
    int els = 0;
    const char *names[3] = {"a_geo.ifc", "c_base.ifc", "d_copy.ifc"};
    int idx[3] = {A, C, D};
    for (int i = 0; i < 3; i++) {
        cc_link_id(&s->links[idx[i]], 1, id);
        snprintf(body, sizeof body, "{\"id\":\"%.22s\",\"link\":\"%s\"}", id, names[i]);
        els += call(s, "query", "element", body, &o) == CC_OK && strstr(buf_cstr(&o), "\"read_only\":true") != NULL;
    }
    check_true("query element of three links", els == 3);

    /* paper: a plan with the copy moved away differs; moved back, the same bytes */
    call(s, "action", "manage_links", "{\"op\":\"place\",\"name\":\"d_copy.ifc\",\"move_mm\":[0,0,0]}", &o);
    Buf ov, a1, a2, a3;
    buf_init(&ov), buf_init(&a1), buf_init(&a2), buf_init(&a3);
    call(s, "query", "open_views", "", &ov);
    const char *pv = strstr(buf_cstr(&ov), "\"type\":\"plan\"");
    const char *idp = NULL;
    for (const char *q = buf_cstr(&ov); pv && (q = strstr(q, "\"id\":\"")) && q < pv; q += 6) idp = q + 6;
    if (idp) {
        snprintf(body, sizeof body, "{\"view\":\"%.22s\",\"bare\":true}", idp);
        call(s, "query", "view_svg", body, &a1);
        call(s, "action", "manage_links", "{\"op\":\"place\",\"name\":\"d_copy.ifc\",\"move_mm\":[50000,0,0]}", &o);
        call(s, "query", "view_svg", body, &a2);
        call(s, "action", "manage_links", "{\"op\":\"place\",\"name\":\"d_copy.ifc\",\"move_mm\":[0,0,0]}", &o);
        call(s, "query", "view_svg", body, &a3);
    }
    check_true("paper: a moved link moves on the plan", idp && a1.len && a2.len && (a1.len != a2.len || memcmp(a1.p, a2.p, a1.len)));
    check_true("paper: moved back, the same bytes", idp && a1.len == a3.len && !memcmp(a1.p, a3.p, a1.len));
    buf_free(&ov), buf_free(&a1), buf_free(&a2), buf_free(&a3);

    /* unplaced and uncoloured: version 1 again */
    for (int i = 0; i < 4; i++) {
        snprintf(body, sizeof body, "{\"op\":\"place\",\"name\":\"%s\",\"origin\":\"origin\",\"move_mm\":[0,0,0],\"rotation_deg\":0}", s->links[i].name);
        call(s, "action", "manage_links", "{\"op\":\"unpin\",\"name\":\"b_geo.ifc\"}", &o);
        call(s, "action", "manage_links", body, &o);
    }
    check_true("mesh again", cc_mesh_get_form(s, 3, &mp, &mn, &rev, &ms) == 200);
    lk = NULL;
    for (size_t i = 0; mp && i + 8 <= mn; i += 4)
        if (!memcmp(mp + i, "LNKS", 4)) lk = mp + i;
    ver = 0;
    if (lk) memcpy(&ver, lk + 4, 4);
    check_true("LNKS version 1 when nothing is placed", ver == 1);
    call(s, "action", "manage_links", "{\"op\":\"colour\",\"name\":\"d_copy.ifc\",\"colour\":5}", &o);
    cc_mesh_get_form(s, 3, &mp, &mn, &rev, &ms);
    lk = NULL;
    for (size_t i = 0; mp && i + 8 <= mn; i += 4)
        if (!memcmp(mp + i, "LNKS", 4)) lk = mp + i;
    ver = 0;
    if (lk) memcpy(&ver, lk + 4, 4);
    check_true("LNKS version 2 with a colour", ver == 2);

    /* WAVE1B: a linked model with a reference plane (an IfcVirtualElement, no body) and opening
     * ghosts: placed, drawn, measured without a crash; as a .bimble and as its IFC4 export */
    {
        cc_session *r = cc_session_new(cc_platform_native());
        char rb[512];
        snprintf(rb, sizeof rb, "%s/frame.json", gold);
        int ok = r && !cc_session_open(r, rb, &err);
        ok = ok && call(r, "verb", "reference_plane_add",
                        "{\"storey\":\"1jLno3kWj4Hfl6pUGBDd7E\",\"name\":\"RP\",\"origin\":[1000,2000,1500],\"normal\":[0,0,1],"
                        "\"width\":4000,\"height\":2000}", &o) == CC_OK;
        ok = ok && call(r, "action", "save_as", "{\"path\":\"" DIR "/rp.bimble\"}", &o) == CC_OK;
        ok = ok && call(r, "action", "export_ifc4", "{\"path\":\"" DIR "/rp.ifc\"}", &o) == CC_OK;
        check_true("a model with a reference plane, saved and exported", ok);
        if (r) cc_session_free(r);
        for (int w = 0; w < 2; w++) {
            snprintf(body, sizeof body, "{\"path\":\"%s\"}", w ? DIR "/rp.ifc" : DIR "/rp.bimble");
            int lk2 = call(s, "action", "link_model", body, &o) == CC_OK;
            const char *nm = w ? "rp.ifc" : "rp.bimble";
            snprintf(body, sizeof body, "{\"op\":\"unpin\",\"name\":\"%s\"}", nm);
            call(s, "action", "manage_links", body, &o);
            snprintf(body, sizeof body, "{\"op\":\"place\",\"name\":\"%s\",\"rotation_deg\":30,\"move_mm\":[500,0,0]}", nm);
            int pl = call(s, "action", "manage_links", body, &o) == CC_OK;
            int li = find_link(s, nm);
            int drawn = cc_mesh_get_form(s, 3, &mp, &mn, &rev, &ms) == 200 && cc_mesh_get_form(s, 1, &mp, &mn, &rev, &ms) == 200;
            call(s, "query", "links_panel", "{}", &o);
            check_true(w ? "its IFC4 linked, placed, drawn" : "the .bimble linked, placed, drawn",
                       lk2 && pl && li >= 0 && s->links[li].placed && drawn && strstr(buf_cstr(&o), nm));
        }
    }

    buf_free(&o);
    cc_session_free(s);
    for (int i = 0; i < 4; i++) remove(L[i]);
    remove(DIR "/rp.bimble"), remove(DIR "/rp.ifc");
    rmdir(DIR);
    return report("link_place");
}
