/* Suite toggles: the plan annotation toggles (ticket KEY1, session/toggles.h), through the session's
 * calls, on a copy of golden/drawing/model.json (the platform's clock 2026-01-01):
 * - `actions`: section_labels, openings, section_marks, slab_levels enabled with a model (ribbon.rs:612
 *   `_ => loaded`), not without one; pressed (all on: PlanTemplate::default, template.rs:222-226);
 * - each action turns its flag (pressed off) and sheet S-101's `sheet_svg` loses exactly what the
 *   template flag draws there (hand-checked: the element labels "30/40", "20/20" and the material
 *   label; the opening size "150.1x20"; the north section's two "B" marks and their 7 shapes; no
 *   slab level on S-101); turned again, the default bytes come back (the paper cache keys on the
 *   toggles), which draw_sheet_test checks against Rust's golden sheet/S-101.svg;
 * - sheet_pdf follows (Section labels); view_pdf of golden/frame.json's plans: Slab levels changes
 *   one, Section labels several, each comes back on; the toggles stay when another model opens
 *   (Rust: the viewer's plan template);
 * - no history step and the same rev. Fails on the code before KEY1 (the actions were never enabled). */
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../session/calls.h"
#include "../session/session.h"
#include "../session/sheet_svg.h"
#include "../session/toggles.h"
#include "../src/draw_plan.h"
#include "../src/draw_sheet.h"
#include "../src/draw_style.h"
#include "../src/arena.h"
#include "../src/json_r.h"
#include "../src/ifc_out.h" /* CC_IFC_VERSION */
#include "../src/paper.h"
#include "check.h"
#include "../host/host.h"

#define G "golden/drawing"
#define DIR "out/test-toggles"
static const char *const NAMES[] = {"section_labels", "openings", "section_marks", "slab_levels"};

static int64_t new_year(void *ctx) { return (void)ctx, 1767225600; /* 2026-01-01 00:00 UTC */ }

static size_t count(const Buf *b, const char *t) {
    size_t n = strlen(t), k = 0;
    for (size_t i = 0; i + n <= b->len; i++)
        if (!memcmp(b->p + i, t, n)) k++;
    return k;
}

static int same(const Buf *a, const Buf *b) { return a->len == b->len && !memcmp(a->p, b->p, a->len); }

/* `"pressed":` of the action `name` in the actions JSON: 1, 0, or -1 when absent; enabled likewise. */
static int flag(cc_session *s, const char *name, const char *key) {
    Buf b;
    buf_init(&b);
    cc_call(s, "query", "actions", "", 0, &b);
    buf_putc(&b, 0);
    char pat[96];
    snprintf(pat, sizeof pat, "\"name\":\"%s\"", name);
    const char *at = strstr(b.p, pat);
    int r = -1;
    if (at) {
        const char *obj = at;
        while (obj > b.p && *obj != '{') obj--;
        const char *k = strstr(obj, key);
        if (k) r = !strncmp(k + strlen(key), "true", 4);
    }
    buf_free(&b);
    return r;
}

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_clear(out);
    return cc_call(s, op, name, body, strlen(body), out);
}

/* The sheet_svg answer's "svg" string as it is in the JSON (escaped): enough to compare and count. */
static void svg_of(cc_session *s, const char *body, Buf *out) {
    int rc = call(s, "query", "sheet_svg", body, out);
    check_true("sheet_svg answers", rc == CC_OK);
}

