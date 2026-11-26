/* Suite capabilities (ticket FAKE1, session/capabilities.h): `query capabilities` answers one
 * object of booleans, keys sorted, the same with and without a model; and each false row is false
 * because the dispatcher really has nothing behind it (a stub answer, or an action never enabled),
 * each true row because it answers: a row that drifts from the dispatcher fails here. */
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "check.h"

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_init(out);
    return cc_call(s, op, name, body, strlen(body), out);
}

/* The answer as a heap string ("" on an error, the status in *st). */
static char *ask(cc_session *s, const char *op, const char *name, const char *body, int *st) {
    Buf b;
    *st = call(s, op, name, body, &b);
    const char *t = buf_cstr(&b);
    char *z = malloc(strlen(t) + 1);
    strcpy(z, t);
    buf_free(&b);
    return z;
}

/* The capability `name` in the answer `caps`: 1, 0, or -1 when absent. */
static int cap(const char *caps, const char *name) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\":", name);
    const char *p = strstr(caps, k);
    if (!p) return -1;
    p += strlen(k);
    return !strncmp(p, "true", 4) ? 1 : !strncmp(p, "false", 5) ? 0 : -1;
}

/* The action `name` is enabled now. */
static int enabled(cc_session *s, const char *name) {
    int st;
    char *a = ask(s, "query", "actions", "", &st), k[96];
    int on = 0;
    snprintf(k, sizeof k, "\"name\":\"%s\"", name);
    char *p = strstr(a, k);
    if (p) { /* {"enabled":X,"label":"…","name":"<name>"…}: back to the object's start */
        while (p > a && *p != '{') p--;
        on = !strncmp(p, "{\"enabled\":true", 15);
    }
    free(a);
    return on;
}

