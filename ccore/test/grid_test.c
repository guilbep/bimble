/* Suite grid: levels and grids in the model (ticket G1, ADR 0056). No Rust oracle: the goldens
 * golden/grids/verbs.txt (a transcript of the six verbs and the two queries), model.json (the
 * model at its end), grid.ifc and grid.ifc2x3.ifc (its IFC export) are this suite's own
 * (CCORE_WRITE_GOLDEN=1 rewrites them). Covers: the existing goldens keep their bytes (no
 * "is_storey", no "grids"); v1 and v2 round trips with equal model hash; each command with its
 * inverse and its JSON; each rule; a storey's new elevation moves its elements in the scene; the
 * IFC export reads back (cc_ifc_read) with the same axes; untrusted files give errors. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/command.h"
#include "../src/file.h"
#include "../src/grid.h"
#include "../src/guid.h"
#include "../src/history.h"
#include "../src/ifc_out.h"
#include "../src/ifc_read.h"
#include "../src/json_r.h"
#include "../src/model.h"
#include "../src/query.h"
#include "../src/scene.h"
#include "../src/verb_table.h"
#include "../src/verbs.h"
#include "check.h"

static const char *golden;

/* frame.json ids */
#define N1 "1uAZA0Yfn05f$xRd1k4uPR"   /* storey Niveau 1, 0.0, 30 elements */
#define N2 "1jLno3kWj4Hfl6pUGBDd7E"   /* storey Niveau 2, 3000.0, 30 elements */
#define FOND "04Aobkc8v6$94cGl_9HNVX" /* storey Fondations, -800.0, 12 elements */
#define IFC_TIME "2026-01-01T00:00:00"

static void strip_nl(char *s, size_t *n) {
    while (*n && (s[*n - 1] == '\n' || s[*n - 1] == '\r')) s[--*n] = 0;
}

static void model_text(const cc_model *m, Buf *out) {
    buf_clear(out);
    if (cc_model_save(m, out)) buf_clear(out);
}

static void hash_hex(const Buf *t, char hex[65]) {
    unsigned char h[32];
    cc_blake3(t->p, t->len, h);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
}

static const cc_gid *gid(const char *s) {
    static cc_gid g;
    memcpy(g.c, s, 22);
    return &g;
}

/* The level's not_storey, or -1 (with a line) when the level is missing: a readable failure. */
static int level_flag(const cc_model *m, const char *id) {
    const cc_storey *s = cc_model_storey(m, gid(id));
    if (!s) printf("  no level %s\n", id);
    return s ? s->not_storey : -1;
}

