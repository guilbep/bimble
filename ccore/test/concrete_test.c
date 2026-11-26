/* Suite concrete (ticket CONC1, ADR 0077, DEVIATIONS 34): the Concrete tab's footing, strip
 * footing, raft, opening and sleeve on the C core, through cc_call as the page and the host use
 * them (the place tool's clicks and the verbs), on the 72-element fixture.
 * Rust v0.19.0 has none of these tools: every expected value is hand-computed here.
 *   - pad footing: one click on a column (centred under it, top at its base), one in the open;
 *     1200 x 1200 x 500 = 0.72 m3; undo removes it in one step;
 *   - strip footing: one click on a wall (6000 long): along it, 600 x 400 -> 1.44 m3,
 *     property Bimble/PredefinedType STRIP_FOOTING;
 *   - opening: two clicks on the wall (6000 x 200 x 3000 = 3.6 m3): 1000 x 1200, sill 900 ->
 *     3.6 - 0.24 = 3.36 m3; refused over another opening and outside the wall; moved by
 *     opening_set (GlobalId kept); follows the wall when it moves; deleted with the wall (one undo
 *     brings both back); a slab opening 600 x 400 through 220 mm -> -0.0528 m3;
 *   - sleeve: diameter 200 through the 220 mm slab: the 32-gon's volume 16 r^2 sin(2 pi / 32) t;
 *   - raft: three clicks and Enter: an IfcSlab on the lowest storey, top at its elevation,
 *     BASESLAB, 0.5 * 6 * 4 * 0.4 = 4.8 m3;
 *   - IFC4: IfcFooting STRIP_FOOTING, IfcSlab BASESLAB, IfcOpeningElement + IfcRelVoidsElement
 *     (the sleeve SLEEVE / USERDEFINED); save and reopen give the same bytes;
 *   - set_openings: written and read back as the session journal does; hostile values refused.
 * On the code before CONC1 every verb answers "unknown verb" and set_placing refuses the kinds. */
#include <stdlib.h>
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "check.h"
#include "file.h"
#include "ifc_out.h"
#include "scene.h"
#include "boolean.h" /* WAVE1B (B1) */
#include "ifc_in.h"  /* WAVE1B (B3) */

#define N1 "1uAZA0Yfn05f$xRd1k4uPR" /* Niveau 1, elevation 0 */
#define FONDATIONS "04Aobkc8v6$94cGl_9HNVX"
#define SLAB "1khkLES9P8rPMAGC5wqOCk" /* D2: 18300 x 10300, 220 thick */

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
static int call_ok(cc_session *s, const char *op, const char *name, const char *body) {
    int st;
    char *r = ask(s, op, name, body, &st);
    if (st != CC_OK) printf("  (%s %s: %s)\n", op, name, r);
    free(r);
    return st == CC_OK;
}
static char *call_err(cc_session *s, const char *op, const char *name, const char *body) {
    int st;
    char *r = ask(s, op, name, body, &st);
    if (st == CC_OK) {
        free(r);
        return NULL;
    }
    return r;
}
static int has(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }
static int near(double a, double b, double tol) { return (a > b ? a - b : b - a) <= tol; }

/* The element's volume in m3, from its mesh in the scene (divergence theorem, mm3 / 1e9). */
static double volume(const cc_model *m, const cc_gid *id) {
    cc_scene sc;
    cc_error e;
    if (cc_scene_build(&sc, m, &e)) return -1;
    double v = -1;
    for (size_t k = 0; k < sc.nelements; k++) {
        if (cc_gid_cmp(&sc.elements[k].id, id)) continue;
        Arena a;
        arena_init(&a, 0);
        cc_mesh me;
        if (!cc_scene_world_mesh(&sc, k, &a, &me)) {
            double s6 = 0;
            for (uint32_t t = 0; t + 2 < me.ni; t += 3) {
                const float *p = me.pos[me.idx[t]], *q = me.pos[me.idx[t + 1]], *r = me.pos[me.idx[t + 2]];
                double c[3] = {(double)q[1] * r[2] - (double)q[2] * r[1], (double)q[2] * r[0] - (double)q[0] * r[2],
                               (double)q[0] * r[1] - (double)q[1] * r[0]};
                s6 += p[0] * c[0] + p[1] * c[1] + p[2] * c[2];
            }
            v = s6 / 6.0 / 1e9;
        }
        arena_free(&a);
    }
    cc_scene_free(&sc);
    return v;
}

