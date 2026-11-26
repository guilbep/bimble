/* Suite dxf_in (ticket L2): the DXF reference reader (src/dxf_in.h).
 * Golden: golden/dxf/<file>.txt, written by ccore_golden (reference::read over the dxf crate) for
 * each synthetic file of golden/dxf/fixtures/: the notes, then each polyline as Rust's {:?}, or
 * Rust's error text. Text and counts must be equal; numbers are compared exactly, and a number
 * that is not byte-equal must be within 4 ulp (sin, cos and atan2 here are correctly rounded,
 * glibc's are not always: golden/DEVIATIONS.md 14). The count of such numbers is printed.
 * Then the C additions (layers, texts), the budgets on hostile files (self-inserting and huge
 * arrays of blocks, absurd angles, 2,000,000 points), and every prefix and byte flip of the
 * fixtures read without a crash (run it under ASan + UBSan). */
#include <dirent.h>
#include <stdint.h>
#include <string.h>
#include "../src/dxf_in.h"
#include "../src/fmt.h"
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "check.h"

static size_t ulp_numbers, exact_numbers;

static void render(const char *name, const uint8_t *b, size_t n, Buf *o) {
    Buf err;
    buf_init(&err);
    int kind = cc_dxf_name_kind(name, strlen(name), &err);
    if (kind < 0) {
        buf_puts(o, "error "), buf_append(o, err.p, err.len), buf_putc(o, '\n');
        buf_free(&err);
        return;
    }
    cc_dxf d;
    if (cc_dxf_read(b, n, &d, &err)) {
        buf_puts(o, "error "), buf_append(o, err.p, err.len), buf_putc(o, '\n');
        buf_free(&err);
        return;
    }
    Buf notes;
    buf_init(&notes);
    size_t k = cc_dxf_notes(&d, &notes);
    buf_puts(o, "notes "), buf_u64(o, k), buf_putc(o, '\n');
    buf_append(o, notes.p, notes.len);
    buf_puts(o, "polylines "), buf_u64(o, d.npolylines), buf_putc(o, '\n');
    for (size_t i = 0; i < d.npolylines; i++) {
        buf_putc(o, '[');
        for (size_t j = d.start[i]; j < d.start[i + 1]; j++) {
            if (j > d.start[i]) buf_puts(o, ", ");
            buf_putc(o, '['), fmt_f64_debug(o, d.xy[2 * j]), buf_puts(o, ", "), fmt_f64_debug(o, d.xy[2 * j + 1]);
            buf_putc(o, ']');
        }
        buf_puts(o, "]\n");
    }
    buf_free(&notes);
    buf_free(&err);
    cc_dxf_free(&d);
}

static int numch(char c) { return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == '+'; }

static int64_t ordered(double x) {
    int64_t k;
    memcpy(&k, &x, 8);
    return k < 0 ? INT64_MIN - k : k;
}

/* Equal text, numbers within 4 ulp. */
static int near_equal(const char *a, size_t an, const char *b, size_t bn) {
    size_t i = 0, j = 0;
    while (i < an && j < bn) {
        if (numch(a[i]) && numch(b[j]) && (i == 0 || !numch(a[i - 1]))) {
            size_t ie = i, je = j;
            while (ie < an && numch(a[ie])) ie++;
            while (je < bn && numch(b[je])) je++;
            char x[64], y[64];
            if (ie - i >= sizeof x || je - j >= sizeof y) return 0;
            memcpy(x, a + i, ie - i), x[ie - i] = 0;
            memcpy(y, b + j, je - j), y[je - j] = 0;
            if (strcmp(x, y)) {
                double dx = strtod(x, NULL), dy = strtod(y, NULL);
                int64_t d = ordered(dx) - ordered(dy);
                if (d < -4 || d > 4) {
                    printf("  number %s vs %s\n", x, y);
                    return 0;
                }
                ulp_numbers++;
            } else exact_numbers++;
            i = ie, j = je;
            continue;
        }
        if (a[i] != b[j]) return 0;
        i++, j++;
    }
    return i == an && j == bn;
}

static uint8_t *file_bytes(const char *dir, const char *name, size_t *n) { return (uint8_t *)slurp(dir, name, n); }

