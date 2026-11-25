/* Suite sheets (ticket SH1): views placed on sheets. No Rust oracle for the four verbs
 * (sheet_place, sheet_move, sheet_unplace, sheet_remove) nor the query sheet_layout: the goldens
 * golden/sheets/verbs.txt (a transcript: each verb, each refusal, undo and redo, the queries) and
 * golden/sheets/model.json (the model at its end) are this suite's own (CCORE_WRITE_GOLDEN=1
 * rewrites them; golden/DEVIATIONS.md). The verbs run on the session's model (cc_verb_run, with
 * seeded ids), their commands go through the session (`apply`), so the history, undo labels and
 * queries are the session's. Also: a sheet tab opens (no such sheet otherwise); the 200 placed
 * limit; undoing every step gives frame.json's bytes back; the file keeps Rust's `Sheet` shape. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "../src/command.h"
#include "../src/file.h"
#include "../src/guid.h"
#include "../src/verb_table.h"
#include "../src/verbs.h"
#include "check.h"

static const char *golden;

/* frame.json's default views (view.rs default_views ids) */
#define PLAN_N1 "3w8zGYnP$Ay1lyvMZjDnXq" /* Plan Niveau 1 */
#define V3D "1v7Vtm2FfafmYPKIQEXnlY"     /* 3D */
#define NOBODY "2zzzzzzzzzzzzzzzzzzzzz"  /* a GlobalId of nothing */

static void strip_nl(char *s, size_t *n) {
    while (*n && (s[*n - 1] == '\n' || s[*n - 1] == '\r')) s[--*n] = 0;
}

static void write_file(const char *name, const char *p, size_t n, int nl) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", golden, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n || (nl && fputc('\n', f) == EOF)) {
        printf("cannot write %s\n", path);
        exit(2);
    }
    fclose(f);
    printf("  wrote %s\n", path);
}

/* "<new-K>" in s replaced by the text of from_u128(K) (the seeded ids). */
static void unmask(const char *s, Buf *out) {
    buf_clear(out);
    while (*s) {
        if (!strncmp(s, "<new-", 5)) {
            uint64_t k = 0;
            const char *p = s + 5;
            while (*p >= '0' && *p <= '9') k = k * 10 + (uint64_t)(*p++ - '0');
            if (*p == '>') {
                cc_guid g = cc_guid_from_u128(0, k);
                buf_append(out, g.c, 22);
                s = p + 1;
                continue;
            }
        }
        buf_putc(out, *s++);
    }
}

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_clear(out);
    return cc_call(s, op, name, body, strlen(body), out);
}

static cc_session *open_frame(void) {
    char path[4096];
    snprintf(path, sizeof path, "%s/frame.json", golden);
    cc_session *s = cc_session_new(cc_platform_native());
    cc_error e;
    if (!s || cc_session_open(s, path, &e)) {
        printf("open %s: %s\n", path, s ? e.msg : "out of memory");
        exit(2);
    }
    return s;
}

/* ---- the transcript ---- */

