/* Suite sheet_pdf: the PDF export in the app (ticket D7, session/sheet_svg.h cc_paper_pdf_query),
 * through cc_call as the native host and the wasm module call it.
 *   - Without fonts, with the test clock (2026-01-01), the file named frame.json (the project) and
 *     the version Rust's goldens name: `query sheet_pdf` of S-101 is golden/drawing/pdf/
 *     S-101-helvetica.pdf byte for byte once the producer is masked (D6's comparison): the
 *     Helvetica fallback is Rust's own Helvetica file. Font files that are not the shipped ones
 *     fall back the same way.
 *   - SIZE1: Helvetica is the default even with the Roboto files at hand (the folder set): the
 *     same golden bytes; `{"font":"roboto"}` without files: the Helvetica golden too; a font that is
 *     neither: an error. `pdf_fonts`: {"helvetica":true,"roboto":false} without the files or with
 *     other files, true once the folder holds the shipped ones.
 *   - `{"font":"roboto"}` with the shipped Roboto files (read from the folder): %PDF- … %%EOF,
 *     the object count of D6's session check (15: two embedded faces), both FontFile2 streams.
 *   - {"path"}: the same bytes written to the file, the answer {"bytes", "path"}; a folder that
 *     does not exist: "<file>: cannot write".
 *   - `view_pdf` of a plan view: a PDF; the errors (unknown sheet or view, missing field, a path
 *     that is not a string).
 * Fails on the code before D7: `sheet_pdf` was an unknown query; before SIZE1: the folder made
 * Roboto the default, and `pdf_fonts` was an unknown query. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h> /* rmdir */
#ifdef _WIN32
#include <io.h>
#define mkdir(path, mode) mkdir(path) /* WIN2: no mode argument there */
#endif
#include "../host/host.h" /* cc_platform_native */
#include "../session/calls.h"
#include "../session/session.h"
#include "../session/sheet_svg.h"
#include "../src/buf.h"
#include "../src/model.h"
#include "check.h"

#define G "golden/drawing"
#define FONTS "fonts"
#define RUST_VERSION "0.18.1" /* the goldens' producer (pdf_out_test.c) */
#define DIR "out/test-sheet_pdf"

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

/* pdf_out_test.c's mask: "(Bimble 0.18.1)" -> "(Bimble xxxxxx)". */
static void mask(Buf *b) {
    const char *v = "(Bimble " RUST_VERSION ")";
    size_t n = strlen(v);
    for (size_t i = 0; i + n <= b->len; i++)
        if (!memcmp(b->p + i, v, n)) {
            memset(b->p + i + 8, 'x', n - 9);
            return;
        }
}

static int is_pdf(const Buf *b) {
    return b->len > 16 && !memcmp(b->p, "%PDF-", 5) && !memcmp(b->p + b->len - 5, "%%EOF", 5);
}

