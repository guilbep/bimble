/* Suite batch (ticket SMALL1): query "batch" (session/calls.c) answers several queries in one call.
 * Each part is byte for byte what the query answers alone with the same body (an answer as
 * {"ok":<its bytes>}, an error as {"error":<its text as a JSON string>}), on golden/frame.json before
 * and after an edit, with and without a selection. Bounded: more than 32 queries, a name outside the
 * list ("batch" itself among them: no recursion), a body that is not {"queries":[…]}, an args that
 * is not a string are refused, and nothing is asked then. */
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "json_w.h"
#include "check.h"

static const char *const NAMES[] = {
    "actions", "annotation_types", "collab", "coordinates", "element", "elements", "foundation_rows",
    "grid_axes", "grids", "history", "level_refusals", "links", "materials", "measure", "measurements",
    "north", "open_views", "opening_rows", "openings", "placing", "project", "reference_planes",
    "schedule_list", "schedules", "setting_out", "sheets", "state", "status", "storeys", "view_annotations",
    "view_clip", "view_references", "view_visibility", "views", "visibility", "visibility_sets",
};
#define NN (sizeof NAMES / sizeof *NAMES)

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_init(out);
    return cc_call(s, op, name, body, strlen(body), out);
}

/* The batch of `n` queries (names[i], bodies[i]) and what it must answer, built from each query asked alone. */
static void compare(cc_session *s, const char *what, const char *const *names, const char *const *bodies, size_t n) {
    Buf req, want, got, one;
    buf_init(&req), buf_init(&want);
    JsonW w;
    jw_init(&w, &req, 0);
    jw_obj_begin(&w);
    jw_key(&w, "queries");
    jw_arr_begin(&w);
    buf_putc(&want, '[');
    for (size_t i = 0; i < n; i++) {
        if (bodies[i]) {
            jw_obj_begin(&w);
            jw_key(&w, "args"), jw_str(&w, bodies[i]);
            jw_key(&w, "name"), jw_str(&w, names[i]);
            jw_obj_end(&w);
        } else jw_str(&w, names[i]);
        int st = call(s, "query", names[i], bodies[i] ? bodies[i] : "", &one);
        if (i) buf_putc(&want, ',');
        if (st == CC_OK) buf_puts(&want, "{\"ok\":"), buf_append(&want, one.p, one.len);
        else {
            Buf t;
            buf_init(&t);
            JsonW e;
            jw_init(&e, &t, 0);
            jw_str_n(&e, one.p ? one.p : "", one.len);
            jw_finish(&e);
            buf_puts(&want, "{\"error\":"), buf_append(&want, t.p, t.len);
            buf_free(&t);
        }
        buf_putc(&want, '}');
        buf_free(&one);
    }
    buf_putc(&want, ']');
    jw_arr_end(&w);
    jw_obj_end(&w);
    jw_finish(&w);
    int st = call(s, "query", "batch", buf_cstr(&req), &got);
    check_true(what, st == CC_OK);
    check_eq(what, got.p, got.len, want.p, want.len);
    buf_free(&req), buf_free(&want), buf_free(&got);
}

static void every(cc_session *s, const char *what) {
    const char *bodies[NN];
    for (size_t i = 0; i < NN; i++) bodies[i] = NULL;
    /* the empty body for the first 32, then the same names with the page's "{}" and some arguments */
    compare(s, what, NAMES, bodies, 32);
    const char *rest[NN];
    size_t k = 0;
    for (size_t i = 32; i < NN; i++) rest[k] = NAMES[i], bodies[k++] = "{}";
    rest[k] = "element", bodies[k++] = "{\"id\":\"0000000000000000000000\"}"; /* an error part */
    rest[k] = "coordinates", bodies[k++] = "{\"at\":[1000,2000,0]}";
    rest[k] = "state", bodies[k++] = "not json";
    compare(s, what, rest, bodies, k);
}

static void refused(cc_session *s, const char *what, const char *body) {
    Buf b;
    int st = call(s, "query", "batch", body, &b);
    check_true(what, st == CC_ERR && b.len > 0);
    buf_free(&b);
}

int main(int argc, char **argv) {
    char path[4096];
    snprintf(path, sizeof path, "%s/../frame.json", argc > 1 ? argv[1] : "ccore/golden/floats");
    cc_session *s = cc_session_new(cc_platform_native());
    cc_error err;
    if (!s || cc_session_open(s, path, &err)) return printf("open: %s\n", s ? err.msg : "no session"), 1;

    every(s, "batch at rev 1: each part as the query alone");
    Buf b;
    call(s, "query", "elements", "{}", &b);
    const char *id = strstr(buf_cstr(&b), "\"id\":\"");
    char sel[64] = "[]";
    if (id) snprintf(sel, sizeof sel, "[\"%.22s\"]", id + 6);
    buf_free(&b);
    check_true("an element to select", id != NULL);
    call(s, "select", "-", sel, &b), buf_free(&b);
    every(s, "batch with a selection");
    char del[96];
    snprintf(del, sizeof del, "{\"ids\":%s}", sel);
    int st = call(s, "verb", "delete", del, &b);
    if (st != CC_OK) printf("delete: %s\n", buf_cstr(&b));
    buf_free(&b);
    check_true("an edit (delete the selected element)", st == CC_OK && cc_session_rev(s) == 2);
    every(s, "batch after the edit");

    /* bounds: nothing asked, an error answered */
    char big[4096] = "{\"queries\":[";
    for (int i = 0; i < 33; i++) strcat(big, i ? ",\"state\"" : "\"state\"");
    strcat(big, "]}");
    refused(s, "33 queries refused", big);
    big[strlen(big) - 2 - strlen(",\"state\"")] = 0, strcat(big, "]}");
    Buf ok;
    check_true("32 queries answered", call(s, "query", "batch", big, &ok) == CC_OK && ok.p[0] == '[');
    buf_free(&ok);
    refused(s, "batch inside batch refused", "{\"queries\":[\"state\",\"batch\"]}");
    refused(s, "an unknown name refused", "{\"queries\":[\"state\",\"no_such\"]}");
    refused(s, "a name outside the list refused (settings reads a file)", "{\"queries\":[\"settings\"]}");
    refused(s, "args not a string refused", "{\"queries\":[{\"name\":\"element\",\"args\":{\"id\":\"x\"}}]}");
    refused(s, "no queries refused", "{}");
    refused(s, "not JSON refused", "[");
    refused(s, "an empty body refused", "");
    Buf e;
    check_true("an empty list answers []", call(s, "query", "batch", "{\"queries\":[]}", &e) == CC_OK && e.len == 2 && !memcmp(e.p, "[]", 2));
    buf_free(&e);
    cc_session_free(s);
    return report("batch");
}