static const char *const STEPS[] = {
    "add_sheet {\"number\":\"S-101\",\"name\":\"Plans\"}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" PLAN_N1 "\",\"at_mm\":[300,250]}",
    "query sheet_layout {\"id\":\"<new-1>\"}",
    "undo",
    "redo",
    /* ADR 0046: the sheet draws the view at the view's own scale */
    "set_view {\"id\":\"" PLAN_N1 "\",\"scale\":50}",
    "query sheet_layout {\"id\":\"<new-1>\"}",
    /* the first placement of a view whose scale is not said sets it (adopted_scales) */
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" V3D "\",\"at_mm\":[600,300],\"scale\":200}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" V3D "\",\"at_mm\":[700,300],\"scale\":50}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" V3D "\",\"at_mm\":[700,300]}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" PLAN_N1 "\",\"at_mm\":[100,100],\"scale\":20}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" PLAN_N1 "\",\"at_mm\":[100,100],\"scale\":50}",
    "undo",
    "query sheet_layout {\"id\":\"<new-1>\"}",
    /* refusals */
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" PLAN_N1 "\",\"at_mm\":[900,100]}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" NOBODY "\",\"at_mm\":[100,100]}",
    "sheet_place {\"sheet\":\"" NOBODY "\",\"view\":\"" PLAN_N1 "\",\"at_mm\":[100,100]}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" PLAN_N1 "\",\"at_mm\":[100]}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" PLAN_N1 "\",\"at_mm\":[1e308,1]}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" PLAN_N1 "\",\"at_mm\":[100,100],\"scale\":0}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" PLAN_N1 "\",\"at_mm\":[100,100],\"scale\":2.5}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" PLAN_N1 "\",\"at_mm\":[100,100],\"scale\":1e300}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"" PLAN_N1 "\",\"at_mm\":[100,100],\"x\":1}",
    "sheet_place {\"sheet\":\"<new-1>\",\"view\":\"bad\",\"at_mm\":[100,100]}",
    "sheet_place {\"view\":\"" PLAN_N1 "\",\"at_mm\":[100,100]}",
    "sheet_place [1]",
    /* move */
    "sheet_move {\"sheet\":\"<new-1>\",\"placed\":\"<new-2>\",\"at_mm\":[320.5,260]}",
    "undo",
    "redo",
    "sheet_move {\"sheet\":\"<new-1>\",\"placed\":\"<new-2>\",\"at_mm\":[320.5,260]}",
    "sheet_move {\"sheet\":\"<new-1>\",\"placed\":\"" NOBODY "\",\"at_mm\":[10,10]}",
    "sheet_move {\"sheet\":\"<new-1>\",\"placed\":\"<new-2>\",\"at_mm\":[841.5,10]}",
    "sheet_move {\"sheet\":\"<new-1>\",\"placed\":\"<new-2>\",\"at_mm\":[-0.0,0]}",
    "undo",
    /* a crop gives the box */
    "set_view {\"id\":\"" PLAN_N1 "\",\"crop\":{\"min\":[0,0],\"max\":[10000,5000]}}",
    "query sheet_layout {\"id\":\"<new-1>\"}",
    /* unplace */
    "sheet_unplace {\"sheet\":\"<new-1>\",\"placed\":\"<new-4>\"}",
    "undo",
    "redo",
    "sheet_unplace {\"sheet\":\"<new-1>\",\"placed\":\"<new-4>\"}",
    "sheet_unplace {\"sheet\":\"<new-1>\"}",
    /* Rust's set_sheet keeps the placed views on the paper */
    "add_sheet {\"number\":\"S-102\",\"name\":\"Coupes\",\"paper\":\"A3\",\"portrait\":true}",
    "set_sheet {\"id\":\"<new-1>\",\"paper\":\"A4\"}",
    "set_sheet {\"id\":\"<new-1>\",\"paper\":\"A0\",\"portrait\":true}",
    "query sheet_layout {\"id\":\"<new-1>\"}",
    "undo",
    /* remove */
    "sheet_remove {\"id\":\"<new-6>\"}",
    "undo",
    "redo",
    "sheet_remove {\"id\":\"<new-6>\"}",
    "sheet_remove {\"id\":\"" PLAN_N1 "\"}",
    "sheet_remove {}",
    "query sheets",
    "query sheet_layout {\"id\":\"" NOBODY "\"}",
    "query sheet_layout {\"id\":\"bad\"}",
    "query sheet_layout {}",
};

