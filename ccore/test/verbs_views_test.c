/* Suite verbs_views (ticket SEC1): views made, changed and removed by verb (src/verbs_views.h),
 * a section shown live (query view_clip), and what a view hides stored in the view (views.h
 * cc_sv_vis_load).
 *   - the transcript: verbs, undo and redo on a copy of frame.json, seeded ids, against
 *     golden/views/verbs.txt (its own golden: Rust has no such verbs, DEVIATIONS 21) and the model
 *     written after it, golden/views/model.json;
 *   - Rust's form: golden/drawing/model.json (written by Rust, ccore_golden: sections north and
 *     east, a cropped plan, a plan hiding IfcSlab) is read and written back byte for byte; a
 *     section made by view_add and view_edit is Rust's "Section B-B" but its id; removing one
 *     and undoing gives the same bytes again;
 *   - view_clip of a section against D4's frame (draw_view.c cc_view_prepare): the cut and the
 *     depth are the same planes; views.rs section_clip_keeps_what_is_beyond_the_cut;
 *   - per view: IfcSlab hidden in one view is hidden there only; hide in view is stored with the
 *     view and comes back with it; undo; show_all_in_view clears and is one undo step. */
#include <stdint.h>
#include <string.h>
#include "../host/host.h"
#include "../session/calls.h"
#include "../session/session.h"
#include "../session/views.h"
#include "../src/command.h"
#include "../src/draw_view.h"
#include "../src/file.h"
#include "../src/guid.h"
#include "../src/history.h"
#include "../src/json_r.h"
#include "../src/query.h"
#include "../src/scene.h"
#include "../src/verbs.h"
#include "../src/view_io.h"
#include "check.h"

static const char *golden = "golden";

static char *frame(const char *name, size_t *n) { return slurp(golden, name, n); }

static void strip_nl(char *p, size_t *n) {
    while (*n && (p[*n - 1] == '\n' || p[*n - 1] == '\r')) (*n)--;
    (void)p;
}

static void write_file(const char *name, const char *p, size_t n, int nl) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", golden, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(p, 1, n, f);
    if (nl) fputc('\n', f);
    fclose(f);
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

#define N1 "1uAZA0Yfn05f$xRd1k4uPR" /* frame.json: Niveau 1 */
#define PLAN_N1 "<plan-n1>"          /* the default plan of Niveau 1, filled in below */

static const char *const STEPS[] = {
    "view_add {\"kind\":\"plan\",\"storey\":\"" N1 "\"}",
    "undo",
    "redo",
    "view_add {\"kind\":\"ceiling\",\"storey\":\"" N1 "\"}",
    "view_add {\"kind\":\"section\",\"facing\":\"north\",\"origin\":[9000,5000]}",
    "view_add {\"kind\":\"section\",\"facing\":\"west\",\"origin\":[6000,0],\"name\":\"  West cut \"}",
    "view_add {\"kind\":\"three_d\"}",
    "view_add {\"from\":\"<new-3>\"}",
    "view_add {\"from\":\"" PLAN_N1 "\"}",
    "view_add {\"kind\":\"section\",\"facing\":\"up\",\"origin\":[0,0]}",
    "view_add {\"kind\":\"section\",\"facing\":\"east\"}",
    "view_add {\"kind\":\"plan\",\"storey\":\"00000000000000000000ZZ\"}",
    "view_add {\"kind\":\"plan\",\"storey\":\"" N1 "\",\"facing\":\"north\"}",
    "view_add {\"kind\":\"section\",\"facing\":\"north\",\"origin\":[2e7,0]}",
    "view_add {\"kind\":\"wall\"}",
    "view_add {\"kind\":\"plan\",\"storey\":\"" N1 "\",\"name\":\" \"}",
    "view_add {\"from\":\"00000000000000000000ZZ\"}",
    "view_add {\"kind\":\"three_d\",\"zoom\":2}",
    "view_edit {\"id\":\"<new-3>\",\"name\":\"Section A-A\",\"depth_mm\":4000}",
    "undo",
    "redo",
    "view_edit {\"id\":\"<new-3>\",\"name\":\"Section A-A\",\"depth_mm\":4000}",
    "view_edit {\"id\":\"<new-3>\",\"facing\":\"south\",\"origin\":[9000,5500]}",
    "view_edit {\"id\":\"<new-3>\",\"depth_mm\":-1}",
    "view_edit {\"id\":\"<new-1>\",\"origin\":[0,0]}",
    "view_edit {\"id\":\"<new-1>\",\"levels\":{\"top\":\"unlimited\",\"depth\":\"below\"}}",
    "view_edit {\"id\":\"<new-3>\",\"levels\":{\"top\":\"above\"}}",
    "view_edit {\"id\":\"<new-1>\",\"levels\":{\"top\":\"sky\"}}",
    "view_edit {\"id\":\"" PLAN_N1 "\",\"visibility\":{\"hidden_classes\":[\"IfcSlab\",\"IfcBeam\",\"IfcSlab\"]}}",
    "undo",
    "redo",
    "view_edit {\"id\":\"" PLAN_N1 "\",\"visibility\":{\"hidden_classes\":[\"IfcBeam\",\"IfcSlab\"]}}",
    "view_edit {\"id\":\"" PLAN_N1 "\",\"visibility\":{\"hidden_elements\":[\"nope\"]}}",
    "view_edit {\"id\":\"" PLAN_N1 "\",\"visibility\":{\"hidden_walls\":[]}}",
    "view_edit {\"id\":\"<new-3>\",\"visibility\":{\"hidden_classes\":[],\"hidden_storeys\":[\"Niveau 2\"]}}",
    "view_remove {\"id\":\"<new-2>\"}",
    "undo",
    "redo",
    "view_remove {\"id\":\"" PLAN_N1 "\"}",
    "undo",
    "view_remove {\"id\":\"<new-2>\"}",
    "view_remove {\"id\":\"1v7Vtm2FfafmYPKIQEXnlY\"}",
    "query views",
};

