/* Suite verbs_materials (ticket MAT1): the material commands, verbs and queries.
 * - golden/materials/transcript.txt (Rust's ccore_golden, materials_files): each command of the
 *   transcript parsed from Rust's JSON, described before it applies, applied on frame2.json; its
 *   inverse written as JSON and described after, or its error text: byte for byte.
 * - golden/materials/after.json: the model after the transcript, saved; after.ifc: its IFC4
 *   export (the version masked as ifc_out_test does). The inverses in reverse give frame2.json.
 * - .bimble round trip: frame2.json (three materials: full, partial and library values, a name
 *   with JSON escapes) and after.json load and save to their bytes, in the text and the binary
 *   form.
 * - The verbs material_add, material_set, material_remove (C additions, DEVIATIONS 29): one
 *   command each, one history step, undo; their refusals.
 * - The library (library.c) spot-checked against Rust's table, file and line cited.
 * - The queries material_library and materials_full. */
#include <string.h>
#include "../src/command.h"
#include "../src/file.h"
#include "../src/guid.h"
#include "../src/history.h"
#include "../src/ifc_out.h"
#include "../src/library.h"
#include "../src/model.h"
#include "../src/query.h"
#include "../src/verbs.h"
#include "check.h"

static const char *golden;

static int load(cc_model *m, const char *name) {
    size_t n;
    char *t = slurp(golden, name, &n);
    cc_error e;
    int rc = cc_model_load(m, t, n, &e);
    if (rc) printf("load %s: %s\n", name, e.msg);
    free(t);
    return rc;
}

static void mask_version(const char *s, size_t n, Buf *out) { /* as ifc_out_test.c */
    static const char *from[] = {"'bimble " CC_IFC_VERSION "'", "'" CC_IFC_VERSION "'"};
    static const char *to[] = {"'bimble <version>'", "'<version>'"};
    buf_clear(out);
    for (size_t i = 0; i < n;) {
        int hit = 0;
        for (int k = 0; k < 2 && !hit; k++) {
            size_t fl = strlen(from[k]);
            if (n - i >= fl && !memcmp(s + i, from[k], fl)) buf_puts(out, to[k]), i += fl, hit = 1;
        }
        if (!hit) buf_putc(out, s[i++]);
    }
}

static void cmd_json(const cc_command *c, Buf *out) {
    JsonW w;
    buf_clear(out);
    jw_init(&w, out, 0);
    cc_command_write(&w, c);
    jw_finish(&w);
}

/* The next line of t (without '\n'), NULL at the end. */
static const char *next_line(const char **t, size_t *n) {
    if (!**t) return NULL;
    const char *s = *t, *e = strchr(s, '\n');
    if (!e) e = s + strlen(s);
    *n = (size_t)(e - s);
    *t = *e ? e + 1 : e;
    return s;
}

static int starts(const char *s, size_t n, const char *p) {
    size_t k = strlen(p);
    return n >= k && !memcmp(s, p, k);
}