static void transcript(void) {
    size_t fn;
    char *base = slurp(golden, "frame.json", &fn);
    strip_nl(base, &fn);
    cc_session *s = open_frame();
    Buf t, err, args, out, model, ans;
    buf_init(&t), buf_init(&err), buf_init(&args), buf_init(&out), buf_init(&model), buf_init(&ans);
    cc_guid_seed(1);
    int applied = 0, refused = 0, none = 0, steps = 0;
    for (size_t i = 0; i < sizeof STEPS / sizeof STEPS[0]; i++) {
        const char *st = STEPS[i];
        buf_puts(&out, "> "), buf_puts(&out, st), buf_putc(&out, '\n');
        if (!strcmp(st, "undo") || !strcmp(st, "redo")) {
            size_t u0 = s->hist.nundo;
            check_true(st, call(s, st, "", "", &ans) == CC_OK);
            check_true(st[0] == 'u' ? "undo takes one step back" : "redo one step", s->hist.nundo == (st[0] == 'u' ? u0 - 1 : u0 + 1));
        } else if (!strncmp(st, "query ", 6)) {
            const char *sp = strchr(st + 6, ' ');
            char name[64];
            snprintf(name, sizeof name, "%.*s", (int)(sp ? sp - st - 6 : (long)strlen(st + 6)), st + 6);
            unmask(sp ? sp + 1 : "", &args);
            int rc = call(s, "query", name, buf_cstr(&args), &ans);
            buf_puts(&out, "  "), buf_u64(&out, (uint64_t)rc), buf_putc(&out, ' ');
            buf_append(&out, ans.p, ans.len), buf_putc(&out, '\n');
            continue;
        } else {
            const char *sp = strchr(st, ' ');
            char name[64];
            snprintf(name, sizeof name, "%.*s", (int)(sp - st), st);
            unmask(sp + 1, &args);
            cc_cmds l;
            cc_cmds_init(&l);
            int rc = cc_verb_run(cc_session_model(s), name, args.p, args.len, &l, &err);
            if (rc) {
                buf_puts(&out, "  error "), buf_append(&out, err.p, err.len), buf_putc(&out, '\n');
                refused++;
            } else if (!l.n) {
                buf_puts(&out, "  no command\n");
                none++;
            } else {
                buf_clear(&t);
                cc_commands_json(l.v, l.n, &t);
                buf_puts(&out, "  "), buf_append(&out, t.p, t.len), buf_putc(&out, '\n');
                check_true("one command per verb", l.n == 1);
                /* through the session, as the journal and `apply` carry it */
                rc = call(s, "apply", "", buf_cstr(&t), &ans);
                if (rc != CC_OK) buf_puts(&out, "  apply error "), buf_append(&out, ans.p, ans.len), buf_putc(&out, '\n');
                else applied++, steps++;
            }
            cc_cmds_free(&l);
        }
        call(s, "query", "history", "", &ans);
        buf_puts(&out, "  history "), buf_append(&out, ans.p, ans.len), buf_putc(&out, '\n');
    }
    cc_guid_seed(0);
    printf("  transcript: %zu steps, %d applied, %d refused, %d without a command\n", sizeof STEPS / sizeof STEPS[0],
           applied, refused, none);
    const cc_model *m = cc_session_model(s);
    cc_error e;
    check_true("the model validates", cc_model_validate(m, &e) == 0);
    check_true("one sheet left", m->nsheets == 1);
    check_true("the sheet holds two placed views", m->nsheets == 1 && m->sheets[0].nplaced == 2);
    check_true("every verb refusal was seen", refused >= 20);
    check_true("cc_model_save", cc_model_save(m, &model) == 0);
    /* Rust's Sheet and Placed: {"id","number","name","paper"[,"portrait"],"placed":[{"id","view","scale","at_mm"}]} */
    check_true("the file holds Rust's Placed shape",
               strstr(buf_cstr(&model), "\"placed\": [\n          {\n            \"id\": \"0000000000000000000002\",\n"
                                        "            \"view\": \"" PLAN_N1 "\",\n            \"scale\": 100,\n"
                                        "            \"at_mm\": [\n              320.5,\n              260.0\n") != NULL);
    if (getenv("CCORE_WRITE_GOLDEN")) {
        write_file("sheets/verbs.txt", out.p, out.len, 0);
        write_file("sheets/model.json", model.p, model.len, 1);
    }
    size_t n1, n2;
    char *g1 = slurp(golden, "sheets/verbs.txt", &n1), *g2 = slurp(golden, "sheets/model.json", &n2);
    strip_nl(g2, &n2);
    check_eq("transcript == sheets/verbs.txt", out.p, out.len, g1, n1);
    check_eq("model == sheets/model.json", model.p, model.len, g2, n2);
    /* The file reads back to the same bytes. */
    cc_model back;
    int lrc = cc_model_load(&back, model.p, model.len, &e);
    check_true("the file loads", lrc == 0);
    if (!lrc) {
        buf_clear(&t);
        cc_model_save(&back, &t);
        check_eq("load, save: same bytes", t.p, t.len, model.p, model.len);
        cc_model_free(&back);
    }
    /* Every step undone: frame.json's bytes. */
    for (int k = 0; k < 200 && s->hist.nundo; k++) call(s, "undo", "", "", &ans);
    buf_clear(&t);
    cc_model_save(cc_session_model(s), &t);
    check_eq("all undone == frame.json", t.p, t.len, base, fn);
    free(g1), free(g2), free(base);
    buf_free(&t), buf_free(&err), buf_free(&args), buf_free(&out), buf_free(&model), buf_free(&ans);
    cc_session_free(s);
}

/* ---- the rest: limits, the sheet tab, the verb table ---- */