static void transcript(void) {
    size_t fn;
    char *base = frame("frame.json", &fn);
    cc_model m;
    cc_error e;
    cc_history h;
    Buf t, err, args, line, out, model, step;
    buf_init(&t), buf_init(&err), buf_init(&args), buf_init(&line), buf_init(&out), buf_init(&model), buf_init(&step);
    cc_history_init(&h);
    check_true("frame loads", cc_model_load(&m, base, fn, &e) == 0);
    cc_gid n1;
    memcpy(n1.c, N1, 22);
    cc_gid plan_n1 = cc_view_derived_id("bimble view plan down ", &n1);
    cc_guid_seed(1);
    int applied = 0, refused = 0, none = 0;
    for (size_t i = 0; i < sizeof STEPS / sizeof STEPS[0]; i++) {
        buf_clear(&step);
        for (const char *s = STEPS[i]; *s; s++) { /* PLAN_N1 -> its derived id */
            if (!strncmp(s, PLAN_N1, strlen(PLAN_N1))) {
                buf_append(&step, plan_n1.c, 22);
                s += strlen(PLAN_N1) - 1;
            } else buf_putc(&step, *s);
        }
        const char *s = buf_cstr(&step);
        buf_puts(&out, "> "), buf_puts(&out, s), buf_putc(&out, '\n');
        if (!strcmp(s, "undo") || !strcmp(s, "redo")) {
            int ok = s[0] == 'u' ? cc_history_undo(&h, &m) : cc_history_redo(&h, &m);
            check_true(s, ok == 1);
        } else if (!strncmp(s, "query ", 6)) {
            buf_clear(&t);
            int st = cc_query(&m, s + 6, "", 0, &t);
            buf_puts(&out, "  "), buf_u64(&out, (uint64_t)st), buf_putc(&out, ' ');
            buf_append(&out, t.p, t.len), buf_putc(&out, '\n');
            continue;
        } else {
            const char *sp = strchr(s, ' ');
            char name[64];
            snprintf(name, sizeof name, "%.*s", (int)(sp - s), s);
            unmask(sp + 1, &args);
            cc_cmds l;
            cc_cmds_init(&l);
            int rc = cc_verb_run(&m, name, args.p, args.len, &l, &err);
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
                cc_cmds back;
                cc_cmds_init(&back);
                Buf t2;
                buf_init(&t2);
                if (!cc_commands_parse(t.p, t.len, &back, &err)) cc_commands_json(back.v, back.n, &t2);
                char what[160];
                snprintf(what, sizeof what, "step %zu: commands JSON round trip", i);
                check_eq(what, t2.p, t2.len, t.p, t.len);
                buf_free(&t2);
                cc_cmds_free(&back);
                rc = cc_history_apply(&h, &m, l.v, l.n, &err);
                if (rc) buf_puts(&out, "  apply error "), buf_append(&out, err.p, err.len), buf_putc(&out, '\n');
                else applied++;
            }
            cc_cmds_free(&l);
        }
        buf_clear(&line);
        cc_history_labels(&h, 1, &line, NULL);
        buf_puts(&out, "  undo "), buf_append(&out, line.p, line.len), buf_putc(&out, '\n');
    }
    cc_guid_seed(0);
    printf("  transcript: %zu steps, %d applied, %d refused, %d without a command\n", sizeof STEPS / sizeof STEPS[0],
           applied, refused, none);
    check_true("the model validates", cc_model_validate(&m, &e) == 0);
    check_true("7 views stored (plan, ceiling, 2 sections, 3D, 2 copies, the default plan; one removed)", m.nviews == 7);
    if (cc_model_save(&m, &model)) buf_clear(&model);
    /* The model read back writes the same bytes (canonical). */
    cc_model m2;
    check_true("written model reads back", cc_model_load(&m2, model.p, model.len, &e) == 0);
    Buf again;
    buf_init(&again);
    if (!cc_model_save(&m2, &again)) check_eq("written again: same bytes", again.p, again.len, model.p, model.len);
    buf_free(&again);
    cc_model_free(&m2);
    if (getenv("CCORE_WRITE_GOLDEN")) {
        write_file("views/verbs.txt", out.p, out.len, 0);
        write_file("views/model.json", model.p, model.len, 1);
    }
    size_t n1n, n2n;
    char *g1 = slurp(golden, "views/verbs.txt", &n1n), *g2 = slurp(golden, "views/model.json", &n2n);
    strip_nl(g2, &n2n);
    check_eq("transcript == views/verbs.txt", out.p, out.len, g1, n1n);
    check_eq("model == views/model.json", model.p, model.len, g2, n2n);
    free(g1), free(g2);
    cc_model_free(&m);
    cc_history_free(&h);
    buf_free(&t), buf_free(&err), buf_free(&args), buf_free(&line), buf_free(&out), buf_free(&model), buf_free(&step);
    free(base);
}

