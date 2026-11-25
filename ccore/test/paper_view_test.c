/* Suite paper_view (ticket PAPER1): a view On paper, Export view, several sheets as one PDF,
 * through cc_call as the native host and the wasm module call them.
 *   - `query view_svg {view}` of every effective view of golden/drawing/model.json (a copy named
 *     frame.json: the project), with the test clock (2026-01-01) and the version Rust's goldens
 *     name: its "svg" is D4's golden/drawing/view/<nn>.svg byte for byte (ccore_golden.rs
 *     drawing_views: the default template with the project, the section marks of every view), its
 *     "paper" and "portrait" the <nn>.info.txt's; a view that cannot be drawn gives index.txt's
 *     error. `bare`: paper.rs's On paper picture (no frame: smaller, still titled).
 *   - The switch: paper_view is enabled on a plan or a section shown (ribbon.rs:1478), not on the
 *     3D view, not with a sheet shown; running it puts the view On paper (open_views paper.on,
 *     the action pressed, the change JSON's view[6]); again: Model. Measuring switches back to
 *     Model (paper.rs:217). The view keeps its picture per view (paper_views). Export view
 *     (on_paper) is enabled with a model and toggles its window (pressed).
 *   - `query sheets_pdf`: one sheet is sheet_pdf's file byte for byte; S-101 then S-102 is the
 *     C-own golden golden/paper_c/S-101+S-102-helvetica.pdf (2 pages, one pair of fonts; no Rust
 *     oracle, DEVIATIONS 22); the errors.
 * Fails on the code before PAPER1: view_svg and sheets_pdf were unknown queries, paper_view was
 * never enabled and open_views said paper.on false always. Set PAPER1_WRITE=1 to write the 2-page
 * golden again (then check it with qpdf --check and pdftotext). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h> /* rmdir */
#include "../host/host.h" /* cc_platform_native */
#include "../session/calls.h"
#include "../session/session.h"
#include "../session/sheet_svg.h"
#include "../src/buf.h"
#include "../src/json_w.h"
#include "../src/model.h"
#include "check.h"

#define G "golden/drawing"
#define RUST_VERSION "0.18.1"
#define DIR "out/test-paper_view"
#define TWO "golden/paper_c/S-101+S-102-helvetica.pdf"

static int64_t new_year(void *ctx) { return (void)ctx, 1767225600; /* 2026-01-01 00:00 UTC */ }

static const char *find(const char *p, size_t n, const char *s) {
    size_t k = strlen(s);
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp(p + i, s, k)) return p + i;
    return NULL;
}

static int count(const char *p, size_t n, const char *s) {
    int c = 0;
    size_t k = strlen(s);
    for (size_t i = 0; i + k <= n; i++) c += !memcmp(p + i, s, k);
    return c;
}

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_clear(out);
    out->oom = 0;
    return cc_call(s, op, name, body, strlen(body), out);
}

/* "<name>":<enabled> / pressed of action `name` in `query actions`. */
static int action(cc_session *s, const char *name, const char *field) {
    Buf b;
    buf_init(&b);
    call(s, "query", "actions", "", &b);
    char k[96], f[32];
    snprintf(k, sizeof k, "\"name\":\"%s\"", name);
    snprintf(f, sizeof f, "\"%s\":true", field);
    const char *p = find(b.p, b.len, k);
    int on = -1;
    if (p) {
        const char *o = p, *e = p;
        while (o > b.p && *o != '{') o--;
        while (e < b.p + b.len && *e != '}') e++;
        on = find(o, (size_t)(e - o), f) != NULL;
    }
    buf_free(&b);
    return on;
}

static int paper_on(cc_session *s) {
    Buf b;
    buf_init(&b);
    call(s, "query", "open_views", "", &b);
    int on = find(b.p, b.len, "\"paper\":{\"offered\":true,\"on\":true}") != NULL;
    buf_free(&b);
    return on;
}