int main(void) {
    const cc_platform *pf = cc_platform_native();
    cc_session *s = cc_session_new(pf);
    int st;
    char *c0 = ask(s, "query", "capabilities", "", &st);
    check_true("query capabilities -> 200, an object", st == CC_OK && c0[0] == '{');
    /* Keys sorted, values booleans only. */
    {
        const char *p = c0;
        char prev[64] = "", key[64];
        int sorted = 1, n = 0, bools = 1;
        while ((p = strchr(p, '"'))) {
            const char *e = strchr(p + 1, '"');
            size_t l = (size_t)(e - p - 1);
            if (l >= sizeof key) break;
            memcpy(key, p + 1, l), key[l] = 0;
            if (strcmp(prev, key) >= 0) sorted = 0;
            strcpy(prev, key);
            n++;
            if (strncmp(e + 1, ":true", 5) && strncmp(e + 1, ":false", 6)) bools = 0;
            p = e + 1;
        }
        check_true("keys sorted", sorted);
        check_true("values true or false", bools);
        check_true("at least 30 rows", n >= 30);
    }
    for (const char *const *k = (const char *const[]){"about", "base_point_move", "collaboration", "export_pdf", "export_svg", "export_view", "georef", "grids",
                                                      "history", "lca", "levels", "materials", "paper_view", "phases", "plan_annotations",
                                                      "project_info", "quantities", "recent_files", "reference_planes", "references", "setting_out", "schedules", "settings",
                                                      "sheets", "standards", "subprojects", "user", "users", "versions", "views", NULL};
         *k; k++) {
        char what[96];
        snprintf(what, sizeof what, "row %s present", *k);
        check_true(what, cap(c0, *k) >= 0);
    }
    /* The native build: the full core and a file system. */
    check_true("native: quantities, export_pdf, versions true", cap(c0, "quantities") == 1 && cap(c0, "export_pdf") == 1 && cap(c0, "versions") == 1);
    check_true("SEC1: views true (view_add, view_remove, view_edit)", cap(c0, "views") == 1);
    check_true("G2's pages: levels, grids, georef true", cap(c0, "levels") == 1 && cap(c0, "grids") == 1 && cap(c0, "georef") == 1);

    cc_error err;
    if (cc_session_open(s, "golden/frame.json", &err)) {
        printf("open: %s\n", err.msg);
        return 1;
    }
    char *c1 = ask(s, "query", "capabilities", "", &st);
    check_true("the same answer with a model open", !strcmp(c0, c1));

    /* False rows: nothing behind them. */
    char *rf = ask(s, "query", "recent_files", "", &st);
    /* FS1: the native core keeps the recent files (settings_file.h): the row is true and the query answers a list. */
    check_true("native: recent_files true, and query recent_files answers a list", cap(c1, "recent_files") == 1 && st == CC_OK && rf[0] == '[');
    char *sd = ask(s, "query", "standards", "", &st);
    check_true("standards false <=> query standards is null", (cap(c1, "standards") == 0) == !strcmp(sd, "null"));
    char *se = ask(s, "query", "state", "", &st);
    /* SET1: user true: state.user follows Settings → You (null with no name, as Rust's calls.rs:253) */
    check_true("user true: state.user null with no name set", cap(c1, "user") == 1 && strstr(se, "\"user\":null") != NULL);
    {
        char *r = ask(s, "action", "set_settings", "{\"user_name\":\"Ada Lovelace\"}", &st);
        char *se2 = ask(s, "query", "state", "", &st);
        check_true("user true: a name set -> state.user", strstr(se2, "\"user\":{\"email\":null,\"name\":\"Ada Lovelace\",\"trigram\":\"AL\"}") != NULL);
        free(r), free(se2);
    }
    check_true("settings false <=> action settings off", (cap(c1, "settings") == 0) == !enabled(s, "settings"));
    check_true("about false <=> action about off", (cap(c1, "about") == 0) == !enabled(s, "about"));
    check_true("materials false <=> action materials off", (cap(c1, "materials") == 0) == !enabled(s, "materials"));
    { /* MAT1: materials true, the action on with a model (ribbon.rs:611 `_ => loaded`), the page's data served */
        char *mp = ask(s, "query", "materials_page", "", &st);
        check_true("MAT1: materials true, action on, query materials_page answers",
                   cap(c1, "materials") == 1 && enabled(s, "materials") && st == CC_OK && !strncmp(mp, "{\"editable\":true,", 17));
        free(mp);
    }
    check_true("collaboration false <=> action shared_folder off", (cap(c1, "collaboration") == 0) == !enabled(s, "shared_folder"));
    /* PAPER1: export_view and paper_view are served now: Export view is enabled with a model, and
     * view_svg draws a view (false before PAPER1). */
    check_true("export_view true <=> action on_paper on", cap(c1, "export_view") == 1 && enabled(s, "on_paper"));
    {
        char *vs = ask(s, "query", "view_svg", "{\"view\":\"nope\"}", &st);
        check_true("paper_view true <=> query view_svg is served (an unknown view's error)",
                   cap(c1, "paper_view") == 1 && st == CC_ERR && !strncmp(vs, "no view with id", 15));
        free(vs);
    }
    check_true("plan_annotations false <=> action section_labels off", (cap(c1, "plan_annotations") == 0) == !enabled(s, "section_labels"));
    /* True rows: they answer. */
    char *sl = ask(s, "query", "schedule_list", "", &st);
    check_true("quantities true <=> schedule_list lists tables", (cap(c1, "quantities") == 1) == (st == CC_OK && strlen(sl) > 2));
    char *pj = ask(s, "query", "project", "", &st);
    check_true("project_info true: query project answers", cap(c1, "project_info") == 1 && st == CC_OK);
    char *sh = ask(s, "query", "sheets", "", &st);
    check_true("sheets true: query sheets answers", cap(c1, "sheets") == 1 && st == CC_OK && sh[0] == '[');
    check_true("history true: action history on with a model (FAKE1)", cap(c1, "history") == 1 && enabled(s, "history"));
    { /* SETOUT: setting_out true: the query answers and the markers' actions are on with a model */
        char *so = ask(s, "query", "setting_out", "", &st);
        check_true("SETOUT: setting_out true <=> query setting_out answers, show_base_point on",
                   cap(c1, "setting_out") == 1 && st == CC_OK && !strncmp(so, "{\"base_point\":", 14) && enabled(s, "show_base_point"));
        free(so);
        char *rp = ask(s, "query", "reference_planes", "", &st);
        check_true("WAVE1B: reference_planes true <=> query reference_planes answers, action reference_planes on",
                   cap(c1, "reference_planes") == 1 && st == CC_OK && !strncmp(rp, "{\"active\":", 10) && enabled(s, "reference_planes"));
        free(rp);
        char *bm = ask(s, "action", "base_point_move", "", &st);
        check_true("SETOUT: base_point_move false <=> no such action", cap(c1, "base_point_move") == 0 && st == CC_ERR);
        free(bm);
    }
    free(c0), free(c1), free(rf), free(sd), free(se), free(sl), free(pj), free(sh);
    cc_session_free(s);
    return report("capabilities");
}