/* ---- Rust's form (golden/drawing/model.json, written by Rust) ---- */

static const cc_view *view_named(const cc_model *m, const char *name) {
    for (size_t i = 0; i < m->nviews; i++)
        if (m->views[i].name.n == strlen(name) && !memcmp(m->views[i].name.p, name, m->views[i].name.n)) return &m->views[i];
    return NULL;
}

static int run1(cc_history *h, cc_model *m, const char *verb, const char *args, Buf *err) {
    cc_cmds l;
    cc_cmds_init(&l);
    int rc = cc_verb_run(m, verb, args, strlen(args), &l, err);
    if (!rc && l.n) rc = cc_history_apply(h, m, l.v, l.n, err);
    cc_cmds_free(&l);
    return rc;
}

static void rust_form(void) {
    size_t n;
    char *text = frame("drawing/model.json", &n);
    cc_model m;
    cc_error e;
    check_true("drawing/model.json loads", cc_model_load(&m, text, n, &e) == 0);
    Buf w, err, a, b;
    buf_init(&w), buf_init(&err), buf_init(&a), buf_init(&b);
    size_t tn = n;
    strip_nl(text, &tn);
    check_true("written", cc_model_save(&m, &w) == 0);
    check_eq("Rust's model with sections, crop, hidden IfcSlab: written back byte for byte", w.p, w.len, text, tn);
    const cc_view *bb = view_named(&m, "Section B-B");
    check_true("Rust's Section B-B is there", bb != NULL);
    /* view_remove and undo: the same bytes */
    cc_history h;
    cc_history_init(&h);
    char args[128];
    snprintf(args, sizeof args, "{\"id\":\"%.22s\"}", bb ? bb->id.c : "");
    check_true("view_remove Section B-B", run1(&h, &m, "view_remove", args, &err) == 0 && !view_named(&m, "Section B-B"));
    check_true("undo", cc_history_undo(&h, &m) == 1);
    buf_clear(&w);
    cc_model_save(&m, &w);
    check_eq("removed and undone: same bytes", w.p, w.len, text, tn);
    /* view_add + view_edit make Rust's Section B-B (but its id) */
    check_true("view_add section east through 3000,2000",
               run1(&h, &m, "view_add", "{\"kind\":\"section\",\"facing\":\"east\",\"origin\":[3000,2000],\"name\":\"Mine\"}", &err) == 0);
    const cc_view *mine = view_named(&m, "Mine");
    char edit[200];
    snprintf(edit, sizeof edit, "{\"id\":\"%.22s\",\"name\":\"Section B-B\",\"depth_mm\":5000}", mine ? mine->id.c : "");
    check_true("view_edit name, depth", mine && run1(&h, &m, "view_edit", edit, &err) == 0);
    bb = NULL;
    const cc_view *made = NULL;
    for (size_t i = 0; i < m.nviews; i++)
        if (m.views[i].name.n == 11 && !memcmp(m.views[i].name.p, "Section B-B", 11)) {
            if (made || bb) (made ? (void)0 : (void)0);
            if (!bb && strncmp(m.views[i].id.c, mine ? mine->id.c : "", 22)) bb = &m.views[i];
            else made = &m.views[i];
        }
    check_true("both there", bb && made);
    if (bb && made) {
        cc_command c1, c2;
        memset(&c1, 0, sizeof c1), memset(&c2, 0, sizeof c2);
        c1.op = c2.op = CC_CMD_ADD_VIEW;
        c1.view = *bb, c2.view = *made;
        c2.view.id = bb->id;
        cc_commands_json(&c1, 1, &a), cc_commands_json(&c2, 1, &b);
        check_eq("view_add + view_edit: Rust's view, field for field", b.p, b.len, a.p, a.len);
    }
    cc_history_free(&h);
    cc_model_free(&m);
    buf_free(&w), buf_free(&err), buf_free(&a), buf_free(&b);
    free(text);
}

