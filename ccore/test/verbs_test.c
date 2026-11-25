/* Suite verbs: commands, history and the six verbs against Rust's transcript (golden/verbs.txt,
 * verbs_after_undo.json, verbs_after.json; format and masking in golden/README.md), plus
 * hand-made cases for what the transcript does not reach (rename, set_project, rollback, JSON). */
#define _POSIX_C_SOURCE 199309L /* clock_gettime under -std=c11 */
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "../src/command.h"
#include "../src/file.h"
#include "../src/guid.h"
#include "../src/history.h"
#include "../src/library.h"
#include "../src/model.h"
#include "../src/verbs.h"
#include "check.h"

static const char *golden;

/* ---- masking (golden/README.md "Masking of new GlobalIds") ---- */

#define MAX_NEW 256
static cc_gid known[1024];
static size_t nknown;
static cc_gid news[MAX_NEW]; /* news[N - 1] is <new-N> */
static size_t nnews;

static int is_id_char(char c) { return c != 0 && strchr(cc_gid_alphabet, c) != NULL; }

static int is_known(const char *s) {
    for (size_t i = 0; i < nknown; i++)
        if (memcmp(known[i].c, s, 22) == 0) return 1;
    return 0;
}

/* N of a new id (numbered on first sight), 0 for a frame id or not an id. */
static size_t number(const char *s, size_t n) {
    if (n != 22 || !cc_gid_valid(s, n) || is_known(s)) return 0;
    if (memcmp(s, "0000000000000000000000", 22) == 0) return 0;
    for (size_t i = 0; i < nnews; i++)
        if (memcmp(news[i].c, s, 22) == 0) return i + 1;
    if (nnews == MAX_NEW) return 0;
    memcpy(news[nnews].c, s, 22);
    return ++nnews;
}

/* Each token that is a new id: "<new-N>" (mode 0) or from_u128(N) (mode 1). */
static void mask(const char *s, size_t n, Buf *out, int mode) {
    for (size_t i = 0; i < n;) {
        if (!is_id_char(s[i])) { buf_putc(out, s[i++]); continue; }
        size_t j = i;
        while (j < n && is_id_char(s[j])) j++;
        size_t k = number(s + i, j - i);
        if (!k) buf_append(out, s + i, j - i);
        else if (mode == 0) buf_puts(out, "<new-"), buf_u64(out, k), buf_putc(out, '>');
        else {
            cc_guid g = cc_guid_from_u128(0, k);
            buf_append(out, g.c, 22);
        }
        i = j;
    }
}

/* Tokens equal to from_u128(N) of a numbered N back to "<new-N>". */
static void remask(const char *s, size_t n, Buf *out) {
    for (size_t i = 0; i < n;) {
        if (!is_id_char(s[i])) { buf_putc(out, s[i++]); continue; }
        size_t j = i;
        while (j < n && is_id_char(s[j])) j++;
        size_t hit = 0;
        for (size_t k = 1; j - i == 22 && k <= nnews && !hit; k++) {
            cc_guid g = cc_guid_from_u128(0, k);
            if (memcmp(g.c, s + i, 22) == 0) hit = k;
        }
        if (hit) buf_puts(out, "<new-"), buf_u64(out, hit), buf_putc(out, '>');
        else buf_append(out, s + i, j - i);
        i = j;
    }
}

/* "<new-N>" back to the id the C core made. */
static void unmask(const char *s, size_t n, Buf *out) {
    for (size_t i = 0; i < n;) {
        if (n - i > 6 && memcmp(s + i, "<new-", 5) == 0) {
            size_t j = i + 5, v = 0;
            while (j < n && s[j] >= '0' && s[j] <= '9') v = v * 10 + (size_t)(s[j++] - '0');
            if (j < n && s[j] == '>' && v >= 1 && v <= nnews) {
                buf_append(out, news[v - 1].c, 22);
                i = j + 1;
                continue;
            }
        }
        buf_putc(out, s[i++]);
    }
}

