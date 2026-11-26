/* Suite viewsheet: views, sheets and schedules in the C model (ticket A6a).
 * - golden/verbs_all_after.json, golden/frame2/verbs_all_after.json (ids unmasked: <new-N> is
 *   from_u128(N), golden/README.md): load then save gives the same bytes; v2 round trip.
 * - golden/verbs_all.txt, golden/frame2/verbs_all.txt: the commands of every 200 verb record
 *   (as Rust's verbs made them) applied through cc_history_apply, undo/redo replayed; the undo and
 *   redo label lists, element counts and the final model text against Rust's. The commands
 *   written back as JSON are the transcript's bytes.
 * - hand cases: every view kind and field (visibility, references, annotations, crop), sheets with
 *   placed views and texts, validation texts, inverses, labels, broken input. */
#include <stdint.h>
#include <string.h>
#include "../src/command.h"
#include "../src/file.h"
#include "../src/guid.h"
#include "../src/history.h"
#include "../src/inflate.h"
#include "../src/model.h"
#include "../src/view_io.h"
#include "check.h"

static const char *golden;

#define EQ(what, buf, want) check_eq(what, (buf).p ? (buf).p : "", (buf).len, want, strlen(want))

static int contains(const char *s, const char *sub) { return strstr(s, sub) != NULL; }

/* "<new-N>" -> from_u128(N) */
static void unmask(const char *s, size_t n, Buf *out) {
    for (size_t i = 0; i < n;) {
        if (i + 5 < n && !memcmp(s + i, "<new-", 5)) {
            size_t j = i + 5;
            uint64_t k = 0;
            while (j < n && s[j] >= '0' && s[j] <= '9') k = k * 10 + (uint64_t)(s[j++] - '0');
            if (j < n && s[j] == '>' && j > i + 5) {
                cc_guid g = cc_guid_from_u128(0, k);
                buf_append(out, g.c, 22);
                i = j + 1;
                continue;
            }
        }
        buf_putc(out, s[i++]);
    }
}

static char *slurp_unmasked(const char *name, size_t *n) {
    size_t rn;
    char *raw = slurp(golden, name, &rn);
    if (rn && raw[rn - 1] == '\n') rn--;
    Buf b;
    buf_init(&b);
    unmask(raw, rn, &b);
    free(raw);
    char *p = malloc(b.len + 1);
    memcpy(p, b.p, b.len);
    p[b.len] = 0;
    *n = b.len;
    buf_free(&b);
    return p;
}

static int load(cc_model *m, const char *p, size_t n, cc_error *err) { return cc_model_load(m, p, n, err); }

/* ---- golden model files ---- */

static void golden_file(const char *name) {
    size_t n;
    char *text = slurp_unmasked(name, &n);
    cc_model m;
    cc_error err;
    char what[256];
    int rc = load(&m, text, n, &err);
    if (rc) printf("  %s: %s\n", name, err.msg);
    snprintf(what, sizeof what, "%s loads", name);
    check_true(what, rc == 0);
    snprintf(what, sizeof what, "%s has views, sheets, schedules", name);
    check_true(what, m.nviews == 2 && m.nsheets == 3 && m.nschedules == 2);
    Buf b, v2, back;
    buf_init(&b), buf_init(&v2), buf_init(&back);
    cc_model_save(&m, &b);
    snprintf(what, sizeof what, "%s: load, save gives the same bytes", name);
    check_eq(what, b.p, b.len, text, n);
    /* v2: save, load, save as text */
    check_true("save v2", cc_model_save_v2(&m, &v2) == 0);
    cc_model m2;
    rc = load(&m2, v2.p, v2.len, &err);
    if (rc) printf("  v2 %s: %s\n", name, err.msg);
    cc_model_save(&m2, &back);
    snprintf(what, sizeof what, "%s: v2 round trip gives the same text", name);
    check_eq(what, back.p, back.len, text, n);
    /* The CBOR is the same twice (what the view writer counts matches what it writes: a wrong
     * map length would not load back). */
    Buf c1, c2;
    buf_init(&c1), buf_init(&c2);
    cc_model_to_cbor(&m, &c1);
    cc_model_to_cbor(&m2, &c2);
    snprintf(what, sizeof what, "%s: CBOR stable", name);
    check_true(what, c1.len == c2.len && !memcmp(c1.p, c2.p, c1.len));
    /* Broken binaries: every truncation of the CBOR body (wrapped again) fails cleanly. */
    int crashes = 0;
    for (size_t cut = 0; cut < c1.len; cut += 1 + cut / 50) {
        Buf w;
        buf_init(&w);
        buf_append(&w, CC_MAGIC, CC_MAGIC_LEN);
        cc_gzip((const unsigned char *)c1.p, cut, &w);
        cc_model t;
        if (load(&t, w.p, w.len, &err) == 0) crashes++;
        cc_model_free(&t);
        buf_free(&w);
    }
    snprintf(what, sizeof what, "%s: truncated CBOR refused", name);
    check_true(what, crashes == 0);
    /* Text truncations and byte flips: no crash, refused or loaded. */
    for (size_t cut = 0; cut < n; cut += 1 + cut / 40) {
        cc_model t;
        load(&t, text, cut, &err);
        cc_model_free(&t);
    }
    char *flip = malloc(n);
    for (size_t i = 0; i < n; i += 7) {
        memcpy(flip, text, n);
        flip[i] ^= (char)(1 + i % 7);
        cc_model t;
        load(&t, flip, n, &err);
        cc_model_free(&t);
    }
    free(flip);
    buf_free(&c1), buf_free(&c2);
    buf_free(&b), buf_free(&v2), buf_free(&back);
    cc_model_free(&m);
    cc_model_free(&m2);
    free(text);
}