static int cmp_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void goldens(const char *gdir) {
    char fdir[4096];
    snprintf(fdir, sizeof fdir, "%s/fixtures", gdir);
    DIR *dd = opendir(fdir);
    if (!dd) {
        check_true("golden/dxf/fixtures opens", 0);
        return;
    }
    char *names[256];
    size_t nn = 0;
    struct dirent *de;
    while ((de = readdir(dd)) && nn < 256)
        if (de->d_name[0] != '.') {
            size_t l = strlen(de->d_name);
            names[nn] = malloc(l + 1);
            memcpy(names[nn++], de->d_name, l + 1);
        }
    closedir(dd);
    qsort(names, nn, sizeof names[0], cmp_names);
    check_true("at least 10 DXF fixtures", nn >= 10);
    size_t bytes_equal = 0;
    for (size_t i = 0; i < nn; i++) {
        size_t n, gn;
        uint8_t *b = file_bytes(fdir, names[i], &n);
        char gname[512];
        snprintf(gname, sizeof gname, "%s.txt", names[i]);
        char *g = slurp(gdir, gname, &gn);
        Buf o;
        buf_init(&o);
        render(names[i], b, n, &o);
        char what[600];
        snprintf(what, sizeof what, "golden dxf/%s", gname);
        if (o.len == gn && !memcmp(o.p, g, gn)) {
            bytes_equal++;
            passed++;
        } else if (near_equal(o.p, o.len, g, gn)) passed++;
        else {
            size_t k = 0; /* the first differing byte, and 200 bytes from there */
            while (k < o.len && k < gn && o.p[k] == g[k]) k++;
            size_t a = k > 40 ? k - 40 : 0;
            printf("FAIL %s at byte %zu\n  got:  %.*s\n  want: %.*s\n", what, k, (int)(o.len - a > 200 ? 200 : o.len - a), o.p + a,
                   (int)(gn - a > 200 ? 200 : gn - a), g + a);
            failed++;
        }
        buf_free(&o);
        free(b), free(g), free(names[i]);
    }
    printf("goldens: %zu of %zu files byte-equal; numbers %zu exact, %zu within 4 ulp\n", bytes_equal, nn,
           exact_numbers, ulp_numbers);
}

static int read_text(const char *t, cc_dxf *d, Buf *err) {
    buf_init(err);
    return cc_dxf_read((const uint8_t *)t, strlen(t), d, err);
}

static void extras(const char *gdir) {
    size_t n;
    char fdir[4096];
    snprintf(fdir, sizeof fdir, "%s/fixtures", gdir);
    uint8_t *b = file_bytes(fdir, "plan_m.dxf", &n);
    cc_dxf d;
    Buf err;
    buf_init(&err);
    check_true("plan_m reads", cc_dxf_read(b, n, &d, &err) == 0);
    int walls = -1, furn = -1;
    for (size_t i = 0; i < d.nlayers; i++) {
        if (!strcmp(d.layers[i].name, "WALLS")) walls = (int)i;
        if (!strcmp(d.layers[i].name, "FURNITURE")) furn = (int)i;
    }
    check_true("layers WALLS and FURNITURE", walls >= 0 && furn >= 0);
    if (walls >= 0 && furn >= 0) {
        check_true("WALLS: 6 polylines, on", d.layers[walls].polylines == 6 && d.layers[walls].on && !d.layers[walls].frozen);
        check_true("FURNITURE: off and frozen", !d.layers[furn].on && d.layers[furn].frozen);
        check_true("the first polyline is on WALLS", d.npolylines && d.layer[0] == (uint32_t)walls);
    }
    check_true("two texts", d.ntexts == 2);
    if (d.ntexts == 2) {
        check_true("TEXT Room 1 at (2000, 7000), 250 high", !strcmp(d.texts[0].text, "Room 1") && d.texts[0].at[0] == 2000.0 &&
                                                               d.texts[0].at[1] == 7000.0 && d.texts[0].height == 250.0);
        check_true("MTEXT at (4000, 7000)", !strcmp(d.texts[1].text, "Kitchen\\Pcorner") && d.texts[1].at[0] == 4000.0);
    }
    cc_dxf_free(&d);
    free(b);
    b = file_bytes(fdir, "blocks_in.dxf", &n);
    check_true("blocks_in reads", cc_dxf_read(b, n, &d, &err) == 0);
    check_true("blocks_in: a text per DOOR copy (2 + 6 in FRAME arrays)", d.ntexts == 8);
    cc_dxf_free(&d);
    free(b);
    buf_free(&err);
}

