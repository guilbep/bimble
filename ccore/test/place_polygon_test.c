/* Suite place_polygon (ticket SLAB1): the polygon slab placed by clicks (op "tool", calls.h;
 * edit.rs:318-385). Replays golden/place/polygon.txt (Rust's Editor::click / close_polygon on
 * golden/frame.json, made by ccore_golden on oracle/slab1) through the session's tool calls: the
 * same commands (the new id masked), the same error texts, the same points left. Then by hand:
 * the L-shaped slab's area and volume (outline and mesh), one undo step, Esc takes the last corner
 * back then stops placing, the hint after 3 corners, the ghost from 2 corners on. */
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

/* The storey at elevation 0. */
static const cc_storey *storey0(const cc_model *m) {
    for (size_t i = 0; i < m->nstoreys; i++)
        if (m->storeys[i].elevation == 0.0) return &m->storeys[i];
    return NULL;
}

/* "[[x,y],[x,y],…]" -> points; returns the count. */
static size_t parse_points(const char *p, double (*out)[2], size_t cap) {
    size_t n = 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    while (n < cap && (p = strchr(p, '['))) {
        char *e;
        out[n][0] = strtod(p + 1, &e);
        out[n][1] = strtod(e + 1, &e);
        n++;
        p = e;
    }
    return n;
}

/* The element's add_element command as Rust writes it, the id as <new-1>. */
static char *add_json(const cc_element *e) {
    cc_command c;
    memset(&c, 0, sizeof c);
    c.op = CC_CMD_ADD_ELEMENT;
    c.element = *e;
    Buf b;
    buf_init(&b);
    cc_commands_json(&c, 1, &b);
    const char *t = buf_cstr(&b);
    char id[23];
    memcpy(id, e->id.c, 22), id[22] = 0;
    char *z = malloc(strlen(t) + 16), *o = z;
    for (const char *q = t; *q;) {
        if (!strncmp(q, id, 22)) memcpy(o, "<new-1>", 7), o += 7, q += 22;
        else *o++ = *q++;
    }
    *o = 0;
    buf_free(&b);
    return z;
}

/* The mesh volume of the scene element `id` (divergence theorem), mm^3; -1 if not found. */
static double mesh_volume(const cc_scene *sc, const cc_gid *id) {
    for (size_t k = 0; k < sc->nelements; k++) {
        if (memcmp(sc->elements[k].id.c, id->c, 22)) continue;
        Arena a;
        arena_init(&a, 0);
        cc_mesh m;
        double v = -1.0;
        if (!cc_scene_world_mesh(sc, k, &a, &m)) {
            v = 0.0;
            for (uint32_t i = 0; i + 2 < m.ni; i += 3) {
                const float *p = m.pos[m.idx[i]], *q = m.pos[m.idx[i + 1]], *r = m.pos[m.idx[i + 2]];
                v += ((double)p[0] * ((double)q[1] * r[2] - (double)q[2] * r[1]) - (double)p[1] * ((double)q[0] * r[2] - (double)q[2] * r[0]) +
                      (double)p[2] * ((double)q[0] * r[1] - (double)q[1] * r[0])) / 6.0;
            }
        }
        arena_free(&a);
        return v;
    }
    return -1.0;
}