/* ---- transcript replay ---- */

static const char *line_end(const char *p, const char *end) {
    const char *q = p;
    while (q < end && *q != '\n') q++;
    return q;
}

static void replay(const char *start_name, const char *transcript, const char *after) {
    size_t sn, tn, an;
    char *start = slurp(golden, start_name, &sn);
    if (sn && start[sn - 1] == '\n') sn--;
    char *t = slurp_unmasked(transcript, &tn);
    char *want = slurp_unmasked(after, &an);
    cc_model m;
    cc_error err;
    if (load(&m, start, sn, &err)) printf("  %s: %s\n", start_name, err.msg);
    cc_history h;
    cc_history_init(&h);
    const char *p = t, *end = t + tn;
    int records = 0, applied = 0, bad = 0;
    Buf e, lab, js;
    buf_init(&e), buf_init(&lab), buf_init(&js);
    char what[512];
    while (p < end) {
        const char *l1 = line_end(p, end);
        if (l1 - p < 4 || memcmp(p, ">>> ", 4)) {
            p = l1 + 1;
            continue;
        }
        records++;
        const char *head = p + 4;
        const char *status = l1 + 1, *l2 = line_end(status, end);
        const char *undo = l2 + 1, *l3 = line_end(undo, end);
        const char *redo = l3 + 1, *l4 = line_end(redo, end);
        const char *els = l4 + 1, *l5 = line_end(els, end);
        snprintf(what, sizeof what, "%s record %d: %.*s", transcript, records, (int)(l1 - head < 200 ? l1 - head : 200), head);
        if (!memcmp(head, "verb ", 5) && !memcmp(status, "200 ", 4)) {
            cc_cmds cmds;
            cc_cmds_init(&cmds);
            buf_clear(&e);
            if (cc_commands_parse(status + 4, (size_t)(l2 - status - 4), &cmds, &e)) {
                printf("  %s: parse: %.*s\n", what, (int)e.len, e.p);
                bad++;
            } else {
                /* written back: the transcript's bytes */
                buf_clear(&js);
                cc_commands_json(cmds.v, cmds.n, &js);
                check_eq(what, js.p, js.len, status + 4, (size_t)(l2 - status - 4));
                buf_clear(&e);
                if (cc_history_apply(&h, &m, cmds.v, cmds.n, &e)) {
                    printf("  %s: apply: %.*s\n", what, (int)e.len, e.p);
                    bad++;
                } else applied++;
            }
            cc_cmds_free(&cmds);
        } else if (!memcmp(head, "undo ", 5) || !memcmp(head, "redo ", 5)) {
            int r = head[0] == 'u' ? cc_history_undo(&h, &m) : cc_history_redo(&h, &m);
            check_eq(what, r ? "200 true" : "200 false", r ? 8 : 9, status, (size_t)(l2 - status));
        }
        buf_clear(&lab);
        buf_puts(&lab, "undo ");
        cc_history_labels(&h, 0, &lab, NULL);
        check_eq(what, lab.p, lab.len, undo, (size_t)(l3 - undo));
        buf_clear(&lab);
        buf_puts(&lab, "redo ");
        cc_history_labels(&h, 0, NULL, &lab);
        check_eq(what, lab.p, lab.len, redo, (size_t)(l4 - redo));
        buf_clear(&lab);
        buf_puts(&lab, "elements ");
        buf_u64(&lab, m.nelements);
        check_eq(what, lab.p, lab.len, els, (size_t)(l5 - els));
        p = l5 + 1;
    }
    printf("  %s: %d records, %d command lists applied\n", transcript, records, applied);
    snprintf(what, sizeof what, "%s: every command list applies", transcript);
    check_true(what, bad == 0 && applied > 20);
    Buf out;
    buf_init(&out);
    cc_model_save(&m, &out);
    snprintf(what, sizeof what, "%s: final model == %s", transcript, after);
    check_eq(what, out.p, out.len, want, an);
    /* The model validates as loaded would. */
    check_true("final model validates", cc_model_validate(&m, &err) == 0);
    /* Undo everything: back to the start file, byte for byte. */
    while (cc_history_undo(&h, &m)) {
    }
    buf_clear(&out);
    cc_model_save(&m, &out);
    snprintf(what, sizeof what, "%s: undo all gives %s", transcript, start_name);
    check_eq(what, out.p, out.len, start, sn);
    buf_free(&out);
    buf_free(&e), buf_free(&lab), buf_free(&js);
    cc_history_free(&h);
    cc_model_free(&m);
    free(start), free(t), free(want);
}

