/* Suite ref (ticket L1): reference plans read from PDFs, byte for byte against Rust
 * (golden/reference/, `reference_files` of crates/bimble-api/examples/ccore_golden.rs:
 * bimble_drawing::reference::read over lopdf), then the session's link_pdf, links and
 * view_references.
 * - every golden/reference/<file>.txt: the read of reference/fixtures/<file> or of D1's
 *   drawing/pdf/<file> at 1:100, the same text (notes, counts, each polyline as Rust's {:?}, or the
 *   FNV-1a of those lines for the 30,000-segment many.pdf). encrypted.pdf is a deviation
 *   (DEVIATIONS.md section 13): C refuses it with its own text.
 * - scales.txt (lines.pdf at other scales, bad ones refused) and names.txt (by file name).
 * - bad files: every prefix of the small fixtures and 3,000 byte changes of each: no crash (the
 *   sanitizers watch), an answer or an error text each time.
 * - the session: link_pdf on the frame's ground plan (an add_view: one undo step), query links,
 *   query view_references (segments in view mm, the count), undo, redo, a 3D view refused, a bad
 *   file refused with nothing changed, the saved file keeps the reference and an unlinked model
 *   keeps its bytes. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#include <dirent.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include "../session/calls.h"
#include "../session/session.h"
#include "../host/host.h"
#include "check.h"
#include "fmt.h"
#include "ref_read.h"

#define G "golden/reference"

static uint64_t fnv64(const char *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)p[i]) * 0x100000001b3ull;
    return h;
}

/* The dumper's reference_text. */
static void ref_text(Buf *b, int rc, const cc_refread *r, const Buf *err) {
    if (rc) {
        buf_puts(b, "error ");
        buf_append(b, err->p, err->len);
        buf_putc(b, '\n');
        return;
    }
    for (size_t i = 0; i < r->notes.len;) {
        const char *e = memchr(r->notes.p + i, '\n', r->notes.len - i);
        size_t k = (size_t)(e - (r->notes.p + i));
        buf_puts(b, "note ");
        buf_append(b, r->notes.p + i, k);
        buf_putc(b, '\n');
        i += k + 1;
    }
    buf_puts(b, "polylines "), buf_u64(b, r->nlines), buf_puts(b, " points "), buf_u64(b, r->npts), buf_putc(b, '\n');
    Buf body;
    buf_init(&body);
    for (size_t k = 0; k < r->nlines; k++) {
        buf_putc(&body, '[');
        for (size_t i = r->start[k]; i < r->start[k + 1]; i++) {
            if (i > r->start[k]) buf_puts(&body, ", ");
            buf_putc(&body, '['), fmt_f64_debug(&body, r->xy[i][0]), buf_puts(&body, ", ");
            fmt_f64_debug(&body, r->xy[i][1]), buf_putc(&body, ']');
        }
        buf_puts(&body, "]\n");
    }
    if (body.len > 256 * 1024) {
        char hex[17];
        uint64_t h = fnv64(body.p, body.len);
        for (int i = 0; i < 16; i++) hex[i] = "0123456789abcdef"[(h >> (60 - 4 * i)) & 15];
        hex[16] = 0;
        buf_puts(b, "fnv "), buf_puts(b, hex), buf_puts(b, " bytes "), buf_u64(b, body.len), buf_putc(b, '\n');
        size_t i = 0;
        for (int l = 0; l < 20 && i < body.len; l++) {
            const char *e = memchr(body.p + i, '\n', body.len - i);
            size_t k = (size_t)(e - (body.p + i)) + 1;
            buf_append(b, body.p + i, k);
            i += k;
        }
    } else buf_append(b, body.p, body.len);
    buf_free(&body);
}

static void read_text(Buf *out, const char *name, const char *bytes, size_t n, double scale) {
    cc_refread r;
    Buf err;
    buf_init(&err);
    int rc = cc_ref_read(name, strlen(name), (const unsigned char *)bytes, n, scale, &r, &err);
    ref_text(out, rc, &r, &err);
    cc_refread_free(&r);
    buf_free(&err);
}