static void transcript(void) {
    size_t tn, f2n, an, in;
    char *t = slurp(golden, "materials/transcript.txt", &tn);
    char *f2 = slurp(golden, "frame2.json", &f2n);
    char *after = slurp(golden, "materials/after.json", &an);
    char *ifc = slurp(golden, "materials/after.ifc", &in);
    cc_model m;
    if (load(&m, "frame2.json")) exit(1);
    cc_cmds keep; /* the parsed commands: the model borrows their strings until the end */
    cc_cmds_init(&keep);
    cc_command inv[64];
    size_t ninv = 0, records = 0;
    Buf got, err;
    buf_init(&got), buf_init(&err);
    const char *p = t;
    for (;;) {
        size_t n1, n2, n3, n4;
        const char *l1 = next_line(&p, &n1);
        if (!l1) break;
        const char *l2 = next_line(&p, &n2), *l3 = next_line(&p, &n3), *l4 = next_line(&p, &n4);
        if (!l2 || !l3 || !l4 || !starts(l1, n1, "cmd ") || !starts(l2, n2, "describe ")) {
            check_true("transcript: four lines per record", 0);
            break;
        }
        records++;
        char what[64];
        snprintf(what, sizeof what, "materials record %zu", records);
        size_t before = keep.n;
        buf_clear(&err);
        if (cc_commands_parse(l1 + 4, n1 - 4, &keep, &err) || keep.n != before + 1) {
            /* a single object: wrap it in an array */
            Buf arr;
            buf_init(&arr);
            buf_putc(&arr, '['), buf_append(&arr, l1 + 4, n1 - 4), buf_putc(&arr, ']');
            keep.n = before;
            buf_clear(&err);
            int rc = cc_commands_parse(arr.p, arr.len, &keep, &err);
            buf_free(&arr);
            if (rc || keep.n != before + 1) {
                printf("%s: parse: %.*s\n", what, (int)err.len, err.p);
                check_true(what, 0);
                continue;
            }
        }
        const cc_command *c = &keep.v[keep.n - 1];
        cmd_json(c, &got); /* Rust's JSON back, byte for byte */
        check_eq(what, got.p, got.len, l1 + 4, n1 - 4);
        buf_clear(&got);
        cc_command_describe(c, &m, &got);
        check_eq(what, got.p, got.len, l2 + 9, n2 - 9);
        buf_clear(&err);
        cc_command iv;
        if (cc_command_apply(&m, c, &iv, &err) == 0) {
            check_true(what, starts(l3, n3, "ok "));
            cmd_json(&iv, &got);
            check_eq(what, got.p, got.len, l3 + 3, n3 - 3);
            buf_clear(&got);
            cc_command_describe(&iv, &m, &got);
            check_eq(what, got.p, got.len, l4 + 5, n4 - 5);
            if (ninv < 64) inv[ninv++] = iv;
        } else {
            check_true(what, starts(l3, n3, "err "));
            check_eq(what, err.p, err.len, l3 + 4, n3 - 4);
            check_eq(what, l4, n4, "undo -", 6);
        }
    }
    check_true("transcript: 26 records", records == 26);
    cc_error ve;
    check_true("model valid after the transcript", cc_model_validate(&m, &ve) == 0);
    buf_clear(&got);
    cc_model_save(&m, &got);
    check_eq("materials/after.json", got.p, got.len, after, an);
    Buf ifct, masked;
    buf_init(&ifct), buf_init(&masked);
    check_true("after exports", cc_ifc_export(&m, "materials.ifc", "2026-01-01T00:00:00", CC_IFC4, &ifct) == 0);
    mask_version(ifct.p, ifct.len, &masked);
    check_eq("materials/after.ifc", masked.p, masked.len, ifc, in);
    for (size_t i = ninv; i-- > 0;) {
        cc_command back;
        buf_clear(&err);
        check_true("an inverse applies", cc_command_apply(&m, &inv[i], &back, &err) == 0);
    }
    buf_clear(&got);
    cc_model_save(&m, &got);
    check_eq("undo gives frame2.json back", got.p, got.len, f2, f2n);
    buf_free(&ifct), buf_free(&masked), buf_free(&got), buf_free(&err);
    cc_model_free(&m);
    cc_cmds_free(&keep);
    free(t), free(f2), free(after), free(ifc);
}

/* Load and save give the same bytes: the text form, and the binary form read back. */
static void round_trip(const char *name) {
    size_t n;
    char *t = slurp(golden, name, &n);
    cc_model m, m2;
    cc_error e;
    char what[128];
    snprintf(what, sizeof what, "round trip %s", name);
    if (cc_model_load(&m, t, n, &e)) {
        check_true(what, 0);
        free(t);
        return;
    }
    Buf b, bin;
    buf_init(&b), buf_init(&bin);
    cc_model_save(&m, &b);
    check_eq(what, b.p, b.len, t, n);
    check_true(what, cc_model_save_v2(&m, &bin) == 0);
    check_true(what, cc_model_load(&m2, bin.p, bin.len, &e) == 0);
    buf_clear(&b);
    cc_model_save(&m2, &b);
    check_eq(what, b.p, b.len, t, n);
    buf_free(&b), buf_free(&bin);
    cc_model_free(&m), cc_model_free(&m2);
    free(t);
}

/* ---- verbs ---- */

static int run(cc_model *m, cc_history *h, const char *verb, const char *args, Buf *err) {
    cc_cmds l;
    cc_cmds_init(&l);
    buf_clear(err);
    int rc = cc_verb_run(m, verb, args, strlen(args), &l, err);
    if (rc == 0) {
        rc = (int)l.n;
        if (l.n && cc_history_apply(h, m, l.v, l.n, err)) rc = -1;
    }
    cc_cmds_free(&l);
    return rc;
}