/* ---- hand cases ---- */

static cc_model frame;
static char *frame_text;
static size_t frame_n;
static char storey_lo[23], storey_hi[23]; /* frame's lowest and highest storey ids */

/* Applies a JSON command list; returns 0 or -1 with the error in err. */
static int apply(cc_history *h, cc_model *m, const char *json, Buf *err) {
    cc_cmds l;
    cc_cmds_init(&l);
    buf_clear(err);
    int rc = cc_commands_parse(json, strlen(json), &l, err);
    if (!rc) rc = cc_history_apply(h, m, l.v, l.n, err);
    cc_cmds_free(&l);
    return rc;
}

static void expect_apply(cc_history *h, cc_model *m, const char *json, const char *want) {
    Buf err;
    buf_init(&err);
    int rc = apply(h, m, json, &err);
    char what[300];
    snprintf(what, sizeof what, "%.280s", json);
    if (!want) {
        if (rc) printf("  %s: %.*s\n", what, (int)err.len, err.p);
        check_true(what, rc == 0);
    } else {
        buf_putc(&err, 0);
        if (!rc || strcmp(err.p, want)) printf("  %s\n    want %s\n    got  rc %d %s\n", what, want, rc, err.p);
        check_true(what, rc != 0 && !strcmp(err.p, want));
    }
    buf_free(&err);
}

static void expect_label(cc_history *h, const char *want) {
    Buf b;
    buf_init(&b);
    cc_history_labels(h, 1, &b, NULL);
    check_eq("label", b.p, b.len, want, strlen(want));
    buf_free(&b);
}

/* A 3D view id (derived), the low storey's plan down id. */
static void ids(cc_gid *v3d, cc_gid *plan) {
    cc_guid g = cc_guid_derived("bimble view 3d", 14);
    memcpy(v3d->c, g.c, 22);
    char seed[64];
    snprintf(seed, sizeof seed, "bimble view plan down %s", storey_hi);
    g = cc_guid_derived(seed, strlen(seed));
    memcpy(plan->c, g.c, 22);
}

static const char *RANGE =
    "\"range\":{\"top\":{\"level\":\"above\",\"offset_mm\":0.0},\"cut\":{\"level\":\"associated\",\"offset_mm\":%s},"
    "\"bottom\":{\"level\":\"associated\",\"offset_mm\":0.0},\"depth\":{\"level\":\"%s\",\"offset_mm\":%s}}";