/* ---- the session: view_clip of sections, visibility per view ---- */

static int call(cc_session *s, const char *op, const char *name, const char *body, Buf *out) {
    buf_init(out);
    return cc_call(s, op, name, body, strlen(body), out);
}

static char *ask(cc_session *s, const char *op, const char *name, const char *body, int *st) {
    Buf b;
    int r = call(s, op, name, body, &b);
    if (st) *st = r;
    const char *t = buf_cstr(&b);
    char *z = malloc(strlen(t) + 1);
    strcpy(z, t);
    buf_free(&b);
    return z;
}

/* The number after `"key":` in `json` (first one), NaN when none. */
static double num_at(const char *json, const char *key, int index) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = strstr(json, k);
    if (!p) return 0.0 / 0.0;
    p += strlen(k);
    if (*p == '[') p++;
    for (int i = 0; i < index; i++) {
        p = strchr(p, ',');
        if (!p) return 0.0 / 0.0;
        p++;
    }
    return strtod(p, NULL);
}

static double clip_at(const char *json, const char *mm, int k) { /* clip.min[k] / clip.max[k] */
    const char *c = strstr(json, "\"clip\":{");
    if (!c) return 0.0 / 0.0;
    return num_at(c, mm, k);
}

static void go(cc_session *s, const char *id) {
    char body[96];
    snprintf(body, sizeof body, "{\"op\":\"open\",\"tab\":{\"type\":\"view\",\"id\":\"%.22s\"}}", id);
    Buf o;
    call(s, "tab", "-", body, &o);
    buf_free(&o);
}

/* The id of the last stored view named `name`. */
static const char *stored(cc_session *s, const char *name) {
    const cc_view *v = view_named(cc_session_model(s), name);
    return v ? v->id.c : "";
}