static void compare(const char *what, const Buf *got, const char *want, size_t n) {
    check_eq(what, got->p ? got->p : "", got->len < 300 ? got->len : 300, want, n < 300 ? n : 300);
    if (got->len != n || memcmp(got->p, want, n)) {
        size_t i = 0;
        while (i < got->len && i < n && got->p[i] == want[i]) i++;
        size_t from = i > 60 ? i - 60 : 0;
        printf("FAIL %s: first difference at byte %zu\n  got:  %.*s\n  want: %.*s\n", what, i,
               (int)((got->len - from) < 200 ? got->len - from : 200), got->p + from, (int)((n - from) < 200 ? n - from : 200),
               want + from);
        failed++;
    }
}

static char *input(const char *name, size_t *n) {
    char path[512];
    snprintf(path, sizeof path, G "/fixtures/%s", name);
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return slurp(G "/fixtures", name, n);
    }
    return slurp("golden/drawing/pdf", name, n);
}

static int goldens(void) {
    DIR *dir = opendir(G);
    if (!dir) {
        printf("cannot open " G "\n");
        exit(2);
    }
    struct dirent *e;
    int nfiles = 0, nmatch = 0;
    while ((e = readdir(dir))) {
        size_t len = strlen(e->d_name);
        if (len < 9 || strcmp(e->d_name + len - 8, ".pdf.txt")) continue;
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)(len - 4), e->d_name);
        size_t n, wn;
        char *bytes = input(name, &n), *want = slurp(G, e->d_name, &wn);
        Buf got;
        buf_init(&got);
        read_text(&got, name, bytes, n, 100.0);
        nfiles++;
        if (!strcmp(name, "encrypted.pdf")) { /* DEVIATIONS.md section 13 */
            check_true("encrypted.pdf: Rust has no page (lopdf could not decrypt it)", !strcmp(want, "error the PDF has no page\n"));
            check_true("encrypted.pdf: refused with its own text", strstr(buf_cstr(&got), "error the PDF is encrypted") == buf_cstr(&got));
        } else {
            compare(name, &got, want, wn);
            nmatch += got.len == wn && !memcmp(got.p, want, wn);
        }
        buf_free(&got);
        free(bytes), free(want);
    }
    closedir(dir);
    printf("  %d files read, %d byte for byte with Rust\n", nfiles, nmatch);
    check_true("all the goldens read", nfiles >= 15);
    return nmatch;
}

static void scales_and_names(void) {
    size_t n, wn;
    char *lines = input("lines.pdf", &n);
    static const double S[] = {1.0, 0.5, 50.0, 333.3, 100000.0, 1e-7, 0.0, -1.0, 100000.5, 0.0 / 0.0, 1.0 / 0.0};
    Buf got;
    buf_init(&got);
    for (size_t i = 0; i < sizeof S / sizeof S[0]; i++) {
        buf_puts(&got, "scale ");
        if (S[i] != S[i]) buf_puts(&got, "NaN");
        else if (S[i] > 1e300) buf_puts(&got, "inf");
        else fmt_f64_debug(&got, S[i]);
        buf_putc(&got, '\n');
        read_text(&got, "lines.pdf", lines, n, S[i]);
    }
    char *want = slurp(G, "scales.txt", &wn);
    compare("scales.txt", &got, want, wn);
    free(want);
    buf_clear(&got);
    static const char *const NAMES[] = {"PLAN.PDF", "plan.Pdf", "plan.dwg", "plan.txt", "plan"};
    for (size_t i = 0; i < 5; i++) {
        buf_puts(&got, "name "), buf_puts(&got, NAMES[i]), buf_putc(&got, '\n');
        const char *b = i == 0 ? lines : i == 1 ? "%PDF-1.4 garbage" : i == 2 ? "AC1032" : "";
        read_text(&got, NAMES[i], b, i == 0 ? n : strlen(b), 100.0);
    }
    want = slurp(G, "names.txt", &wn);
    compare("names.txt", &got, want, wn);
    free(want), free(lines);
    buf_free(&got);
}