static void test_svg(cc_session *ses) {
    size_t n;
    char *index = slurp(G, "view/index.txt", &n);
    check_true("view/index.txt", index != NULL);
    if (!index) return;
    Buf got, want, body;
    buf_init(&got), buf_init(&want), buf_init(&body);
    int drawn = 0, errors = 0;
    for (char *line = index, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char nn[3], id[23];
        if (strlen(line) < 27 || line[2] != ' ' || line[25] != ' ' || line[26] != '"') continue; /* "<nn> <id> \"name\"" */
        memcpy(nn, line, 2), nn[2] = 0;
        memcpy(id, line + 3, 22), id[22] = 0;
        buf_clear(&body);
        buf_puts(&body, "{\"view\":\""), buf_puts(&body, id), buf_puts(&body, "\"}");
        int rc = call(ses, "query", "view_svg", buf_cstr(&body), &got);
        char path[64], what[96];
        snprintf(path, sizeof path, "view/%s.svg", nn);
        size_t gn;
        char full[96];
        snprintf(full, sizeof full, G "/%s", path);
        FILE *gf = fopen(full, "rb");
        if (gf) fclose(gf);
        char *g = gf ? slurp(G, path, &gn) : NULL;
        if (!g) { /* index.txt's error line "<nn> error: <text>" */
            const char *e = strstr(next ? next : "", "error: ");
            char err[160] = "";
            for (const char *l = next; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
                if (!strncmp(l, nn, 2) && !strncmp(l + 2, " error: ", 8)) {
                    size_t k = 0;
                    for (const char *c = l + 10; *c && *c != '\n' && k + 1 < sizeof err; c++) err[k++] = *c;
                    err[k] = 0;
                    break;
                }
            (void)e;
            snprintf(what, sizeof what, "view %s: index.txt's error", nn);
            check_eq(what, got.p, got.len, err, strlen(err));
            check_true("an error is CC_ERR", rc == CC_ERR);
            errors++;
            continue;
        }
        buf_clear(&want);
        JsonW w;
        jw_init(&w, &want, 0);
        jw_obj_begin(&w);
        jw_key(&w, "svg"), jw_str_n(&w, g, gn);
        jw_obj_end(&w);
        jw_finish(&w);
        free(g);
        /* {"height_mm":…,"paper":…,"portrait":…,"scale":…,"svg":"…","width_mm":…}: the svg field
         * is the golden's JSON text ("{"svg": … "}" without its braces). */
        const char *sv = find(got.p, got.len, "\"svg\":");
        size_t wl = want.len - 2;
        snprintf(what, sizeof what, "view %s: view_svg = drawing/view/%s.svg", nn, nn);
        check_true(what, rc == CC_OK && sv && (size_t)(got.p + got.len - sv) > wl && !memcmp(sv, want.p + 1, wl) &&
                             sv[wl] == ',');
        if (!(rc == CC_OK && sv && !memcmp(sv, want.p + 1, wl))) printf("  %.*s\n", (int)(got.len < 200 ? got.len : 200), got.p);
        /* The paper and the orientation of <nn>.info.txt. */
        snprintf(path, sizeof path, "view/%s.info.txt", nn);
        char *info = slurp(G, path, &gn);
        char ppr[48];
        const char *pl = info ? strstr(info, "paper: ") : NULL;
        int portrait = info && strstr(info, "portrait: true") != NULL;
        if (pl) {
            size_t k = 0;
            for (pl += 7; *pl && *pl != ',' && k + 1 < 8; pl++) k++;
            snprintf(ppr, sizeof ppr, "\"paper\":\"%.*s\",\"portrait\":%s", (int)k, pl - k, portrait ? "true" : "false");
            snprintf(what, sizeof what, "view %s: paper and portrait of info.txt", nn);
            check_true(what, find(got.p, got.len, ppr) != NULL);
        }
        free(info);
        drawn++;
    }
    check_true("views drawn: 9 (of 11: the 3D view and an empty ceiling give errors)", drawn == 9 && errors == 2);
    free(index);
    buf_free(&want), buf_free(&body);

    /* The On paper picture: bare (no frame, title block, legend), still titled. */
    const char *rdc = "1J608tQTSX8dmrRXtg5W_S"; /* 01 "Plan Rez-de-chaussée" */
    char b1[96], b2[96];
    snprintf(b1, sizeof b1, "{\"view\":\"%s\"}", rdc);
    snprintf(b2, sizeof b2, "{\"view\":\"%s\",\"bare\":true}", rdc);
    Buf full;
    buf_init(&full);
    call(ses, "query", "view_svg", b1, &full);
    int rc = call(ses, "query", "view_svg", b2, &got);
    check_true("bare: answers", rc == CC_OK);
    check_true("bare: smaller than the sheet", got.len < full.len);
    check_true("bare: the view's title", find(got.p, got.len, "Plan Rez-de-chaussée, 1:100") != NULL);
    check_true("bare: no frame note", find(got.p, got.len, "Bimble " RUST_VERSION) == NULL && find(full.p, full.len, "Bimble " RUST_VERSION));
    snprintf(b2, sizeof b2, "{\"view\":\"%s\",\"font\":\"helvetica\"}", rdc);
    rc = call(ses, "query", "view_svg", b2, &got);
    check_true("font helvetica: the SVG names Helvetica", rc == CC_OK && find(got.p, got.len, "font-family=\\\"Helvetica"));
    buf_free(&full);
    /* Errors. */
    rc = call(ses, "query", "view_svg", "{\"view\":\"nope\"}", &got);
    check_eq("unknown view", got.p, got.len, "no view with id \"nope\"", 22);
    check_true("unknown view: CC_ERR", rc == CC_ERR);
    rc = call(ses, "query", "view_svg", "{}", &got);
    check_eq("missing view", got.p, got.len, "missing field `view`", 20);
    snprintf(b2, sizeof b2, "{\"view\":\"%s\",\"bare\":1}", rdc);
    rc = call(ses, "query", "view_svg", b2, &got);
    check_eq("bare: a number", got.p, got.len, "bare: true or false", 19);
    snprintf(b2, sizeof b2, "{\"view\":\"%s\",\"font\":\"arial\"}", rdc);
    rc = call(ses, "query", "view_svg", b2, &got);
    check_eq("font arial", got.p, got.len, "font: \"helvetica\" or \"roboto\"", 29);
    buf_free(&got);
}

static void test_switch(cc_session *ses) {
    Buf b;
    buf_init(&b);
    const char *rdc = "1J608tQTSX8dmrRXtg5W_S", *three_d = "1v7Vtm2FfafmYPKIQEXnlY", *sec = "06bbYdqnUIivUb3ybSfqfB";
    char t[128];
    snprintf(t, sizeof t, "{\"op\":\"open\",\"tab\":{\"type\":\"view\",\"id\":\"%s\"}}", three_d);
    check_true("open the 3D view", call(ses, "tab", "", t, &b) == CC_OK);
    check_true("3D: paper_view disabled", action(ses, "paper_view", "enabled") == 0);
    check_true("3D: running it is refused", call(ses, "action", "paper_view", "", &b) == CC_ERR);
    check_true("Export view (on_paper) enabled with a model", action(ses, "on_paper", "enabled") == 1);
    snprintf(t, sizeof t, "{\"op\":\"open\",\"tab\":{\"type\":\"view\",\"id\":\"%s\"}}", rdc);
    check_true("open the plan", call(ses, "tab", "", t, &b) == CC_OK);
    check_true("plan: paper_view enabled, not pressed", action(ses, "paper_view", "enabled") == 1 && action(ses, "paper_view", "pressed") == 0);
    check_true("plan: offered, Model", !paper_on(ses));
    check_true("paper_view runs", call(ses, "action", "paper_view", "", &b) == CC_OK);
    check_true("On paper: open_views paper.on", paper_on(ses));
    check_true("On paper: pressed", action(ses, "paper_view", "pressed") == 1);
    cc_change_json(ses, &b);
    check_true("On paper: the change JSON's view[6] is 1", find(b.p, b.len, ",1,1]") != NULL || find(b.p, b.len, ",1,2]") != NULL);
    /* Another view: Model there; back: still On paper (per view). */
    snprintf(t, sizeof t, "{\"op\":\"open\",\"tab\":{\"type\":\"view\",\"id\":\"%s\"}}", sec);
    call(ses, "tab", "", t, &b);
    check_true("a section: offered, Model", action(ses, "paper_view", "enabled") == 1 && !paper_on(ses));
    snprintf(t, sizeof t, "{\"op\":\"activate\",\"tab\":{\"type\":\"view\",\"id\":\"%s\"}}", rdc);
    check_true("back to the plan", call(ses, "tab", "", t, &b) == CC_OK);
    check_true("the plan is still On paper", paper_on(ses));
    /* Measuring is done on the model (paper.rs:217). */
    check_true("measure runs", call(ses, "action", "measure", "", &b) == CC_OK);
    check_true("measuring: Model", !paper_on(ses) && action(ses, "paper_view", "pressed") == 0);
    call(ses, "action", "measure", "", &b);
    check_true("measure off: stays Model", !paper_on(ses));
    call(ses, "action", "paper_view", "", &b);
    check_true("On paper again", paper_on(ses));
    call(ses, "action", "paper_view", "", &b);
    check_true("paper_view again: Model", !paper_on(ses));
    /* Export view toggles its window (pressed). */
    call(ses, "action", "on_paper", "", &b);
    check_true("on_paper: pressed (its window)", action(ses, "on_paper", "pressed") == 1);
    call(ses, "action", "on_paper", "", &b);
    check_true("on_paper again: not pressed", action(ses, "on_paper", "pressed") == 0);
    buf_free(&b);
}

static void test_sheets_pdf(cc_session *ses) {
    const cc_model *m = cc_session_model(ses);
    const cc_sheet *s1 = NULL, *s2 = NULL;
    for (size_t i = 0; i < m->nsheets; i++) {
        if (m->sheets[i].number.n == 5 && !memcmp(m->sheets[i].number.p, "S-101", 5)) s1 = &m->sheets[i];
        if (m->sheets[i].number.n == 5 && !memcmp(m->sheets[i].number.p, "S-102", 5)) s2 = &m->sheets[i];
    }
    check_true("S-101, S-102", s1 && s2);
    if (!s1 || !s2) return;
    char body[200];
    Buf one, got;
    buf_init(&one), buf_init(&got);
    snprintf(body, sizeof body, "{\"sheet\":\"%.22s\"}", s1->id.c);
    call(ses, "query", "sheet_pdf", body, &one);
    snprintf(body, sizeof body, "{\"sheets\":[\"%.22s\"]}", s1->id.c);
    int rc = call(ses, "query", "sheets_pdf", body, &got);
    check_true("sheets_pdf of one sheet = sheet_pdf's file", rc == CC_OK && got.len == one.len && !memcmp(got.p, one.p, one.len));
    snprintf(body, sizeof body, "{\"sheets\":[\"%.22s\",\"%.22s\"]}", s1->id.c, s2->id.c);
    rc = call(ses, "query", "sheets_pdf", body, &got);
    check_true("two sheets: answers", rc == CC_OK);
    check_true("two sheets: %PDF- … %%EOF", got.len > 16 && !memcmp(got.p, "%PDF-", 5) && !memcmp(got.p + got.len - 5, "%%EOF", 5));
    check_true("two sheets: /Count 2, two pages", find(got.p, got.len, "/Count 2") && count(got.p, got.len, "/Type /Page\n") == 2);
    check_true("two sheets: one pair of fonts", count(got.p, got.len, "/BaseFont /Helvetica\n") == 1 &&
                                                   count(got.p, got.len, "/BaseFont /Helvetica-Bold\n") == 1);
    check_true("two sheets: both numbers", find(got.p, got.len, "(S-101)") && find(got.p, got.len, "(S-102)"));
    if (getenv("PAPER1_WRITE")) {
        mkdir("golden/paper_c", 0755);
        FILE *f = fopen(TWO, "wb");
        check_true("write " TWO, f && fwrite(got.p, 1, got.len, f) == got.len);
        if (f) fclose(f);
    }
    size_t gn = 0;
    FILE *tf = fopen(TWO, "rb");
    if (tf) fclose(tf);
    char *g = tf ? slurp("golden/paper_c", "S-101+S-102-helvetica.pdf", &gn) : NULL;
    check_eq("two sheets = " TWO, got.p, got.len, g ? g : "", g ? gn : 0);
    free(g);
    /* A path: written there. */
    snprintf(body, sizeof body, "{\"sheets\":[\"%.22s\",\"%.22s\"],\"path\":\"" DIR "/two.pdf\"}", s1->id.c, s2->id.c);
    Buf ans;
    buf_init(&ans);
    rc = call(ses, "query", "sheets_pdf", body, &ans);
    char want[120];
    snprintf(want, sizeof want, "{\"bytes\":%zu,\"pages\":2,\"path\":\"" DIR "/two.pdf\"}", got.len);
    check_eq("path: the answer", ans.p, ans.len, want, strlen(want));
    size_t fn;
    char *file = slurp(DIR, "two.pdf", &fn);
    check_true("path: the same bytes", file && fn == got.len && !memcmp(file, got.p, fn));
    free(file);
    remove(DIR "/two.pdf");
    /* Roboto (the shipped files from the folder): one subset of both pages' glyphs per weight. */
    check_true("font dir", cc_paper_set_font_dir(ses, "../crates/bimble-drawing/fonts") == 0);
    snprintf(body, sizeof body, "{\"sheets\":[\"%.22s\",\"%.22s\"],\"font\":\"roboto\"}", s1->id.c, s2->id.c);
    rc = call(ses, "query", "sheets_pdf", body, &ans);
    check_true("roboto: two pages, two embedded faces", rc == CC_OK && find(ans.p, ans.len, "/Count 2") &&
                                                           count(ans.p, ans.len, "/FontFile2 ") == 2 &&
                                                           count(ans.p, ans.len, "/Type /Page\n") == 2);
    if (getenv("PAPER1_WRITE")) {
        FILE *rf = fopen(DIR "/two-roboto.pdf", "wb"); /* for qpdf --check by hand */
        if (rf) fwrite(ans.p, 1, ans.len, rf), fclose(rf);
    }
    cc_paper_set_font_dir(ses, NULL);
    /* Errors. */
    rc = call(ses, "query", "sheets_pdf", "{}", &ans);
    check_eq("missing sheets", ans.p, ans.len, "missing field `sheets`", 22);
    rc = call(ses, "query", "sheets_pdf", "{\"sheets\":[]}", &ans);
    check_eq("no sheet", ans.p, ans.len, "sheets: at least one sheet", 26);
    rc = call(ses, "query", "sheets_pdf", "{\"sheets\":\"x\"}", &ans);
    check_eq("sheets a string", ans.p, ans.len, "sheets: an array of sheet ids", 29);
    rc = call(ses, "query", "sheets_pdf", "{\"sheets\":[\"nope\"]}", &ans);
    check_eq("unknown sheet", ans.p, ans.len, "no sheet with id \"nope\"", 23);
    check_true("unknown sheet: CC_ERR", rc == CC_ERR);
    buf_free(&one), buf_free(&got), buf_free(&ans);
}

int main(void) {
    size_t n;
    char *json = slurp(G, "model.json", &n);
    mkdir("out", 0755);
    mkdir(DIR, 0755);
    const char *copy = DIR "/frame.json";
    FILE *f = fopen(copy, "wb");
    check_true("copy model.json", f && json && fwrite(json, 1, n, f) == n);
    if (f) fclose(f);
    free(json);
    cc_platform pf = *cc_platform_native();
    pf.unix_s = new_year;
    cc_session *ses = cc_session_new(&pf);
    cc_error e;
    if (!ses || cc_session_open(ses, copy, &e)) {
        check_true("frame.json opens", 0);
        return report("paper_view");
    }
    check_true("set version", cc_paper_set_version(ses, RUST_VERSION) == 0);
    test_svg(ses);
    test_switch(ses);
    test_sheets_pdf(ses);
    cc_session_free(ses);
    remove(copy);
    if (!getenv("PAPER1_WRITE")) rmdir(DIR);
    return report("paper_view");
}