static const cc_material *by_name(const cc_model *m, const char *name) {
    for (size_t i = 0; i < m->nmaterials; i++)
        if (m->materials[i].name.n == strlen(name) && !memcmp(m->materials[i].name.p, name, strlen(name)))
            return &m->materials[i];
    return NULL;
}

static void err_is(const char *what, const Buf *err, const char *want) { check_eq(what, err->p, err->len, want, strlen(want)); }

static void verbs(void) {
    cc_model m;
    if (load(&m, "frame2.json")) return;
    cc_history h;
    cc_history_init(&h);
    Buf err, js, before;
    buf_init(&err), buf_init(&js), buf_init(&before);
    cc_model_save(&m, &before);
    char a[512];
    /* add from the library: one add_material, the grade's values (library.rs:102) */
    check_true("material_add S235: one command", run(&m, &h, "material_add", "{\"library\":\"S235\"}", &err) == 1);
    const cc_material *s = by_name(&m, "S235");
    check_true("material_add S235: added, steel, 235 MPa", s && s->category == CC_CAT_STEEL && s->props.has_strength &&
                                                         s->props.strength_mpa == 235.0 && s->props.e_mpa == 210000.0);
    check_true("one history step", h.nundo == 1 && !strcmp(h.undo[0].label, "add the material S235"));
    /* a name already there: refused (any case, trimmed) */
    check_true("material_add c30/37 refused", run(&m, &h, "material_add", "{\"library\":\"C30/37\",\"name\":\" c30/37 \"}", &err) < 0);
    err_is("material_add taken", &err, "material_add: there is already a material named \"c30/37\"");
    check_true("material_add S235 again refused", run(&m, &h, "material_add", "{\"library\":\"S235\"}", &err) < 0);
    check_true("material_add unknown grade", run(&m, &h, "material_add", "{\"library\":\"C99/99\"}", &err) < 0);
    err_is("material_add unknown grade text", &err, "material_add: no grade \"C99/99\" in the library");
    check_true("material_add needs a name", run(&m, &h, "material_add", "{}", &err) < 0);
    check_true("material_add bad category", run(&m, &h, "material_add", "{\"name\":\"X\",\"category\":\"wood\"}", &err) < 0);
    check_true("material_add unknown field", run(&m, &h, "material_add", "{\"name\":\"X\",\"colour\":1}", &err) < 0);
    /* a blank one */
    check_true("material_add blank", run(&m, &h, "material_add", "{\"name\":\"Mortier\",\"category\":\"masonry\"}", &err) == 1);
    const cc_material *mo = by_name(&m, "Mortier");
    check_true("blank: masonry, no values", mo && mo->category == CC_CAT_MASONRY && !mo->props.has_density &&
                                             !mo->props.has_e && !mo->props.has_strength && !mo->props.source.n);
    if (!mo) { /* the verb is not there (the code before MAT1): nothing more to check */
        cc_history_free(&h);
        cc_model_free(&m);
        buf_free(&err), buf_free(&js), buf_free(&before);
        return;
    }
    char mo_id[23];
    memcpy(mo_id, mo->id.c, 22), mo_id[22] = 0;
    /* set: rename, family, values; the same values: no step */
    snprintf(a, sizeof a, "{\"id\":\"%s\",\"name\":\"Mortier M10\",\"props\":{\"density_kg_m3\":1800,\"strength_mpa\":10}}", mo_id);
    check_true("material_set: one command", run(&m, &h, "material_set", a, &err) == 1);
    check_true("material_set label", !strcmp(h.undo[h.nundo - 1].label, "rename the material Mortier to Mortier M10"));
    mo = by_name(&m, "Mortier M10");
    check_true("material_set applied", mo && mo->props.has_density && mo->props.density_kg_m3 == 1800.0 && !mo->props.has_e);
    check_true("material_set same: no step", run(&m, &h, "material_set", a, &err) == 0);
    snprintf(a, sizeof a, "{\"id\":\"%s\",\"name\":\"S235\"}", mo_id);
    check_true("material_set taken name refused", run(&m, &h, "material_set", a, &err) < 0);
    err_is("material_set taken text (the command's)", &err, "invalid: there is already a material named \"S235\"");
    snprintf(a, sizeof a, "{\"id\":\"%s\",\"props\":{\"e_mpa\":-5}}", mo_id);
    check_true("material_set bad value refused", run(&m, &h, "material_set", a, &err) < 0);
    err_is("material_set bad value text", &err, "invalid: material modulus: a positive number up to 1000000");
    snprintf(a, sizeof a, "{\"id\":\"%s\",\"props\":{}}", mo_id);
    check_true("material_set props {} clears", run(&m, &h, "material_set", a, &err) == 1 && by_name(&m, "Mortier M10") &&
                                                   !by_name(&m, "Mortier M10")->props.has_density);
    check_true("material_set no such material", run(&m, &h, "material_set", "{\"id\":\"0000000000000000000000\"}", &err) < 0);
    err_is("material_set no such text", &err, "material_set: no material 0000000000000000000000");
    /* assign (Rust's verb set_material), then remove refused while in use */
    const cc_element *col = NULL;
    for (size_t i = 0; i < m.nelements && !col; i++)
        if (m.elements[i].cls == CC_IFC_COLUMN) col = &m.elements[i];
    char col_id[23];
    memcpy(col_id, col->id.c, 22), col_id[22] = 0;
    snprintf(a, sizeof a, "{\"ids\":[\"%s\"],\"material\":\"Mortier M10\"}", col_id);
    check_true("set_material: the column made of it", run(&m, &h, "set_material", a, &err) == 1);
    snprintf(a, sizeof a, "{\"id\":\"%s\"}", mo_id);
    check_true("material_remove in use refused", run(&m, &h, "material_remove", a, &err) < 0);
    err_is("material_remove in use text", &err, "material_remove: Mortier M10 is the material of 1 element: give them another material first");
    snprintf(a, sizeof a, "{\"ids\":[\"%s\"],\"material\":\"C30/37\"}", col_id);
    check_true("set_material back", run(&m, &h, "set_material", a, &err) == 1);
    snprintf(a, sizeof a, "{\"id\":\"%s\"}", mo_id);
    check_true("material_remove: one command", run(&m, &h, "material_remove", a, &err) == 1);
    check_true("material_remove: gone", !by_name(&m, "Mortier M10"));
    check_true("material_remove label", !strcmp(h.undo[h.nundo - 1].label, "delete the material Mortier M10"));
    /* undo every step: the bytes of before */
    size_t steps = h.nundo;
    check_true("seven steps", steps == 7);
    while (cc_history_undo(&h, &m)) {}
    buf_clear(&js);
    cc_model_save(&m, &js);
    check_eq("undo all: frame2.json", js.p, js.len, before.p, before.len);
    for (size_t i = 0; i < steps; i++) cc_history_redo(&h, &m);
    check_true("redo all: S235 there, Mortier gone", by_name(&m, "S235") && !by_name(&m, "Mortier M10"));
    cc_history_free(&h);
    cc_model_free(&m);
    buf_free(&err), buf_free(&js), buf_free(&before);
}

