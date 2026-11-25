/* Suite verbs_all: the 14 verbs against Rust's transcripts of every verb (golden/verbs_all.txt
 * on frame.json, golden/frame2/verbs_all.txt on frame2.json, with their after-models; format in
 * golden/README.md), the verb table's schemas and list (golden/verbs_schema/, verbs_list.json)
 * through the `describe` and `verbs` queries, and the C additions dry_run and if_rev. */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <string.h>
#include "../src/command.h"
#include "../src/file.h"
#include "../src/guid.h"
#include "../src/history.h"
#include "../src/model.h"
#include "../src/query.h"
#include "../src/verb_table.h"
#include "../src/verbs.h"
#include "../src/view_io.h"
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

static void labels_lines(const cc_history *h, Buf *out) {
    buf_puts(out, "undo ");
    cc_history_labels(h, 0, out, NULL);
    buf_puts(out, "\nredo ");
    cc_history_labels(h, 0, NULL, out);
    buf_putc(out, '\n');
}

static void add_known(cc_gid g) {
    if (nknown < sizeof known / sizeof known[0]) known[nknown++] = g;
}

static cc_gid derived(const char *a, const char *b, size_t bn, const char *c) {
    char seed[128];
    size_t n = strlen(a);
    memcpy(seed, a, n);
    memcpy(seed + n, b, bn), n += bn;
    memcpy(seed + n, c, strlen(c)), n += strlen(c);
    cc_guid g = cc_guid_derived(seed, n);
    cc_gid o;
    memcpy(o.c, g.c, 22);
    return o;
}

/* Replays golden/<dir>/verbs_all.txt on golden/<model>; compares the transcript and the model
 * after it. */