static void hostile(void) {
    cc_dxf d;
    Buf err, notes;
    /* A block inserting itself as a 1000 x 1000 array: Rust would loop for ages; the budget stops it. */
    const char *bomb =
        "0\nSECTION\n2\nBLOCKS\n0\nBLOCK\n2\nB\n10\n0\n20\n0\n0\nTEXT\n10\n0\n20\n0\n1\nx\n"
        "0\nINSERT\n2\nB\n10\n0\n20\n0\n70\n1000\n71\n1000\n0\nENDBLK\n0\nENDSEC\n"
        "0\nSECTION\n2\nENTITIES\n0\nINSERT\n2\nB\n10\n0\n20\n0\n70\n1000\n71\n1000\n0\nENDSEC\n0\nEOF\n";
    check_true("block bomb reads", read_text(bomb, &d, &err) == 0);
    check_true("block bomb: stopped by the budget", d.over_budget);
    buf_init(&notes);
    cc_dxf_notes(&d, &notes);
    check_true("block bomb: its note", strstr(buf_cstr(&notes), "stopped expanding blocks") != NULL);
    buf_free(&notes), buf_free(&err), cc_dxf_free(&d);

    /* Absurd angles: Rust's `while sweep <= 0 { sweep += TAU }` would not end. */
    const char *angles = "0\nSECTION\n2\nENTITIES\n0\nARC\n10\n0\n20\n0\n40\n1\n50\n1e300\n51\n0\n"
                         "0\nARC\n10\n0\n20\n0\n40\n1\n50\n0\n51\n1e300\n0\nELLIPSE\n10\n0\n20\n0\n11\n1\n21\n0\n41\n1e300\n42\n0\n"
                         "0\nCIRCLE\n10\nnan\n20\n0\n40\n1\n0\nLINE\n10\ninf\n20\n0\n11\n1\n21\n1\n0\nENDSEC\n0\nEOF\n";
    check_true("absurd angles read", read_text(angles, &d, &err) == 0);
    check_true("absurd angles: nothing drawn, the huge sweep truncated", d.npolylines == 0 && d.truncated);
    buf_free(&err), cc_dxf_free(&d);

    /* 2,000,000 points: a polyline of 1,999,000, one of 1,001 that does not fit, then a line that does. */
    Buf big;
    buf_init(&big);
    buf_puts(&big, "0\nSECTION\n2\nENTITIES\n0\nLWPOLYLINE\n");
    for (int i = 0; i < 1999000; i++) buf_puts(&big, "10\n1\n20\n2\n");
    buf_puts(&big, "0\nLWPOLYLINE\n");
    for (int i = 0; i < 1001; i++) buf_puts(&big, "10\n1\n20\n2\n");
    buf_puts(&big, "0\nLINE\n10\n0\n20\n0\n11\n1\n21\n0\n0\nENDSEC\n0\nEOF\n");
    buf_init(&err);
    check_true("2M points reads", cc_dxf_read((const uint8_t *)big.p, big.len, &d, &err) == 0);
    check_true("2M points: truncated, the first polyline and the line", d.truncated && d.npolylines == 2 && d.npts == 1999002);
    buf_init(&notes);
    cc_dxf_notes(&d, &notes);
    check_true("2M points: Rust's note", strstr(buf_cstr(&notes), "stopped at 2000000 points\n") != NULL);
    buf_free(&notes), buf_free(&err), cc_dxf_free(&d), buf_free(&big);

    /* Deep 102 groups and xdata control groups: no recursion. */
    Buf deep;
    buf_init(&deep);
    buf_puts(&deep, "0\nSECTION\n2\nENTITIES\n0\nLINE\n");
    for (int i = 0; i < 200000; i++) buf_puts(&deep, "102\n{G\n");
    for (int i = 0; i < 200000; i++) buf_puts(&deep, "102\n}\n");
    buf_puts(&deep, "1001\nA\n");
    for (int i = 0; i < 200000; i++) buf_puts(&deep, "1002\n{\n");
    for (int i = 0; i < 200000; i++) buf_puts(&deep, "1002\n}\n");
    buf_puts(&deep, "10\n0\n20\n0\n11\n5\n21\n0\n0\nENDSEC\n0\nEOF\n");
    buf_init(&err);
    check_true("deep groups read", cc_dxf_read((const uint8_t *)deep.p, deep.len, &d, &err) == 0 && d.npolylines == 1);
    buf_free(&err), cc_dxf_free(&d), buf_free(&deep);

    /* Binary DXF is refused with a text. */
    check_true("binary DXF refused", read_text("AutoCAD Binary DXF\r\n\x1a", &d, &err) == -1 &&
                                         strstr(buf_cstr(&err), "binary DXF") != NULL);
    buf_free(&err);
    check_true(".DWG refused with Rust's text", cc_dxf_name_kind("A.DWG", 5, &err) == -1 && strstr(buf_cstr(&err), "ODA File Converter"));
    buf_free(&err);
    check_true(".Dxf is a DXF", cc_dxf_name_kind("plan.Dxf", 8, &err) == 1);
    buf_free(&err);
}