/* The library against Rust's table (crates/bimble-core/src/library.rs on oracle/rust). */
static void library(void) {
    size_t n;
    const cc_library_material *l = cc_library(&n);
    check_true("library: 16 grades (library.rs:92-133)", n == 16);
    const cc_library_material *c = cc_library_lookup("C30/37", 6);
    /* library.rs:95 entry("C30/37", Concrete, 2500.0, 33_000.0, 30.0, "fck", c) */
    check_true("C30/37 (library.rs:95)", c && c->category == CC_CAT_CONCRETE && c->density_kg_m3 == 2500.0 &&
                                         c->e_mpa == 33000.0 && c->strength_mpa == 30.0 && !strcmp(c->strength_symbol, "fck"));
    /* library.rs:90 let c = "EN 1992-1-1 Table 3.1; 25 kN/m³ reinforced (EN 1991-1-1)" */
    check_true("concrete source (library.rs:90)", c && !strcmp(c->source, "EN 1992-1-1 Table 3.1; 25 kN/m\xc2\xb3 reinforced (EN 1991-1-1)"));
    /* library.rs:102 entry("S355", Steel, 7850.0, 210_000.0, 355.0, "fy", s); :91 the source */
    const cc_library_material *s = cc_library_lookup("Steel S355 J2", 13); /* library.rs:190 */
    check_true("S355 (library.rs:102)", s && s->category == CC_CAT_STEEL && s->density_kg_m3 == 7850.0 &&
                                        s->e_mpa == 210000.0 && s->strength_mpa == 355.0 && !strcmp(s->strength_symbol, "fy") &&
                                        !strcmp(s->source, "EN 1993-1-1 Table 3.1 (t \xe2\x89\xa4 40 mm)"));
    /* library.rs:105-111 B500B, Steel, 7850.0, 205_000.0, 500.0, "fyk", "SIA 262 (Es = 205 GPa)" */
    const cc_library_material *b = cc_library_lookup("B500B", 5);
    check_true("B500B (library.rs:105)", b && b->e_mpa == 205000.0 && b->strength_mpa == 500.0 && !strcmp(b->strength_symbol, "fyk") &&
                                         !strcmp(b->source, "SIA 262 (Es = 205 GPa)"));
    /* library.rs:114-120 C24, Timber, 420.0, 11_000.0, 24.0, "fm,k", "EN 338 (ρmean)" */
    const cc_library_material *t = cc_library_lookup("Timber C24", 10);
    check_true("C24 (library.rs:114)", t && t->category == CC_CAT_TIMBER && t->density_kg_m3 == 420.0 && t->e_mpa == 11000.0 &&
                                       !strcmp(t->strength_symbol, "fm,k") && !strcmp(t->source, "EN 338 (\xcf\x81mean)"));
    /* library.rs:125 ("GL28h", 12_600.0, 28.0, 460.0), fm,g,k, "EN 14080 (E0,g,mean, ρg,mean)" */
    const cc_library_material *g = cc_library_lookup("gl28h", 5); /* library.rs:193 */
    check_true("GL28h (library.rs:125)", g && g->e_mpa == 12600.0 && g->strength_mpa == 28.0 && g->density_kg_m3 == 460.0 &&
                                         !strcmp(g->strength_symbol, "fm,g,k"));
    check_true("first and last in order", !strcmp(l[0].name, "C20/25") && !strcmp(l[n - 1].name, "GL32h"));
}