static void transcript(const char *model, const char *txt, const char *after_name, int want_steps) {
    size_t fn, tn, an;
    char *frame = slurp(golden, model, &fn);
    char *want = slurp(golden, txt, &tn);
    char *want_after = slurp(golden, after_name, &an);
    nknown = 0, nnews = 0;
    cc_model m;
    cc_error e;
    cc_guid_seed(1);
    if (cc_model_load(&m, frame, fn, &e)) {
        printf("%s does not load: %s\n", model, e.msg);
        exit(2);
    }
    add_known(m.project_id);
    for (size_t i = 0; i < m.nstoreys; i++) add_known(m.storeys[i].id);
    for (size_t i = 0; i < m.nmaterials; i++) add_known(m.materials[i].id);
    for (size_t i = 0; i < m.nelements; i++) add_known(m.elements[i].id);
    add_known(derived("bimble view 3d", "", 0, ""));
    for (size_t i = 0; i < m.nstoreys; i++) {
        add_known(derived("bimble view plan down ", m.storeys[i].id.c, 22, ""));
        add_known(derived("bimble view plan up ", m.storeys[i].id.c, 22, ""));
    }
    for (size_t i = 0; i < m.nviews; i++) add_known(m.views[i].id);
    add_known(derived("", m.project_id.c, 22, "/site"));
    add_known(derived("", m.project_id.c, 22, "/building"));

    cc_history h;
    cc_history_init(&h);
    Buf got, args, ans, err, after;
    buf_init(&got), buf_init(&args), buf_init(&ans), buf_init(&err), buf_init(&after);
    int steps = 0;
    for (const char *line = want; line < want + tn;) {
        const char *eol = memchr(line, '\n', (size_t)(want + tn - line));
        if (!eol) eol = want + tn;
        if (eol - line > 4 && memcmp(line, ">>> ", 4) == 0) {
            const char *op = line + 4, *sp1 = memchr(op, ' ', (size_t)(eol - op));
            const char *name = sp1 + 1, *sp2 = memchr(name, ' ', (size_t)(eol - name));
            const char *a = sp2 + 1;
            char opz[16], namez[64];
            snprintf(opz, sizeof opz, "%.*s", (int)(sp1 - op), op);
            snprintf(namez, sizeof namez, "%.*s", (int)(sp2 - name), name);
            buf_clear(&args);
            unmask(a, (size_t)(eol - a), &args);
            buf_clear(&ans);
            buf_clear(&err);
            int ok = 1;
            if (strcmp(opz, "verb") == 0) {
                cc_cmds l;
                cc_cmds_init(&l);
                if (cc_verb_run(&m, namez, args.p, args.len, &l, &err) ||
                    (cc_commands_json(l.v, l.n, &ans), cc_history_apply(&h, &m, l.v, l.n, &err)))
                    ok = 0;
                cc_cmds_free(&l);
            } else if (strcmp(opz, "query") == 0) {
                ok = cc_query(&m, namez, args.p, args.len, &ans) == 200;
                if (!ok) buf_append(&err, ans.p, ans.len);
            } else {
                int r = strcmp(opz, "redo") == 0 ? cc_history_redo(&h, &m) : cc_history_undo(&h, &m);
                buf_puts(&ans, r ? "true" : "false");
            }
            if (ok) {
                Buf t;
                buf_init(&t);
                mask(ans.p, ans.len, &t, 0);
                buf_clear(&ans);
                buf_append(&ans, t.p, t.len);
                buf_free(&t);
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
        }
        line = eol + 1;
    }
    model_text(&m, &after);
    printf("%s: %d steps\n", txt, steps);
    check_true("step count", steps == want_steps);
    check_eq(txt, got.p, got.len, want, tn);
    if (got.len != tn || memcmp(got.p, want, tn)) {
        size_t i = 0, ln = 1;
        while (i < got.len && i < tn && got.p[i] == want[i]) ln += got.p[i++] == '\n';
        size_t b = i;
        while (b && got.p[b - 1] != '\n') b--;
        printf("first difference at byte %zu (line %zu):\n  got:  %.300s\n  want: %.300s\n", i, ln, got.p + b, want + b);
    }
    check_eq(after_name, after.p, after.len, want_after, an);
    if (after.len != an || memcmp(after.p, want_after, an)) {
        size_t i = 0;
        while (i < after.len && i < an && after.p[i] == want_after[i]) i++;
        printf("first difference at byte %zu:\n  got:  %.200s\n  want: %.200s\n", i, after.p + i, want_after + i);
    }
    /* Undo everything: the file's bytes again. */
    while (cc_history_undo(&h, &m)) {}
    Buf s;
    buf_init(&s);
    cc_model_save(&m, &s);
    check_eq("undo all = the model file", s.p, s.len, frame, fn);
    buf_free(&s);
    cc_guid_seed(0);
    cc_history_free(&h);
    cc_model_free(&m);
    buf_free(&got), buf_free(&args), buf_free(&ans), buf_free(&err), buf_free(&after);
    free(frame), free(want), free(want_after);
}

/* serde_json::to_string_pretty of compact JSON text (2 spaces, "key": value), plus "\n". */
static void pretty(const char *s, size_t n, Buf *out) {
    int depth = 0, in = 0, esc = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (in) {
            buf_putc(out, c);
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') in = 0;
            continue;
        }
        switch (c) {
        case '"': in = 1, buf_putc(out, c); break;
        case '{':
        case '[':
            if (i + 1 < n && (s[i + 1] == '}' || s[i + 1] == ']')) {
                buf_putc(out, c), buf_putc(out, s[++i]);
                break;
            }
            buf_putc(out, c), buf_putc(out, '\n'), depth++;
            buf_fill(out, ' ', 2 * (size_t)depth);
            break;
        case '}':
        case ']':
            depth--;
            buf_putc(out, '\n'), buf_fill(out, ' ', 2 * (size_t)depth), buf_putc(out, c);
            break;
        case ',': buf_puts(out, ",\n"), buf_fill(out, ' ', 2 * (size_t)depth); break;
        case ':': buf_puts(out, ": "); break;
        default: buf_putc(out, c);
        }
    }
    buf_putc(out, '\n');
}