/* The dumper's model_text: number new ids, rename them from_u128(N), load and save (sorted),
 * then mask. */
static void model_text(const cc_model *m, Buf *out) {
    Buf t, r, s;
    buf_init(&t), buf_init(&r), buf_init(&s);
    cc_model_save(m, &t);
    mask(t.p, t.len, &r, 1);
    cc_model back;
    cc_error e;
    if (cc_model_load(&back, r.p, r.len, &e)) printf("renumbered model does not load: %s\n", e.msg);
    cc_model_save(&back, &s);
    cc_model_free(&back);
    remask(s.p, s.len, out);
    buf_free(&t), buf_free(&r), buf_free(&s);
}

/* ---- the transcript ---- */

static void labels_lines(const cc_history *h, Buf *out) {
    buf_puts(out, "undo ");
    cc_history_labels(h, 0, out, NULL);
    buf_puts(out, "\nredo ");
    cc_history_labels(h, 0, NULL, out);
    buf_putc(out, '\n');
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void transcript(void) {
    size_t fn, tn, un, an;
    char *frame = slurp(golden, "frame.json", &fn);
    char *want = slurp(golden, "verbs.txt", &tn);
    char *want_undo = slurp(golden, "verbs_after_undo.json", &un);
    char *want_after = slurp(golden, "verbs_after.json", &an);
    cc_model m;
    cc_error e;
    cc_guid_seed(1);
    if (cc_model_load(&m, frame, fn, &e)) {
        printf("frame.json does not load: %s\n", e.msg);
        exit(2);
    }
    known[nknown++] = m.project_id;
    for (size_t i = 0; i < m.nstoreys && nknown < 1024; i++) known[nknown++] = m.storeys[i].id;
    for (size_t i = 0; i < m.nmaterials && nknown < 1024; i++) known[nknown++] = m.materials[i].id;
    for (size_t i = 0; i < m.nelements && nknown < 1024; i++) known[nknown++] = m.elements[i].id;

    cc_history h;
    cc_history_init(&h);
    Buf got, args, ans, err, after_undo, after;
    buf_init(&got), buf_init(&args), buf_init(&ans), buf_init(&err), buf_init(&after_undo), buf_init(&after);
    int undos = 0, steps = 0;
    double t0 = now_ms();
    for (const char *line = want; line < want + tn;) {
        const char *eol = memchr(line, '\n', (size_t)(want + tn - line));
        if (!eol) eol = want + tn;
        if (eol - line > 4 && memcmp(line, ">>> ", 4) == 0) {
            /* >>> <op> <name> <args-json> */
            const char *op = line + 4, *sp1 = memchr(op, ' ', (size_t)(eol - op));
            const char *name = sp1 + 1, *sp2 = memchr(name, ' ', (size_t)(eol - name));
            const char *a = sp2 + 1;
            char opz[16], namez[64];
            snprintf(opz, sizeof opz, "%.*s", (int)(sp1 - op), op);
            snprintf(namez, sizeof namez, "%.*s", (int)(sp2 - name), name);
            buf_clear(&args);
            unmask(a, (size_t)(eol - a), &args);
            buf_clear(&ans);
            int ok = 1;
            if (strcmp(opz, "verb") == 0) {
                cc_cmds l;
                cc_cmds_init(&l);
                buf_clear(&err);
                if (cc_verb_run(&m, namez, args.p, args.len, &l, &err) ||
                    (cc_commands_json(l.v, l.n, &ans), cc_history_apply(&h, &m, l.v, l.n, &err))) {
                    ok = 0;
                } else {
                    Buf t;
                    buf_init(&t);
                    mask(ans.p, ans.len, &t, 0);
                    buf_clear(&ans);
                    buf_append(&ans, t.p, t.len);
                    buf_free(&t);
                }
                cc_cmds_free(&l);
            } else {
                int r = strcmp(opz, "redo") == 0 ? cc_history_redo(&h, &m) : cc_history_undo(&h, &m);
                buf_puts(&ans, r ? "true" : "false");
            }
            buf_puts(&got, ">>> ");
            buf_puts(&got, opz);
            buf_putc(&got, ' ');
            buf_puts(&got, namez);
            buf_putc(&got, ' ');
            mask(args.p, args.len, &got, 0);
            buf_puts(&got, ok ? "\n200 " : "\n400 ");
            if (ok) buf_append(&got, ans.p, ans.len);
            else buf_append(&got, err.p, err.len);
            buf_putc(&got, '\n');
            labels_lines(&h, &got);
            buf_puts(&got, "elements ");
            buf_u64(&got, m.nelements);
            buf_puts(&got, "\n\n");
            steps++;
            if (strcmp(opz, "undo") == 0 && ++undos == 3) model_text(&m, &after_undo);
        }
        line = eol + 1;
    }
    double t1 = now_ms();
    model_text(&m, &after);
    printf("transcript: %d steps replayed in %.3f ms\n", steps, t1 - t0);
    check_true("transcript has 18 steps", steps == 18);
    check_eq("verbs.txt", got.p, got.len, want, tn);
    check_eq("verbs_after_undo.json", after_undo.p, after_undo.len, want_undo, un);
    check_eq("verbs_after.json", after.p, after.len, want_after, an);
    if (got.len != tn || memcmp(got.p, want, tn)) {
        size_t i = 0;
        while (i < got.len && i < tn && got.p[i] == want[i]) i++;
        printf("first difference at byte %zu:\n  got:  %.200s\n  want: %.200s\n", i, got.p + i, want + i);
    }

    /* Undo everything: the file's bytes again. */
    while (cc_history_undo(&h, &m)) {}
    Buf s;
    buf_init(&s);
    cc_model_save(&m, &s);
    check_eq("undo all = frame.json", s.p, s.len, frame, fn);
    check_true("nothing left to undo", h.nundo == 0 && cc_history_undo(&h, &m) == 0);
    /* Redo everything (8 steps on the redo stack): the model at the end again. */
    while (cc_history_redo(&h, &m)) {}
    buf_clear(&after_undo);
    model_text(&m, &after_undo);
    check_eq("redo all = verbs_after.json", after_undo.p, after_undo.len, want_after, an);

    buf_free(&s);
    cc_history_free(&h);
    cc_model_free(&m);
    buf_free(&got), buf_free(&args), buf_free(&ans), buf_free(&err), buf_free(&after_undo), buf_free(&after);
    free(frame), free(want), free(want_undo), free(want_after);
}

/* ---- hand-made cases ---- */

static cc_model frame_model(void) {
    size_t fn;
    char *frame = slurp(golden, "frame.json", &fn);
    cc_model m;
    cc_error e;
    if (cc_model_load(&m, frame, fn, &e)) exit(2);
    free(frame);
    return m;
}

/* apply(JSON) through the history; returns the error text ("" when it applied). */
static void apply_json(cc_history *h, cc_model *m, const char *json, Buf *err) {
    cc_cmds l;
    cc_cmds_init(&l);
    buf_clear(err);
    if (!cc_commands_parse(json, strlen(json), &l, err)) cc_history_apply(h, m, l.v, l.n, err);
    cc_cmds_free(&l);
}

static void verb(const cc_model *m, const char *name, const char *args, Buf *out) {
    cc_cmds l;
    cc_cmds_init(&l);
    buf_clear(out);
    Buf err;
    buf_init(&err);
    if (cc_verb_run(m, name, args, strlen(args), &l, &err)) buf_puts(out, "400 "), buf_append(out, err.p, err.len);
    else buf_puts(out, "200 "), cc_commands_json(l.v, l.n, out);
    buf_free(&err);
    cc_cmds_free(&l);
}

/* buf_cstr: never NULL (check_eq memcmps even an empty buffer). */
#define EQ(what, buf, lit) check_eq(what, buf_cstr(&(buf)), (buf).len, lit, strlen(lit))

static void hand(void) {
    cc_model m = frame_model();
    cc_history h;
    cc_history_init(&h);
    Buf err, b, before;
    buf_init(&err), buf_init(&b), buf_init(&before);
    cc_model_save(&m, &before);
    const char *col = "004z1Cm4z4hgNOg0ez4Id1";
    char json[512];

    /* rename: label, undo, the user check, an unknown id */
    snprintf(json, sizeof json, "[{\"op\":\"rename\",\"id\":\"%s\",\"name\":\"P-01\"}]", col);
    apply_json(&h, &m, json, &err);
    EQ("rename applies", err, "");
    buf_clear(&b);
    cc_history_json(&h, &b);
    const cc_element *e = cc_model_element(&m, (const cc_gid *)col);
    check_true("renamed", e && e->name.n == 4 && memcmp(e->name.p, "P-01", 4) == 0);
    check_true("rename label", strstr(b.p, "\"undo\":[\"rename ") != NULL && strstr(b.p, " to P-01\"]") != NULL);
    check_true("history json: redo first", strncmp(b.p, "{\"redo\":[],\"undo\":[", 19) == 0);
    snprintf(json, sizeof json, "[{\"op\":\"rename\",\"id\":\"%s\",\"name\":\" \\u3000 \"}]", col);
    apply_json(&h, &m, json, &err);
    EQ("rename blank", err, "invalid: the name is empty");
    apply_json(&h, &m, "[{\"op\":\"rename\",\"id\":\"0000000000000000000000\",\"name\":\"X\"}]", &err);
    EQ("rename unknown", err, "no such object: 0000000000000000000000");
    check_true("undo rename", cc_history_undo(&h, &m) == 1);

    /* set_project: validation, label, undo */
    apply_json(&h, &m, "[{\"op\":\"set_project\",\"project\":{\"client\":\"ACME\",\"address\":\"Rue 1\"}}]", &err);
    EQ("set_project applies", err, "");
    check_true("project sorted", m.nproject == 2 && memcmp(m.project[0].key.p, "address", 7) == 0);
    buf_clear(&b);
    cc_history_labels(&h, 0, &b, NULL);
    EQ("set_project label", b, "[\"change the project information\"]");
    apply_json(&h, &m, "[{\"op\":\"set_project\",\"project\":{\"bad key\":\"x\"}}]", &err);
    EQ("set_project bad key", err,
       "invalid: project information: \"bad key\" is not a key (letters, digits and _, at most 64)");
    check_true("undo set_project", cc_history_undo(&h, &m) == 1 && m.nproject == 0);

    /* set_property checks (Command::apply) */
    snprintf(json, sizeof json, "[{\"op\":\"set_property\",\"id\":\"%s\",\"key\":\"Note\",\"value\":\"a\\nb\"}]", col);
    apply_json(&h, &m, json, &err);
    EQ("value with a line break", err, "invalid: the value has a line break or a control character");
    snprintf(json, sizeof json, "[{\"op\":\"set_property\",\"id\":\"%s\",\"key\":\" Note\"}]", col);
    apply_json(&h, &m, json, &err);
    EQ("padded key", err, "invalid: the property name starts or ends with a space");

    /* A failing group keeps nothing (rollback), and pushes no step. */
    size_t nundo = h.nundo;
    snprintf(json, sizeof json,
             "[{\"op\":\"rename\",\"id\":\"%s\",\"name\":\"Z\"},{\"op\":\"remove_material\",\"id\":\"%.22s\"}]", col,
             m.materials[0].id.c);
    apply_json(&h, &m, json, &err);
    buf_clear(&b);
    cc_buf_gid(&b, &m.materials[0].id);
    buf_puts(&b, " is still used by elements");
    check_eq("remove a used material", err.p, err.len, b.p, b.len);
    buf_clear(&b);
    cc_model_save(&m, &b);
    check_eq("rollback leaves the model", b.p, b.len, before.p, before.len);
    check_true("rollback pushes no step", h.nundo == nundo);

    /* Storeys and materials: add, in use, remove, undo. */
    apply_json(&h, &m,
               "[{\"op\":\"add_storey\",\"id\":\"0000000000000000000001\",\"name\":\"Toit\",\"elevation\":6000.0},"
               "{\"op\":\"add_material\",\"id\":\"0000000000000000000002\",\"name\":\"S355\",\"category\":\"steel\"}]",
               &err);
    EQ("add storey and material", err, "");
    buf_clear(&b);
    cc_history_labels(&h, 1, &b, NULL);
    EQ("group label", b, "[\"add the storey Toit and 1 more change\"]");
    apply_json(&h, &m, "[{\"op\":\"add_storey\",\"id\":\"0000000000000000000001\",\"name\":\"T\",\"elevation\":1.0}]", &err);
    EQ("storey exists", err, "object already exists: 0000000000000000000001");
    apply_json(&h, &m, "[{\"op\":\"remove_storey\",\"id\":\"0000000000000000000001\"},{\"op\":\"remove_material\",\"id\":\"0000000000000000000002\"}]", &err);
    EQ("remove storey and material", err, "");
    check_true("undo remove", cc_history_undo(&h, &m) == 1 && m.nstoreys == 4 && m.nmaterials == 2);
    check_true("undo add", cc_history_undo(&h, &m) == 1 && m.nstoreys == 3 && m.nmaterials == 1);
    check_true("redo add", cc_history_redo(&h, &m) == 1 && m.nstoreys == 4);
    check_true("undo add again", cc_history_undo(&h, &m) == 1 && m.nstoreys == 3);

    /* Command JSON: parse then write gives serde's bytes; -0.0 kept in the command. */
    const char *cmd = "[{\"op\":\"add_element\",\"id\":\"0000000000000000000003\",\"class\":\"IfcBeam\",\"name\":\"B9\","
                      "\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"material\":\"0aEm4Mq4nC8emERFhb_kvJ\",\"placement\":"
                      "{\"origin\":[0.0,2500.0,2750.0],\"z_axis\":[1.0,0.0,0.0],\"x_axis\":[-0.0,1.0,0.0]},"
                      "\"generator\":{\"kind\":\"rect_beam\",\"width\":250.0,\"height\":500.0,\"length\":6000.0},"
                      "\"properties\":{\"Note\":\"x\"}},{\"op\":\"set_property\",\"id\":\"0000000000000000000003\",\"key\":\"Note\"},"
                      "{\"op\":\"set_element_material\",\"id\":\"0000000000000000000003\",\"material\":\"0aEm4Mq4nC8emERFhb_kvJ\"}]";
    cc_cmds l;
    cc_cmds_init(&l);
    buf_clear(&err);
    check_true("parse commands", cc_commands_parse(cmd, strlen(cmd), &l, &err) == 0 && l.n == 3);
    buf_clear(&b);
    cc_commands_json(l.v, l.n, &b);
    check_eq("command JSON round trip", b.p, b.len, cmd, strlen(cmd));
    check_true("apply element group", cc_history_apply(&h, &m, l.v, l.n, &err) == 0);
    e = cc_model_element(&m, (const cc_gid *)"0000000000000000000003");
    check_true("note removed, x_axis canonical", e && e->nproperties == 0 && !(*(const uint64_t *)&e->placement.x_axis[0] >> 63));
    cc_cmds_free(&l);
    check_true("undo element group", cc_history_undo(&h, &m) == 1 && !cc_model_element(&m, (const cc_gid *)"0000000000000000000003"));
    cc_cmds_init(&l);
    check_true("unknown op", cc_commands_parse("[{\"op\":\"set_view\"}]", 18, &l, &err) == -1);
    check_true("bad id", cc_commands_parse("[{\"op\":\"remove_element\",\"id\":\"x\"}]", 34, &l, &err) == -1);
    EQ("bad id text", err, "invalid IFC GlobalId: \"x\"");
    cc_cmds_free(&l);

    /* Verbs outside the transcript. */
    verb(&m, "copy", "{}", &b);
    EQ("copy without arguments", b, "400 bad arguments: missing field `ids`");
    verb(&m, "place", "{\"kind\":\"column\",\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"a\":[0,0],\"x\":1}", &b);
    EQ("unknown field", b, "400 bad arguments: unknown field `x`, expected one of `kind`, `storey`, `a`, `b`, `sizes`, `material`");
    verb(&m, "place", "{\"kind\":\"column\",\"a\":[0,0]}", &b);
    EQ("missing field", b, "400 bad arguments: missing field `storey`");
    verb(&m, "place", "{\"kind\":\"wall\",\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"a\":[0,0],\"b\":[3,4]}", &b);
    EQ("too close", b, "400 the two points are too close");
    verb(&m, "place", "{\"kind\":\"column\",\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"a\":[2e7,0]}", &b);
    EQ("out of range", b, "400 a: 20000000 is not a finite value within \xc2\xb1" "10000000 mm");
    verb(&m, "place", "{\"kind\":\"column\",\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"a\":[0,0],\"material\":\"Bois\"}", &b);
    EQ("unknown material", b, "400 unknown material: Bois");
    verb(&m, "set_note", "{\"id\":\"0000000000000000000000\",\"text\":\"x\"}", &b);
    EQ("note on no element", b, "400 no element GlobalId(0000000000000000000000)");
    snprintf(json, sizeof json, "{\"id\":\"%s\",\"params\":{\"thickness\":300}}", col);
    verb(&m, "set_generator", json, &b);
    EQ("not a size of this", b, "400 thickness: not a size of this \"rect_column\"");
    snprintf(json, sizeof json, "{\"id\":\"%s\",\"params\":{}}", col);
    verb(&m, "set_generator", json, &b);
    EQ("no size given", b, "400 params: no size given");
    snprintf(json, sizeof json, "{\"id\":\"%s\",\"text\":\" \"}", col);
    verb(&m, "set_note", json, &b);
    EQ("empty note on none: nothing", b, "200 []");
    snprintf(json, sizeof json, "{\"ids\":[\"%s\"],\"material\":\"C30/37\"}", col);
    verb(&m, "set_material", json, &b);
    EQ("same material: nothing", b, "200 []");
    /* A diagonal beam: the direction through cc_hypot (3-4-5: exact). */
    verb(&m, "place", "{\"kind\":\"beam\",\"storey\":\"1uAZA0Yfn05f$xRd1k4uPR\",\"a\":[0,0],\"b\":[3000,4000]}", &b);
    check_true("diagonal beam", strstr(b.p, "\"z_axis\":[0.6,0.8,0.0],\"x_axis\":[-0.8,0.6,0.0]") != NULL &&
                                    strstr(b.p, "\"length\":5000.0") != NULL);

    /* Library lookup. */
    const cc_library_material *lm = cc_library_lookup("Steel S355 J2", 13);
    check_true("lookup S355", lm && strcmp(lm->name, "S355") == 0);
    lm = cc_library_lookup("gl24h 24x46", 11);
    check_true("lookup GL24h", lm && strcmp(lm->name, "GL24h") == 0);
    lm = cc_library_lookup("Timber C24", 10);
    check_true("lookup C24", lm && strcmp(lm->name, "C24") == 0);
    check_true("lookup C25/30", (lm = cc_library_lookup("C25/30", 6)) && strcmp(lm->name, "C25/30") == 0);
    check_true("lookup none", cc_library_lookup("B\xc3\xa9ton", 6) == NULL);

    /* Maths. */
    check_true("sqrt 2", cc_sqrt(2.0) == 1.4142135623730951);
    check_true("sqrt 1e-310", cc_sqrt(1e-310) == 9.999999999999986e-156);
    check_true("hypot 3 4", cc_hypot(3.0, 4.0) == 5.0 && cc_hypot(-6000.0, 0.0) == 6000.0);
    check_true("hypot 1 1", cc_hypot(1.0, 1.0) == 1.4142135623730951);

    cc_history_free(&h);
    cc_model_free(&m);
    buf_free(&err), buf_free(&b), buf_free(&before);
}

/* Ticket J1: add_element grows the element array (twice the room when full) instead of copying
 * it into the arena on every add. 300 adds at shuffled positions: the array moves a few times,
 * not 300; every element is found, the order stays sorted; undoing them all gives back the
 * model's exact text. */
static void grow(void) {
    size_t fn;
    char *frame = slurp(golden, "frame.json", &fn);
    cc_model m;
    cc_error e;
    if (!frame || cc_model_load(&m, frame, fn, &e)) {
        check_true("grow: frame.json loads", 0);
        free(frame);
        return;
    }
    Buf before, after, err, cmd;
    buf_init(&before), buf_init(&after), buf_init(&err), buf_init(&cmd);
    cc_model_save(&m, &before);
    cc_history h;
    cc_history_init(&h);
    size_t moves = 0, n0 = m.nelements;
    int bad = 0;
    for (int i = 0; i < 300; i++) {
        const cc_element *old = m.elements;
        char id[23];
        int k = (i * 7919) % 300; /* shuffled: inserts land everywhere in the order */
        snprintf(id, sizeof id, "%c%s%03d", "0123"[k % 4], "J1grow000000000000", k);
        buf_clear(&cmd);
        buf_puts(&cmd, "[{\"op\":\"add_element\",\"id\":\"");
        buf_puts(&cmd, id);
        buf_puts(&cmd, "\",\"class\":\"IfcColumn\",\"name\":\"G\",\"storey\":\"");
        buf_append(&cmd, m.storeys[0].id.c, 22);
        buf_puts(&cmd, "\",\"material\":\"");
        buf_append(&cmd, m.materials[0].id.c, 22);
        buf_puts(&cmd, "\",\"placement\":{\"origin\":[1.0,2.0,0.0],\"z_axis\":[0.0,0.0,1.0],\"x_axis\":[1.0,0.0,0.0]},"
                       "\"generator\":{\"kind\":\"rect_column\",\"width\":300.0,\"depth\":300.0,\"height\":3000.0}}]");
        apply_json(&h, &m, buf_cstr(&cmd), &err);
        if (err.len) bad++;
        if (m.elements != old) moves++;
        cc_gid g;
        memcpy(g.c, id, 22);
        if (!cc_model_element(&m, &g)) bad++;
    }
    for (size_t i = 1; i < m.nelements; i++)
        if (cc_gid_cmp(&m.elements[i - 1].id, &m.elements[i].id) >= 0) bad++;
    check_true("grow: 300 adds applied, found, sorted", !bad && m.nelements == n0 + 300);
    check_true("grow: the array moved at most 8 times in 300 adds", moves <= 8);
    while (cc_history_undo(&h, &m)) {}
    cc_model_save(&m, &after);
    check_eq("grow: undo all gives the model back", after.p, after.len, before.p, before.len);
    cc_history_free(&h);
    cc_model_free(&m);
    buf_free(&before), buf_free(&after), buf_free(&err), buf_free(&cmd);
    free(frame);
}

int main(int argc, char **argv) {
    golden = argc > 1 ? argv[1] : "golden/floats";
    /* run.sh passes golden/floats: the verbs goldens are one level up. */
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", golden);
    golden = dir;
    transcript();
    hand();
    grow();
    return report("verbs");
}