static void hand(void) {
    cc_model m;
    cc_error err;
    load(&m, frame_text, frame_n, &err);
    cc_history h;
    cc_history_init(&h);
    cc_gid v3d, plan;
    ids(&v3d, &plan);
    char j[4096], range[512];
    Buf e;
    buf_init(&e);

    /* 3D view: crop refused, scale bounds, name, references in 3D */
    snprintf(j, sizeof j, "[{\"op\":\"add_view\",\"id\":\"%.22s\",\"name\":\"3D\",\"kind\":{\"type\":\"three_d\"},"
             "\"crop\":{\"min\":[0.0,0.0],\"max\":[1.0,1.0]}}]", v3d.c);
    char want[512];
    snprintf(want, sizeof want, "invalid: view 3D (%.22s): a 3D view has no crop region", v3d.c);
    expect_apply(&h, &m, j, want);
    snprintf(j, sizeof j, "[{\"op\":\"add_view\",\"id\":\"%.22s\",\"name\":\"3D\",\"kind\":{\"type\":\"three_d\"},\"scale\":0}]", v3d.c);
    snprintf(want, sizeof want, "invalid: view 3D (%.22s): scale 1:0: must be between 1:1 and 1:5000", v3d.c);
    expect_apply(&h, &m, j, want);
    snprintf(j, sizeof j, "[{\"op\":\"add_view\",\"id\":\"%.22s\",\"name\":\" \",\"kind\":{\"type\":\"three_d\"}}]", v3d.c);
    snprintf(want, sizeof want, "invalid: view   (%.22s): a view needs a name", v3d.c);
    expect_apply(&h, &m, j, want);
    snprintf(j, sizeof j, "[{\"op\":\"add_view\",\"id\":\"%.22s\",\"name\":\"3D\",\"kind\":{\"type\":\"three_d\",\"x\":1}}]", v3d.c);
    expect_apply(&h, &m, j, "unknown field `x`, there are no fields");
    snprintf(j, sizeof j, "[{\"op\":\"add_view\",\"id\":\"%.22s\",\"name\":\"3D\",\"kind\":{\"type\":\"four_d\"}}]", v3d.c);
    expect_apply(&h, &m, j, "unknown variant `four_d`, expected one of `three_d`, `plan`, `section`");
    snprintf(j, sizeof j, "[{\"op\":\"add_view\",\"id\":\"%.22s\",\"name\":\"3D\",\"kind\":{\"type\":\"three_d\"},\"zoom\":2}]", v3d.c);
    expect_apply(&h, &m, j, "unknown field `zoom`, expected one of `id`, `name`, `kind`, `visibility`, `references`, "
                            "`annotations`, `true_north`, `scale`, `crop`");
    snprintf(j, sizeof j, "[{\"op\":\"add_view\",\"id\":\"%.22s\",\"name\":\"3D\",\"kind\":{\"type\":\"three_d\"},\"true_north\":true,\"scale\":50}]", v3d.c);
    expect_apply(&h, &m, j, NULL);
    expect_label(&h, "[\"add the view 3D\"]");
    snprintf(want, sizeof want, "object already exists: %.22s", v3d.c);
    expect_apply(&h, &m, j, want);
    snprintf(j, sizeof j, "[{\"op\":\"set_view\",\"id\":\"%.22s\",\"name\":\"Axo\",\"kind\":{\"type\":\"three_d\"}}]", v3d.c);
    expect_apply(&h, &m, j, NULL);
    expect_label(&h, "[\"rename the view 3D to Axo\"]");
    snprintf(j, sizeof j, "[{\"op\":\"set_view\",\"id\":\"0000000000000000000001\",\"name\":\"X\",\"kind\":{\"type\":\"three_d\"}}]");
    expect_apply(&h, &m, j, "no such object: 0000000000000000000001");

    /* Plan: ranges */
    snprintf(range, sizeof range, RANGE, "-5000.0", "associated", "0.0");
    snprintf(j, sizeof j, "[{\"op\":\"add_view\",\"id\":\"%.22s\",\"name\":\"P\",\"kind\":{\"type\":\"plan\",\"storey\":\"%s\",\"look\":\"down\",%s}}]",
             plan.c, storey_hi, range);
    snprintf(want, sizeof want, "invalid: view P (%.22s): looking down, the planes must be in order: view depth ≤ bottom ≤ cut ≤ top", plan.c);
    expect_apply(&h, &m, j, want);
    snprintf(range, sizeof range, RANGE, "1200.0", "associated", "2e6");
    snprintf(j, sizeof j, "[{\"op\":\"add_view\",\"id\":\"%.22s\",\"name\":\"P\",\"kind\":{\"type\":\"plan\",\"storey\":\"%s\",\"look\":\"down\",%s}}]",
             plan.c, storey_hi, range);
    snprintf(want, sizeof want, "invalid: view P (%.22s): offsets must be numbers within ±1000 m", plan.c);
    expect_apply(&h, &m, j, want);
    snprintf(range, sizeof range, RANGE, "1200.0", "associated", "0.0");
    snprintf(j, sizeof j, "[{\"op\":\"add_view\",\"id\":\"%.22s\",\"name\":\"P\",\"kind\":{\"type\":\"plan\",\"storey\":\"nope\",\"look\":\"down\",%s}}]",
             plan.c, range);
    snprintf(want, sizeof want, "invalid: view P (%.22s): unknown storey nope", plan.c);
    expect_apply(&h, &m, j, want);
    /* looking up: depth below top is refused */
    snprintf(j, sizeof j, "[{\"op\":\"add_view\",\"id\":\"%.22s\",\"name\":\"P\",\"kind\":{\"type\":\"plan\",\"storey\":\"%s\",\"look\":\"up\",%s}}]",
             plan.c, storey_hi, range);
    snprintf(want, sizeof want, "invalid: view P (%.22s): looking up, the planes must be in order: bottom ≤ cut ≤ top ≤ view depth", plan.c);
    expect_apply(&h, &m, j, want);
    snprintf(range, sizeof range, RANGE, "1200.0", "below", "-0.0");
    snprintf(j, sizeof j, "[{\"op\":\"add_view\",\"id\":\"%.22s\",\"name\":\"Plan low\",\"kind\":{\"type\":\"plan\",\"storey\":\"%s\",\"look\":\"down\",%s},"
             "\"scale\":20,\"crop\":{\"min\":[-0.0,-1.5],\"max\":[100.0,200.0]},"
             "\"visibility\":{\"hidden_classes\":[\"IfcSlab\"],\"isolated_elements\":[\"x\",\"y\"]},"
             "\"references\":[{\"id\":\"0000000000000000000011\",\"name\":\"archi.dxf\",\"format\":\"dxf\",\"offset_mm\":[1.0,2.0],"
             "\"rotation_deg\":-0.0,\"polylines\":[[[0.0,0.0],[1.5,-0.0]],[]]}],"
             "\"annotations\":[{\"type\":\"dimension\",\"id\":\"0000000000000000000012\",\"a\":{\"at\":[0.0,0.0,0.0],\"element\":\"0000000000000000000013\"},"
             "\"b\":{\"at\":[1000.0,0.0,0.0]},\"offset_mm\":-0.0,\"axis\":\"vertical\"},"
             "{\"type\":\"text\",\"id\":\"0000000000000000000014\",\"text\":\"Note \\\"é\\\"\",\"at\":[1.0,2.0,3.0],\"size_mm\":3.5,\"angle_deg\":-0.0,\"kind\":\"\"}]}]",
             plan.c, storey_hi, range);
    expect_apply(&h, &m, j, NULL);
    expect_label(&h, "[\"add the view Plan low\"]");
    /* a storey in use by a plan view stays */
    snprintf(j, sizeof j, "[{\"op\":\"remove_storey\",\"id\":\"%s\"}]", storey_hi);
    snprintf(want, sizeof want, "%s is still used by elements", storey_hi);
    expect_apply(&h, &m, j, want);
    /* Section */
    expect_apply(&h, &m, "[{\"op\":\"add_view\",\"id\":\"0000000000000000000021\",\"name\":\"Section A\",\"kind\":{\"type\":\"section\",\"origin\":[0.0,1000.0],\"facing\":\"east\",\"depth_mm\":-1.0}}]",
                 "invalid: view Section A (0000000000000000000021): section origin and depth must be numbers, depth ≥ 0");
    expect_apply(&h, &m, "[{\"op\":\"add_view\",\"id\":\"0000000000000000000021\",\"name\":\"Section A\",\"kind\":{\"type\":\"section\",\"origin\":[0.0,1000.0],\"facing\":\"east\",\"depth_mm\":0.0}}]", NULL);
    expect_apply(&h, &m, "[{\"op\":\"add_view\",\"id\":\"0000000000000000000022\",\"name\":\"Bad text\",\"kind\":{\"type\":\"section\",\"origin\":[0.0,0.0],\"facing\":\"north\",\"depth_mm\":0.0},"
                 "\"annotations\":[{\"type\":\"text\",\"id\":\"0000000000000000000023\",\"text\":\"x\",\"at\":[0.0,0.0,0.0],\"size_mm\":60.0}]}]",
                 "invalid: view Bad text (0000000000000000000022): text size must be 0.5 to 50 mm");

    /* Sheets */
    expect_apply(&h, &m, "[{\"op\":\"add_sheet\",\"id\":\"0000000000000000000031\",\"number\":\"S-1\",\"name\":\"x\",\"paper\":\"A5\"}]",
                 "invalid: sheet S-1 x: paper \"A5\": one of A4, A3, A2, A1, A0");
    expect_apply(&h, &m, "[{\"op\":\"add_sheet\",\"id\":\"0000000000000000000031\",\"number\":\"S-1\",\"name\":\"x\",\"paper\":\"A1\","
                 "\"placed\":[{\"id\":\"0000000000000000000032\",\"view\":\"0000000000000000000099\",\"at_mm\":[10.0,10.0]}]}]",
                 "invalid: sheet S-1 x: no view 0000000000000000000099");
    expect_apply(&h, &m, "[{\"op\":\"add_sheet\",\"id\":\"0000000000000000000031\",\"number\":\"S-1\",\"name\":\"x\",\"paper\":\"A1\",\"portrait\":true,"
                 "\"placed\":[{\"id\":\"0000000000000000000032\",\"view\":\"0000000000000000000021\",\"at_mm\":[700.0,10.0]}]}]",
                 "invalid: sheet S-1 x: a view is placed off the paper");
    expect_apply(&h, &m, "[{\"op\":\"add_sheet\",\"id\":\"0000000000000000000031\",\"number\":\"S-1\",\"name\":\"x\",\"paper\":\"A1\","
                 "\"placed\":[{\"id\":\"0000000000000000000032\",\"view\":\"0000000000000000000021\",\"scale\":0,\"at_mm\":[700.0,10.0]}]}]",
                 "invalid: sheet S-1 x: scale 1:0: must be 1:1 to 1:5000");
    snprintf(j, sizeof j, "[{\"op\":\"add_sheet\",\"id\":\"0000000000000000000031\",\"number\":\"S-1\",\"name\":\"Plans\",\"paper\":\"A1\",\"portrait\":true,"
             "\"fields\":{\"revision\":\"B\",\"phase\":\"51\",\"revision\":\"C\"},"
             "\"placed\":[{\"id\":\"0000000000000000000032\",\"view\":\"0000000000000000000021\",\"at_mm\":[300.0,-0.0]},"
             "{\"id\":\"0000000000000000000033\",\"view\":\"%.22s\",\"scale\":50,\"at_mm\":[300.0,400.0]}],"
             "\"texts\":[{\"id\":\"0000000000000000000034\",\"text\":\"Titre\",\"at_mm\":[10.0,10.0],\"size_mm\":5.0,\"angle_deg\":90.0,\"kind\":\"title\"}]}]",
             v3d.c);
    expect_apply(&h, &m, j, NULL);
    expect_label(&h, "[\"add the sheet S-1\"]");
    expect_apply(&h, &m, "[{\"op\":\"add_sheet\",\"id\":\"0000000000000000000035\",\"number\":\"S-1\",\"name\":\"y\",\"paper\":\"A3\"}]",
                 "invalid: sheet number S-1 is already used");
    /* a placed stored view can't be removed; a default one (reset) can */
    expect_apply(&h, &m, "[{\"op\":\"remove_view\",\"id\":\"0000000000000000000021\"}]", "0000000000000000000021 is still used by elements");
    snprintf(j, sizeof j, "[{\"op\":\"remove_view\",\"id\":\"%.22s\"}]", v3d.c);
    expect_apply(&h, &m, j, NULL);
    expect_label(&h, "[\"delete the view Axo\"]");
    expect_apply(&h, &m, "[{\"op\":\"remove_view\",\"id\":\"0000000000000000000041\"}]", "no such object: 0000000000000000000041");
    expect_apply(&h, &m, "[{\"op\":\"set_sheet\",\"id\":\"0000000000000000000031\",\"number\":\"S-1\",\"name\":\"Plans 2\",\"paper\":\"A0\"}]", NULL);
    expect_label(&h, "[\"change the sheet S-1\"]");
    check_true("undo set_sheet", cc_history_undo(&h, &m) == 1);
    expect_apply(&h, &m, "[{\"op\":\"set_sheet\",\"id\":\"0000000000000000000039\",\"number\":\"S-9\",\"name\":\"x\",\"paper\":\"A0\"}]",
                 "no such object: 0000000000000000000039");

    /* Schedules */
    const char *col = "{\"name\":\"Concrete columns\",\"classes\":[\"IfcColumn\"],\"family\":\"concrete\",\"fields\":[\"level\",\"volume\"],\"group_by_level\":true,\"totals\":true}";
    snprintf(j, sizeof j, "[{\"op\":\"set_schedules\",\"schedules\":[%s]}]", col);
    expect_apply(&h, &m, j, NULL);
    expect_label(&h, "[\"add the schedule Concrete columns\"]");
    snprintf(j, sizeof j, "[{\"op\":\"set_schedules\",\"schedules\":[%s,{\"name\":\"Mine\",\"classes\":[],\"fields\":[\"mass\"]}]}]", col);
    expect_apply(&h, &m, j, NULL);
    expect_label(&h, "[\"add the schedule Mine\"]");
    snprintf(j, sizeof j, "[{\"op\":\"set_schedules\",\"schedules\":[%s,{\"name\":\"Yours\",\"classes\":[],\"fields\":[\"mass\"]}]}]", col);
    expect_apply(&h, &m, j, NULL);
    expect_label(&h, "[\"rename the schedule Mine to Yours\"]");
    snprintf(j, sizeof j, "[{\"op\":\"set_schedules\",\"schedules\":[%s,{\"name\":\"Yours\",\"classes\":[],\"fields\":[\"mass\",\"name\"]}]}]", col);
    expect_apply(&h, &m, j, NULL);
    expect_label(&h, "[\"change the schedule Yours\"]");
    expect_apply(&h, &m, j, NULL);
    expect_label(&h, "[\"change the schedules\"]");
    snprintf(j, sizeof j, "[{\"op\":\"set_schedules\",\"schedules\":[%s]}]", col);
    expect_apply(&h, &m, j, NULL);
    expect_label(&h, "[\"delete the schedule Yours\"]");
    snprintf(j, sizeof j, "[{\"op\":\"set_schedules\",\"schedules\":[%s,%s]}]", col, col);
    expect_apply(&h, &m, j, "invalid: two schedules are named Concrete columns");
    expect_apply(&h, &m, "[{\"op\":\"set_schedules\",\"schedules\":[{\"name\":\"X\",\"classes\":[],\"fields\":[]}]}]", "invalid: schedule X: 1 to 32 columns");
    expect_apply(&h, &m, "[{\"op\":\"set_schedules\",\"schedules\":[{\"name\":\" \",\"classes\":[],\"fields\":[\"name\"]}]}]",
                 "invalid: a schedule needs a name (at most 100 characters)");
    expect_apply(&h, &m, "[{\"op\":\"set_schedules\",\"schedules\":[{\"name\":\"X\",\"classes\":[],\"fields\":[\"bogus\"]}]}]",
                 "unknown variant `bogus`, expected one of `level`, `location`, `name`, `class`, `material`, `section`, `length`, "
                 "`width`, `height`, `area`, `volume`, `mass`, `global_id`");
    /* MAT1: set_material is a command of the C core now (verbs_materials_test.c): serde's text */
    expect_apply(&h, &m, "[{\"op\":\"set_material\",\"id\":\"0000000000000000000001\"}]", "missing field `name`");

    /* Saved, loaded, saved: same bytes; -0.0 written 0.0; omissions as Rust */
    Buf a, b, v2;
    buf_init(&a), buf_init(&b), buf_init(&v2);
    cc_model_save(&m, &a);
    buf_putc(&a, 0);
    a.len--;
    check_true("no -0.0 saved", !contains(a.p, "-0.0"));
    check_true("axis written", contains(a.p, "\"axis\": \"vertical\""));
    check_true("zero angle omitted, kind \"\" kept", contains(a.p, "\"size_mm\": 3.5,\n            \"kind\": \"\""));
    check_true("reference visible defaulted", contains(a.p, "\"visible\": true"));
    check_true("placed scale written", contains(a.p, "\"scale\": 100"));
    check_true("sheet fields sorted, last kept", contains(a.p, "\"phase\": \"51\",\n          \"revision\": \"C\""));
    check_true("schedule without family", contains(a.p, "\"name\": \"Concrete columns\""));
    cc_model m2;
    int rc = load(&m2, a.p, a.len, &err);
    if (rc) printf("  reload: %s\n", err.msg);
    cc_model_save(&m2, &b);
    check_eq("views, sheets: load, save, same bytes", b.p, b.len, a.p, a.len);
    cc_model_save_v2(&m2, &v2);
    cc_model m3;
    rc = load(&m3, v2.p, v2.len, &err);
    if (rc) printf("  v2 reload: %s\n", err.msg);
    buf_clear(&b);
    cc_model_save(&m3, &b);
    check_eq("views, sheets: v2 round trip", b.p, b.len, a.p, a.len);
    cc_model_free(&m3);
    /* load-time validation: a view key that is not its id, two sheet numbers */
    {
        Buf t;
        buf_init(&t);
        const char *k = strstr(a.p, "\"0000000000000000000021\": {");
        buf_append(&t, a.p, (size_t)(k - a.p));
        buf_puts(&t, "\"0000000000000000000029\"");
        buf_puts(&t, k + 24);
        rc = load(&m3, t.p, t.len, &err);
        check_true("view key != id refused", rc && contains(err.msg, "invalid model: view 0000000000000000000029: key does not match id"));
        if (!(rc && contains(err.msg, "key does not match"))) printf("  got %s\n", err.msg);
        cc_model_free(&m3);
        buf_free(&t);
    }
    {
        const char *s = "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"0000000000000000000009\","
                        "\"project_name\":\"d\",\"storeys\":{},\"materials\":{},\"elements\":{},\"sheets\":{"
                        "\"0000000000000000000001\":{\"id\":\"0000000000000000000001\",\"number\":\"A\",\"name\":\"x\",\"paper\":\"A4\"},"
                        "\"0000000000000000000002\":{\"id\":\"0000000000000000000002\",\"number\":\"A\",\"name\":\"y\",\"paper\":\"A4\"}}}}";
        rc = load(&m3, s, strlen(s), &err);
        check_true("two sheet numbers refused", rc && contains(err.msg, "invalid model: two sheets are numbered A"));
        cc_model_free(&m3);
        const char *s2 = "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"0000000000000000000009\","
                         "\"project_name\":\"d\",\"storeys\":{},\"materials\":{},\"elements\":{},\"schedules\":["
                         "{\"name\":\"Été\",\"classes\":[],\"fields\":[\"name\"]},{\"name\":\"éTÉ\",\"classes\":[],\"fields\":[\"name\"]}]}}";
        rc = load(&m3, s2, strlen(s2), &err);
        check_true("two schedule names (case) refused", rc && contains(err.msg, "invalid model: two schedules are named éTÉ"));
        if (!rc || !contains(err.msg, "two schedules")) printf("  got %s\n", err.msg);
        cc_model_free(&m3);
        const char *s3 = "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"0000000000000000000009\","
                         "\"project_name\":\"d\",\"storeys\":{},\"materials\":{},\"elements\":{},\"views\":{"
                         "\"0000000000000000000001\":{\"id\":\"0000000000000000000001\",\"name\":\"v\",\"kind\":{\"type\":\"plan\",\"storey\":\"s\",\"look\":\"down\"}}}}}";
        rc = load(&m3, s3, strlen(s3), &err);
        check_true("plan without range refused", rc && contains(err.msg, "missing field `range`"));
        cc_model_free(&m3);
    }
    /* Undo everything: back to frame's bytes */
    while (cc_history_undo(&h, &m)) {
    }
    buf_clear(&b);
    cc_model_save(&m, &b);
    check_eq("hand cases: undo all gives frame.json", b.p, b.len, frame_text, frame_n);
    buf_free(&a), buf_free(&b), buf_free(&v2), buf_free(&e);
    cc_model_free(&m2);
    cc_history_free(&h);
    cc_model_free(&m);
}

int main(int argc, char **argv) {
    golden = argc > 1 ? argv[1] : "golden/floats";
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", golden);
    golden = dir;
    frame_text = slurp(golden, "frame.json", &frame_n);
    if (frame_n && frame_text[frame_n - 1] == '\n') frame_n--;
    cc_error err;
    if (load(&frame, frame_text, frame_n, &err)) printf("frame: %s\n", err.msg);
    /* storeys by elevation */
    size_t lo = 0, hi = 0;
    for (size_t i = 0; i < frame.nstoreys; i++) {
        if (frame.storeys[i].elevation < frame.storeys[lo].elevation) lo = i;
        if (frame.storeys[i].elevation > frame.storeys[hi].elevation) hi = i;
    }
    memcpy(storey_lo, frame.storeys[lo].id.c, 22);
    memcpy(storey_hi, frame.storeys[hi].id.c, 22);
    golden_file("verbs_all_after.json");
    golden_file("frame2/verbs_all_after.json");
    replay("frame.json", "verbs_all.txt", "verbs_all_after.json");
    replay("frame2.json", "frame2/verbs_all.txt", "frame2/verbs_all_after.json");
    hand();
    cc_model_free(&frame);
    free(frame_text);
    return report("viewsheet");
}