static int call(cc_session *s, const char *name, const char *body, Buf *out) {
    buf_clear(out);
    out->oom = 0;
    return cc_call(s, "query", name, body, strlen(body), out);
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
        return report("sheet_pdf");
    }
    const cc_model *m = cc_session_model(ses);
    const cc_sheet *s101 = NULL;
    for (size_t i = 0; i < m->nsheets; i++)
        if (m->sheets[i].number.n == 5 && !memcmp(m->sheets[i].number.p, "S-101", 5)) s101 = &m->sheets[i];
    check_true("S-101 found", s101 != NULL);
    if (!s101) return report("sheet_pdf");
    char body[600];
    snprintf(body, sizeof body, "{\"sheet\":\"%.22s\"}", s101->id.c);
    size_t gn;
    char *golden = slurp(G, "pdf/S-101-helvetica.pdf", &gn);
    int gobj = count(golden, gn, " 0 obj\n");
    Buf got;
    buf_init(&got);

    /* Helvetica: no fonts handed over, Rust's version. */
    check_true("set version", cc_paper_set_version(ses, RUST_VERSION) == 0);
    int rc = call(ses, "sheet_pdf", body, &got);
    check_true("sheet_pdf answers", rc == CC_OK);
    if (rc != CC_OK) printf("  %.*s\n", (int)(got.len < 300 ? got.len : 300), got.p);
    check_true("sheet_pdf: %PDF- ... %%EOF", is_pdf(&got));
    check_true("sheet_pdf: the producer names the version", find(got.p, got.len, "(Bimble " RUST_VERSION ")") != NULL);
    check_true("sheet_pdf: the golden's object count", count(got.p, got.len, " 0 obj\n") == gobj && gobj == 7);
    mask(&got);
    check_eq("sheet_pdf without fonts = pdf/S-101-helvetica.pdf (producer masked)", got.p, got.len, golden, gn);

    /* SIZE1: pdf_fonts without files; Roboto asked for without files: the Helvetica golden. */
    rc = call(ses, "pdf_fonts", "{}", &got);
    check_eq("pdf_fonts without files", got.p, got.len, "{\"helvetica\":true,\"roboto\":false}", 33);
    char rbody[600];
    snprintf(rbody, sizeof rbody, "{\"sheet\":\"%.22s\",\"font\":\"roboto\"}", s101->id.c);
    rc = call(ses, "sheet_pdf", rbody, &got);
    mask(&got);
    check_eq("sheet_pdf font roboto without files = the Helvetica golden", got.p, got.len, golden, gn);
    snprintf(rbody, sizeof rbody, "{\"sheet\":\"%.22s\",\"font\":\"helvetica\"}", s101->id.c);
    rc = call(ses, "sheet_pdf", rbody, &got);
    mask(&got);
    check_eq("sheet_pdf font helvetica = the Helvetica golden", got.p, got.len, golden, gn);
    snprintf(rbody, sizeof rbody, "{\"sheet\":\"%.22s\",\"font\":\"arial\"}", s101->id.c);
    rc = call(ses, "sheet_pdf", rbody, &got);
    check_eq("font arial: an error", got.p, got.len, "font: \"helvetica\" or \"roboto\"", 29);
    check_true("font arial: CC_ERR", rc == CC_ERR);

    /* Files that are not the shipped Roboto: the same Helvetica file, asked for or not. */
    static const unsigned char junk[64] = "not a font";
    check_true("set junk fonts", cc_paper_set_fonts(ses, junk, sizeof junk, junk, sizeof junk) == 0);
    rc = call(ses, "sheet_pdf", body, &got);
    mask(&got);
    check_eq("sheet_pdf with other font files = the Helvetica golden", got.p, got.len, golden, gn);
    snprintf(rbody, sizeof rbody, "{\"sheet\":\"%.22s\",\"font\":\"roboto\"}", s101->id.c);
    rc = call(ses, "sheet_pdf", rbody, &got);
    mask(&got);
    check_eq("sheet_pdf font roboto with other files = the Helvetica golden", got.p, got.len, golden, gn);
    rc = call(ses, "pdf_fonts", "{}", &got);
    check_eq("pdf_fonts with other files: roboto false", got.p, got.len, "{\"helvetica\":true,\"roboto\":false}", 33);
    cc_paper_set_fonts(ses, NULL, 0, NULL, 0);

    /* view_pdf of S-101's first placed view (a plan), Helvetica. */
    char vbody[80];
    snprintf(vbody, sizeof vbody, "{\"view\":\"%.22s\"}", s101->placed[0].view.c);
    rc = call(ses, "view_pdf", vbody, &got);
    check_true("view_pdf answers a PDF", rc == CC_OK && is_pdf(&got) && count(got.p, got.len, " 0 obj\n") == gobj);
    check_true("view_pdf: Helvetica, nothing embedded", find(got.p, got.len, "/Helvetica") && !find(got.p, got.len, "/FontFile2"));

    /* The errors. */
    rc = call(ses, "sheet_pdf", "{\"sheet\":\"nope\"}", &got);
    check_eq("unknown sheet", got.p, got.len, "no sheet with id \"nope\"", 23);
    check_true("unknown sheet: an error", rc == CC_ERR);
    rc = call(ses, "view_pdf", "{\"view\":\"nope\"}", &got);
    check_eq("unknown view", got.p, got.len, "no view with id \"nope\"", 22);
    rc = call(ses, "sheet_pdf", "{}", &got);
    check_eq("missing field", got.p, got.len, "missing field `sheet`", 21);
    snprintf(body, sizeof body, "{\"sheet\":\"%.22s\",\"path\":3}", s101->id.c);
    rc = call(ses, "sheet_pdf", body, &got);
    check_eq("path not a string", got.p, got.len, "path: a string", 14);
    snprintf(body, sizeof body, "{\"sheet\":\"%.22s\",\"path\":\"" DIR "/none/x.pdf\"}", s101->id.c);
    rc = call(ses, "sheet_pdf", body, &got);
    check_eq("a folder that does not exist", got.p, got.len, DIR "/none/x.pdf: cannot write", strlen(DIR "/none/x.pdf: cannot write"));
    check_true("cannot write: an error", rc == CC_ERR);

    /* SIZE1: the folder set: Helvetica still the default, byte for byte Rust's Helvetica file. */
    check_true("set font dir", cc_paper_set_font_dir(ses, FONTS) == 0);
    snprintf(body, sizeof body, "{\"sheet\":\"%.22s\"}", s101->id.c);
    rc = call(ses, "sheet_pdf", body, &got);
    mask(&got);
    check_eq("sheet_pdf with the Roboto folder, no font asked = the Helvetica golden", got.p, got.len, golden, gn);
    rc = call(ses, "pdf_fonts", "{}", &got);
    check_eq("pdf_fonts with the folder: roboto true", got.p, got.len, "{\"helvetica\":true,\"roboto\":true}", 32);

    /* Roboto, asked for, read from the folder. */
    snprintf(body, sizeof body, "{\"sheet\":\"%.22s\",\"font\":\"roboto\"}", s101->id.c);
    rc = call(ses, "sheet_pdf", body, &got);
    check_true("Roboto: %PDF- ... %%EOF", rc == CC_OK && is_pdf(&got));
    check_true("Roboto: 15 objects (D6's session check), both faces embedded",
               count(got.p, got.len, " 0 obj\n") == 15 && count(got.p, got.len, "/FontFile2") == 2 &&
                   find(got.p, got.len, "/BaseFont /Roboto") != NULL);
    Buf roboto;
    buf_init(&roboto);
    buf_append(&roboto, got.p, got.len);

    /* {"path"}: the same bytes in the file. */
    snprintf(body, sizeof body, "{\"sheet\":\"%.22s\",\"font\":\"roboto\",\"path\":\"" DIR "/S-101 Plan and section.pdf\"}", s101->id.c);
    rc = call(ses, "sheet_pdf", body, &got);
    char want[200];
    int wn = snprintf(want, sizeof want, "{\"bytes\":%zu,\"path\":\"" DIR "/S-101 Plan and section.pdf\"}", roboto.len);
    check_eq("path: the answer {bytes, path}", got.p, got.len, want, (size_t)wn);
    size_t fn;
    char *file = slurp(DIR, "S-101 Plan and section.pdf", &fn);
    check_eq("path: the file holds the PDF", file ? file : "", file ? fn : 0, roboto.p, roboto.len);
    free(file);
    remove(DIR "/S-101 Plan and section.pdf");

    buf_free(&got), buf_free(&roboto);
    free(golden);
    cc_session_free(ses);
    remove(copy);
    rmdir(DIR);
    return report("sheet_pdf");
}