static void section_clip_vs_d4(void) {
    const cc_platform *pf = cc_platform_native();
    cc_session *s = cc_session_new(pf);
    cc_error err;
    if (!s || cc_session_open(s, "golden/drawing/model.json", &err)) {
        printf("FAIL open drawing/model.json: %s\n", s ? err.msg : "oom");
        failed++;
        return;
    }
    const cc_model *m = cc_session_model(s);
    const char *names[2] = {"Section A", "Section B-B"};
    for (int i = 0; i < 2; i++) {
        const cc_view *v = view_named(m, names[i]);
        check_true(names[i], v != NULL);
        if (!v) continue;
        go(s, v->id.c);
        int st;
        char *c = ask(s, "query", "view_clip", "", &st);
        char what[160];
        snprintf(what, sizeof what, "%s: view_clip is a section with a box, its facing, no cut height", names[i]);
        check_true(what, st == CC_OK && strstr(c, "\"type\":\"section\"") && strstr(c, "\"clip\":{") &&
                             strstr(c, v->facing == CC_FACING_NORTH ? "\"facing\":\"north\"" : "\"facing\":\"east\"") &&
                             strstr(c, "\"cut\":null"));
        const cc_scene *sc = cc_session_scene(s);
        cc_view_setup su;
        Buf e2;
        buf_init(&e2);
        int rc = cc_view_prepare(sc, m, v, &su, &e2);
        check_true("D4 prepares the section", rc == 0);
        int k = v->facing == CC_FACING_NORTH ? 1 : 0; /* both look along +k */
        double cut = clip_at(c, "min", k), far = clip_at(c, "max", k);
        snprintf(what, sizeof what, "%s: the cut plane is D4's (%g vs %g)", names[i], cut, -su.cut);
        check_true(what, rc == 0 && (float)cut == (float)-su.cut);
        if (v->depth_mm > 0) {
            snprintf(what, sizeof what, "%s: the depth is D4's (%g vs %g)", names[i], far, -su.level);
            check_true(what, (float)far == (float)-su.level);
        } else {
            snprintf(what, sizeof what, "%s: depth 0: to the model's bounds (D4: unlimited)", names[i]);
            check_true(what, su.level < -1e300 && far > cut);
        }
        double dx = num_at(c, "dir", 0), dy = num_at(c, "dir", 1);
        check_true("dir is the facing", (k == 1 && dx == 0 && dy == 1) || (k == 0 && dx == 1 && dy == 0));
        cc_view_setup_free(&su);
        buf_free(&e2);
        free(c);
    }
    cc_session_free(s);
}

