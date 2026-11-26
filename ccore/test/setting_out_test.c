/* Suite setting_out (ticket SETOUT, ADR 0075, session/setting_out.h): the Setting out tab's data
 * on the C core, through cc_call as the page and the host use it.
 *   - query setting_out: the project base point at the world origin with its survey coordinates
 *     (the georeference's east, north, height), project zero = the height, north directions, the
 *     survey point (at the base point until set);
 *   - actions show_base_point / show_survey_point: pressed when shown, session state (rev kept);
 *   - action view_true_north: off while true north is project north; with a turn, in 3D it is
 *     the section box's orientation (ISS71), in a plan one set_view step (undoable);
 *   - verb georef_set: angles brought into (-180, 180], no -0.0; survey_point_mm set, removed,
 *     refused out of range; one undo step each;
 *   - the file: a model without a survey point saves to the same bytes as before (no key); with
 *     one, the key "survey_point_mm" after Rust's six, read back equal (JSON and CBOR v2);
 *     hostile values refused on load (non-finite, out of range, wrong length, wrong type).
 * Hand-computed expectations (no Rust reference: Rust has none of this, DEVIATIONS 32). */
#include <stdlib.h>
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "check.h"
#include "file.h"
#include "georef.h"

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

/* The action's `"enabled"` (key 'e') or `"pressed"` (key 'p') flag. */
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

static long rev(cc_session *s) { return (long)s->rev; }

static void check_str(const char *what, const char *got, const char *want) { check_eq(what, got, strlen(got), want, strlen(want)); }

static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

static void load_err(const char *what, const char *georef_json) {
    char text[512];
    snprintf(text, sizeof text,
             "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"1ECFGZP4n20ADaVDzRQc6J\",\"project_name\":\"x\","
             "\"storeys\":[],\"materials\":[],\"elements\":[],\"georef\":%s}}",
             georef_json);
    cc_model m;
    cc_error e;
    int rc = cc_model_load(&m, text, strlen(text), &e);
    check_true(what, rc != 0);
    if (rc == 0) cc_model_free(&m);
}

