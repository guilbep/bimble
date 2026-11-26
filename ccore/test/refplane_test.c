/* Suite refplane (ticket WAVE1B, ADR 0075 decisions 9-11, src/refplane.h, session/refplanes.h):
 * reference planes on the C core, through cc_call as the page and the host use them.
 *   - verb reference_plane_add by origin + normal, by three points; refused on one line; rename,
 *     delete, undo, redo (the element paths);
 *   - query reference_planes: the stored planes (corners hand-computed), the implicit ones (levels,
 *     grid axes), the active one;
 *   - the work plane: a horizontal plane raises what the place tool makes (column at 1500 mm),
 *     a vertical one moves the point onto its trace; Measure ends a ray that meets nothing on it;
 *   - the three-point tool (action reference_planes): three measure clicks make one plane;
 *   - the file: frame.json saved after a plane was added and deleted keeps its bytes; a plane
 *     reads back equal; hostile planes refused on load (generator, openings, sizes, axes, count);
 *   - IFC: IfcVirtualElement 'REFERENCE_PLANE' with its outline, no material; imported back as a
 *     plane with the same frame and sizes.
 * Hand-computed expectations (Rust v0.19.0 has no reference plane: DEVIATIONS 32). */
#include <stdlib.h>
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "check.h"
#include "file.h"
#include "ifc_in.h"
#include "ifc_out.h"
#include "refplane.h"

static char *ask(cc_session *s, const char *op, const char *name, const char *body, int *st) {
    Buf b;
    buf_init(&b);
    *st = cc_call(s, op, name, body, strlen(body), &b);
    const char *t = buf_cstr(&b);
    char *z = malloc(strlen(t) + 1);
    strcpy(z, t);
    buf_free(&b);
    return z;
}
static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }
static size_t count_class(const cc_model *m) {
    size_t n = 0;
    for (size_t i = 0; i < m->nelements; i++) n += m->elements[i].cls == CC_IFC_VIRTUAL_ELEMENT;
    return n;
}
static const cc_element *plane_named(const cc_model *m, const char *name) {
    for (size_t i = 0; i < m->nelements; i++)
        if (m->elements[i].cls == CC_IFC_VIRTUAL_ELEMENT && m->elements[i].name.n == strlen(name) &&
            !memcmp(m->elements[i].name.p, name, m->elements[i].name.n))
            return &m->elements[i];
    return NULL;
}
static int near(double a, double b) { return a - b < 1e-6 && b - a < 1e-6; }

#define HEAD                                                                                                           \
    "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"1ECFGZP4n20ADaVDzRQc6J\",\"project_name\":\"x\","  \
    "\"storeys\":{\"0aaaaaaaaaaaaaaaaaaaaa\":{\"id\":\"0aaaaaaaaaaaaaaaaaaaaa\",\"name\":\"L0\",\"elevation\":0.0}},"     \
    "\"materials\":{\"0bbbbbbbbbbbbbbbbbbbbb\":{\"id\":\"0bbbbbbbbbbbbbbbbbbbbb\",\"name\":\"C\",\"category\":\"concrete\"}},"
#define PLANE(id, extra, props)                                                                                         \
    "\"" id "\":{\"id\":\"" id "\",\"class\":\"IfcVirtualElement\",\"name\":\"RP\",\"storey\":\"0aaaaaaaaaaaaaaaaaaaaa\"," \
    "\"material\":\"0bbbbbbbbbbbbbbbbbbbbb\",\"placement\":{\"origin\":[0.0,0.0,0.0],\"z_axis\":[0.0,0.0,1.0],"           \
    "\"x_axis\":[1.0,0.0,0.0]}" extra ",\"properties\":{" props "}}"

static int loads(const char *text) {
    cc_model m;
    cc_error e;
    int rc = cc_model_load(&m, text, strlen(text), &e);
    if (rc == 0) cc_model_free(&m);
    return rc == 0;
}