static void schemas(void) {
    cc_model m;
    cc_model_init(&m);
    Buf ans, pr;
    buf_init(&ans), buf_init(&pr);
    size_t wn;
    char *want = slurp(golden, "verbs_list.json", &wn);
    check_true("verbs query", cc_query(&m, "verbs", "", 0, &ans) == 200);
    pretty(ans.p, ans.len, &pr);
    check_eq("verbs_list.json", pr.p, pr.len, want, wn);
    free(want);
    check_true("14 verbs", cc_verb_count == 14);
    for (size_t i = 0; i < cc_verb_count; i++) {
        char path[128], args[128];
        snprintf(path, sizeof path, "verbs_schema/%s.json", cc_verb_table[i].name);
        snprintf(args, sizeof args, "{\"name\":\"%s\"}", cc_verb_table[i].name);
        want = slurp(golden, path, &wn);
        buf_clear(&ans), buf_clear(&pr);
        check_true(path, cc_query(&m, "describe", args, strlen(args), &ans) == 200);
        pretty(ans.p, ans.len, &pr);
        check_eq(path, pr.p, pr.len, want, wn);
        check_true("not read-only", cc_verb_table[i].read_only == 0);
        free(want);
    }
    buf_clear(&ans);
    check_true("describe nope", cc_query(&m, "describe", "{\"name\":\"nope\"}", 15, &ans) == 400);
    check_eq("describe nope text", ans.p, ans.len, "no verb with this name", 22);
    buf_clear(&ans);
    check_true("describe without name", cc_query(&m, "describe", "{}", 2, &ans) == 400);
    buf_free(&ans), buf_free(&pr);
    cc_model_free(&m);
}

/* dry_run answers the commands and leaves the model; if_rev refuses a moved model. */
static void dry_run_if_rev(void) {
    size_t fn;
    char *frame = slurp(golden, "frame.json", &fn);
    cc_model m;
    cc_error e;
    if (cc_model_load(&m, frame, fn, &e)) exit(2);
    Buf err, a, b, s;
    buf_init(&err), buf_init(&a), buf_init(&b), buf_init(&s);
    const char *args = "{\"ids\":[\"004z1Cm4z4hgNOg0ez4Id1\"],\"angle_deg\":30}";
    cc_cmds l;
    cc_cmds_init(&l);
    check_true("dry run", cc_verb_run_ex(&m, "rotate", args, strlen(args), 1, 7, 7, &l, &err) == 0 && l.n == 1);
    cc_commands_json(l.v, l.n, &a);
    cc_cmds_free(&l);
    cc_model_save(&m, &s);
    check_eq("dry run leaves the model", s.p, s.len, frame, fn);
    cc_cmds_init(&l);
    check_true("real run", cc_verb_run_ex(&m, "rotate", args, strlen(args), 0, -1, 7, &l, &err) == 0);
    cc_commands_json(l.v, l.n, &b);
    cc_cmds_free(&l);
    check_eq("dry run answers the real commands", a.p, a.len, b.p, b.len);
    cc_cmds_init(&l);
    buf_clear(&err);
    check_true("if_rev mismatch", cc_verb_run_ex(&m, "rotate", args, strlen(args), 0, 6, 7, &l, &err) == CC_VERB_CONFLICT &&
                                      l.n == 0);
    const char *want = "conflict: the model changed (rev 7, expected 6)";
    check_eq("conflict text", err.p, err.len, want, strlen(want));
    buf_clear(&err);
    check_true("unknown verb before if_rev", cc_verb_run_ex(&m, "fly", "{}", 2, 0, 6, 7, &l, &err) == -1 &&
                                                 strncmp(buf_cstr(&err), "unknown verb \"fly\"", 18) == 0);
    cc_cmds_free(&l);
    buf_free(&err), buf_free(&a), buf_free(&b), buf_free(&s);
    cc_model_free(&m);
    free(frame);
}

int main(int argc, char **argv) {
    golden = argc > 1 ? argv[1] : "golden/floats";
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", golden); /* run.sh passes golden/floats */
    golden = dir;
    transcript("frame.json", "verbs_all.txt", "verbs_all_after.json", 76);
    transcript("frame2.json", "frame2/verbs_all.txt", "frame2/verbs_all_after.json", 106);
    schemas();
    dry_run_if_rev();
    return report("verbs_all");
}
