/* Suite view_sheet (ticket VIEWSHEET, ADR 0076, session/view_sheet.h): the View, Sheets and
 * Annotate tabs' tools on the C core, through cc_call as the page and the host use it.
 *   - actions new_view, draw_section, sheet_place_view, sheet_revision: listed after SETOUT's; enabled
 *     with an editable model (new_view), a plan shown (draw_section), a sheet shown (sheet_place_view,
 *     sheet_revision); draw_section is a switch (session state, rev kept), off in 3D and when
 *     the place tool starts;
 *   - verb view_add with `scale` and a section's `depth_mm`: one command, one undo step; bad
 *     values refused (a fraction, 0, 5001, a depth on a plan, a negative depth);
 *   - query annotation_types: the standards' 25 text types and the dimension types, none in use;
 *     action annotation_type: a known name is used, null is Default, hostile values refused
 *     (unknown name, 201 bytes, a number, two keys, not JSON) and the state kept;
 *   - the annotate tool: a text placed while "Arial 5 mm black" is in use has that kind and 5 mm;
 *     a dimension the dimension type's kind.
 * Hand-computed expectations (no Rust reference for the C additions, DEVIATIONS 33). Before
 * VIEWSHEET none of the actions existed (flag -1), view_add refused `scale` (unknown field) and
 * annotation_types was an unknown query. */
#include <stdlib.h>
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "check.h"

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

/* The action's `"enabled"` (key 'e') or `"pressed"` (key 'p') flag; -1 not listed. */
static int flag(cc_session *s, const char *name, char key) {
    int st;
    char *a = ask(s, "query", "actions", "", &st), k[96];
    snprintf(k, sizeof k, "\"name\":\"%s\"", name);
    char *p = strstr(a, k);
    int on = -1;
    if (p) {
        char *o = p;
        while (o > a && *o != '{') o--;
        char *end = strchr(p, '}');
        const char *want = key == 'e' ? "\"enabled\":true" : "\"pressed\":true";
        char *f = strstr(o, want);
        on = f && f < end;
    }
    free(a);
    return on;
}

static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

static int ok(cc_session *s, const char *op, const char *name, const char *body) {
    int st;
    free(ask(s, op, name, body, &st));
    return st == CC_OK;
}

/* The verb's error text (empty when it ran). */
static char *verb(cc_session *s, const char *name, const char *body, int *st) { return ask(s, "verb", name, body, st); }

#define PLAN1 "3w8zGYnP$Ay1lyvMZjDnXq" /* the default plan of Niveau 1 in frame.json */
#define STOREY1 "1uAZA0Yfn05f$xRd1k4uPR"