int main(void) {
    size_t n;
    char *json = slurp(G, "model.json", &n);
    const char *copy = DIR "/frame.json";
#ifdef _WIN32
    mkdir(DIR);
#else
    mkdir(DIR, 0755);
#endif
    FILE *f = fopen(copy, "wb");
    check_true("copy model.json", f && fwrite(json, 1, n, f) == n);
    if (f) fclose(f);
    free(json);
    cc_platform pf = *cc_platform_native();
    pf.unix_s = new_year;
    cc_session *s = cc_session_new(&pf);
    check_true("a session", s != NULL);
    if (!s) return report("toggles");
    for (int i = 0; i < 4; i++) check_true("no model: not enabled", flag(s, NAMES[i], "\"enabled\":") == 0);
    cc_error e;
    if (cc_session_open(s, copy, &e)) {
        check_true("frame.json opens", 0);
        cc_session_free(s);
        return report("toggles");
    }
    for (int i = 0; i < 4; i++) {
        check_true("a model: enabled", flag(s, NAMES[i], "\"enabled\":") == 1);
        check_true("pressed: on by default", flag(s, NAMES[i], "\"pressed\":") == 1);
    }
    const cc_model *m = cc_session_model(s);
    const cc_sheet *s101 = NULL;
    for (size_t i = 0; i < m->nsheets; i++)
        if (m->sheets[i].number.n == 5 && !memcmp(m->sheets[i].number.p, "S-101", 5)) s101 = &m->sheets[i];
    check_true("sheet S-101", s101 != NULL);
    if (!s101) return report("toggles");
    char body[64];
    snprintf(body, sizeof body, "{\"id\":\"%.22s\"}", s101->id.c);
    uint64_t rev0 = s->rev;
    size_t undo0 = s->hist.nundo;
    Buf def, got, out, pdf0, pdf1;
    buf_init(&def), buf_init(&got), buf_init(&out), buf_init(&pdf0), buf_init(&pdf1);
    svg_of(s, body, &def);

    /* What each flag draws on S-101's plan, hand-checked in the SVG (draw_plan.c:761 and 1298 the
     * element labels, 1200 the opening sizes, 1305 the section marks): the texts that go, the
     * texts left of 44, the shapes left of 52. Slab levels: none on S-101 (its plan sees no slab top
     * of 12 mm on paper, draw_plan.c:1217): the bytes stay; the view_pdf part below shows one. */
    static const struct { const char *gone; size_t texts, shapes; } WANT[4] = {
        {">30/40<", 41, 52}, {">150.1x20<", 43, 52}, {">B<", 42, 45}, {NULL, 44, 52}};
    const char *SHAPES[] = {"<line", "<path", "<polyline", "<polygon"};
    size_t shapes0 = 0;
    for (int k = 0; k < 4; k++) shapes0 += count(&def, SHAPES[k]);
    check_true("S-101 by default: 44 texts, 52 shapes", count(&def, "<text") == 44 && shapes0 == 52);
    for (int i = 0; i < 4; i++) {
        char what[160];
        snprintf(what, sizeof what, "%s: the action runs", NAMES[i]);
        check_true(what, call(s, "action", NAMES[i], "", &out) == CC_OK);
        snprintf(what, sizeof what, "%s: pressed off", NAMES[i]);
        check_true(what, flag(s, NAMES[i], "\"pressed\":") == 0);
        svg_of(s, body, &got);
        size_t shapes = 0;
        for (int k = 0; k < 4; k++) shapes += count(&got, SHAPES[k]);
        printf("  %s off: %zu -> %zu bytes, %zu -> %zu texts, %zu -> %zu shapes\n", NAMES[i], def.len, got.len,
               count(&def, "<text"), count(&got, "<text"), shapes0, shapes);
        snprintf(what, sizeof what, "%s off: S-101 has %zu texts and %zu shapes left%s", NAMES[i], WANT[i].texts, WANT[i].shapes,
                 WANT[i].gone ? ", its own gone" : ", the same bytes");
        check_true(what, count(&got, "<text") == WANT[i].texts && shapes == WANT[i].shapes &&
                             (WANT[i].gone ? count(&got, WANT[i].gone) < count(&def, WANT[i].gone) : same(&got, &def)));
        check_true("no history step, rev kept", s->rev == rev0 && s->hist.nundo == undo0);
        call(s, "action", NAMES[i], "", &out);
        snprintf(what, sizeof what, "%s on again: pressed, the default bytes (the cache follows)", NAMES[i]);
        svg_of(s, body, &got);
        check_true(what, flag(s, NAMES[i], "\"pressed\":") == 1 && same(&got, &def));
    }
    /* sheet_pdf follows too (not cached): Section labels off changes it, on again gives it back. */
    {
        char pb[64];
        snprintf(pb, sizeof pb, "{\"sheet\":\"%.22s\"}", s101->id.c);
        int r0 = call(s, "query", "sheet_pdf", pb, &pdf0);
        call(s, "action", "section_labels", "", &out);
        int r1 = call(s, "query", "sheet_pdf", pb, &pdf1);
        check_true("sheet_pdf: Section labels off changes it", r0 == CC_OK && r1 == CC_OK && !same(&pdf0, &pdf1));
        call(s, "action", "section_labels", "", &out);
        call(s, "query", "sheet_pdf", pb, &pdf1);
        check_true("sheet_pdf: on again, the same bytes", same(&pdf0, &pdf1));
    }
    /* view_pdf of the plans of golden/frame.json: Slab levels changes one (a slab top seen from
     * above), Section labels several; each comes back. The toggles stay across a model opened. */
    {
        size_t fn;
        char *fj = slurp("golden", "frame.json", &fn);
        const char *fcopy = DIR "/frame2.json";
        FILE *ff = fopen(fcopy, "wb");
        if (ff) fwrite(fj, 1, fn, ff), fclose(ff);
        free(fj);
        cc_error fe;
        check_true("golden/frame.json opens", cc_session_open(s, fcopy, &fe) == 0);
        Buf vj;
        buf_init(&vj);
        call(s, "query", "open_views", "", &vj);
        Arena va;
        arena_init(&va, 0);
        JrError je;
        const JrVal *ov = jr_parse(&va, vj.p, vj.len, &je), *vs = ov ? jr_get(ov, "views") : NULL;
        int plans = 0, changed[4] = {0}, back = 1;
        for (size_t i = 0; vs && vs->type == JR_ARR && i < vs->n; i++) {
            const JrVal *id = jr_get(&vs->items[i], "id"), *ty = jr_get(&vs->items[i], "type");
            if (!id || !ty || strcmp(ty->s, "plan")) continue;
            plans++;
            char vb[64];
            snprintf(vb, sizeof vb, "{\"view\":\"%s\"}", id->s);
            int r0 = call(s, "query", "view_pdf", vb, &pdf0);
            for (int t = 0; t < 4; t++) {
                call(s, "action", NAMES[t], "", &out);
                int r1 = call(s, "query", "view_pdf", vb, &pdf1);
                if (r0 == CC_OK && r1 == CC_OK && !same(&pdf0, &pdf1)) changed[t]++;
                call(s, "action", NAMES[t], "", &out);
                call(s, "query", "view_pdf", vb, &pdf1);
                back &= same(&pdf0, &pdf1);
            }
        }
        printf("  frame.json: %d plans; view_pdf changed by section_labels %d, openings %d, section_marks %d, slab_levels %d\n",
               plans, changed[0], changed[1], changed[2], changed[3]);
        check_true("view_pdf: Slab levels off changes a plan's PDF, Section labels several", changed[3] >= 1 && changed[0] >= 2);
        check_true("view_pdf: each toggle on again gives the same bytes", back);
        arena_free(&va);
        buf_free(&vj);
        remove(fcopy);
    }
    cc_session_free(s);
    remove(copy);
    rmdir(DIR);
    buf_free(&def), buf_free(&got), buf_free(&out), buf_free(&pdf0), buf_free(&pdf1);
    return report("toggles");
}