static void queries(void) {
    cc_model m;
    if (load(&m, "frame2.json")) return;
    Buf out;
    buf_init(&out);
    check_true("query material_library", cc_query(&m, "material_library", "", 0, &out) == 200);
    static const char FIRST[] = "[{\"category\":\"concrete\",\"density_kg_m3\":2500.0,\"e_mpa\":30000.0,\"ifc_category\":"
                                "\"concrete\",\"name\":\"C20/25\",\"source\":\"EN 1992-1-1 Table 3.1; 25 kN/m\xc2\xb3 "
                                "reinforced (EN 1991-1-1)\",\"strength_mpa\":20.0,\"strength_symbol\":\"fck\"},";
    check_true("material_library first entry", out.len > sizeof FIRST && !memcmp(out.p, FIRST, sizeof FIRST - 1));
    buf_clear(&out);
    check_true("query materials_full", cc_query(&m, "materials_full", "", 0, &out) == 200);
    static const char BOIS[] = "{\"category\":\"timber\",\"element_count\":";
    int found = 0;
    for (size_t i = 0; i + sizeof BOIS < out.len && !found; i++) found = !memcmp(out.p + i, BOIS, sizeof BOIS - 1);
    check_true("materials_full: the timber row", found);
    static const char PARTIAL[] = "\"name\":\"Bois \\\"local\\\" \\\\ \xc3\xa9pic\xc3\xa9" "a\",\"props\":{\"density_kg_m3\":450.0,\"strength_mpa\":22.5}}";
    found = 0;
    for (size_t i = 0; i + sizeof PARTIAL <= out.len + 1 && !found; i++) found = !memcmp(out.p + i, PARTIAL, sizeof PARTIAL - 1);
    check_true("materials_full: partial values, escapes", found);
    buf_clear(&out);
    int props = 0;
    check_true("query materials", cc_query(&m, "materials", "", 0, &out) == 200);
    for (size_t i = 0; i + 5 <= out.len; i++) props |= !memcmp(out.p + i, "props", 5);
    check_true("query materials stays Rust's summary (no props)", !props);
    buf_free(&out);
    cc_model_free(&m);
}

int main(int argc, char **argv) {
    golden = argc > 1 ? argv[1] : "golden/floats";
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", golden); /* run.sh passes golden/floats */
    golden = dir;
    cc_guid_seed(1);
    transcript();
    round_trip("frame2.json");
    round_trip("materials/after.json");
    verbs();
    library();
    queries();
    return report("verbs_materials");
}