/* Fills a large stretch of the stack below the caller with 0xA5, so a struct the reader frees
 * before initialising it holds 0xA5A5... pointers (a certain crash), not what an earlier call left. */
static void __attribute__((noinline)) poison_stack(void) {
    volatile unsigned char buf[1 << 16];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = 0xA5;
}

static int __attribute__((noinline)) read_poisoned(const uint8_t *b, size_t n) {
    cc_dxf d;
    Buf err;
    buf_init(&err);
    poison_stack();
    int rc = cc_dxf_read(b, n, &d, &err);
    if (rc == 0) cc_dxf_free(&d);
    buf_free(&err);
    return rc;
}

/* An entity list whose first entity fails before any field is read (the 20:14 crash: an Ent freed
 * uninitialised), then every prefix of the fixtures, each read on a poisoned stack. */
static void poisoned(const char *gdir) {
    static const char *const T[] = {
        "0\nSECTION\n2\nENTITIES\n8\nX\n0\nENDSEC\n0\nEOF\n",
        "0\nSECTION\n2\nENTITIES\n0\nLINE\n10\nabc\n0\nENDSEC\n0\nEOF\n",
        "0\nSECTION\n2\nBLOCKS\n0\nBLOCK\n2\nB\n8\n0\n0\nENDBLK\n0\nENDSEC\n0\nEOF\n",
        "0\nSECTION\n2\nBLOCKS\n0\nBLOCK\n2\nB\n0\n5\n0\nENDBLK\n0\nENDSEC\n0\nEOF\n",
        "0\nSECTION\n2\nENTITIES\n0\nLWPOLYLINE\n20\n1\n0\nENDSEC\n0\nEOF\n",
        "0\nSECTION\n2\nENTITIES\n0\nPOLYLINE\n0\nVERTEX\n10\nx\n0\nENDSEC\n0\nEOF\n",
        "0\nSECTION\n2\nENTITIES\n0\nINSERT\n66\n1\n2\nB\n0\nATTRIB\n10\n?\n",
        "0\nSECTION\n2\nENTITIES\n0\nLINE\n"};
    size_t runs = 0;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) read_poisoned((const uint8_t *)T[i], strlen(T[i])), runs++;
    static const char *const F[] = {"plan_m.dxf", "blocks_in.dxf", "r12.dxf", "self_insert.dxf", "truncated.dxf"};
    char fdir[4096];
    snprintf(fdir, sizeof fdir, "%s/fixtures", gdir);
    for (size_t f = 0; f < sizeof F / sizeof F[0]; f++) {
        size_t n;
        uint8_t *b = file_bytes(fdir, F[f], &n);
        for (size_t k = 0; k <= n; k++) read_poisoned(b, k), runs++;
        free(b);
    }
    check_true("every prefix read on a poisoned stack", runs > 3000);
}