static const cc_element *newest(const cc_session *s, const char *name) {
    for (size_t i = 0; i < s->model.nelements; i++)
        if (s->model.elements[i].name.n == strlen(name) && !memcmp(s->model.elements[i].name.p, name, strlen(name)))
            return &s->model.elements[i];
    return NULL;
}
static cc_gid gid_of(const char *t) {
    cc_gid g;
    memcpy(g.c, t, 22);
    return g;
}
static const char *prop(const cc_element *e) {
    static char buf[64];
    const cc_str *v = cc_element_predefined(e);
    if (!v) return "";
    snprintf(buf, sizeof buf, "%.*s", (int)v->n, v->p);
    return buf;
}

static size_t n_el(const cc_session *s) { return s->model.nelements; }

int main(void) {
    const cc_platform *pf = cc_platform_native();
    cc_session *s = cc_session_new(pf);
    cc_error err;
    if (cc_session_open(s, "golden/frame.json", &err)) {
        printf("open: %s\n", err.msg);
        return 1;
    }
    Buf before;
    buf_init(&before);
    cc_model_save(&s->model, &before);
    size_t n0 = n_el(s);
    int st;
    char *r;

    /* ---- the actions ---- */
    r = ask(s, "query", "actions", "", &st);
    check_true("actions: place_footing, place_strip_footing, place_raft, place_opening, place_sleeve",
               has(r, "\"name\":\"place_footing\"") && has(r, "\"name\":\"place_strip_footing\"") &&
                   has(r, "\"name\":\"place_raft\"") && has(r, "\"name\":\"place_opening\"") && has(r, "\"name\":\"place_sleeve\""));
    free(r);

    /* ---- pad footing under a column (P1.00 at 0, 0 on Niveau 1) ---- */
    check_true("set_placing footing", call_ok(s, "set_placing", "", "{\"kind\":\"footing\",\"storey\":\"" N1 "\"}"));
    r = ask(s, "query", "placing", "", &st);
    check_true("placing: concrete sizes while a concrete kind is chosen",
               has(r, "\"clicks_needed\":1") && has(r, "\"concrete\":{\"footing_depth\":500.0,\"footing_length\":1200.0") &&
                   has(r, "\"kind\":\"footing\""));
    free(r);
    uint64_t rev0 = s->rev;
    check_true("pad: one click on the column", call_ok(s, "tool", "click", "{\"at\":[40,30],\"free\":true}"));
    const cc_element *f1 = newest(s, "F1");
    check_true("pad: F1 added, one step", f1 && n_el(s) == n0 + 1 && s->rev > rev0);
    if (f1) {
        check_true("pad: IfcFooting, pad_footing 1200 x 1200 x 500", f1->cls == CC_IFC_FOOTING && f1->generator.kind == CC_GEN_PAD_FOOTING &&
                                                                           f1->generator.u.col.width == 1200 && f1->generator.u.col.depth == 1200 &&
                                                                           f1->generator.u.col.height == 500);
        check_true("pad: centred under the column, top at its base", f1->placement.origin[0] == 0 && f1->placement.origin[1] == 0 &&
                                                                         f1->placement.origin[2] == -500);
        check_true("pad: no PredefinedType property (PAD_FOOTING is the class default)", !cc_element_predefined(f1));
        check_true("pad: 0.72 m3", near(volume(&s->model, &f1->id), 0.72, 1e-6));
    }
    check_true("undo: the pad goes in one step", call_ok(s, "undo", "", "") && n_el(s) == n0 && !newest(s, "F1"));
    check_true("pad: a free click", call_ok(s, "tool", "click", "{\"at\":[3000,3000],\"free\":true}"));
    f1 = newest(s, "F1");
    check_true("pad: free at the level, top at its elevation", f1 && f1->placement.origin[0] == 3000 && f1->placement.origin[2] == -500);

    /* ---- a wall, then the strip footing under it ---- */
    check_true("wall placed", call_ok(s, "verb", "place", "{\"kind\":\"wall\",\"storey\":\"" N1 "\",\"a\":[0,-2000],\"b\":[6000,-2000]}"));
    const cc_element *w = NULL;
    for (size_t i = 0; i < s->model.nelements; i++)
        if (s->model.elements[i].cls == CC_IFC_WALL) w = &s->model.elements[i];
    cc_gid wid = w ? w->id : gid_of("0000000000000000000000");
    check_true("wall: 6000 x 200 x 3000 = 3.6 m3", w && near(volume(&s->model, &wid), 3.6, 1e-6));
    check_true("set_placing strip_footing", call_ok(s, "set_placing", "", "{\"kind\":\"strip_footing\"}"));
    size_t n1 = n_el(s);
    check_true("strip: one click on the wall", call_ok(s, "tool", "click", "{\"at\":[3000,-1950],\"free\":true}"));
    const cc_element *f2 = newest(s, "F2");
    check_true("strip: placed at the first click", f2 && n_el(s) == n1 + 1);
    if (f2) {
        check_true("strip: along the wall, 6000 x 600 x 400, top at its base",
                   f2->cls == CC_IFC_FOOTING && f2->generator.kind == CC_GEN_WALL && f2->generator.u.wall.length == 6000 &&
                       f2->generator.u.wall.thickness == 600 && f2->generator.u.wall.height == 400 && f2->placement.origin[0] == 0 &&
                       f2->placement.origin[1] == -2000 && f2->placement.origin[2] == -400);
        check_true("strip: STRIP_FOOTING", !strcmp(prop(f2), "STRIP_FOOTING"));
        check_true("strip: 1.44 m3", near(volume(&s->model, &f2->id), 1.44, 1e-6));
    }
    check_true("strip: two clicks in the open", call_ok(s, "tool", "click", "{\"at\":[0,-6000],\"free\":true}") &&
                                                    call_ok(s, "tool", "click", "{\"at\":[4000,-6000],\"free\":true}") && newest(s, "F3") &&
                                                    newest(s, "F3")->generator.u.wall.length == 4000);

    /* ---- the opening in the wall ---- */
    check_true("set_placing opening", call_ok(s, "set_placing", "", "{\"kind\":\"opening\"}"));
    check_true("opening: first click", call_ok(s, "tool", "click", "{\"at\":[1000,-2000],\"free\":true}"));
    rev0 = s->rev;
    check_true("opening: second click", call_ok(s, "tool", "click", "{\"at\":[2000,-2000],\"free\":true}"));
    w = cc_model_element(&s->model, &wid);
    check_true("opening: one in the wall, its own GlobalId, one step", w && w->nopenings == 1 && w->openings[0].has_id && s->rev == rev0 + 1);
    cc_gid oid = w && w->nopenings ? w->openings[0].id : wid;
    check_true("opening: 1000 x 1200, sill 900, centred at 1500",
               w && w->nopenings && w->openings[0].generator.kind == CC_GEN_RECT_COLUMN && w->openings[0].generator.u.col.width == 1000 &&
                   w->openings[0].generator.u.col.height == 1200 && w->openings[0].placement.origin[0] == 1500 &&
                   w->openings[0].placement.origin[2] == 900);
    check_true("opening: the wall is 3.6 - 0.24 = 3.36 m3", near(volume(&s->model, &wid), 3.36, 1e-6));
    char body[512], ws[23], os[23];
    memcpy(ws, wid.c, 22), ws[22] = 0, memcpy(os, oid.c, 22), os[22] = 0;
    { /* ---- WAVE1B (B2): the opening listed, picked by a ray, previewed while placing ---- */
        const cc_element *wl = cc_model_element(&s->model, &wid);
        const cc_storey *wst = wl ? cc_model_storey(&s->model, &wl->storey) : NULL;
        double z0 = (wl ? wl->placement.origin[2] : 0) + (wst ? wst->elevation : 0); /* the wall's base, world */
        snprintf(body, sizeof body, "{\"host\":\"%s\"}", ws);
        r = ask(s, "query", "openings", body, &st);
        char want[160];
        snprintf(want, sizeof want, "\"centre\":[1500.0,-2000.0,%.1f]", z0 + 900 + 600); /* sill 900 + half of 1200 */
        check_true("B2 query openings: 1000 x 1200, sill 900, centre (1500, -2000, base + 1500), its GlobalId",
                   st == CC_OK && has(r, "\"kind\":\"opening\"") && has(r, "\"width\":1000.0") && has(r, "\"height\":1200.0") &&
                       has(r, "\"sill\":900.0") && has(r, want) && has(r, os) && has(r, "\"wall\":true"));
        if (!has(r, want)) printf("  got %s (want %s)\n", r, want);
        free(r);
        const cc_scene *sc = cc_session_scene(s);
        double o[3] = {sc ? sc->origin[0] : 0, sc ? sc->origin[1] : 0, sc ? sc->origin[2] : 0};
        /* a ray along +y through the hole (x 1500, at its middle height): the opening */
        snprintf(body, sizeof body, "{\"ray\":[%.17g,%.17g,%.17g,0,1,0]}", 1500 - o[0], -9000 - o[1], z0 + 1500 - o[2]);
        r = ask(s, "query", "opening_at", body, &st);
        check_true("B2 opening_at: a ray through the hole -> the wall and the opening", st == CC_OK && has(r, ws) && has(r, os));
        free(r);
        /* beside it (x 500): the wall's face, no opening */
        snprintf(body, sizeof body, "{\"ray\":[%.17g,%.17g,%.17g,0,1,0]}", 500 - o[0], -9000 - o[1], z0 + 1500 - o[2]);
        r = ask(s, "query", "opening_at", body, &st);
        check_true("B2 opening_at: a ray on the wall beside it -> null", st == CC_OK && !strcmp(r, "null"));
        free(r);
        /* the ghost of the next opening while placing (CONC1 drew none) */
        check_true("B2 set_placing opening", call_ok(s, "set_placing", "", "{\"kind\":\"opening\"}"));
        free(ask(s, "tool", "click", "{\"at\":[3000,-2000],\"free\":true}", &st));
        r = ask(s, "tool", "pointer", "{\"at\":[3600,-2000],\"free\":true}", &st);
        check_true("B2 opening preview: the tool answers a ghost (the void's solid)", st == CC_OK && has(r, "\"ghost\":{\"edges\":["));
        free(r);
        free(ask(s, "tool", "cancel", "", &st));
        check_true("B2 opening preview: nothing added", cc_model_element(&s->model, &wid)->nopenings == 1);
    }
    snprintf(body, sizeof body, "{\"host\":\"%s\",\"a\":[1600,-2000],\"width\":400}", ws);
    r = call_err(s, "verb", "opening_add", body);
    check_true("opening: refused over another", has(r, "overlaps another opening"));
    free(r);
    snprintf(body, sizeof body, "{\"host\":\"%s\",\"a\":[5800,-2000],\"width\":1000}", ws);
    r = call_err(s, "verb", "opening_add", body);
    check_true("opening: refused outside the wall", has(r, "leaves its host"));
    free(r);
    snprintf(body, sizeof body, "{\"host\":\"%s\",\"a\":[3000,-2000],\"height\":2900,\"sill\":200}", ws);
    r = call_err(s, "verb", "opening_add", body);
    check_true("opening: refused above the wall's top", has(r, "leaves its host"));
    free(r);
    snprintf(body, sizeof body, "{\"host\":\"%s\",\"id\":\"%s\",\"a\":[4000,-2000],\"width\":800}", ws, os);
    check_true("opening_set: moved and resized", call_ok(s, "verb", "opening_set", body));
    w = cc_model_element(&s->model, &wid);
    check_true("opening_set: the same GlobalId at 4000, 800 wide", w && w->nopenings == 1 && !cc_gid_cmp(&w->openings[0].id, &oid) &&
                                                                       w->openings[0].placement.origin[0] == 4000 &&
                                                                       w->openings[0].generator.u.col.width == 800);
    check_true("opening_set: 3.6 - 0.8 * 1.2 * 0.2 = 3.408 m3", near(volume(&s->model, &wid), 3.408, 1e-6));
    { /* ---- WAVE1B (B1): a sleeve in the wall: cut (with or without the kernel), counted as the exact cylinder ---- */
        printf("  (boolean kernel: %s)\n", cc_boolean_kernel() ? "yes" : "no: the prism cases");
        snprintf(body, sizeof body, "{\"host\":\"%s\",\"a\":[2000,-2000],\"shape\":\"round\",\"diameter\":200}", ws);
        check_true("B1 a sleeve of 200 in the wall", call_ok(s, "verb", "opening_add", body));
        double ngon = 16.0 * 0.1 * 0.1 * 0.19509032201612825 * 0.2; /* the 32-gon through 200 mm, m3 */
        double cyl = 3.141592653589793 * 0.1 * 0.1 * 0.2;          /* pi r^2 t = 0.00628319 m3 */
        check_true("B1 the wall's mesh is cut by the 32-gon: 3.408 - 0.0062429 m3", near(volume(&s->model, &wid), 3.408 - ngon, 1e-6));
        double fix = cc_sleeve_volume_fix_m3(cc_model_element(&s->model, &wid)); /* what cc_measure adds (quantity_test checks it there) */
        check_true("B1 the quantities: 3.408 - pi 0.1^2 0.2 = 3.4017168 m3 (the exact cylinder)", near(volume(&s->model, &wid) + fix, 3.408 - cyl, 1e-6)); /* the mesh is f32 */
        printf("  wall: mesh %.7f m3, quantities %.7f m3 (hand: %.7f)\n", volume(&s->model, &wid), volume(&s->model, &wid) + fix, 3.408 - cyl);
        r = ask(s, "query", "opening_rows", "", &st);
        check_true("B1 opening_rows: the sleeve's own row, 0.006283185307179587 m3, and the box's 0.192",
                   st == CC_OK && has(r, "\"kind\":\"sleeve\"") && has(r, "\"volume_m3\":0.00628318530717958") && has(r, "\"volume_m3\":0.192"));
        if (!has(r, "0.00628318530717958")) printf("  got %s\n", r);
        free(r);
        Buf ifc;
        buf_init(&ifc);
        check_true("B1 IFC4: the sleeve's body is an IfcCircleProfileDef of radius 100",
                   cc_ifc_export(&s->model, "x.ifc", "2026-11-26T10:00:00", CC_IFC4, &ifc) == 0 && has(buf_cstr(&ifc), "IFCCIRCLEPROFILEDEF(.AREA.,$,$,100.0)"));
        buf_clear(&ifc);
        check_true("B1 IFC2x3: the circle profile placed (Position required there)",
                   cc_ifc_export(&s->model, "x.ifc", "2026-11-26T10:00:00", CC_IFC2X3, &ifc) == 0 && has(buf_cstr(&ifc), "IFCCIRCLEPROFILEDEF(.AREA.,$,#"));
        const char *dump = getenv("WAVE1B_SLEEVE_IFC");
        if (dump) {
            FILE *f = fopen(dump, "wb");
            buf_clear(&ifc);
            cc_ifc_export(&s->model, "x.ifc", "2026-11-26T10:00:00", CC_IFC4, &ifc);
            if (f) fwrite(ifc.p, 1, ifc.len, f), fclose(f);
        }
        buf_free(&ifc);
        free(ask(s, "undo", "", "", &st));
    }
    snprintf(body, sizeof body, "{\"ids\":[\"%s\"],\"d\":[100,0,0]}", ws);
    check_true("the wall moves", call_ok(s, "verb", "move", body));
    w = cc_model_element(&s->model, &wid);
    check_true("the opening follows (host-local)", w && w->placement.origin[0] == 100 && w->nopenings == 1 &&
                                                       w->openings[0].placement.origin[0] == 4000);
    snprintf(body, sizeof body, "{\"ids\":[\"%s\"]}", ws);
    check_true("the wall is deleted", call_ok(s, "verb", "delete", body) && !cc_model_element(&s->model, &wid));
    check_true("one undo brings the wall and its opening back",
               call_ok(s, "undo", "", "") && cc_model_element(&s->model, &wid) && cc_model_element(&s->model, &wid)->nopenings == 1);

    /* ---- a slab opening, a sleeve ---- */
    cc_gid sid = gid_of(SLAB);
    double vs = volume(&s->model, &sid);
    check_true("slab D2: 18.3 x 10.3 x 0.22 m3", near(vs, 18.3 * 10.3 * 0.22, 1e-6));
    check_true("slab opening: two corners", call_ok(s, "verb", "opening_add", "{\"host\":\"" SLAB "\",\"a\":[2000,2000],\"b\":[2600,2400]}"));
    check_true("slab opening: -0.6 x 0.4 x 0.22 = -0.0528 m3", near(volume(&s->model, &sid), vs - 0.0528, 1e-6));
    r = call_err(s, "verb", "opening_add", "{\"host\":\"" SLAB "\",\"a\":[18000,5000],\"width\":600}");
    check_true("slab opening: refused across the outline", has(r, "leaves its host"));
    free(r);
    check_true("set_placing sleeve", call_ok(s, "set_placing", "", "{\"kind\":\"sleeve\",\"storey\":\"1jLno3kWj4Hfl6pUGBDd7E\",\"concrete\":{\"sleeve_diameter\":200}}"));
    check_true("sleeve: one click on the slab", call_ok(s, "tool", "click", "{\"at\":[5000,5000],\"free\":true}"));
    const cc_element *sl = cc_model_element(&s->model, &sid);
    double rr = 0;
    check_true("sleeve: a round opening of the slab (32-gon, r 100)",
               sl && sl->nopenings == 2 && cc_opening_is_sleeve(&sl->openings[1], &rr) && near(rr, 100, 1e-9));
    double ngon = 16.0 * 0.1 * 0.1 * 0.19509032201612825 * 0.22; /* 32/2 r^2 sin(2 pi/32) t, m3 */
    check_true("sleeve: the 32-gon's volume 0.00686665 m3 out of the slab", near(volume(&s->model, &sid), vs - 0.0528 - ngon, 1e-6));
    check_true("sleeve: 0.64 % under the cylinder's pi r^2 t", near(ngon / (3.141592653589793 * 0.01 * 0.22), 0.99359, 1e-5));

    /* ---- raft ---- */
    check_true("set_placing raft", call_ok(s, "set_placing", "", "{\"kind\":\"raft\"}"));
    r = ask(s, "query", "placing", "", &st);
    check_true("raft: clicks_needed 0 (an outline), thickness 400", has(r, "\"clicks_needed\":0") && has(r, "\"raft_thickness\":400.0"));
    free(r);
    size_t n2 = n_el(s);
    check_true("raft: three corners", call_ok(s, "tool", "click", "{\"at\":[0,0],\"free\":true}") &&
                                          call_ok(s, "tool", "click", "{\"at\":[6000,0],\"free\":true}") &&
                                          call_ok(s, "tool", "click", "{\"at\":[6000,4000],\"free\":true}") && n_el(s) == n2);
    check_true("raft: Enter closes", call_ok(s, "tool", "close", ""));
    const cc_element *ra = newest(s, "R1");
    cc_gid fond = gid_of(FONDATIONS);
    check_true("raft: R1, an IfcSlab on the lowest storey, top at its elevation, BASESLAB",
               ra && n_el(s) == n2 + 1 && ra->cls == CC_IFC_SLAB && !cc_gid_cmp(&ra->storey, &fond) &&
                   ra->placement.origin[2] == -400 && ra->generator.u.slab.thickness == 400 && !strcmp(prop(ra), "BASESLAB"));
    if (ra) check_true("raft: 4.8 m3", near(volume(&s->model, &ra->id), 4.8, 1e-6));

    /* ---- IFC4 ---- */
    Buf ifc;
    buf_init(&ifc);
    check_true("IFC4 export", !cc_ifc_export(&s->model, "c.ifc", "2026-11-26T10:00:00", CC_IFC4, &ifc));
    const char *t = buf_cstr(&ifc);
    check_true("IFC4: IfcFooting STRIP_FOOTING", has(t, ".STRIP_FOOTING.)"));
    check_true("IFC4: IfcFooting PAD_FOOTING", has(t, ".PAD_FOOTING.)"));
    check_true("IFC4: IfcSlab BASESLAB", has(t, ".BASESLAB.)"));
    char want[128];
    snprintf(want, sizeof want, "IFCOPENINGELEMENT('%s',", os);
    check_true("IFC4: the wall's opening with its GlobalId", has(t, want));
    check_true("IFC4: IfcRelVoidsElement", has(t, "IFCRELVOIDSELEMENT("));
    check_true("IFC4: the sleeve SLEEVE, USERDEFINED", has(t, ",$,$,$,'SLEEVE',") && has(t, ",$,.USERDEFINED.)"));
    Buf ifc2;
    buf_init(&ifc2);
    check_true("IFC2x3 export", !cc_ifc_export(&s->model, "c.ifc", "2026-11-26T10:00:00", CC_IFC2X3, &ifc2) && has(buf_cstr(&ifc2), "IFCRELVOIDSELEMENT("));
    buf_free(&ifc2);
    buf_free(&ifc);

    /* ---- save, reopen: the same bytes ---- */
    Buf a1, a2;
    buf_init(&a1), buf_init(&a2);
    cc_model_save(&s->model, &a1);
    cc_model m2;
    cc_error e2;
    int lrc = cc_model_load(&m2, a1.p, a1.len, &e2);
    check_true("reopen", lrc == 0);
    if (!lrc) {
        cc_model_save(&m2, &a2);
        check_true("save, reopen, save: the same bytes", a1.len == a2.len && !memcmp(a1.p, a2.p, a1.len));
        cc_model_free(&m2);
    }
    buf_free(&a1), buf_free(&a2);

    /* ---- set_openings in the journal: written and read back ---- */
    {
        cc_cmds l;
        cc_cmds_init(&l);
        cc_command *c = cc_cmds_push(&l, CC_CMD_SET_OPENINGS);
        const cc_element *sw = cc_model_element(&s->model, &wid);
        c->id = wid, c->element.openings = sw->openings, c->element.nopenings = sw->nopenings;
        Buf j1, j2;
        buf_init(&j1), buf_init(&j2);
        cc_commands_json(l.v, l.n, &j1);
        cc_cmds back;
        cc_cmds_init(&back);
        Buf perr;
        buf_init(&perr);
        int prc = cc_commands_parse(j1.p, j1.len, &back, &perr);
        if (!prc) cc_commands_json(back.v, back.n, &j2);
        check_true("set_openings: JSON round trip", !prc && j1.len == j2.len && !memcmp(j1.p, j2.p, j1.len) && has(j1.p, "\"op\":\"set_openings\""));
        buf_free(&j1), buf_free(&j2), buf_free(&perr);
        cc_cmds_free(&back), cc_cmds_free(&l);
    }
    /* Hostile set_openings through apply: refused, the model kept. */
    uint64_t rv = s->rev;
    snprintf(body, sizeof body, "[{\"op\":\"set_openings\",\"id\":\"%s\",\"openings\":[{\"placement\":{\"origin\":[0,0,1e999],\"z_axis\":[0,0,1],\"x_axis\":[1,0,0]},\"generator\":{\"kind\":\"rect_column\",\"width\":10,\"depth\":10,\"height\":10}}]}]", ws);
    check_true("hostile: a non-finite origin refused", !call_ok(s, "apply", "", body) && s->rev == rv);
    snprintf(body, sizeof body, "[{\"op\":\"set_openings\",\"id\":\"%s\",\"openings\":{}}]", ws);
    check_true("hostile: openings not a list refused", !call_ok(s, "apply", "", body) && s->rev == rv);
    snprintf(body, sizeof body,
             "[{\"op\":\"set_openings\",\"id\":\"%s\",\"openings\":[{\"id\":\"%s\",\"placement\":{\"origin\":[0,0,0],\"z_axis\":[0,0,1],\"x_axis\":[1,0,0]},\"generator\":{\"kind\":\"rect_column\",\"width\":10,\"depth\":10,\"height\":10}},"
             "{\"id\":\"%s\",\"placement\":{\"origin\":[50,0,0],\"z_axis\":[0,0,1],\"x_axis\":[1,0,0]},\"generator\":{\"kind\":\"rect_column\",\"width\":10,\"depth\":10,\"height\":10}}]}]",
             ws, os, os);
    check_true("hostile: two openings with one GlobalId refused", !call_ok(s, "apply", "", body) && s->rev == rv);
    check_true("hostile: a host that does not exist refused",
               !call_ok(s, "apply", "", "[{\"op\":\"set_openings\",\"id\":\"3zzzzzzzzzzzzzzzzzzzzz\",\"openings\":[]}]") && s->rev == rv);
    r = call_err(s, "verb", "opening_add", "{\"host\":\"" SLAB "\",\"a\":[1e400,0]}");
    check_true("hostile: a non-finite point refused", has(r, "a: a point"));
    free(r);
    r = call_err(s, "verb", "footing_add", "{\"storey\":\"" N1 "\",\"a\":[0,0],\"width\":-5}");
    check_true("hostile: a negative size refused", has(r, "width"));
    free(r);
    r = call_err(s, "set_placing", "", "{\"concrete\":{\"footing_width\":1e9}}");
    check_true("hostile: set_placing concrete out of range refused", has(r, "footing_width"));
    free(r);

    { /* ---- WAVE1B (B3): export -> import -> the same model up to what IFC does not carry ---- */
        Buf ifc;
        buf_init(&ifc);
        int ex = cc_ifc_export(&s->model, "x.ifc", "2026-11-26T10:00:00", CC_IFC4, &ifc);
        const char *dump = getenv("WAVE1B_RT_IFC");
        if (dump) {
            FILE *f = fopen(dump, "wb");
            if (f) fwrite(ifc.p, 1, ifc.len, f), fclose(f);
        }
        cc_model im;
        cc_ifc_report rep;
        int irc = ex ? -1 : cc_ifc_import(&im, &rep, ifc.p, ifc.len, &e2);
        check_true("B3 export and import", irc == 0);
        size_t checked = 0, bad = 0, nop = 0;
        char why[256] = "";
#define NEAR6(a, b) ((a) - (b) < 1e-6 && (b) - (a) < 1e-6)
        for (size_t i = 0; irc == 0 && i < s->model.nelements; i++) {
            const cc_element *a = &s->model.elements[i];
            int mine = a->cls == CC_IFC_FOOTING || a->nopenings || cc_element_predefined(a);
            if (!mine) continue;
            const cc_element *b = cc_model_element(&im, &a->id);
            checked++;
            const cc_str *pa = cc_element_predefined(a), *pb = b ? cc_element_predefined(b) : NULL;
            int ok = b && b->cls == a->cls && b->generator.kind == a->generator.kind && b->nopenings == a->nopenings &&
                     (!pa || (pb && pb->n == pa->n && !memcmp(pb->p, pa->p, pa->n)));
            for (int k = 0; ok && k < 3; k++)
                ok = NEAR6(a->placement.origin[k], b->placement.origin[k]) && NEAR6(a->placement.x_axis[k], b->placement.x_axis[k]) &&
                     NEAR6(a->placement.z_axis[k], b->placement.z_axis[k]);
            if (ok && a->generator.kind == CC_GEN_WALL)
                ok = NEAR6(a->generator.u.wall.length, b->generator.u.wall.length) && NEAR6(a->generator.u.wall.thickness, b->generator.u.wall.thickness) &&
                     NEAR6(a->generator.u.wall.height, b->generator.u.wall.height);
            if (ok && a->generator.kind == CC_GEN_PAD_FOOTING)
                ok = NEAR6(a->generator.u.col.width, b->generator.u.col.width) && NEAR6(a->generator.u.col.height, b->generator.u.col.height);
            for (size_t k = 0; ok && k < a->nopenings; k++) {
                const cc_opening *x = &a->openings[k], *y = &b->openings[k];
                double rx = 0, ry = 0;
                ok = x->has_id == y->has_id && !cc_gid_cmp(&x->id, &y->id) && x->generator.kind == y->generator.kind &&
                     cc_opening_is_sleeve(x, &rx) == cc_opening_is_sleeve(y, &ry) && NEAR6(rx, ry);
                for (int q = 0; ok && q < 3; q++)
                    ok = NEAR6(x->placement.origin[q], y->placement.origin[q]) && NEAR6(x->placement.z_axis[q], y->placement.z_axis[q]) &&
                         NEAR6(x->placement.x_axis[q], y->placement.x_axis[q]);
                if (ok && x->generator.kind == CC_GEN_RECT_COLUMN)
                    ok = NEAR6(x->generator.u.col.width, y->generator.u.col.width) && NEAR6(x->generator.u.col.depth, y->generator.u.col.depth) &&
                         NEAR6(x->generator.u.col.height, y->generator.u.col.height);
                nop++;
            }
            if (!ok) {
                bad++;
                if (!why[0])
                    snprintf(why, sizeof why, "%.*s: class %d/%d gen %d/%d openings %zu/%zu", (int)a->name.n, a->name.p, a->cls, b ? (int)b->cls : -1,
                             a->generator.kind, b ? (int)b->generator.kind : -1, a->nopenings, b ? b->nopenings : 0);
            }
        }
#undef NEAR6
        printf("  B3 round trip: %zu elements (footings, hosts, rafts), %zu openings compared, %zu differ%s%s\n", checked, nop, bad, bad ? ": " : "", why);
        check_true("B3 round trip: strip footings, BASESLAB, openings and sleeves come back parametric, the same", irc == 0 && checked >= 5 && nop >= 3 && bad == 0);
        if (irc == 0) cc_model_free(&im), cc_ifc_report_free(&rep);
        buf_free(&ifc);
    }
    /* A model without the new data keeps its bytes (frame.json reopened and saved). */
    cc_model m3;
    Buf b3;
    buf_init(&b3);
    if (!cc_model_load(&m3, before.p, before.len, &e2)) {
        cc_model_save(&m3, &b3);
        check_true("frame.json: the same bytes", b3.len == before.len && !memcmp(b3.p, before.p, before.len));
        cc_model_free(&m3);
    }
    buf_free(&b3);
    buf_free(&before);
    cc_session_free(s);
    return report("concrete");
}