int main(void) {
    const cc_platform *pf = cc_platform_native();
    cc_session *s = cc_session_new(pf);
    int st;
    check_true("no model: new_view listed, off", flag(s, "new_view", 'e') == 0);
    check_true("no model: draw_section, sheet_place_view, sheet_revision listed, off",
               flag(s, "draw_section", 'e') == 0 && flag(s, "sheet_place_view", 'e') == 0 && flag(s, "sheet_revision", 'e') == 0);

    cc_error err;
    if (cc_session_open(s, "golden/frame.json", &err)) {
        printf("open: %s\n", err.msg);
        return 1;
    }
    check_true("model in 3D: new_view on, draw_section off (no plan shown)", flag(s, "new_view", 'e') == 1 && flag(s, "draw_section", 'e') == 0);
    check_true("model, no sheet: sheet_place_view, sheet_revision off", flag(s, "sheet_place_view", 'e') == 0 && flag(s, "sheet_revision", 'e') == 0);

    /* ---- the section tool: a switch in a plan ---- */
    check_true("tab open the plan of Niveau 1", ok(s, "tab", "", "{\"op\":\"open\",\"tab\":{\"type\":\"view\",\"id\":\"" PLAN1 "\"}}"));
    check_true("plan shown: draw_section on, not pressed", flag(s, "draw_section", 'e') == 1 && flag(s, "draw_section", 'p') == 0);
    unsigned long r0 = (unsigned long)s->rev;
    check_true("action draw_section -> pressed, rev kept", ok(s, "action", "draw_section", "") && flag(s, "draw_section", 'p') == 1 && (unsigned long)s->rev == r0);
    check_true("place_column stops the section tool", ok(s, "action", "place_column", "") && flag(s, "draw_section", 'p') == 0);
    ok(s, "action", "place_column", "");
    ok(s, "action", "draw_section", "");
    check_true("draw_section again: pressed", flag(s, "draw_section", 'p') == 1);
    check_true("action draw_section twice: off", ok(s, "action", "draw_section", "") && flag(s, "draw_section", 'p') == 0);

    /* ---- view_add with scale and depth_mm: one undo step ---- */
    size_t u0 = s->hist.nundo;
    char *e = verb(s, "view_add", "{\"kind\":\"section\",\"facing\":\"north\",\"origin\":[9000,5000],\"scale\":50,\"depth_mm\":2000}", &st);
    check_true("view_add section with scale 50 and depth 2000: ok, one undo step", st == CC_OK && s->hist.nundo == u0 + 1);
    free(e);
    char *v = ask(s, "query", "views", "", &st);
    check_true("the section is listed with 1:50", has(v, "Section 1 (looking north)") && has(v, "\"scale\":50"));
    free(v);
    e = verb(s, "view_add", "{\"kind\":\"plan\",\"storey\":\"" STOREY1 "\",\"scale\":20,\"name\":\"Formwork\"}", &st);
    check_true("view_add plan with scale 20 and a name: ok", st == CC_OK && s->hist.nundo == u0 + 2);
    free(e);
    static const char *const BAD[] = {
        "{\"kind\":\"three_d\",\"scale\":0}",
        "{\"kind\":\"three_d\",\"scale\":5001}",
        "{\"kind\":\"three_d\",\"scale\":12.5}",
        "{\"kind\":\"three_d\",\"scale\":\"50\"}",
        "{\"kind\":\"plan\",\"storey\":\"" STOREY1 "\",\"depth_mm\":100}",
        "{\"kind\":\"section\",\"facing\":\"east\",\"origin\":[0,0],\"depth_mm\":-1}",
        "{\"kind\":\"section\",\"facing\":\"east\",\"origin\":[0,0],\"depth_mm\":1e300}",
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        e = verb(s, "view_add", BAD[i], &st);
        char what[200];
        snprintf(what, sizeof what, "view_add refused: %.120s", BAD[i]);
        check_true(what, st != CC_OK && s->hist.nundo == u0 + 2);
        free(e);
    }

    /* ---- sheets: sheet_place_view and sheet_revision with a sheet shown ---- */
    e = verb(s, "add_sheet", "{}", &st);
    free(e);
    char *sh = ask(s, "query", "sheets", "", &st);
    char *idp = strstr(sh, "\"id\":\"");
    char sid[23] = {0};
    if (idp) memcpy(sid, idp + 6, 22);
    free(sh);
    char body[128];
    snprintf(body, sizeof body, "{\"op\":\"open\",\"tab\":{\"type\":\"sheet\",\"id\":\"%s\"}}", sid);
    check_true("tab open the sheet", ok(s, "tab", "", body));
    check_true("sheet shown: sheet_place_view, sheet_revision on; draw_section off",
               flag(s, "sheet_place_view", 'e') == 1 && flag(s, "sheet_revision", 'e') == 1 && flag(s, "draw_section", 'e') == 0);
    check_true("action sheet_place_view, sheet_revision: ok (the page's dialogs)", ok(s, "action", "sheet_place_view", "") && ok(s, "action", "sheet_revision", ""));

    /* ---- annotation types ---- */
    char *t = ask(s, "query", "annotation_types", "", &st);
    check_true("annotation_types: none in use, 25 text types from Arial 5 mm black", st == CC_OK && has(t, "{\"dimension\":null,\"dimension_types\":[{") &&
               has(t, "\"text\":null,\"text_types\":[{\"font\":\"Arial\",\"name\":\"Arial 5 mm black\",\"size_mm\":5.0}"));
    size_t ntt = 0;
    for (const char *p = t; (p = strstr(p, "\"font\":")); p++) ntt++;
    check_true("annotation_types: 25 text types", ntt == 25);
    char dim[96] = {0};
    const char *dn = strstr(t, "\"dimension_types\":[{");
    dn = dn ? strstr(dn, "\"name\":\"") : NULL;
    if (dn) {
        dn += 8;
        size_t k = 0;
        while (dn[k] && dn[k] != '"' && k < sizeof dim - 1) dim[k] = dn[k], k++;
    }
    free(t);
    check_true("annotation_types: a dimension type", dim[0] != 0);
    check_true("annotation_type text Arial 5 mm black: ok", ok(s, "action", "annotation_type", "{\"text\":\"Arial 5 mm black\"}"));
    snprintf(body, sizeof body, "{\"dimension\":\"%s\"}", dim);
    check_true("annotation_type dimension: ok", ok(s, "action", "annotation_type", body));
    static const char *const HOSTILE[] = {
        "{\"text\":\"Comic Sans 5 mm\"}", "{\"text\":5}", "{\"text\":\"\"}", "{\"text\":null,\"dimension\":null}", "{\"font\":null}",
        "[\"text\"]", "not json", "",
    };
    for (size_t i = 0; i < sizeof HOSTILE / sizeof HOSTILE[0]; i++) {
        char what[96];
        snprintf(what, sizeof what, "annotation_type refused: %.40s", HOSTILE[i]);
        check_true(what, !ok(s, "action", "annotation_type", HOSTILE[i]));
    }
    char longer[256]; /* a name of 201 bytes: over CC_VS_KIND_MAX */
    strcpy(longer, "{\"text\":\"");
    memset(longer + 9, 'x', 201);
    strcpy(longer + 210, "\"}");
    check_true("annotation_type refused: a 201-byte name", !ok(s, "action", "annotation_type", longer));
    t = ask(s, "query", "annotation_types", "", &st);
    check_true("after the refusals: the types in use kept", has(t, "\"text\":\"Arial 5 mm black\"") && has(t, dim));
    free(t);

    /* ---- the annotate tool gives them to new texts and dimensions (in the plan) ---- */
    check_true("back to the plan", ok(s, "tab", "", "{\"op\":\"open\",\"tab\":{\"type\":\"view\",\"id\":\"" PLAN1 "\"}}"));
    check_true("annotate text", ok(s, "tool", "annotate", "{\"mode\":\"text\",\"text\":\"Note\"}"));
    char *c1 = ask(s, "tool", "click", "{\"at\":[1000,1000],\"free\":true}", &st);
    int clicked = st == CC_OK;
    free(c1);
    char *an = ask(s, "query", "view_annotations", "", &st);
    check_true("the text placed has the type's kind and 5 mm", clicked && has(an, "\"kind\":\"Arial 5 mm black\"") && has(an, "\"size_mm\":5.0"));
    free(an);
    check_true("annotation_type text null: Default again", ok(s, "action", "annotation_type", "{\"text\":null}"));
    t = ask(s, "query", "annotation_types", "", &st);
    check_true("text in use: null", has(t, "\"text\":null"));
    free(t);

    cc_session_free(s);
    return report("view_sheet");
}