/* Damaged files: every prefix, and byte changes (a fixed generator): no crash, an answer. */
static void damaged(void) {
    static const char *const F[] = {"lines.pdf", "flate.pdf", "cycle.pdf", "self-form.pdf", "truncated.pdf"};
    uint64_t seed = 0x9e3779b97f4a7c15ull;
    size_t runs = 0, ok = 0;
    for (size_t f = 0; f < sizeof F / sizeof F[0]; f++) {
        size_t n;
        char *b = input(F[f], &n);
        char *m = malloc(n + 1);
        for (size_t k = 0; k <= n; k++) {
            cc_refread r;
            Buf err;
            buf_init(&err);
            int rc = cc_ref_read("x.pdf", 5, (const unsigned char *)b, k, 100.0, &r, &err);
            runs++, ok += rc == 0 || err.len > 0;
            cc_refread_free(&r), buf_free(&err);
        }
        for (int t = 0; t < 3000; t++) {
            memcpy(m, b, n);
            for (int c = 0; c < 1 + t % 4; c++) {
                seed = seed * 6364136223846793005ull + 1442695040888963407ull;
                size_t at = (size_t)(seed >> 33) % n;
                static const char SET[] = "0123456789 \n()<>[]/%RobjstreamendQqcmlS-.";
                m[at] = (t & 1) ? SET[(seed >> 13) % (sizeof SET - 1)] : (char)(seed >> 21);
            }
            cc_refread r;
            Buf err;
            buf_init(&err);
            int rc = cc_ref_read("x.pdf", 5, (const unsigned char *)m, n, 100.0, &r, &err);
            runs++, ok += rc == 0 || err.len > 0;
            cc_refread_free(&r), buf_free(&err);
        }
        free(m), free(b);
    }
    printf("  %zu damaged reads, %zu answered\n", runs, ok);
    check_true("every damaged read answers (polylines or an error text)", ok == runs);
}

/* FIXLIB: lopdf's DEREF_LIMIT (128 references followed). /Root is a chain of `n` objects each
 * holding only a reference to the next, then the catalog: n = 127 is 128 hops (read), n = 128 is
 * 129 (no catalog). The answer differs at the bound, so a missing check fails here instead of
 * hanging on cycle.pdf. */
static void pdf_chain(size_t n, Buf *pdf) {
    size_t *off = calloc(n + 5, sizeof *off);
    buf_clear(pdf);
    buf_puts(pdf, "%PDF-1.4\n");
    char t[256];
    for (size_t k = 1; k <= n + 4; k++) {
        off[k] = pdf->len;
        size_t c = n + 1; /* the catalog */
        if (k <= n) snprintf(t, sizeof t, "%zu 0 obj\n%zu 0 R\nendobj\n", k, k + 1);
        else if (k == c) snprintf(t, sizeof t, "%zu 0 obj\n<< /Type /Catalog /Pages %zu 0 R >>\nendobj\n", k, c + 1);
        else if (k == c + 1) snprintf(t, sizeof t, "%zu 0 obj\n<< /Type /Pages /Kids [%zu 0 R] /Count 1 >>\nendobj\n", k, c + 2);
        else if (k == c + 2)
            snprintf(t, sizeof t, "%zu 0 obj\n<< /Type /Page /Parent %zu 0 R /MediaBox [0 0 595 842] /Contents %zu 0 R >>\nendobj\n",
                     k, c + 1, c + 3);
        else snprintf(t, sizeof t, "%zu 0 obj\n<< /Length 15 >>\nstream\n0 0 m 72 0 l S\nendstream\nendobj\n", k);
        buf_puts(pdf, t);
    }
    size_t x = pdf->len;
    snprintf(t, sizeof t, "xref\n0 %zu\n0000000000 65535 f \n", n + 5);
    buf_puts(pdf, t);
    for (size_t k = 1; k <= n + 4; k++) {
        snprintf(t, sizeof t, "%010zu 00000 n \n", off[k]);
        buf_puts(pdf, t);
    }
    snprintf(t, sizeof t, "trailer\n<< /Size %zu /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n", n + 5, x);
    buf_puts(pdf, t);
    free(off);
}

/* FIXLIB: a page dictionary of `extra` keys /K0../Kn plus /Contents twice: first a missing object,
 * last the line (lopdf's set: the last value, in the first one's place). */