int main(void) {
    const cc_platform *pf = cc_platform_native();
    cc_session *s = cc_session_new(pf);
    int st;
    char *q = ask(s, "query", "setting_out", "", &st);
    check_true("no model: setting_out null", st == CC_OK && !strcmp(q, "null"));
    free(q);
    check_true("no model: show_base_point off", flag(s, "show_base_point", 'e') == 0);

    cc_error err;
    if (cc_session_open(s, "golden/frame.json", &err)) {
        printf("open: %s\n", err.msg);
        return 1;
    }
    /* The model's file bytes before anything: saved again they must be the same. */
    Buf before;
    buf_init(&before);
    cc_model_save(&s->model, &before);

    q = ask(s, "query", "setting_out", "", &st);
    check_str("frame.json: base point at the origin, no georeference", q,
              "{\"base_point\":{\"at_mm\":[0.0,0.0,0.0],\"shown\":false,\"survey_m\":[0.0,0.0,0.0]},\"crs\":null,"
              "\"north\":{\"project\":[0.0,1.0],\"rotation_deg\":0.0,\"true\":[0.0,1.0]},\"project_zero_m\":0.0,"
              "\"survey_point\":{\"at_mm\":[0.0,0.0,0.0],\"shown\":false,\"survey_m\":[0.0,0.0,0.0],\"set\":false}}");
    free(q);

    /* Markers: session state, the rev does not move. */
    long r0 = rev(s);
    check_true("show_base_point enabled, not pressed", flag(s, "show_base_point", 'e') == 1 && flag(s, "show_base_point", 'p') == 0);
    free(ask(s, "action", "show_base_point", "", &st));
    check_true("show_base_point -> pressed, rev kept", st == CC_OK && flag(s, "show_base_point", 'p') == 1 && rev(s) == r0);
    q = ask(s, "query", "setting_out", "", &st);
    check_true("query: base point shown", has(q, "\"base_point\":{\"at_mm\":[0.0,0.0,0.0],\"shown\":true"));
    free(q);
    free(ask(s, "action", "show_survey_point", "", &st));
    q = ask(s, "query", "setting_out", "", &st);
    check_true("show_survey_point -> pressed, shown in the query", flag(s, "show_survey_point", 'p') == 1 && has(q, "\"survey_point\":{\"at_mm\":[0.0,0.0,0.0],\"shown\":true"));
    free(q);
    free(ask(s, "action", "show_base_point", "", &st));
    check_true("show_base_point again -> not pressed", flag(s, "show_base_point", 'p') == 0);

    /* North: off with no turn. */
    check_true("view_true_north off without a turn", flag(s, "view_true_north", 'e') == 0);
    q = ask(s, "verb", "georef_set", "{\"crs\":\"EPSG:2056\",\"east_m\":2600000,\"north_m\":1200000,\"height_m\":432.8,\"rotation_deg\":30}", &st);
    check_true("georef_set -> ok, one step", st == CC_OK);
    free(q);
    check_true("view_true_north on with a 30 degree turn, not pressed", flag(s, "view_true_north", 'e') == 1 && flag(s, "view_true_north", 'p') == 0);
    q = ask(s, "query", "setting_out", "", &st);
    /* survey of the origin = (east, north, height); plan turn project = 30° -> up = (sin 30°, cos 30°). */
    check_true("base point survey = (2600000, 1200000, 432.8)", has(q, "\"survey_m\":[2600000.0,1200000.0,432.8]"));
    check_true("project zero = 432.8 m", has(q, "\"project_zero_m\":432.8"));
    check_true("crs EPSG:2056, rotation 30", has(q, "\"crs\":\"EPSG:2056\"") && has(q, "\"rotation_deg\":30.0"));
    check_true("project north up = (sin 30, cos 30)", has(q, "\"project\":[0.49999999999999994,0.8660254037844387]") || has(q, "\"project\":[0.5,0.8660254037844386]") || has(q, "\"project\":[0.49999999999999994,0.8660254037844386]") || has(q, "\"project\":[0.5,0.8660254037844387]"));
    check_true("true north up = (0, 1) (no map turn)", has(q, "\"true\":[0.0,1.0]"));
    free(q);
    r0 = rev(s);
    free(ask(s, "action", "view_true_north", "", &st));
    check_true("view_true_north in 3D: the box's orientation, pressed, rev kept", st == CC_OK && flag(s, "view_true_north", 'p') == 1 &&
               flag(s, "box_north_true", 'p') == 1 && rev(s) == r0);
    free(ask(s, "action", "view_true_north", "", &st));
    check_true("view_true_north again: project north", flag(s, "view_true_north", 'p') == 0 && flag(s, "box_north_project", 'p') == 1);

    /* Angles normalised, no -0.0. */
    free(ask(s, "verb", "georef_set", "{\"rotation_deg\":350,\"map_rotation_deg\":-180}", &st));
    check_true("350 -> -10, -180 -> 180", st == CC_OK && s->model.georef.rotation_deg == -10.0 && s->model.georef.map_rotation_deg == 180.0);
    free(ask(s, "verb", "georef_set", "{\"rotation_deg\":-0.0,\"map_rotation_deg\":-360}", &st));
    check_true("-0.0 -> +0.0, -360 -> 0", st == CC_OK && s->model.georef.rotation_deg == 0.0 && 1.0 / s->model.georef.rotation_deg > 0 &&
               s->model.georef.map_rotation_deg == 0.0 && 1.0 / s->model.georef.map_rotation_deg > 0);
    q = ask(s, "verb", "georef_set", "{\"rotation_deg\":400}", &st);
    check_true("400 degrees refused", st != CC_OK && has(q, "at most 360 degrees"));
    free(q);

    /* The survey point. */
    r0 = rev(s);
    free(ask(s, "verb", "georef_set", "{\"survey_point_mm\":[12000,-5000,1500]}", &st));
    q = ask(s, "query", "setting_out", "", &st);
    if (rev(s) != r0 + 1) printf("  got %s (rev %ld, was %ld)\n", q, rev(s), r0);
    check_true("survey point set: one step, at its place", rev(s) == r0 + 1 && has(q, "\"survey_point\":{\"at_mm\":[12000.0,-5000.0,1500.0]"));
    check_true("its survey coordinates: E + 12 m, N - 5 m, H + 1.5 m", has(q, "\"survey_m\":[2600012.0,1199995.0,434.3],\"set\":true"));
    free(q);
    q = ask(s, "verb", "georef_set", "{\"survey_point_mm\":[1e8,0,0]}", &st);
    check_true("survey point beyond 1e7 mm refused", st != CC_OK && has(q, "at most 1e7 mm"));
    free(q);
    q = ask(s, "verb", "georef_set", "{\"survey_point_mm\":[1,2]}", &st);
    check_true("survey point of 2 numbers refused", st != CC_OK && has(q, "survey_point_mm"));
    free(q);
    q = ask(s, "verb", "georef_set", "{\"survey_point_mm\":[\"a\",2,3]}", &st);
    check_true("survey point of a string refused", st != CC_OK);
    free(q);

    /* Saved: the key after Rust's six; read back equal, JSON and CBOR. */
    Buf j, c;
    buf_init(&j), buf_init(&c);
    cc_model_save(&s->model, &j);
    check_true("saved: survey_point_mm after height_m", has(buf_cstr(&j), "\"height_m\": 432.8,\n      \"survey_point_mm\": [\n        12000.0,\n        -5000.0,\n        1500.0\n      ]"));
    cc_model back;
    int rc = cc_model_load(&back, j.p, j.len, &err);
    check_true("read back: same georeference", rc == 0 && cc_georef_eq(&back.georef, &s->model.georef));
    if (rc == 0) cc_model_free(&back);
    cc_model_save_v2(&s->model, &c);
    rc = cc_model_load(&back, c.p, c.len, &err);
    check_true("CBOR v2 read back: same georeference", rc == 0 && cc_georef_eq(&back.georef, &s->model.georef));
    if (rc == 0) cc_model_free(&back);
    buf_free(&j), buf_free(&c);

    /* Undo: the survey point goes, then the angles come back. */
    free(ask(s, "action", "undo", "", &st));
    check_true("undo: no survey point", !s->model.georef.has_survey_point);
    free(ask(s, "verb", "georef_set", "{\"survey_point_mm\":[1,2,3]}", &st));
    free(ask(s, "verb", "georef_set", "{\"survey_point_mm\":[]}", &st));
    check_true("[] removes the survey point", st == CC_OK && !s->model.georef.has_survey_point);

    /* Back to no georeference at all: the file's bytes are those of before. */
    free(ask(s, "verb", "georef_set", "{\"clear\":true}", &st));
    Buf after;
    buf_init(&after);
    cc_model_save(&s->model, &after);
    check_true("no georeference again: the same bytes as frame.json saved before", before.len == after.len && !memcmp(before.p, after.p, before.len));
    buf_free(&before), buf_free(&after);

    /* In a plan: view_true_north is one set_view step. */
    free(ask(s, "verb", "georef_set", "{\"rotation_deg\":30}", &st));
    q = ask(s, "query", "open_views", "", &st);
    char *p = strstr(q, "\"kind\":\"plan\""), id[64] = "";
    if (p) { /* the plan's id: back to its object's "id" */
        while (p > q && strncmp(p, "{\"", 2)) p--;
        char *ip = strstr(p, "\"id\":\"");
        if (ip) sscanf(ip + 6, "%63[^\"]", id);
    }
    if (!id[0]) printf("  views: %.300s\n", q);
    free(q);
    char body[160];
    snprintf(body, sizeof body, "{\"op\":\"open\",\"tab\":{\"type\":\"view\",\"id\":\"%s\"}}", id);
    free(ask(s, "tab", "", body, &st));
    int opened = st == CC_OK;
    if (!opened) printf("  open plan %s: failed\n", id);
    r0 = rev(s);
    free(ask(s, "action", "view_true_north", "", &st));
    check_true("plan: view_true_north -> a step, pressed", opened && st == CC_OK && rev(s) == r0 + 1 && flag(s, "view_true_north", 'p') == 1);
    free(ask(s, "action", "undo", "", &st));
    check_true("plan: undo -> project north again", flag(s, "view_true_north", 'p') == 0);

    /* Hostile files. */
    load_err("load: survey_point_mm of 2 numbers refused", "{\"survey_point_mm\":[1,2]}");
    load_err("load: survey_point_mm beyond 1e7 refused", "{\"survey_point_mm\":[1e9,0,0]}");
    load_err("load: survey_point_mm a string refused", "{\"survey_point_mm\":\"x\"}");
    load_err("load: survey_point_mm holding a string refused", "{\"survey_point_mm\":[0,\"x\",0]}");
    load_err("load: survey_point_mm twice refused", "{\"survey_point_mm\":[0,0,0],\"survey_point_mm\":[0,0,0]}");
    load_err("load: survey_point_mm of 1e400 refused", "{\"survey_point_mm\":[1e400,0,0]}");
    load_err("load: an unknown georef key still refused", "{\"survey_point\":[0,0,0]}");

    cc_session_free(s);
    return report("setting_out");
}