static void rules(void) {
    cc_session *s = open_frame();
    Buf ans, err, b;
    buf_init(&ans), buf_init(&err), buf_init(&b);
    check_true("add_sheet", call(s, "verb", "add_sheet", "{\"number\":\"S-1\"}", &ans) == CC_OK);
    const cc_model *m = cc_session_model(s);
    char sid[23];
    memcpy(sid, m->sheets[0].id.c, 22), sid[22] = 0;
    char body[256];
    snprintf(body, sizeof body, "{\"sheet\":\"%s\",\"view\":\"" PLAN_N1 "\",\"at_mm\":[10,10]}", sid);
    int ok = 1;
    for (int k = 0; k < 200 && ok; k++) ok = call(s, "verb", "sheet_place", body, &ans) == CC_OK;
    check_true("200 placed views", ok && m->sheets[0].nplaced == 200);
    int rc = call(s, "verb", "sheet_place", body, &ans);
    const char *want = "sheet_place: sheet S-1 New sheet: too many views or texts";
    check_eq("the 201st is refused (sheet.rs MAX_PLACED)", ans.p, rc == CC_ERR ? ans.len : 0, want, strlen(want));
    /* a dry run answers the command, applies nothing */
    size_t before = s->hist.nundo;
    snprintf(body, sizeof body, "{\"id\":\"%s\",\"dry_run\":true}", sid);
    rc = call(s, "verb", "sheet_remove", body, &ans);
    check_true("dry run: remove_sheet answered", rc == CC_OK && strstr(buf_cstr(&ans), "\"op\":\"remove_sheet\""));
    check_true("dry run: nothing applied", s->hist.nundo == before && m->nsheets == 1);
    /* the sheet tab */
    snprintf(body, sizeof body, "{\"op\":\"open\",\"tab\":{\"type\":\"sheet\",\"id\":\"%s\"}}", sid);
    check_true("open a sheet tab -> 200", call(s, "tab", "", body, &ans) == CC_OK);
    snprintf(body, sizeof body, "{\"op\":\"activate\",\"tab\":{\"type\":\"sheet\",\"id\":\"%s\"}}", sid);
    check_true("activate a sheet tab -> 200", call(s, "tab", "", body, &ans) == CC_OK);
    rc = call(s, "tab", "", "{\"op\":\"open\",\"tab\":{\"type\":\"sheet\",\"id\":\"" NOBODY "\"}}", &ans);
    check_eq("an unknown sheet -> no such sheet", ans.p, rc == CC_ERR ? ans.len : 0, "no such sheet", 13);
    rc = call(s, "tab", "", "{\"op\":\"open\",\"tab\":{\"type\":\"project\"}}", &ans);
    check_eq("a project tab still does not open", ans.p, rc == CC_ERR ? ans.len : 0, "op \"open\" takes a view", 22);
    rc = call(s, "tab", "", "{\"op\":\"open\",\"tab\":{\"type\":\"sheet\",\"id\":\"bad\"}}", &ans);
    check_true("a bad sheet id -> 400", rc == CC_ERR);
    /* the open_views of the session stay view tabs only */
    call(s, "query", "open_views", "", &ans);
    check_true("open_views: no sheet tab kept", !strstr(buf_cstr(&ans), "\"sheet\""));
    cc_session_free(s);

    /* No model: the query refuses, the tab too. */
    cc_session *none = cc_session_new(cc_platform_native());
    rc = call(none, "query", "sheet_layout", "{\"id\":\"" NOBODY "\"}", &ans);
    check_eq("sheet_layout without a model", ans.p, rc == CC_ERR ? ans.len : 0, "no model open", 13);
    rc = call(none, "tab", "", "{\"op\":\"open\",\"tab\":{\"type\":\"sheet\",\"id\":\"" NOBODY "\"}}", &ans);
    check_true("a sheet tab without a model -> 400", rc == CC_ERR);
    cc_session_free(none);

    /* The verb table: Rust's list and unknown-verb text keep their 14 verbs; describe answers ours. */
    cc_verbs_list_json(&b);
    check_true("verbs list: no sheet_place (Rust's 14 only)", !strstr(buf_cstr(&b), "sheet_place"));
    size_t names = 0;
    for (const char *q = buf_cstr(&b); (q = strstr(q, "\"name\":")); q++) names++;
    check_true("verbs list: Rust's 14", names == 14);
    static const char *const OURS[] = {"sheet_place", "sheet_move", "sheet_unplace", "sheet_remove"};
    for (int i = 0; i < 4; i++) {
        const cc_verb_def *d = cc_verb_find(OURS[i]);
        check_true(OURS[i], d && d->schema && strstr(d->schema, "\"additionalProperties\":false") && !d->read_only);
    }
    cc_cmds l;
    cc_cmds_init(&l);
    cc_model fm;
    cc_error e;
    size_t fn;
    char *ft = slurp(golden, "frame.json", &fn);
    check_true("frame loads", cc_model_load(&fm, ft, fn, &e) == 0);
    cc_verb_run(&fm, "sheet_nothing", "{}", 2, &l, &err);
    if (!strstr(buf_cstr(&err), "known: place,") || strstr(buf_cstr(&err), "sheet_place")) printf("  %s\n", buf_cstr(&err));
    check_true("unknown verb text lists Rust's verbs only", strstr(buf_cstr(&err), "known: place,") && !strstr(buf_cstr(&err), "sheet_place"));
    cc_cmds_free(&l);
    cc_model_free(&fm);
    free(ft);
    buf_free(&ans), buf_free(&err), buf_free(&b);
}

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    golden = dir;
    transcript();
    rules();
    return report("sheets");
}