static size_t count(const char *hay, const char *needle) {
    size_t n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

/* The "by_view" list of visibility_sets: its number of ids. */
static size_t by_view(cc_session *s) {
    char *v = ask(s, "query", "visibility_sets", "", NULL);
    const char *p = strstr(v, "\"by_view\":["), *q = p ? strchr(p, ']') : NULL;
    size_t n = 0;
    for (const char *x = p; x && x < q; x++) n += *x == '"';
    free(v);
    return n > 2 ? (n - 2) / 2 : 0;
}

/* The number of undo steps (query history: {"redo":[…],"undo":[labels]}). */
static size_t undo_steps(cc_session *s) {
    char *v = ask(s, "query", "history", "", NULL);
    const char *p = strstr(v, "\"undo\":["), *q = p ? strchr(p, ']') : NULL;
    size_t n = 0;
    for (const char *x = p; x && x < q; x++) n += *x == '"';
    free(v);
    return n > 2 ? (n - 2) / 2 : 0;
}

static size_t hidden_in_view(cc_session *s) {
    char *v = ask(s, "query", "visibility_sets", "", NULL);
    const char *p = strstr(v, "\"hidden_in_view\":["), *q = p ? strchr(p, ']') : NULL;
    size_t n = 0;
    for (const char *x = p; x && x < q; x++) n += *x == '"';
    free(v);
    return n > 2 ? (n - 2) / 2 : 0;
}

static void per_view(void) {
    const cc_platform *pf = cc_platform_native();
    cc_session *s = cc_session_new(pf);
    cc_error err;
    if (!s || cc_session_open(s, "golden/frame.json", &err)) {
        printf("FAIL open frame.json: %s\n", s ? err.msg : "oom");
        failed++;
        return;
    }
    const cc_model *m = cc_session_model(s);
    size_t slabs = 0;
    for (size_t i = 0; i < m->nelements; i++) {
        const cc_scene *sc = cc_session_scene(s);
        (void)sc;
    }
    {
        const cc_scene *sc = cc_session_scene(s);
        for (size_t k = 0; sc && k < sc->nelements; k++)
            slabs += sc->elements[k].class_name.n == 7 && !memcmp(sc->elements[k].class_name.p, "IfcSlab", 7);
    }
    printf("  frame: %zu slabs\n", slabs);
    int st;
    /* A section through the middle, looking north (no origin: the session's) */
    char *r = ask(s, "verb", "view_add", "{\"kind\":\"section\",\"facing\":\"north\"}", &st);
    check_true("view_add section without origin: ok", st == CC_OK);
    free(r);
    const cc_view *sec = view_named(m, "Section 1 (looking north)");
    check_true("named as Rust: Section 1 (looking north)", sec != NULL);
    if (sec) {
        const cc_scene *sc = cc_session_scene(s);
        printf("  section origin %g %g\n", sec->origin[0], sec->origin[1]);
        check_true("origin: the middle of the frame, rounded (9000, 5000)", sec->origin[0] == 9000.0 && sec->origin[1] == 5000.0);
        go(s, sec->id.c);
        char *c = ask(s, "query", "view_clip", "", &st);
        double y0 = clip_at(c, "min", 1);
        check_true("section_clip_keeps_what_is_beyond_the_cut: min.y + origin = 5000", sc && y0 + sc->origin[1] == 5000.0);
        free(c);
        char edit[160];
        snprintf(edit, sizeof edit, "{\"id\":\"%.22s\",\"facing\":\"west\",\"depth_mm\":2000}", sec->id.c);
        r = ask(s, "verb", "view_edit", edit, &st);
        free(r);
        c = ask(s, "query", "view_clip", "", &st);
        double x1 = clip_at(c, "max", 0), x0 = clip_at(c, "min", 0);
        check_true("west, depth 2 m: max.x at the cut, min.x 2 m beyond",
                   sc && x1 + sc->origin[0] == 9000.0 && x0 + sc->origin[0] == 7000.0);
        free(c);
    }
    /* IfcSlab hidden in the plan of Niveau 1 only */
    const char *plan = NULL;
    char *ov = ask(s, "query", "open_views", "", &st);
    static char plan_id[23];
    const char *p = strstr(ov, "\"name\":\"Plan Niveau 1\"");
    (void)p;
    cc_gid n1;
    memcpy(n1.c, N1, 22);
    cc_gid g = cc_view_derived_id("bimble view plan down ", &n1);
    memcpy(plan_id, g.c, 22);
    plan = plan_id;
    free(ov);
    go(s, plan);
    check_true("plan: nothing hidden by the view", by_view(s) == 0);
    char edit[200];
    snprintf(edit, sizeof edit, "{\"id\":\"%.22s\",\"visibility\":{\"hidden_classes\":[\"IfcSlab\"]}}", plan);
    r = ask(s, "verb", "view_edit", edit, &st);
    check_true("view_edit hidden_classes IfcSlab: ok", st == CC_OK);
    free(r);
    check_true("plan: every slab hidden by the view", slabs > 0 && by_view(s) == slabs);
    char *ac = ask(s, "query", "actions", "", NULL);
    check_true("show_all_in_view enabled", strstr(ac, "\"name\":\"show_all_in_view\"") != NULL);
    free(ac);
    go(s, stored(s, "Section 1 (looking north)"));
    check_true("the section: slabs shown", by_view(s) == 0);
    go(s, plan);
    check_true("back in the plan: slabs hidden again", by_view(s) == slabs);
    r = ask(s, "action", "undo", "", &st);
    free(r);
    check_true("undo: slabs shown in the plan", by_view(s) == 0);
    r = ask(s, "action", "redo", "", &st);
    free(r);
    check_true("redo: hidden again", by_view(s) == slabs);
    /* hide in view: stored with the view */
    char sel[64];
    snprintf(sel, sizeof sel, "[\"%.22s\"]", m->elements[0].id.c);
    Buf o;
    call(s, "select", "-", sel, &o);
    buf_free(&o);
    size_t steps = undo_steps(s);
    r = ask(s, "action", "hide_in_view", "", &st);
    free(r);
    check_true("hide in view: one hidden in the plan", hidden_in_view(s) == 1);
    const cc_view *pv = cc_model_view(cc_session_model(s), &g);
    check_true("stored in the view (hidden_elements)", pv && pv->visibility.hidden_elements.n == 1);
    check_true("one undo step", undo_steps(s) == steps + 1);
    go(s, stored(s, "Section 1 (looking north)"));
    check_true("another view: nothing hidden in it", hidden_in_view(s) == 0);
    go(s, plan);
    check_true("back: hidden again", hidden_in_view(s) == 1);
    r = ask(s, "action", "show_all_in_view", "", &st);
    free(r);
    check_true("show all in view: nothing hidden, classes too", hidden_in_view(s) == 0 && by_view(s) == 0);
    pv = cc_model_view(cc_session_model(s), &g);
    check_true("stored: the view's visibility is empty", pv && !pv->visibility.hidden_elements.n && !pv->visibility.hidden_classes.n);
    r = ask(s, "action", "undo", "", &st);
    free(r);
    check_true("undo show all: back as it was", hidden_in_view(s) == 1 && by_view(s) == slabs);
    cc_session_free(s);
}

int main(int argc, char **argv) {
    (void)argc, (void)argv;
    transcript();
    rust_form();
    section_clip_vs_d4();
    per_view();
    return report("verbs_views");
}