/* Every prefix (one in 7 bytes) and a flipped byte at every position of each fixture: an answer
 * or an error, never a crash (ASan, UBSan). */
static void mangled(const char *gdir) {
    static const char *const F[] = {"plan_m.dxf", "blocks_in.dxf", "r12.dxf", "self_insert.dxf"};
    char fdir[4096];
    snprintf(fdir, sizeof fdir, "%s/fixtures", gdir);
    size_t runs = 0;
    for (size_t f = 0; f < 4; f++) {
        size_t n;
        uint8_t *b = file_bytes(fdir, F[f], &n);
        uint8_t *c = malloc(n);
        for (size_t k = 0; k < n; k += 7) {
            cc_dxf d;
            Buf err;
            buf_init(&err);
            memcpy(c, b, n);
            if (cc_dxf_read(c, k, &d, &err) == 0) cc_dxf_free(&d);
            static const uint8_t X[] = {'0', '\n', '-', 'e', 0xEF, 0x00};
            c[k] = X[k % 6];
            if (cc_dxf_read(c, n, &d, &err) == 0) cc_dxf_free(&d);
            buf_free(&err);
            runs += 2;
        }
        free(c), free(b);
    }
    check_true("mangled fixtures read or refused", runs > 500);
}

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_clear(out);
    return cc_call(s, op, name, body, strlen(body), out);
}

/* The session calls (links_dxf.h): link_dxf in a plan, the links rows, view_references,
 * manage_links move / rotate / hide / show / reload / remove, each one undo step. */