int main(int argc, char **argv) {
    const char *floats = argc > 1 ? argv[1] : "ccore/golden/floats";
    char path[4096], gdir[4096];
    snprintf(path, sizeof path, "%s/../frame.json", floats);
    snprintf(gdir, sizeof gdir, "%s/../place", floats);
    cc_session *s = cc_session_new(cc_platform_native());
    cc_error err;
    if (!s || cc_session_open(s, path, &err)) {
        printf("open: %s\n", s ? err.msg : "oom");
        return 1;
    }
    const cc_storey *st0 = storey0(cc_session_model(s));
    check_true("frame: a storey at 0", st0 != NULL);
    char sid[23];
    memcpy(sid, st0->id.c, 22), sid[22] = 0;
    int st;
    char body[512];

    /* ---- the golden transcript ---- */
    size_t gn;
    char *g = slurp(gdir, "polygon.txt", &gn);
    int cases = 0;
    for (char *line = g; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (strncmp(line, "case ", 5)) {
            line = nl ? nl + 1 : NULL;
            continue;
        }
        char *rec[6];
        rec[0] = line;
        for (int i = 1; i < 6; i++) {
            line = nl ? nl + 1 : NULL;
            nl = line ? strchr(line, '\n') : NULL;
            if (nl) *nl = 0;
            rec[i] = line ? line : "";
        }
        line = nl ? nl + 1 : NULL;
        const char *name = rec[0] + 5;
        double top = strtod(rec[1] + 11, NULL);
        double pts[64][2];
        size_t np = parse_points(rec[2] + 7, pts, 64);
        int enter = !strcmp(rec[3], "then enter");
        cases++;
        size_t n0 = cc_session_model(s)->nelements;
        snprintf(body, sizeof body, "{\"kind\":\"slab\",\"shape\":\"polygon\",\"storey\":\"%s\",\"top_offset_mm\":%.17g}", sid, top);
        free(ask(s, "set_placing", "-", body, &st));
        char *last = NULL;
        int last_st = CC_OK;
        for (size_t i = 0; i < np; i++) {
            snprintf(body, sizeof body, "{\"at\":[%.17g,%.17g],\"free\":true}", pts[i][0], pts[i][1]);
            free(last);
            last = ask(s, "tool", "click", body, &last_st);
        }
        if (enter) {
            free(last);
            last = ask(s, "tool", "close", "", &last_st);
        }
        char what[256];
        if (!strncmp(rec[4], "ok ", 3)) {
            const char *pl = strstr(last, "\"placed\":\"");
            cc_gid id;
            const cc_element *e = NULL;
            if (pl) memcpy(id.c, pl + 10, 22), e = cc_model_element(cc_session_model(s), &id);
            char *got = e ? add_json(e) : NULL;
            snprintf(what, sizeof what, "golden %s: the same slab as Rust", name);
            check_eq(what, got ? got : last, strlen(got ? got : last), rec[4] + 3, strlen(rec[4] + 3));
            snprintf(what, sizeof what, "golden %s: one element more", name);
            check_true(what, last_st == CC_OK && cc_session_model(s)->nelements == n0 + 1);
            free(got);
            free(ask(s, "undo", "-", "", &st)); /* one step back: the next case is named D3 too */
            snprintf(what, sizeof what, "golden %s: one undo step takes it back", name);
            check_true(what, cc_session_model(s)->nelements == n0);
        } else {
            snprintf(what, sizeof what, "golden %s: Rust's error", name);
            check_eq(what, last, strlen(last), rec[4] + 4, strlen(rec[4] + 4));
            check_true(what, last_st == CC_ERR && cc_session_model(s)->nelements == n0);
        }
        snprintf(what, sizeof what, "golden %s: the points left (%s)", name, rec[5]);
        check_true(what, s->tool.npoints == (size_t)strtoul(rec[5] + 7, NULL, 10));
        free(last);
        free(ask(s, "tool", "cancel", "", &st)); /* placing stopped, the points dropped */
        free(ask(s, "tool", "cancel", "", &st));
        while (s->placing) free(ask(s, "tool", "cancel", "", &st));
    }
    free(g);
    check_true("golden: 9 cases", cases == 9);

    /* ---- by hand: the L of 6 corners ---- */
    snprintf(body, sizeof body, "{\"kind\":\"slab\",\"shape\":\"polygon\",\"storey\":\"%s\",\"top_offset_mm\":0}", sid);
    free(ask(s, "set_placing", "-", body, &st));
    static const double L[6][2] = {{30000, 0}, {36000, 0}, {36000, 2000}, {32000, 2000}, {32000, 5000}, {30000, 5000}};
    char *a = NULL;
    for (int i = 0; i < 6; i++) {
        if (i == 2) {
            snprintf(body, sizeof body, "{\"at\":[%g,%g],\"free\":true}", 36000.0, 2000.0);
            free(a);
            a = ask(s, "tool", "pointer", body, &st);
            check_true("2 corners and the pointer: a ghost (the triangle)", has(a, "\"ghost\":{\"edges\":[") && has(a, "\"ghost_error\":null"));
        }
        snprintf(body, sizeof body, "{\"at\":[%g,%g],\"free\":true}", L[i][0], L[i][1]);
        free(a);
        a = ask(s, "tool", "click", body, &st);
        if (i == 0) check_true("1 corner: no ghost yet", has(a, "\"ghost\":null"));
    }
    free(a);
    a = ask(s, "query", "placing", "", &st);
    check_true("6 corners: the hint names Enter", has(a, "click the first corner or Enter closes."));
    free(a);
    a = ask(s, "tool", "pointer", "{\"at\":[30040,30],\"free\":true}", &st);
    check_true("the pointer on the first corner: the ghost is the closed outline", has(a, "\"ghost\":{"));
    free(a);
    size_t n1 = cc_session_model(s)->nelements;
    a = ask(s, "tool", "click", "{\"at\":[30040,30],\"free\":true}", &st);
    check_true("a click 50 mm from the first corner closes: one slab", st == CC_OK && cc_session_model(s)->nelements == n1 + 1);
    {
        const char *pl = strstr(a, "\"placed\":\"");
        cc_gid id;
        const cc_element *e = NULL;
        if (pl) memcpy(id.c, pl + 10, 22), e = cc_model_element(cc_session_model(s), &id);
        double area2 = 0.0;
        if (e && e->generator.kind == CC_GEN_SLAB) {
            const cc_loop *o = &e->generator.u.slab.outline;
            for (size_t i = 0; i < o->n; i++) {
                const double *p = o->pts[i], *q = o->pts[(i + 1) % o->n];
                area2 += p[0] * q[1] - q[0] * p[1];
            }
        }
        /* By hand: 6000 x 2000 + 2000 x 3000 = 18e6 mm^2; x 250 mm = 4.5e9 mm^3. */
        check_true("the L: area 18 m2 (counter-clockwise outline)", area2 == 2.0 * 18.0e6);
        check_true("the L: origin (30000, 0, 2750)", e && e->placement.origin[0] == 30000.0 && e->placement.origin[1] == 0.0 && e->placement.origin[2] == 2750.0);
        const cc_scene *sc = cc_session_scene(s);
        double v = sc && e ? mesh_volume(sc, &e->id) : -1.0;
        printf("  L slab mesh volume %.6g mm3\n", v);
        check_true("the L: mesh volume 4.5e9 mm3 (1e-6)", v > 0 && (v - 4.5e9) / 4.5e9 < 1e-6 && (4.5e9 - v) / 4.5e9 < 1e-6);
    }
    free(a);
    a = ask(s, "query", "history", "", &st);
    check_true("the L: one history step \"add D3\"", has(a, "\"undo\":[\"add D3\""));
    free(a);

    /* ---- Esc: the last corner, then the tool ---- */
    for (int i = 0; i < 3; i++) {
        snprintf(body, sizeof body, "{\"at\":[%g,%g],\"free\":true}", L[i][0], L[i][1] + 10000.0);
        free(ask(s, "tool", "click", body, &st));
    }
    a = ask(s, "tool", "cancel", "", &st);
    check_true("Esc with 3 corners: 2 left, still placing", s->tool.npoints == 2 && s->placing && has(a, "\"active\":true"));
    free(a);
    free(ask(s, "tool", "cancel", "", &st));
    free(ask(s, "tool", "cancel", "", &st));
    check_true("Esc twice more: no corner, still placing", s->tool.npoints == 0 && s->placing);
    a = ask(s, "tool", "cancel", "", &st);
    check_true("Esc with no corner: placing stops", !s->placing && has(a, "\"active\":false"));
    free(a);
    a = ask(s, "tool", "close", "", &st);
    check_true("Enter while not placing: no change, 200", st == CC_OK && has(a, "\"active\":false"));
    free(a);

    cc_session_free(s);
    return report("place_polygon");
}