static char *frame(const char *name, size_t *n) {
    char *s = slurp(golden, name, n);
    strip_nl(s, n);
    return s;
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

static int apply(cc_history *h, cc_model *m, const char *json, Buf *err) {
    cc_cmds l;
    cc_cmds_init(&l);
    buf_clear(err);
    int rc = cc_commands_parse(json, strlen(json), &l, err);
    if (!rc) rc = cc_history_apply(h, m, l.v, l.n, err);
    cc_cmds_free(&l);
    return rc;
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

/* ---- the transcript: verbs and queries on a copy of frame.json ---- */

static const char *const STEPS[] = {
    /* levels */
    "level_add {\"name\":\"Top of parapet\",\"level_mm\":9100,\"storey\":false}",
    "undo",
    "redo",
    "level_add {\"name\":\"  Bottom of foundation \",\"level_mm\":-1300.5,\"storey\":false}",
    "level_add {\"name\":\"Niveau 3\",\"level_mm\":6000}",
    "query storeys",
    "level_set {\"id\":\"" N2 "\",\"level_mm\":3200}",
    "undo",
    "redo",
    "level_set {\"id\":\"<new-1>\",\"name\":\"Acrotère\"}",
    "level_set {\"id\":\"<new-1>\",\"storey\":true}",
    "undo",
    "level_set {\"id\":\"<new-1>\",\"name\":\"Acrotère\",\"level_mm\":9100,\"storey\":false}",
    "level_set {\"id\":\"" N1 "\",\"storey\":false}",
    "level_set {\"id\":\"" N1 "\",\"level_mm\":2e7}",
    "level_set {\"id\":\"00000000000000000000ZZ\",\"name\":\"x\"}",
    "level_set {\"id\":\"" N1 "\",\"elevation\":1}",
    "level_add {\"name\":\" \",\"level_mm\":0}",
    "level_add {\"name\":\"No level\"}",
    "level_remove {\"id\":\"" N1 "\"}",
    "level_remove {\"id\":\"<new-2>\"}",
    "undo",
    "redo",
    /* grids */
    "grid_add {\"name\":\"Main grid\",\"x\":[0,6000,12000,18000],\"y\":[0,5000,10000]}",
    "undo",
    "redo",
    "grid_add {\"name\":\"Annex\",\"axes\":[{\"tag\":\"A'\",\"start\":[20000,-500],\"end\":[26000,-500]},"
    "{\"tag\":\"1'\",\"start\":[20500,-1000],\"end\":[20500,4000]},{\"tag\":\"Diag\",\"start\":[20000,0],\"end\":[26000,4000]}]}",
    "grid_add {\"name\":\"Side\",\"x\":[0,6000],\"y\":[0],\"tags\":[\"S1\",\"S2\",\"SA\"]}",
    "grid_add {\"name\":\"Only x\",\"x\":[0,6000]}",
    "grid_add {\"name\":\"Both\",\"x\":[0],\"y\":[0],\"axes\":[]}",
    "grid_add {\"name\":\"One way\",\"axes\":[{\"tag\":\"1\",\"start\":[0,0],\"end\":[0,9000]},{\"tag\":\"2\",\"start\":[6000,0],\"end\":[6000,9000]}]}",
    "grid_add {\"name\":\"Twice\",\"x\":[0,6000],\"y\":[0],\"tags\":[\"1\",\"1\",\"A\"]}",
    "grid_add {\"name\":\"Far\",\"x\":[0,2e7],\"y\":[0]}",
    "grid_add {\"name\":\"Tags\",\"x\":[0,6000],\"y\":[0],\"tags\":[\"1\"]}",
    "grid_add {\"name\":\"Odd\",\"x\":[0],\"y\":[0],\"spacing\":3}",
    "grid_axis_set {\"grid\":\"<new-4>\",\"axis\":\"<new-6>\",\"tag\":\"2a\",\"start\":[6000,-1500]}",
    "undo",
    "redo",
    "grid_axis_set {\"grid\":\"<new-4>\",\"axis\":\"<new-6>\",\"tag\":\"2a\"}",
    "grid_axis_set {\"grid\":\"<new-4>\",\"axis\":\"<new-6>\",\"end\":[6000,-1500]}",
    "grid_axis_set {\"grid\":\"<new-4>\",\"axis\":\"<new-6>\",\"tag\":\"3\"}",
    "grid_axis_set {\"grid\":\"<new-4>\",\"axis\":\"<new-13>\",\"tag\":\"Z\"}",
    "grid_remove {\"id\":\"<new-12>\"}",
    "undo",
    "grid_remove {\"id\":\"" N1 "\"}",
    "query grids",
    "query storeys",
};

static void transcript(void) {
    size_t fn;
    char *base = frame("frame.json", &fn);
    cc_model m;
    cc_error e;
    cc_history h;
    Buf t, err, args, line, out, model, ifc4, ifc2;
    buf_init(&t), buf_init(&err), buf_init(&args), buf_init(&line), buf_init(&out), buf_init(&model);
    buf_init(&ifc4), buf_init(&ifc2);
    cc_history_init(&h);
    check_true("frame loads", cc_model_load(&m, base, fn, &e) == 0);
    cc_guid_seed(1);
    int applied = 0, refused = 0, none = 0;
    for (size_t i = 0; i < sizeof STEPS / sizeof STEPS[0]; i++) {
        const char *s = STEPS[i];
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
                /* The journal form: the commands written, parsed back, written again: same text. */
                cc_cmds back;
                cc_cmds_init(&back);
                Buf t2;
                buf_init(&t2);
                int prc = cc_commands_parse(t.p, t.len, &back, &err);
                if (!prc) cc_commands_json(back.v, back.n, &t2);
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
    check_true("three levels added, one removed", m.nstoreys == 5);
    check_true("three grids", m.ngrids == 3);
    model_text(&m, &model);
    check_true("export IFC4", cc_ifc_export(&m, "grid.ifc", IFC_TIME, CC_IFC4, &ifc4) == 0);
    check_true("export IFC2x3", cc_ifc_export(&m, "grid.ifc2x3.ifc", IFC_TIME, CC_IFC2X3, &ifc2) == 0);
    if (getenv("CCORE_WRITE_GOLDEN")) {
        write_file("grids/verbs.txt", out.p, out.len, 0);
        write_file("grids/model.json", model.p, model.len, 1);
        write_file("grids/grid.ifc", ifc4.p, ifc4.len, 0);
        write_file("grids/grid.ifc2x3.ifc", ifc2.p, ifc2.len, 0);
    }
    size_t n1, n2, n3, n4;
    char *g1 = slurp(golden, "grids/verbs.txt", &n1), *g2 = slurp(golden, "grids/model.json", &n2);
    char *g3 = slurp(golden, "grids/grid.ifc", &n3), *g4 = slurp(golden, "grids/grid.ifc2x3.ifc", &n4);
    strip_nl(g2, &n2);
    check_eq("transcript == grids/verbs.txt", out.p, out.len, g1, n1);
    check_eq("model == grids/model.json", model.p, model.len, g2, n2);
    check_eq("IFC4 == grids/grid.ifc", ifc4.p, ifc4.len, g3, n3);
    check_eq("IFC2x3 == grids/grid.ifc2x3.ifc", ifc2.p, ifc2.len, g4, n4);

    /* The IFC: grids, no datum level as a storey, read back with the same axes. */
    const char *ifc = buf_cstr(&ifc4);
    check_true("IFC4 has IfcGrid", strstr(ifc, "=IFCGRID('") != NULL);
    check_true("IFC4 has IfcGridAxis", strstr(ifc, "=IFCGRIDAXIS('1',") != NULL);
    check_true("IFC4: the datum levels are not storeys", !strstr(ifc, "Acrot") && !strstr(ifc, "Bottom of"));
    check_true("IFC4: Niveau 3 is a storey", strstr(ifc, "'Niveau 3'") != NULL);
    {
        /* IFC2x3: the owner history (2nd argument) and 10 arguments, no PredefinedType */
        const char *g = strstr(buf_cstr(&ifc2), "=IFCGRID('");
        int args = 1, depth = 0, quoted = 0, oh = 0;
        for (const char *p = g ? g + 9 : NULL; p && *p && !(depth == 0 && *p == ')'); p++) {
            if (*p == '\'') quoted = !quoted;
            else if (!quoted && *p == '(') depth++;
            else if (!quoted && *p == ')') depth--;
            else if (!quoted && depth == 0 && *p == ',') {
                args++;
                if (args == 2) oh = p[1] == '#';
            }
        }
        check_true("IFC2x3: IfcGrid with an owner history, 10 arguments", g && oh && args == 10);
    }
    cc_ifc_scene sc;
    int rrc = cc_ifc_read(ifc4.p, ifc4.len, &sc, &e);
    if (rrc) printf("  read: %s\n", e.msg);
    size_t total = 0;
    for (size_t i = 0; i < m.ngrids; i++) total += m.grids[i].naxes;
    check_true("IFC4 reads back", rrc == 0);
    check_true("all axes read back", rrc == 0 && sc.ngrids == total);
    int same = rrc == 0 && sc.ngrids == total;
    for (size_t i = 0, k = 0; same && i < m.ngrids; i++) {
        const cc_grid *g = &m.grids[i];
        for (int u = 1; u >= 0; u--) /* the reader goes U then V */
            for (size_t j = 0; same && j < g->naxes; j++) {
                if (cc_grid_axis_is_u(g, j) != u) continue;
                const cc_grid_axis *a = &g->axes[j];
                const cc_ifc_grid *r = &sc.grids[k++];
                same = r->tag.n == a->tag.n && !memcmp(r->tag.p, a->tag.p, a->tag.n) && r->start_mm[0] == a->start[0] &&
                       r->start_mm[1] == a->start[1] && r->end_mm[0] == a->end[0] && r->end_mm[1] == a->end[1];
                if (!same) printf("  axis %.*s read as %.*s\n", (int)a->tag.n, a->tag.p, (int)r->tag.n, r->tag.p);
            }
    }
    check_true("read axes: same tags and ends", same);
    cc_ifc_scene_free(&sc);

    /* Undo everything: frame's bytes, no grids key, same IFC as frame's. */
    while (cc_history_undo(&h, &m)) {
    }
    model_text(&m, &t);
    check_eq("undo all: frame's bytes", t.p, t.len, base, fn);
    buf_clear(&ifc2);
    cc_model fm;
    check_true("frame loads again", cc_model_load(&fm, base, fn, &e) == 0);
    buf_clear(&ifc4);
    check_true("undo all: the IFC of frame", cc_ifc_export(&m, "f.ifc", IFC_TIME, CC_IFC4, &ifc4) == 0 &&
                                                 cc_ifc_export(&fm, "f.ifc", IFC_TIME, CC_IFC4, &ifc2) == 0 &&
                                                 ifc4.len == ifc2.len && !memcmp(ifc4.p, ifc2.p, ifc4.len));
    cc_model_free(&fm);
    cc_model_free(&m);
    cc_history_free(&h);
    buf_free(&t), buf_free(&err), buf_free(&args), buf_free(&line), buf_free(&out), buf_free(&model);
    buf_free(&ifc4), buf_free(&ifc2);
    free(base), free(g1), free(g2), free(g3), free(g4);
}

/* ---- the golden model: v1 and v2 round trips ---- */

static void round_trips(void) {
    size_t gn;
    char *gj = frame("grids/model.json", &gn);
    cc_model a, b;
    cc_error e;
    Buf t, bin, b2;
    buf_init(&t), buf_init(&bin), buf_init(&b2);
    int rc = cc_model_load(&a, gj, gn, &e);
    if (rc) printf("  load grids/model.json: %s\n", e.msg);
    check_true("grids/model.json loads", rc == 0);
    model_text(&a, &t);
    check_eq("grids/model.json load+save", t.p, t.len, gj, gn);
    check_true("v1 has the grids key and a datum level",
               strstr(gj, "\n    \"grids\": {") && strstr(gj, "\"is_storey\": false") && !strstr(gj, "\"is_storey\": true"));
    char h1[65], h2[65];
    hash_hex(&t, h1);
    check_true("save v2", cc_model_save_v2(&a, &bin) == 0);
    rc = cc_model_load(&b, bin.p, bin.len, &e);
    if (rc) printf("  load v2: %s\n", e.msg);
    check_true("v2 loads", rc == 0);
    model_text(&b, &t);
    check_eq("v2 load, save v1 == grids/model.json", t.p, t.len, gj, gn);
    hash_hex(&t, h2);
    check_eq("v2 round trip: same model hash", h2, 64, h1, 64);
    check_true("v2 save again: same bytes", cc_model_save_v2(&b, &b2) == 0 && b2.len == bin.len && !memcmp(b2.p, bin.p, bin.len));
    check_true("grid fields equal after v2", b.ngrids == 3 && a.ngrids == 3 && cc_grid_eq(&a.grids[0], &b.grids[0]) &&
                                                  cc_grid_eq(&a.grids[1], &b.grids[1]));
    /* Truncations of v2: errors, never a crash (ASan in the gate). */
    int loaded = 0;
    for (size_t k = 0; k < bin.len; k += 7) {
        cc_model x;
        if (cc_model_load(&x, bin.p, k, &e) == 0) loaded++;
        cc_model_free(&x);
    }
    check_true("v2 truncations: none loads", loaded == 0);
    cc_model_free(&a), cc_model_free(&b);
    buf_free(&t), buf_free(&bin), buf_free(&b2);
    free(gj);
}

/* Existing files keep their bytes: neither "is_storey" nor "grids" appears. */
static void old_files(void) {
    static const char *const names[] = {"frame", "frame2"};
    for (int k = 0; k < 2; k++) {
        char path[256], what[256];
        size_t jn, bn, cn;
        snprintf(path, sizeof path, "%s.json", names[k]);
        char *json = frame(path, &jn);
        snprintf(path, sizeof path, "bin/%s.bimble", names[k]);
        char *bin = slurp(golden, path, &bn);
        snprintf(path, sizeof path, "bin/%s.cbor", names[k]);
        char *cbor = slurp(golden, path, &cn);
        cc_model m;
        cc_error e;
        Buf t;
        buf_init(&t);
        check_true("load v1", cc_model_load(&m, json, jn, &e) == 0);
        model_text(&m, &t);
        snprintf(what, sizeof what, "golden/%s.json load+save: same bytes", names[k]);
        check_eq(what, t.p, t.len, json, jn);
        check_true("no is_storey, no grids in v1", !strstr(buf_cstr(&t), "is_storey") && !strstr(buf_cstr(&t), "\"grids\"") &&
                                                       m.ngrids == 0);
        buf_clear(&t);
        check_true("storeys query: Rust's keys only", cc_query(&m, "storeys", "", 0, &t) == 200 &&
                                                          !strstr(buf_cstr(&t), "is_storey") && !strstr(buf_cstr(&t), "height_mm"));
        buf_clear(&t);
        check_true("grids query: []", cc_query(&m, "grids", "", 0, &t) == 200 && !strcmp(buf_cstr(&t), "[]"));
        cc_model_free(&m);
        snprintf(what, sizeof what, "golden/bin/%s.bimble loads", names[k]);
        check_true(what, cc_model_load(&m, bin, bn, &e) == 0);
        buf_clear(&t);
        snprintf(what, sizeof what, "golden/bin/%s.bimble: CBOR saved == Rust's CBOR", names[k]);
        check_true(what, cc_model_to_cbor(&m, &t) == 0 && t.len == cn && !memcmp(t.p, cbor, cn));
        cc_model_free(&m);
        buf_free(&t);
        free(json), free(bin), free(cbor);
    }
    Buf t;
    buf_init(&t);
    cc_model empty;
    cc_model_init(&empty);
    check_true("unknown query text stays Rust's",
               cc_query(&empty, "nope", "", 0, &t) == 400 &&
                   !strcmp(buf_cstr(&t), "unknown query \"nope\"; known: storeys, materials, elements, element, project, "
                                          "views, sheets, schedules"));
    cc_model_free(&empty);
    buf_free(&t);
}

/* ---- the scene: a storey's elevation moves its elements ---- */

static double min_z(const cc_scene *s, const cc_gid *id) {
    for (size_t k = 0; k < s->nelements; k++) {
        if (cc_gid_cmp(&s->elements[k].id, id)) continue;
        Arena a;
        arena_init(&a, 0);
        cc_mesh w;
        double z = 1e300;
        if (cc_scene_world_mesh(s, k, &a, &w) == 0)
            for (uint32_t v = 0; v < w.nv; v++) z = w.pos[v][2] < z ? w.pos[v][2] : z;
        arena_free(&a);
        return z + s->origin[2];
    }
    return 1e300;
}

static void scene_move(void) {
    size_t fn;
    char *base = frame("frame.json", &fn);
    cc_model m;
    cc_error e;
    cc_history h;
    Buf err;
    buf_init(&err);
    cc_history_init(&h);
    check_true("frame loads", cc_model_load(&m, base, fn, &e) == 0);
    const cc_gid *on2 = NULL, *on1 = NULL;
    for (size_t i = 0; i < m.nelements; i++) {
        if (!on2 && !cc_gid_cmp(&m.elements[i].storey, gid(N2))) on2 = &m.elements[i].id;
        if (!on1 && !cc_gid_cmp(&m.elements[i].storey, gid(N1))) on1 = &m.elements[i].id;
    }
    check_true("frame has elements on Niveau 1 and Niveau 2", on1 && on2);
    if (!on1 || !on2) {
        cc_model_free(&m);
        cc_history_free(&h);
        buf_free(&err);
        free(base);
        return;
    }
    cc_gid e2 = *on2, e1 = *on1;
    double z2 = cc_model_element(&m, &e2)->placement.origin[2];
    cc_scene s0, s1;
    memset(&s0, 0, sizeof s0), memset(&s1, 0, sizeof s1);
    check_true("scene before", cc_scene_build(&s0, &m, &e) == 0);
    cc_cmds l;
    cc_cmds_init(&l);
    const char *args = "{\"id\":\"" N2 "\",\"level_mm\":3250}";
    int rc = cc_verb_run(&m, "level_set", args, strlen(args), &l, &err) || cc_history_apply(&h, &m, l.v, l.n, &err);
    cc_cmds_free(&l);
    check_true("level_set Niveau 2 to 3250", rc == 0);
    check_true("placement unchanged (relative to the storey)",
               cc_model_element(&m, &e2) && cc_model_element(&m, &e2)->placement.origin[2] == z2);
    check_true("scene after", cc_scene_build(&s1, &m, &e) == 0);
    double d2 = min_z(&s1, &e2) - min_z(&s0, &e2), d1 = min_z(&s1, &e1) - min_z(&s0, &e1);
    printf("  Niveau 2 +250 mm: an element on it moved %.3f mm, one on Niveau 1 %.3f mm\n", d2, d1);
    check_true("the element on Niveau 2 moved up 250 mm", d2 > 249.99 && d2 < 250.01);
    check_true("the element on Niveau 1 stayed", d1 > -0.01 && d1 < 0.01);
    cc_scene_free(&s1);
    memset(&s1, 0, sizeof s1);
    check_true("undo", cc_history_undo(&h, &m) == 1);
    check_true("scene after undo", cc_scene_build(&s1, &m, &e) == 0);
    double back = min_z(&s1, &e2) - min_z(&s0, &e2);
    check_true("undo: back where it was", back > -0.01 && back < 0.01);
    cc_scene_free(&s0), cc_scene_free(&s1);
    cc_model_free(&m);
    cc_history_free(&h);
    buf_free(&err);
    free(base);
}

/* ---- rules on commands ---- */

#define G1 "1GRID00000000000000001"
#define A1 "1AXIS00000000000000001"
#define A2 "1AXIS00000000000000002"
#define DATUM "1LEVEL0000000000000001"
#define GRID_CMD(op, id, a1, tag1, a2)                                                                               \
    "[{\"op\":\"" op "\",\"id\":\"" id "\",\"name\":\"G\",\"axes\":[{\"id\":\"" a1 "\",\"tag\":\"" tag1 "\","       \
    "\"start_mm\":[0,0],\"end_mm\":[0,9000]},{\"id\":\"" a2 "\",\"tag\":\"A\",\"start_mm\":[-1000,0],\"end_mm\":[7000,0]}]}]"

static void rules(void) {
    size_t fn;
    char *base = frame("frame.json", &fn);
    cc_model m;
    cc_error e;
    cc_history h;
    Buf err, t0, t;
    buf_init(&err), buf_init(&t0), buf_init(&t);
    cc_history_init(&h);
    check_true("frame loads", cc_model_load(&m, base, fn, &e) == 0);
    model_text(&m, &t0);
    struct {
        const char *json, *want;
    } bad[] = {
        {"[{\"op\":\"set_storey\",\"id\":\"" DATUM "\",\"name\":\"x\",\"elevation\":0}]", "no such object: " DATUM},
        {"[{\"op\":\"set_storey\",\"id\":\"" N1 "\",\"name\":\"Niveau 1\",\"elevation\":0,\"is_storey\":false}]",
         "invalid: the level Niveau 1 holds elements: it stays a storey"},
        {GRID_CMD("add_grid", N1, A1, "1", A2), "invalid: the grid or axis id " N1 " is already used by another object"},
        {GRID_CMD("add_grid", G1, A1, "1", A1), "invalid: the id " A1 " is used twice in the grids"},
        {GRID_CMD("add_grid", G1, A1, "A", A2), "the tag is used twice"},
        {GRID_CMD("add_grid", G1, A1, "", A2), "a tag of 1 to 32 characters, one line"},
        {GRID_CMD("set_grid", G1, A1, "1", A2), "no such object: " G1},
        {"[{\"op\":\"remove_grid\",\"id\":\"" G1 "\"}]", "no such object: " G1},
        {"[{\"op\":\"add_grid\",\"id\":\"" G1 "\",\"name\":\"G\",\"axes\":[]}]", "1 to 1000 axes, got 0"},
        {"[{\"op\":\"add_grid\",\"id\":\"" G1 "\",\"name\":\"\",\"axes\":[]}]", "a name of 1 to 200 characters"},
        {"[{\"op\":\"add_grid\",\"id\":\"" G1 "\",\"name\":\"G\"}]", "missing field `axes`"},
        {"[{\"op\":\"add_storey\",\"id\":\"" N1 "\",\"name\":\"x\",\"elevation\":0,\"is_storey\":1}]",
         "invalid type: integer `1`, expected a boolean"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = apply(&h, &m, bad[i].json, &err);
        char what[256];
        snprintf(what, sizeof what, "refused %zu: %s", i, bad[i].want);
        int ok = rc != 0 && strstr(buf_cstr(&err), bad[i].want) != NULL;
        if (!ok) printf("  case %zu: rc %d, got \"%s\"\n", i, rc, buf_cstr(&err));
        check_true(what, ok);
    }
    model_text(&m, &t);
    check_eq("refused commands: model unchanged", t.p, t.len, t0.p, t0.len);

    /* A grid, then ids it holds are taken for every other object. */
    int arc = apply(&h, &m, GRID_CMD("add_grid", G1, A1, "1", A2), &err);
    if (arc) printf("  add_grid: %s\n", buf_cstr(&err));
    check_true("add_grid", arc == 0);
    struct {
        const char *json, *want;
    } taken[] = {
        {"[{\"op\":\"add_storey\",\"id\":\"" A1 "\",\"name\":\"x\",\"elevation\":0}]", "object already exists: " A1},
        {"[{\"op\":\"add_material\",\"id\":\"" G1 "\",\"name\":\"x\",\"category\":\"concrete\"}]", "object already exists: " G1},
        {GRID_CMD("add_grid", "1GRID00000000000000002", A1, "1", "1AXIS00000000000000003"), "used twice in the grids"},
        {GRID_CMD("add_grid", G1, A1, "1", A2), "object already exists: " G1},
    };
    for (size_t i = 0; i < sizeof taken / sizeof taken[0]; i++) {
        int rc = apply(&h, &m, taken[i].json, &err);
        char what[256];
        snprintf(what, sizeof what, "id taken %zu: %s", i, taken[i].want);
        int ok = rc != 0 && strstr(buf_cstr(&err), taken[i].want) != NULL;
        if (!ok) printf("  case %zu: rc %d, got \"%s\"\n", i, rc, buf_cstr(&err));
        check_true(what, ok);
    }
    /* set_grid and its inverse; remove_grid and its inverse */
    model_text(&m, &t0);
    check_true("set_grid", apply(&h, &m, GRID_CMD("set_grid", G1, A1, "1b", A2), &err) == 0);
    check_true("set_grid changed the tag", cc_model_grid(&m, gid(G1)) && cc_model_grid(&m, gid(G1))->axes[0].tag.n == 2);
    check_true("undo set_grid", cc_history_undo(&h, &m) == 1);
    model_text(&m, &t);
    check_eq("undo set_grid: the same model", t.p, t.len, t0.p, t0.len);
    check_true("remove_grid", apply(&h, &m, "[{\"op\":\"remove_grid\",\"id\":\"" G1 "\"}]", &err) == 0 && m.ngrids == 0);
    check_true("undo remove_grid", cc_history_undo(&h, &m) == 1);
    model_text(&m, &t);
    check_eq("undo remove_grid: the same model", t.p, t.len, t0.p, t0.len);

    /* A datum level: no element on it, and its IFC is not a storey. */
    check_true("add a datum level",
               apply(&h, &m, "[{\"op\":\"add_storey\",\"id\":\"" DATUM "\",\"name\":\"Datum\",\"elevation\":-0.0,\"is_storey\":false}]",
                     &err) == 0);
    check_true("datum level: elevation canonical, flag kept",
               level_flag(&m, DATUM) == 1 && !strstr(buf_cstr((model_text(&m, &t), &t)), "-0.0"));
    const cc_element *el = &m.elements[0];
    cc_cmds l;
    cc_cmds_init(&l);
    cc_command *c = cc_cmds_push(&l, CC_CMD_ADD_ELEMENT);
    c->element = *el;
    memcpy(c->element.id.c, "1ELEMENT00000000000001", 22);
    c->element.storey = *gid(DATUM);
    int rc = cc_history_apply(&h, &m, l.v, l.n, &err);
    check_true("add_element on a datum level: refused",
               rc != 0 && strstr(buf_cstr(&err), "invalid: the level Datum is not a storey: no element belongs to it"));
    cc_cmds_free(&l);
    check_true("set_storey: the datum becomes a storey",
               apply(&h, &m, "[{\"op\":\"set_storey\",\"id\":\"" DATUM "\",\"name\":\"Datum\",\"elevation\":5}]", &err) == 0 &&
                   level_flag(&m, DATUM) == 0);
    Buf lab;
    buf_init(&lab);
    cc_history_labels(&h, 1, &lab, NULL);
    check_eq("set_storey label", lab.p, lab.len, "[\"change the level Datum\"]", 26);
    check_true("undo", cc_history_undo(&h, &m) == 1 && level_flag(&m, DATUM) == 1);
    buf_free(&lab);

    /* A relation cannot take a grid's id. */
    char rel[512];
    snprintf(rel, sizeof rel,
             "[{\"op\":\"add_relation\",\"id\":\"%s\",\"kind\":\"IfcRelConnectsElements\",\"relating\":\"%.22s\","
             "\"related\":[\"%.22s\"]}]",
             A2, m.elements[0].id.c, m.elements[1].id.c);
    rc = apply(&h, &m, rel, &err);
    check_true("relation with an axis id: refused", rc != 0 && strstr(buf_cstr(&err), "already used by another object"));

    /* The verbs' schemas are JSON, and describe finds them (the session answers C additions). */
    static const char *const VERBS[] = {"level_add", "level_set", "level_remove", "grid_add", "grid_axis_set", "grid_remove"};
    for (int i = 0; i < 6; i++) {
        const cc_verb_def *d = cc_verb_find(VERBS[i]);
        Arena a;
        arena_init(&a, 0);
        JrError je;
        char what[96];
        snprintf(what, sizeof what, "verb %s: in the table, schema is JSON", VERBS[i]);
        check_true(what, d && !d->read_only && jr_parse(&a, d->schema, strlen(d->schema), &je) != NULL);
        arena_free(&a);
    }
    /* dry_run answers the commands without applying (the caller applies), if_rev conflicts. */
    cc_cmds_init(&l);
    const char *ga = "{\"name\":\"G2\",\"x\":[0,1000],\"y\":[0]}";
    rc = cc_verb_run_ex(&m, "grid_add", ga, strlen(ga), 1, 3, 4, &l, &err);
    check_true("if_rev: conflict", rc == CC_VERB_CONFLICT);
    cc_cmds_free(&l);

    cc_model_free(&m);
    cc_history_free(&h);
    buf_free(&err), buf_free(&t0), buf_free(&t);
    free(base);
}

/* ---- untrusted files ---- */

static void file_errors(void) {
    size_t gn;
    char *gj = frame("grids/model.json", &gn);
    struct {
        const char *from, *to, *want;
    } cases[] = {
        {"\"grids\": {", "\"grids\": [", "parse: "},
        {"\"is_storey\": false", "\"is_storey\": \"no\"", "invalid type: string \"no\", expected a boolean"},
        {"\"start_mm\": [", "\"start_mm\": [1, ", "invalid length, expected an array of length 2"},
        {"\"tag\": \"1\"", "\"tag\": 1", "invalid type: integer, expected a string"},
        {"\"tag\": \"2a\"", "\"tag\": \"1\"", "the tag is used twice"},
        {"\"axes\": [", "\"axis\": [", "missing field `axes`"},
        {"\"name\": \"Main grid\"", "\"name\": \"\"", "a name of 1 to 200 characters"},
        {"\"is_storey\": false", "\"is_storey\": true", NULL}, /* accepted: written back without it */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *at = strstr(gj, cases[i].from);
        char what[256];
        snprintf(what, sizeof what, "file case %zu: %s", i, cases[i].want ? cases[i].want : "is_storey true loads");
        if (!at) {
            check_true(what, 0);
            continue;
        }
        Buf t;
        buf_init(&t);
        buf_append(&t, gj, (size_t)(at - gj));
        buf_puts(&t, cases[i].to);
        buf_puts(&t, at + strlen(cases[i].from));
        cc_model m;
        cc_error e;
        int rc = cc_model_load(&m, t.p, t.len, &e);
        int ok = cases[i].want ? rc != 0 && strstr(e.msg, cases[i].want) != NULL : rc == 0;
        if (!ok) printf("  case %zu: rc %d, got \"%s\"\n", i, rc, e.msg);
        check_true(what, ok);
        cc_model_free(&m);
        buf_free(&t);
    }
    /* An element on a datum level, a grid key that is not its id, 1001 axes. */
    {
        size_t fn;
        char *base = frame("frame.json", &fn);
        Buf t;
        buf_init(&t);
        const char *at = strstr(base, "\"elevation\": 3000.0");
        buf_append(&t, base, (size_t)(at - base));
        buf_puts(&t, "\"elevation\": 3000.0, \"is_storey\": false");
        buf_puts(&t, at + strlen("\"elevation\": 3000.0"));
        cc_model m;
        cc_error e;
        int rc = cc_model_load(&m, t.p, t.len, &e);
        check_true("file: element on a datum level refused",
                   rc != 0 && strstr(e.msg, "the level " N2 " is not a storey: no element belongs to it"));
        cc_model_free(&m);
        buf_clear(&t);
        /* "grids" with 1001 axes: refused before any copy */
        buf_append(&t, base, fn - 6); /* before "\n  }\n}" closes model and file */
        buf_puts(&t, ",\n    \"grids\": {\"" G1 "\": {\"id\": \"" G1 "\", \"name\": \"G\", \"axes\": [");
        for (int k = 0; k < 1001; k++) {
            char ax[160];
            snprintf(ax, sizeof ax, "%s{\"id\": \"1AXIS%017d\", \"tag\": \"%d\", \"start_mm\": [0, 0], \"end_mm\": [%d, 1]}",
                     k ? ", " : "", k, k, k + 1);
            buf_puts(&t, ax);
        }
        buf_puts(&t, "]}}\n  }\n}");
        rc = cc_model_load(&m, t.p, t.len, &e);
        if (rc && !strstr(e.msg, "at most 1000 axes")) printf("  1001 axes: %s\n", e.msg);
        check_true("file: 1001 axes refused", rc != 0 && strstr(e.msg, "at most 1000 axes"));
        cc_model_free(&m);
        buf_free(&t);
        free(base);
    }
    {
        const char *key = strstr(gj, "\"grids\": {\n      \"");
        Buf t;
        buf_init(&t);
        size_t off = key ? (size_t)(key - gj) + strlen("\"grids\": {\n      \"") : 0;
        buf_append(&t, gj, off);
        buf_puts(&t, G1);
        buf_puts(&t, gj + off + 22);
        cc_model m;
        cc_error e;
        int rc = key ? cc_model_load(&m, t.p, t.len, &e) : 0;
        check_true("file: grid key does not match id", key && rc != 0 && strstr(e.msg, "key does not match id"));
        if (key) cc_model_free(&m);
        buf_free(&t);
    }
    free(gj);
}

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    golden = dir;
    old_files();
    transcript();
    round_trips();
    scene_move();
    rules();
    file_errors();
    return report("grid");
}