static void session_calls(const char *floats, const char *gdir) {
    char path[4096], body[8192];
    snprintf(path, sizeof path, "%s/../frame.json", floats);
    cc_session *s = cc_session_new(cc_platform_native());
    cc_error err;
    if (!s || cc_session_open(s, path, &err)) {
        check_true("frame.json opens", 0);
        return;
    }
    Buf ans;
    buf_init(&ans);
    snprintf(body, sizeof body, "{\"path\":\"%s/fixtures/r12.dxf\"}", gdir);
    /* FAKE1: ribbon.rs:595 `loaded && flat`: refused by the enable rule now (was the 2D views text) */
    check_true("link_dxf in the 3D view: refused", call(s, "action", "link_dxf", body, &ans) == CC_ERR &&
                                                      strstr(buf_cstr(&ans), "not enabled"));
    call(s, "query", "open_views", "", &ans);
    const char *k = strstr(buf_cstr(&ans), "\"auto_name\":{\"kind\":\"plan\"");
    const char *idp = k ? strstr(k, "\"id\":\"") : NULL;
    if (idp) idp += 6;
    check_true("a plan view", idp != NULL);
    if (!idp) return;
    char vid[23];
    memcpy(vid, idp, 22), vid[22] = 0;
    char tb[256];
    snprintf(tb, sizeof tb, "{\"op\":\"open\",\"tab\":{\"type\":\"view\",\"id\":\"%s\"}}", vid);
    check_true("open the plan's tab", call(s, "tab", "", tb, &ans) == CC_OK);
    snprintf(tb, sizeof tb, "{\"op\":\"activate\",\"tab\":{\"type\":\"view\",\"id\":\"%s\"}}", vid);
    check_true("activate it", call(s, "tab", "", tb, &ans) == CC_OK);
    size_t undo0 = s->hist.nundo;
    snprintf(body, sizeof body, "{\"path\":\"%s/fixtures/r12.dxf\",\"offset_mm\":[0,0],\"rotation_deg\":0}", gdir);
    int rc = call(s, "action", "link_dxf", body, &ans);
    if (rc != CC_OK) printf("  link_dxf: %s\n", buf_cstr(&ans));
    check_true("link_dxf r12.dxf -> 200", rc == CC_OK);
    char rid[23] = "";
    call(s, "query", "links", "", &ans);
    const char *ip = strstr(buf_cstr(&ans), "{\"id\":\"");
    if (ip) memcpy(rid, ip + 7, 22), rid[22] = 0;
    check_true("link_dxf: one undo step", s->hist.nundo == undo0 + 1);
    call(s, "query", "links", "", &ans);
    char row[256];
    snprintf(row, sizeof row, "[{\"id\":\"%s\",\"kind\":\"dxf\",\"name\":\"r12.dxf\",\"visible\":true}]", rid);
    check_eq("query links: the reference's row", ans.p, ans.len, row, strlen(row));
    snprintf(tb, sizeof tb, "{\"view\":\"%s\"}", vid);
    call(s, "query", "view_references", tb, &ans);
    check_true("view_references: r12.dxf listed", strstr(buf_cstr(&ans), "\"name\":\"r12.dxf\"") != NULL);
    const char *seg = strstr(buf_cstr(&ans), "\"polylines\":[[0.0,0.0,");
    check_true("view_references: the first point (0, 0) at the cut height", seg != NULL);
    snprintf(tb, sizeof tb, "{\"op\":\"move\",\"id\":\"%s\",\"offset_mm\":[1000,-500]}", rid);
    check_true("move -> 200, one step", call(s, "action", "manage_links", tb, &ans) == CC_OK && s->hist.nundo == undo0 + 2);
    check_true("move again: no step", call(s, "action", "manage_links", tb, &ans) == CC_OK && s->hist.nundo == undo0 + 2);
    snprintf(tb, sizeof tb, "{\"view\":\"%s\"}", vid);
    call(s, "query", "view_references", tb, &ans);
    check_true("moved: the first point at (1000, -500)", strstr(buf_cstr(&ans), "\"polylines\":[[1000.0,-500.0,") != NULL);
    snprintf(tb, sizeof tb, "{\"op\":\"rotate\",\"id\":\"%s\",\"rotation_deg\":90}", rid);
    check_true("rotate -> 200", call(s, "action", "manage_links", tb, &ans) == CC_OK && s->hist.nundo == undo0 + 3);
    snprintf(tb, sizeof tb, "{\"op\":\"hide\",\"id\":\"%s\"}", rid);
    check_true("hide -> 200", call(s, "action", "manage_links", tb, &ans) == CC_OK);
    call(s, "query", "links", "", &ans);
    check_true("hidden in links", strstr(buf_cstr(&ans), "\"visible\":false") != NULL);
    snprintf(tb, sizeof tb, "{\"view\":\"%s\"}", vid);
    call(s, "query", "view_references", tb, &ans);
    check_true("hidden: listed with no polylines", strstr(buf_cstr(&ans), "\"visible\":false") && strstr(buf_cstr(&ans), "\"polylines\":[]"));
    snprintf(tb, sizeof tb, "{\"op\":\"show\",\"id\":\"%s\"}", rid);
    check_true("show -> 200", call(s, "action", "manage_links", tb, &ans) == CC_OK);
    snprintf(body, sizeof body, "{\"op\":\"reload\",\"id\":\"%s\",\"path\":\"%s/fixtures/feet_bom.dxf\"}", rid, gdir);
    check_true("reload from another file -> 200", call(s, "action", "manage_links", body, &ans) == CC_OK);
    call(s, "query", "links", "", &ans);
    check_true("reloaded: its name, the same id", strstr(buf_cstr(&ans), "feet_bom.dxf") && strstr(buf_cstr(&ans), rid));
    const cc_model *m = cc_session_model(s);
    const cc_reference *r = NULL;
    for (size_t i = 0; i < m->nviews; i++)
        if (m->views[i].nreferences) r = &m->views[i].references[0];
    check_true("reloaded: kept where it was put", r && r->offset_mm[0] == 1000 && r->offset_mm[1] == -500 && r->rotation_deg == 90 &&
                                                    r->npolylines == 1 && r->has_source);
    check_true("undo -> back to r12.dxf", call(s, "action", "undo", "", &ans) == CC_OK);
    call(s, "query", "links", "", &ans);
    check_true("undone: r12.dxf again", strstr(buf_cstr(&ans), "r12.dxf") != NULL);
    snprintf(tb, sizeof tb, "{\"op\":\"remove\",\"id\":\"%s\"}", rid);
    check_true("remove -> 200", call(s, "action", "manage_links", tb, &ans) == CC_OK);
    call(s, "query", "links", "", &ans);
    check_eq("removed: no rows", ans.p, ans.len, "[]", 2);
    check_true("remove again: no reference plan", call(s, "action", "manage_links", tb, &ans) == CC_ERR &&
                                                       strstr(buf_cstr(&ans), "no reference plan"));
    snprintf(body, sizeof body, "{\"path\":\"%s/fixtures/plan.dwg\"}", gdir);
    check_true("a DWG: Rust's text", call(s, "action", "link_dxf", body, &ans) == CC_ERR && strstr(buf_cstr(&ans), "dwg2dxf"));
    snprintf(body, sizeof body, "{\"path\":\"%s/fixtures/truncated.dxf\"}", gdir);
    check_true("a truncated DXF: the reader's text", call(s, "action", "link_dxf", body, &ans) == CC_ERR &&
                                                         strstr(buf_cstr(&ans), "unexpectedly ended"));
    /* The same calls on a PDF reference (L1's link_pdf): manage_links works by id for both. */
    snprintf(body, sizeof body, "{\"path\":\"%s/../reference/fixtures/lines.pdf\",\"offset_mm\":[0,0],\"rotation_deg\":0}", gdir);
    check_true("link_pdf lines.pdf -> 200", call(s, "action", "link_pdf", body, &ans) == CC_OK);
    call(s, "query", "links", "", &ans);
    const char *pp = strstr(buf_cstr(&ans), "{\"id\":\"");
    char pid[23] = "";
    if (pp) memcpy(pid, pp + 7, 22), pid[22] = 0;
    check_true("the PDF's row", pp && strstr(buf_cstr(&ans), "\"kind\":\"pdf\""));
    size_t u = s->hist.nundo;
    snprintf(tb, sizeof tb, "{\"op\":\"move\",\"id\":\"%s\",\"offset_mm\":[250,250]}", pid);
    check_true("PDF: move -> 200, one step", call(s, "action", "manage_links", tb, &ans) == CC_OK && s->hist.nundo == u + 1);
    snprintf(tb, sizeof tb, "{\"op\":\"hide\",\"id\":\"%s\"}", pid);
    check_true("PDF: hide -> 200", call(s, "action", "manage_links", tb, &ans) == CC_OK && s->hist.nundo == u + 2);
    snprintf(tb, sizeof tb, "{\"op\":\"reload\",\"id\":\"%s\"}", pid);
    rc = call(s, "action", "manage_links", tb, &ans);
    if (rc != CC_OK) printf("  reload: %s\n", buf_cstr(&ans));
    check_true("PDF: reload from its source -> 200", rc == CC_OK);
    m = cc_session_model(s);
    r = NULL;
    for (size_t i = 0; i < m->nviews; i++)
        for (size_t j = 0; j < m->views[i].nreferences; j++)
            if (!memcmp(m->views[i].references[j].id.c, pid, 22)) r = &m->views[i].references[j];
    check_true("PDF reloaded: still a pdf, moved, hidden, with lines",
               r && r->format.n == 3 && !memcmp(r->format.p, "pdf", 3) && r->offset_mm[0] == 250 && !r->visible && r->npolylines > 0);
    snprintf(tb, sizeof tb, "{\"op\":\"remove\",\"id\":\"%s\"}", pid);
    check_true("PDF: remove -> 200", call(s, "action", "manage_links", tb, &ans) == CC_OK);
    check_true("PDF: undo the remove", call(s, "action", "undo", "", &ans) == CC_OK);
    call(s, "query", "links", "", &ans);
    check_true("PDF: back after undo", strstr(buf_cstr(&ans), pid) != NULL);
    buf_free(&ans);
    cc_session_free(s);
}

int main(int argc, char **argv) {
    const char *floats = argc > 1 ? argv[1] : "ccore/golden/floats";
    char gdir[4096];
    snprintf(gdir, sizeof gdir, "%s/../dxf", floats);
    goldens(gdir);
    extras(gdir);
    hostile();
    mangled(gdir);
    poisoned(gdir);
    session_calls(floats, gdir);
    return report("dxf_in");
}