int main(void) {
    const cc_platform *pf = cc_platform_native();
    cc_session *s = cc_session_new(pf);
    int st;
    char *q = ask(s, "query", "reference_planes", "", &st);
    check_true("no model: reference_planes null", st == CC_OK && !strcmp(q, "null"));
    free(q);
    cc_error err;
    if (cc_session_open(s, "golden/frame.json", &err)) {
        printf("open: %s\n", err.msg);
        return 1;
    }
    memcpy(s->storey.c, "1uAZA0Yfn05f$xRd1k4uPR", 22), s->has_storey = 1; /* Niveau 1, 0 mm */
    Buf before;
    buf_init(&before);
    cc_model_save(&s->model, &before);

    q = ask(s, "query", "reference_planes", "", &st);
    check_true("frame.json: no plane, none active, the three levels implicit",
               st == CC_OK && has(q, "\"active\":null") && has(q, "\"planes\":[]") && has(q, "\"name\":\"Fondations\"") &&
                   has(q, "\"kind\":\"level\"") && has(q, "\"pick\":{\"on\":false,\"points\":[]}"));
    free(q);

    /* ---- the verb: a horizontal plane at 1500 mm, 4000 x 2000 ---- */
    uint64_t r0 = s->rev;
    q = ask(s, "verb", "reference_plane_add",
            "{\"storey\":\"1jLno3kWj4Hfl6pUGBDd7E\",\"name\":\"Top of sill\",\"origin\":[1000,2000,1500],\"normal\":[0,0,2],"
            "\"width\":4000,\"height\":2000}",
            &st);
    check_true("reference_plane_add origin + normal -> ok, one step", st == CC_OK && s->rev == r0 + 1);
    free(q);
    const cc_element *p = plane_named(&s->model, "Top of sill");
    /* stored in the storey's frame (Niveau 2 at 3000): z = 1500 - 3000; normal made unit */
    check_true("stored: IfcVirtualElement, no generator, z in the storey's frame, unit normal",
               p && p->generator.kind == CC_GEN_NONE && near(p->placement.origin[2], -1500.0) && near(p->placement.z_axis[2], 1.0) &&
                   near(p->placement.x_axis[0], 1.0));
    q = ask(s, "query", "reference_planes", "", &st);
    /* corners: origin +- 2000 along x, +- 1000 along y = z x x = (0, 1, 0) */
    check_true("query: corners (-1000,1000,1500) (3000,1000,1500) (3000,3000,1500) (-1000,3000,1500)",
               has(q, "\"corners\":[[-1000.0,1000.0,1500.0],[3000.0,1000.0,1500.0],[3000.0,3000.0,1500.0],[-1000.0,3000.0,1500.0]]") &&
                   has(q, "\"height\":2000.0") && has(q, "\"width\":4000.0") && has(q, "\"origin\":[1000.0,2000.0,1500.0]"));
    free(q);

    /* ---- three points: (0,0,0) (1000,0,0) (0,0,1000): normal (1,0,0) x (0,0,1) = (0,-1,0), x (1,0,0) ---- */
    q = ask(s, "verb", "reference_plane_add",
            "{\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"points\":[[0,0,0],[1000,0,0],[0,0,1000]]}", &st);
    check_true("reference_plane_add three points -> ok, named RP 1", st == CC_OK);
    free(q);
    const cc_element *v = plane_named(&s->model, "RP 1");
    check_true("three points: normal (0,-1,0), x (1,0,0), origin (0,0,0)",
               v && near(v->placement.z_axis[1], -1.0) && near(v->placement.z_axis[0], 0.0) && near(v->placement.x_axis[0], 1.0) &&
                   near(v->placement.origin[0], 0.0));
    q = ask(s, "verb", "reference_plane_add", "{\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"points\":[[0,0,0],[1000,0,0],[3000,0,0]]}", &st);
    check_true("three points on one line -> refused, said why", st == CC_ERR && has(q, "on one line"));
    free(q);
    q = ask(s, "verb", "reference_plane_add", "{\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"origin\":[0,0,0],\"normal\":[0,0,0]}", &st);
    check_true("a zero normal -> refused", st == CC_ERR && has(q, "normal"));
    free(q);
    q = ask(s, "verb", "reference_plane_add", "{\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"origin\":[0,0,1e300],\"normal\":[0,0,1]}", &st);
    check_true("an origin out of range -> refused", st == CC_ERR);
    free(q);
    q = ask(s, "verb", "reference_plane_add",
            "{\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"name\":\"Top of sill\",\"origin\":[0,0,0],\"normal\":[0,0,1]}", &st);
    check_true("a second plane of the same name -> refused", st == CC_ERR && has(q, "name already"));
    free(q);

    /* ---- rename, the size property, delete, undo ---- */
    char body[256];
    snprintf(body, sizeof body, "{\"id\":\"%.22s\",\"name\":\"Grid face\"}", v->id.c);
    cc_gid vid = v->id;
    free(ask(s, "verb", "rename", body, &st));
    check_true("rename -> ok", st == CC_OK && plane_named(&s->model, "Grid face"));
    snprintf(body, sizeof body, "[{\"op\":\"set_property\",\"id\":\"%.22s\",\"key\":\"Bimble/PlaneWidth\",\"value\":\"abc\"}]", vid.c);
    q = ask(s, "apply", "", body, &st);
    check_true("set_property PlaneWidth \"abc\" on a plane -> refused (the file would not load)", st == CC_ERR && has(q, "width"));
    if (st != CC_ERR || !has(q, "width")) printf("  got: %s\n", q);
    free(q);
    snprintf(body, sizeof body, "{\"ids\":[\"%.22s\"]}", vid.c);
    free(ask(s, "verb", "delete", body, &st));
    check_true("delete -> gone", st == CC_OK && count_class(&s->model) == 1);
    free(ask(s, "undo", "", "", &st));
    check_true("undo -> back, same GlobalId", st == CC_OK && cc_model_element(&s->model, &vid));

    /* ---- the work plane: the horizontal plane raises a column placed on Niveau 1 ---- */
    p = plane_named(&s->model, "Top of sill");
    snprintf(body, sizeof body, "{\"kind\":\"plane\",\"id\":\"%.22s\"}", p->id.c);
    q = ask(s, "tool", "work_plane", body, &st);
    check_true("work_plane plane -> active", st == CC_OK && has(q, "\"active\":{\"id\":") && has(q, "\"name\":\"Top of sill\""));
    free(q);
    memcpy(s->storey.c, "1uAZA0Yfn05f$xRd1k4uPR", 22), s->has_storey = 1;
    free(ask(s, "action", "place_column", "", &st));
    size_t ne = s->model.nelements;
    q = ask(s, "tool", "click", "{\"at\":[50000,50000],\"free\":true}", &st);
    check_true("a column clicked with the work plane at 1500 -> placed", st == CC_OK && s->model.nelements == ne + 1);
    free(q);
    const cc_element *col = NULL;
    for (size_t i = 0; i < s->model.nelements; i++)
        if (near(s->model.elements[i].placement.origin[0], 50000.0) && near(s->model.elements[i].placement.origin[1], 50000.0))
            col = &s->model.elements[i];
    check_true("its base at 1500 mm (storey 0 + 1500)", col && col->cls == CC_IFC_COLUMN && near(col->placement.origin[2], 1500.0));
    free(ask(s, "undo", "", "", &st));
    /* a vertical plane (the grid face, y = 0): the point moves onto its trace */
    snprintf(body, sizeof body, "{\"kind\":\"plane\",\"id\":\"%.22s\"}", vid.c);
    free(ask(s, "tool", "work_plane", body, &st));
    q = ask(s, "tool", "pointer", "{\"at\":[52000,700],\"free\":true}", &st);
    check_true("vertical work plane: the hover moved onto y = 0", st == CC_OK && has(q, "\"at\":[52000.0,0.0]"));
    free(q);
    free(ask(s, "tool", "cancel", "", &st));
    /* a level: Niveau 2 (3000) */
    q = ask(s, "tool", "work_plane", "{\"kind\":\"level\",\"id\":\"1jLno3kWj4Hfl6pUGBDd7E\"}", &st);
    check_true("work_plane level -> active", st == CC_OK && has(q, "\"kind\":\"level\",\"name\":\"Niveau 2\""));
    free(q);
    q = ask(s, "tool", "work_plane", "{\"kind\":\"plane\",\"id\":\"1jLno3kWj4Hfl6pUGBDd7E\"}", &st);
    check_true("work_plane plane with a level's id -> refused", st == CC_ERR);
    free(q);

    /* ---- Measure: a ray that meets nothing ends on the work plane (Niveau 2, z 3000) ---- */
    const cc_scene *sc = cc_session_scene(s);
    double o[3] = {sc->origin[0], sc->origin[1], sc->origin[2]};
    free(ask(s, "action", "measure", "", &st));
    snprintf(body, sizeof body, "{\"ray\":[%.17g,%.17g,%.17g,0,0,-1]}", 90000.0 - o[0], 90000.0 - o[1], 20000.0 - o[2]);
    q = ask(s, "tool", "measure_pointer", body, &st);
    check_true("measure: the hover on the work plane at (90000, 90000, 3000)", st == CC_OK && has(q, "\"hover\":{") && has(q, "3000"));
    free(q);
    free(ask(s, "action", "measure", "", &st));

    /* ---- the three-point tool: on the work plane Niveau 2 ---- */
    free(ask(s, "action", "reference_planes", "", &st));
    q = ask(s, "query", "reference_planes", "", &st);
    check_true("action reference_planes -> the pick tool on", has(q, "\"pick\":{\"on\":true"));
    free(q);
    size_t np = count_class(&s->model);
    uint64_t r1 = s->rev;
    double P[3][2] = {{90000, 90000}, {94000, 90000}, {90000, 93000}};
    for (int i = 0; i < 3; i++) {
        snprintf(body, sizeof body, "{\"ray\":[%.17g,%.17g,%.17g,0,0,-1]}", P[i][0] - o[0], P[i][1] - o[1], 20000.0 - o[2]);
        q = ask(s, "tool", "measure_click", body, &st);
        if (st != CC_OK) printf("click %d: %s\n", i, q);
        free(q);
    }
    check_true("three clicks -> one plane, one step", count_class(&s->model) == np + 1 && s->rev == r1 + 1);
    const cc_element *t3 = plane_named(&s->model, "RP 1");
    check_true("its normal (0,0,1) (the points at z 3000)", t3 && near(t3->placement.z_axis[2], 1.0));
    free(ask(s, "action", "reference_planes", "", &st));
    q = ask(s, "query", "actions", "", &st);
    check_true("the tool off: measure not pressed", st == CC_OK);
    free(q);
    free(ask(s, "tool", "work_plane", "{\"kind\":null}", &st));

    /* ---- save and read back ---- */
    Buf saved;
    buf_init(&saved);
    cc_model_save(&s->model, &saved);
    check_true("the file names IfcVirtualElement, no generator key for it", has(buf_cstr(&saved), "\"IfcVirtualElement\""));
    cc_model m2;
    check_true("it reads back", cc_model_load(&m2, saved.p, saved.len, &err) == 0 && count_class(&m2) == count_class(&s->model));
    Buf again;
    buf_init(&again);
    cc_model_save(&m2, &again);
    check_eq("saved again: the same bytes", again.p, again.len, saved.p, saved.len);
    buf_free(&again);
    cc_model_free(&m2);

    /* ---- IFC: export, import back ---- */
    Buf ifc;
    buf_init(&ifc);
    check_true("IFC4 export ok", cc_ifc_export(&s->model, "x.ifc", "2026-11-26T10:00:00", CC_IFC4, &ifc) == 0);
    check_true("IfcVirtualElement 'REFERENCE_PLANE' with a Curve3D outline",
               has(buf_cstr(&ifc), "IFCVIRTUALELEMENT(") && has(buf_cstr(&ifc), "'REFERENCE_PLANE'") && has(buf_cstr(&ifc), "'Curve3D'"));
    { /* WAVE1B_IFC=<prefix>: the two exports written for IfcOpenShell (scripts/validate_ifc.py) */
        const char *dump = getenv("WAVE1B_IFC");
        Buf x3;
        buf_init(&x3);
        check_true("IFC2x3 export ok, the plane in it",
                   cc_ifc_export(&s->model, "x.ifc", "2026-11-26T10:00:00", CC_IFC2X3, &x3) == 0 && has(buf_cstr(&x3), "IFCVIRTUALELEMENT("));
        for (int k = 0; dump && k < 2; k++) {
            char path[512];
            snprintf(path, sizeof path, "%s%s", dump, k ? "-ifc2x3.ifc" : "-ifc4.ifc");
            FILE *f = fopen(path, "wb");
            if (f) fwrite(k ? x3.p : ifc.p, 1, k ? x3.len : ifc.len, f), fclose(f);
        }
        buf_free(&x3);
    }
    cc_model im;
    cc_ifc_report rep;
    int irc = cc_ifc_import(&im, &rep, ifc.p, ifc.len, &err);
    check_true("imported back", irc == 0);
    if (irc == 0) {
        const cc_element *a = plane_named(&s->model, "Top of sill"), *b = plane_named(&im, "Top of sill");
        double aw, ah, bw, bh;
        if (a && b) cc_refplane_size(a, &aw, &ah), cc_refplane_size(b, &bw, &bh);
        check_true("the plane comes back: same GlobalId, frame and sizes",
                   a && b && !cc_gid_cmp(&a->id, &b->id) && near(b->placement.origin[0], a->placement.origin[0]) &&
                       near(b->placement.origin[2], a->placement.origin[2]) && near(b->placement.z_axis[2], 1.0) && near(bw, 4000.0) &&
                       near(bh, 2000.0) && b->generator.kind == CC_GEN_NONE);
        check_true("as many planes", count_class(&im) == count_class(&s->model));
        cc_model_free(&im);
        cc_ifc_report_free(&rep);
    }
    buf_free(&ifc);

    /* ---- frame.json keeps its bytes: planes deleted, saved again ---- */
    while (count_class(&s->model)) {
        for (size_t i = 0; i < s->model.nelements; i++)
            if (s->model.elements[i].cls == CC_IFC_VIRTUAL_ELEMENT) {
                snprintf(body, sizeof body, "{\"ids\":[\"%.22s\"]}", s->model.elements[i].id.c);
                break;
            }
        free(ask(s, "verb", "delete", body, &st));
        if (st != CC_OK) break;
    }
    Buf after;
    buf_init(&after);
    cc_model_save(&s->model, &after);
    check_eq("frame.json without planes: the same bytes as before", after.p, after.len, before.p, before.len);
    buf_free(&after), buf_free(&before), buf_free(&saved);

    /* ---- hostile files ---- */
    check_true("a plane: loads", loads(HEAD "\"elements\":{" PLANE("0ccccccccccccccccccccc", "", "\"Bimble/PlaneWidth\":\"500\"") "}}}"));
    check_true("a plane with a generator: refused",
               !loads(HEAD "\"elements\":{" PLANE("0ccccccccccccccccccccc", ",\"generator\":{\"rect_column\":{\"width\":1.0,\"depth\":1.0,\"height\":1.0}}", "") "}}}"));
    check_true("a width \"abc\": refused", !loads(HEAD "\"elements\":{" PLANE("0ccccccccccccccccccccc", "", "\"Bimble/PlaneWidth\":\"abc\"") "}}}"));
    check_true("a width 1e9: refused", !loads(HEAD "\"elements\":{" PLANE("0ccccccccccccccccccccc", "", "\"Bimble/PlaneHeight\":\"1e9\"") "}}}"));
    check_true("a width \"nan\": refused", !loads(HEAD "\"elements\":{" PLANE("0ccccccccccccccccccccc", "", "\"Bimble/PlaneHeight\":\"nan\"") "}}}"));
    check_true("an opening: refused",
               !loads(HEAD "\"elements\":{" PLANE("0ccccccccccccccccccccc",
                                                  ",\"openings\":[{\"placement\":{\"origin\":[0.0,0.0,0.0],\"z_axis\":[0.0,0.0,1.0],"
                                                  "\"x_axis\":[1.0,0.0,0.0]},\"generator\":{\"rect_column\":{\"width\":1.0,\"depth\":1.0,\"height\":1.0}}}]",
                                                  "") "}}}"));
    {
        const char *bad = HEAD "\"elements\":{\"0ccccccccccccccccccccc\":{\"id\":\"0ccccccccccccccccccccc\",\"class\":\"IfcVirtualElement\","
                               "\"name\":\"RP\",\"storey\":\"0aaaaaaaaaaaaaaaaaaaaa\",\"material\":\"0bbbbbbbbbbbbbbbbbbbbb\","
                               "\"placement\":{\"origin\":[0.0,0.0,0.0],\"z_axis\":[0.0,0.0,2.0],\"x_axis\":[1.0,0.0,0.0]}}}}}";
        check_true("a normal not of unit length: refused", !loads(bad));
    }
    { /* 1001 planes: over the budget */
        Buf b;
        buf_init(&b);
        buf_puts(&b, HEAD "\"elements\":{");
        for (int i = 0; i < 1001; i++) {
            char id[23];
            snprintf(id, sizeof id, "0c%020d", i);
            if (i) buf_putc(&b, ',');
            buf_puts(&b, "\"");
            buf_puts(&b, id);
            buf_puts(&b, "\":{\"id\":\"");
            buf_puts(&b, id);
            buf_puts(&b, "\",\"class\":\"IfcVirtualElement\",\"name\":\"RP\",\"storey\":\"0aaaaaaaaaaaaaaaaaaaaa\",\"material\":"
                         "\"0bbbbbbbbbbbbbbbbbbbbb\",\"placement\":{\"origin\":[0.0,0.0,0.0],\"z_axis\":[0.0,0.0,1.0],\"x_axis\":[1.0,0.0,0.0]}}");
        }
        buf_puts(&b, "}}}");
        check_true("1001 planes: refused (1000 at most)", !loads(buf_cstr(&b)));
        buf_free(&b);
    }
    cc_session_free(s);
    return report("refplane");
}