static void pdf_big_dict(size_t extra, Buf *pdf) {
    size_t off[6];
    char t[256];
    buf_clear(pdf);
    buf_puts(pdf, "%PDF-1.4\n");
    off[1] = pdf->len, buf_puts(pdf, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[2] = pdf->len, buf_puts(pdf, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    off[3] = pdf->len, buf_puts(pdf, "3 0 obj\n<< /Type /Page /Parent 2 0 R /Contents 99 0 R /MediaBox [0 0 595 842]");
    for (size_t k = 0; k < extra; k++) snprintf(t, sizeof t, " /K%zu %zu", k, k), buf_puts(pdf, t);
    buf_puts(pdf, " /Contents 4 0 R >>\nendobj\n");
    off[4] = pdf->len, buf_puts(pdf, "4 0 obj\n<< /Length 15 >>\nstream\n0 0 m 72 0 l S\nendstream\nendobj\n");
    size_t x = pdf->len;
    buf_puts(pdf, "xref\n0 5\n0000000000 65535 f \n");
    for (int k = 1; k <= 4; k++) snprintf(t, sizeof t, "%010zu 00000 n \n", off[k]), buf_puts(pdf, t);
    snprintf(t, sizeof t, "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n", x);
    buf_puts(pdf, t);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void big_dicts(void) {
    Buf pdf, out;
    buf_init(&pdf), buf_init(&out);
    static const size_t E[] = {3, 100000};
    for (int i = 0; i < 2; i++) {
        pdf_big_dict(E[i], &pdf);
        double t0 = now_s();
        read_text(&out, "dict.pdf", pdf.p, pdf.len, 100.0);
        double dt = now_s() - t0;
        printf("  a page dictionary of %zu keys: %.3f s\n", E[i] + 5, dt);
        char what[128];
        snprintf(what, sizeof what, "%zu keys, /Contents twice: the last one's line", E[i] + 5);
        check_true(what, strstr(buf_cstr(&out), "polylines 1 points 2\n[[0.0, 0.0], [2539.9999999999995, 0.0]]\n") != NULL);
        /* the linear search took 12.5 s for 100,000 keys (clang -O1); the sort, milliseconds */
        if (E[i] > 1000) check_true("100,005 keys read in under 3 s", dt < 3.0);
        buf_clear(&out);
    }
    buf_free(&pdf), buf_free(&out);
}

static void deref_limit(void) {
    Buf pdf, out;
    buf_init(&pdf), buf_init(&out);
    pdf_chain(127, &pdf);
    read_text(&out, "chain.pdf", pdf.p, pdf.len, 100.0);
    printf("  127 references to the catalog: %.*s", (int)(out.len < 120 ? out.len : 120), out.p);
    check_true("127 references (128 hops): the page's line", strstr(buf_cstr(&out), "polylines 1 points 2\n") != NULL);
    buf_clear(&out);
    pdf_chain(128, &pdf);
    read_text(&out, "chain.pdf", pdf.p, pdf.len, 100.0);
    printf("  128 references to the catalog: %.*s", (int)(out.len < 120 ? out.len : 120), out.p);
    check_true("128 references (129 hops): past DEREF_LIMIT, no page", strstr(buf_cstr(&out), "polylines 1 ") == NULL &&
                                                                        !strncmp(buf_cstr(&out), "error ", 6));
    buf_free(&pdf), buf_free(&out);
}

/* ---- the session (session/links_ref.h) ---- */

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

static void expect(const char *what, cc_session *s, const char *op, const char *name, const char *body, int want_st,
                   const char *needle) {
    int st;
    char *t = ask(s, op, name, body, &st);
    int ok = st == want_st && (!needle || strstr(t, needle));
    check_true(what, ok);
    if (!ok) printf("  %s: status %d, %.400s\n", what, st, t);
    free(t);
}

static void session(void) {
    cc_session *s = cc_session_new(cc_platform_native());
    expect("link_pdf: nothing open", s, "action", "link_pdf", "{\"path\": \"" G "/fixtures/lines.pdf\"}", CC_ERR, "not enabled");
    expect("open the frame", s, "action", "open", "{\"path\": \"golden/frame.json\", \"discard\": true}", CC_OK, NULL);
    /* FAKE1: ribbon.rs:595 `loaded && flat`: refused by the enable rule now (was the 2D views text) */
    expect("link_pdf: the 3D view is active", s, "action", "link_pdf", "{\"path\": \"" G "/fixtures/lines.pdf\"}", CC_ERR,
           "not enabled");
    /* the ground plan: the first floor plan of open_views (view.rs effective: 3D, then floor
     * plans by elevation) */
    int st;
    char *views = ask(s, "query", "open_views", "{}", &st);
    char *p1 = strstr(views, "\"views\""), *idp = NULL;
    p1 = p1 ? strstr(p1, "\"plan\"") : NULL;
    for (char *q = p1; q && q > views; q--)
        if (!strncmp(q, "{\"", 2)) {
            char *k = strstr(q, "\"id\":\"");
            if (k && k < p1 + 400) idp = k + 6;
            break;
        }
    check_true("a floor plan in query open_views", idp != NULL);
    if (!idp) printf("  %.800s\n", views);
    char id[23] = {0};
    if (idp) memcpy(id, idp, 22);
    char body[512];
    snprintf(body, sizeof body, "{\"op\": \"open\", \"tab\": {\"type\": \"view\", \"id\": \"%s\"}}", id);
    expect("open the ground plan's tab", s, "tab", "", body, CC_OK, NULL);
    free(views);
    expect("links: none", s, "query", "links", "{}", CC_OK, "[]");
    expect("link_pdf: no body", s, "action", "link_pdf", "", CC_ERR, "path");
    expect("link_pdf: a damaged file", s, "action", "link_pdf", "{\"path\": \"" G "/fixtures/garbage.pdf\"}", CC_ERR,
           "not a readable PDF: couldn't parse input");
    expect("link_pdf: bad scale", s, "action", "link_pdf", "{\"path\": \"" G "/fixtures/lines.pdf\", \"scale\": 0}", CC_ERR,
           "between 1 and 100000");
    expect("undo: nothing linked yet", s, "query", "actions", "{}", CC_OK, NULL);
    expect("link_pdf lines.pdf at 1:100", s, "action", "link_pdf",
           "{\"path\": \"" G "/fixtures/lines.pdf\", \"scale\": 100, \"offset_mm\": [1000, 2000]}", CC_OK, NULL);
    expect("links: the reference row", s, "query", "links", "{}", CC_OK, "\"kind\":\"pdf\",\"name\":\"lines.pdf\",\"visible\":true");
    snprintf(body, sizeof body, "{\"view\": \"%s\"}", id);
    char *vr = ask(s, "query", "view_references", body, &st);
    check_true("view_references: ok", st == CC_OK);
    check_true("view_references: 14 polylines of lines.pdf, 63 segments", strstr(vr, "\"segments\":63") != NULL);
    /* lines.pdf's first point (352.77…, 705.55…) moved by (1000, 2000) */
    check_true("view_references: placed by offset_mm", strstr(vr, "[1352.7777777777778,2705.5555555555557") != NULL ||
                                                            strstr(vr, "[1352.777") != NULL);
    if (st != CC_OK || !strstr(vr, "\"segments\":63")) printf("  %.600s\n", vr);
    free(vr);
    check_true("unsaved after the link (a history step)", cc_session_unsaved(s));
    expect("undo the link", s, "action", "undo", "{}", CC_OK, NULL);
    expect("links: none after undo", s, "query", "links", "{}", CC_OK, "[]");
    vr = ask(s, "query", "view_references", body, &st);
    check_true("view_references: none after undo", st == CC_OK && strstr(vr, "\"segments\":0"));
    free(vr);
    expect("redo the link", s, "action", "redo", "{}", CC_OK, NULL);
    expect("links: back after redo", s, "query", "links", "{}", CC_OK, "lines.pdf");
    expect("link_pdf a second one, placed by Rust's rule", s, "action", "link_pdf", "{\"path\": \"" G "/fixtures/flate.pdf\"}",
           CC_OK, NULL);
    vr = ask(s, "query", "view_references", body, &st);
    check_true("view_references: two references", st == CC_OK && strstr(vr, "flate.pdf") && strstr(vr, "lines.pdf"));
    free(vr);
    expect("view_references: unknown view", s, "query", "view_references", "{\"view\": \"0000000000000000000000\"}", CC_ERR, "no view");
    cc_session_free(s);
}

int main(void) {
    deref_limit();
    big_dicts();
    fflush(stdout); /* first: without the limit cycle.pdf (goldens, damaged) hangs */
    int n = goldens();
    printf("  ref: %d goldens byte for byte\n", n);
    scales_and_names();
    damaged();
    session();
    return report("ref");
}
