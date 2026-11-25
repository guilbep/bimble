/* Suite relation: relations as model objects and IfcElementAssembly (ticket X1, ADR 0070). No
 * Rust oracle: the goldens golden/relations/assembly.json (v1) and assembly.bimble (v2) are this
 * suite's own, built here from golden/frame2.json by Commands (CCORE_WRITE_GOLDEN=1 rewrites
 * them). Covers: round trip v1 and v2 with equal model hash; apply, undo, redo of add_relation,
 * set_relation, remove_relation; each validation rule; remove_element of a related element
 * refused; and that the existing goldens (frame, frame2, v1 and v2) still load and save to the
 * same bytes, without a "relations" key. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/command.h"
#include "../src/file.h"
#include "../src/guid.h"
#include "../src/history.h"
#include "../src/ifc_out.h"
#include "../src/model.h"
#include "../src/relation.h"
#include "../src/scene.h"
#include "check.h"

static const char *golden;

/* frame2.json ids */
#define RDC "1vIjA4wIz6huLvC8B1gv4M"   /* storey Rez-de-chaussée */
#define S355 "09TM5shL90ruo$N1V6tpqq"  /* material */
#define C1 "0uTOdmIsz6iAjMnb$AZIG3"    /* IfcColumn */
#define HEB "1WQ4saTef14QR$c82u9pxW"   /* IfcColumn */
#define GOUSSET "2Xi2EcNOvB3wSWg3M1CB5f" /* IfcBuildingElementProxy */
#define W1 "2XB9jAbxPCKAXC_kVMr$h5"    /* IfcWall */
#define D1 "2FgB3PCsHFafKWKmIHC4zx"    /* IfcSlab */
#define B1 "3QaNBTGrj1jwCHQxXT6adX"    /* IfcBeam */
/* new ids (synthetic) */
#define ASM "1ASSEMBLYP100000000000" /* IfcElementAssembly */
#define AGG "2RELAGGREGATES00000001" /* IfcRelAggregates */
#define CON "2RELCONNECTS0000000001" /* IfcRelConnectsWithRealizingElements */
#define PLACEMENT "\"placement\":{\"origin\":[0,0,0],\"z_axis\":[0,0,1],\"x_axis\":[1,0,0]}"

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

static int load_text(cc_model *m, const char *s, size_t n, cc_error *err) { return cc_model_load(m, s, n, err); }

static char *frame2(size_t *n) {
    char *s = slurp(golden, "frame2.json", n);
    strip_nl(s, n);
    return s;
}

/* Applies a JSON array of commands as one history step. 0 ok, -1 with the error in err. */
static int apply(cc_history *h, cc_model *m, const char *json, Buf *err) {
    cc_cmds l;
    cc_cmds_init(&l);
    buf_clear(err);
    int rc = cc_commands_parse(json, strlen(json), &l, err);
    if (!rc) rc = cc_history_apply(h, m, l.v, l.n, err);
    /* The history's inverses borrow the model's arena, never l's: freeing l is safe. */
    cc_cmds_free(&l);
    return rc;
}

static const char *ASSEMBLY_CMDS =
    "[{\"op\":\"add_element\",\"id\":\"" ASM "\",\"class\":\"IfcElementAssembly\",\"name\":\"Portique P1\","
    "\"storey\":\"" RDC "\",\"material\":\"" S355 "\"," PLACEMENT ","
    "\"properties\":{\"Pset_ElementAssemblyCommon/Reference\":\"P1\"}},"
    "{\"op\":\"add_relation\",\"id\":\"" AGG "\",\"kind\":\"IfcRelAggregates\",\"relating\":\"" ASM "\","
    "\"related\":[\"" HEB "\",\"" GOUSSET "\",\"" B1 "\"]},"
    "{\"op\":\"add_relation\",\"id\":\"" CON "\",\"kind\":\"IfcRelConnectsWithRealizingElements\","
    "\"relating\":\"" HEB "\",\"related\":[\"" B1 "\"],\"realizing\":[\"" GOUSSET "\"],"
    "\"properties\":{\"Bimble_Connection/Fastener\":\"4 x M16 8.8\",\"Bimble_Connection/Reversible\":\"true\"}}]";

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

/* The golden model, built from frame2 by Commands: one assembly of three parts, one connection
 * with a realizing element and properties. Then v1 and v2 round trips. */
static void goldens(void) {
    size_t fn;
    char *base = frame2(&fn);
    cc_model m;
    cc_error e;
    cc_history h;
    Buf err, t, t2, bin;
    buf_init(&err), buf_init(&t), buf_init(&t2), buf_init(&bin);
    cc_history_init(&h);
    check_true("frame2 loads", load_text(&m, base, fn, &e) == 0);
    int rc = apply(&h, &m, ASSEMBLY_CMDS, &err);
    if (rc) printf("  apply: %s\n", buf_cstr(&err));
    check_true("assembly commands apply", rc == 0);
    check_true("two relations", m.nrelations == 2);
    model_text(&m, &t);
    check_true("save v2", cc_model_save_v2(&m, &bin) == 0);
    if (getenv("CCORE_WRITE_GOLDEN")) {
        write_file("relations/assembly.json", t.p, t.len, 1);
        write_file("relations/assembly.bimble", bin.p, bin.len, 0);
    }
    size_t gn, bn;
    char *gj = slurp(golden, "relations/assembly.json", &gn);
    strip_nl(gj, &gn);
    char *gb = slurp(golden, "relations/assembly.bimble", &bn);
    check_eq("built model == relations/assembly.json", t.p, t.len, gj, gn);
    check_true("built model v2 == relations/assembly.bimble", bin.len == bn && !memcmp(bin.p, gb, bn));
    check_true("v1 has the relations key", strstr(gj, "\n    \"relations\": {") != NULL);
    {
        /* the assembly's element object, up to its closing brace: no "generator" key */
        const char *at = strstr(gj, "\n      \"" ASM "\": {"), *end = at ? strstr(at, "\n      }") : NULL;
        int has_gen = 0;
        for (const char *p = at; p && end && p < end; p++) has_gen |= !strncmp(p, "\"generator\"", 11);
        check_true("the assembly has no generator key", at && end && !has_gen);
    }
    char h1[65], h2[65];
    hash_hex(&t, h1);

    /* v1: load, save, same bytes and hash */
    cc_model a;
    rc = load_text(&a, gj, gn, &e);
    if (rc) printf("  load assembly.json: %s\n", e.msg);
    check_true("assembly.json loads", rc == 0);
    model_text(&a, &t2);
    check_eq("assembly.json load+save", t2.p, t2.len, gj, gn);
    hash_hex(&t2, h2);
    check_eq("v1 round trip: same model hash", h2, 64, h1, 64);
    check_true("loaded relations", a.nrelations == 2 && cc_model_relation(&a, gid(AGG)) != NULL);

    /* v2: load Rust-shaped binary, same model; C's v2 again byte-equal; CBOR round trip */
    cc_model b;
    rc = load_text(&b, gb, bn, &e);
    if (rc) printf("  load assembly.bimble: %s\n", e.msg);
    check_true("assembly.bimble loads", rc == 0);
    model_text(&b, &t2);
    check_eq("assembly.bimble load, save v1 == assembly.json", t2.p, t2.len, gj, gn);
    hash_hex(&t2, h2);
    check_eq("v2 round trip: same model hash", h2, 64, h1, 64);
    Buf b2;
    buf_init(&b2);
    check_true("v2 save again == assembly.bimble", cc_model_save_v2(&b, &b2) == 0 && b2.len == bn && !memcmp(b2.p, gb, bn));
    buf_clear(&b2);
    check_true("stored-block v2 loads to the same model",
               cc_model_save_v2_stored(&b, &b2) == 0 && (cc_model_free(&b), load_text(&b, b2.p, b2.len, &e) == 0));
    model_text(&b, &t2);
    check_eq("stored v2 round trip", t2.p, t2.len, gj, gn);

    /* undo: frame2's bytes again (no relations key); redo: the golden */
    check_true("undo", cc_history_undo(&h, &m) == 1);
    model_text(&m, &t2);
    check_eq("undo gives frame2's bytes", t2.p, t2.len, base, fn);
    check_true("redo", cc_history_redo(&h, &m) == 1);
    model_text(&m, &t2);
    check_eq("redo gives the golden", t2.p, t2.len, gj, gn);

    /* The scene of a model with an assembly builds (the assembly has no mesh of its own; X2
     * draws it through its parts). */
    cc_scene sc;
    memset(&sc, 0, sizeof sc);
    int src = cc_scene_build(&sc, &m, &e);
    check_true("scene builds with an assembly", src == 0);
    printf("  scene with an assembly: %zu elements, %zu warnings%s%.*s\n", sc.nelements, sc.nwarnings,
           sc.nwarnings ? ": " : "", sc.nwarnings ? (int)sc.warnings[0].n : 0, sc.nwarnings ? sc.warnings[0].p : "");
    cc_scene_free(&sc);
    /* IFC export of an assembly is X2's: until then an error, never a silent drop (ADR 0003). */
    Buf ifc;
    buf_init(&ifc);
    check_true("IFC export refuses an assembly until X2",
               cc_ifc_export(&m, "a.ifc", "2026-11-25T00:00:00", CC_IFC4, &ifc) != 0);
    buf_free(&ifc);

    buf_free(&b2);
    cc_model_free(&a);
    cc_model_free(&b);
    cc_model_free(&m);
    cc_history_free(&h);
    buf_free(&err), buf_free(&t), buf_free(&t2), buf_free(&bin);
    free(base), free(gj), free(gb);
}

/* Existing files keep their bytes: no "relations" key appears. */
static void old_files(void) {
    static const char *const names[] = {"frame", "frame2"};
    for (int k = 0; k < 2; k++) {
        char path[256], what[256];
        size_t jn, bn, cn;
        snprintf(path, sizeof path, "%s.json", names[k]);
        char *json = slurp(golden, path, &jn);
        strip_nl(json, &jn);
        snprintf(path, sizeof path, "bin/%s.bimble", names[k]);
        char *bin = slurp(golden, path, &bn);
        snprintf(path, sizeof path, "bin/%s.cbor", names[k]);
        char *cbor = slurp(golden, path, &cn);
        cc_model m;
        cc_error e;
        Buf t;
        buf_init(&t);
        check_true("load v1", load_text(&m, json, jn, &e) == 0);
        model_text(&m, &t);
        snprintf(what, sizeof what, "golden/%s.json load+save: same bytes", names[k]);
        check_eq(what, t.p, t.len, json, jn);
        check_true("no relations key in v1", strstr(buf_cstr(&t), "relations") == NULL && m.nrelations == 0);
        cc_model_free(&m);
        snprintf(what, sizeof what, "golden/bin/%s.bimble loads", names[k]);
        check_true(what, load_text(&m, bin, bn, &e) == 0);
        model_text(&m, &t);
        snprintf(what, sizeof what, "golden/bin/%s.bimble, save v1 == %s.json", names[k], names[k]);
        check_eq(what, t.p, t.len, json, jn);
        buf_clear(&t);
        snprintf(what, sizeof what, "golden/bin/%s.bimble: CBOR saved == Rust's CBOR", names[k]);
        check_true(what, cc_model_to_cbor(&m, &t) == 0 && t.len == cn && !memcmp(t.p, cbor, cn));
        cc_model_free(&m);
        buf_free(&t);
        free(json), free(bin), free(cbor);
    }
}

/* A fresh copy of frame2 plus the assembly, for the rule cases. */
static void fresh(cc_model *m, cc_history *h) {
    size_t fn;
    char *base = frame2(&fn);
    cc_error e;
    Buf err;
    buf_init(&err);
    cc_history_init(h);
    if (load_text(m, base, fn, &e) || apply(h, m, ASSEMBLY_CMDS, &err)) {
        printf("cannot set up: %s %s\n", e.msg, buf_cstr(&err));
        exit(2);
    }
    buf_free(&err);
    free(base);
}

/* One command list that must be refused with a text that contains `want`; the model must not
 * change. */
static void refused(const char *what, const char *json, const char *want) {
    cc_model m;
    cc_history h;
    Buf err, before, after;
    buf_init(&err), buf_init(&before), buf_init(&after);
    fresh(&m, &h);
    model_text(&m, &before);
    int rc = apply(&h, &m, json, &err);
    model_text(&m, &after);
    const char *got = buf_cstr(&err);
    int ok = rc != 0 && strstr(got, want) != NULL;
    if (!ok) printf("  %s: rc %d, got \"%s\", want \"%s\"\n", what, rc, got, want);
    check_true(what, ok);
    check_true("refused: model unchanged", before.len == after.len && !memcmp(before.p, after.p, before.len));
    cc_model_free(&m);
    cc_history_free(&h);
    buf_free(&err), buf_free(&before), buf_free(&after);
}

static void accepted(const char *what, const char *json) {
    cc_model m;
    cc_history h;
    Buf err;
    buf_init(&err);
    fresh(&m, &h);
    int rc = apply(&h, &m, json, &err);
    if (rc) printf("  %s: %s\n", what, buf_cstr(&err));
    check_true(what, rc == 0);
    cc_model_free(&m);
    cc_history_free(&h);
    buf_free(&err);
}

#define REL(id, kind, relating, related) \
    "[{\"op\":\"add_relation\",\"id\":\"" id "\",\"kind\":\"" kind "\",\"relating\":\"" relating "\",\"related\":[" related "]}]"
#define NEW1 "00000000000000000000A1"
#define NEW2 "00000000000000000000A2"
#define ASM2 "00000000000000000000B1"
#define ASM3 "00000000000000000000B2"
#define ADD_ASM(id)                                                                                          \
    "{\"op\":\"add_element\",\"id\":\"" id "\",\"class\":\"IfcElementAssembly\",\"name\":\"" id "\",\"storey\":\"" RDC \
    "\",\"material\":\"" S355 "\"," PLACEMENT "}"

static void rules(void) {
    /* ids exist */
    refused("unknown relating", REL(NEW1, "IfcRelConnectsElements", "00000000000000000000ZZ", "\"" W1 "\""),
            "invalid: relation " NEW1 " (IfcRelConnectsElements): unknown element 00000000000000000000ZZ");
    refused("unknown related", REL(NEW1, "IfcRelConnectsElements", W1, "\"00000000000000000000ZZ\""),
            "unknown element 00000000000000000000ZZ");
    refused("unknown realizing",
            "[{\"op\":\"add_relation\",\"id\":\"" NEW1 "\",\"kind\":\"IfcRelConnectsWithRealizingElements\",\"relating\":\"" W1
            "\",\"related\":[\"" D1 "\"],\"realizing\":[\"00000000000000000000ZZ\"]}]",
            "unknown element 00000000000000000000ZZ");
    refused("a storey is not an element", REL(NEW1, "IfcRelConnectsElements", W1, "\"" RDC "\""),
            "unknown element " RDC);
    /* no relation to itself */
    refused("relation to itself", REL(NEW1, "IfcRelConnectsElements", W1, "\"" W1 "\""),
            "relates " W1 " to itself");
    refused("aggregate of itself", REL(NEW1, "IfcRelAggregates", ASM, "\"" ASM "\""), "relates " ASM " to itself");
    refused("realizing the relating element",
            "[{\"op\":\"add_relation\",\"id\":\"" NEW1 "\",\"kind\":\"IfcRelConnectsWithRealizingElements\",\"relating\":\"" W1
            "\",\"related\":[\"" D1 "\"],\"realizing\":[\"" W1 "\"]}]",
            "relates " W1 " to itself");
    refused("connected and realizing",
            "[{\"op\":\"add_relation\",\"id\":\"" NEW1 "\",\"kind\":\"IfcRelConnectsWithRealizingElements\",\"relating\":\"" W1
            "\",\"related\":[\"" D1 "\"],\"realizing\":[\"" D1 "\"]}]",
            D1 " is both connected and realizing");
    /* IfcRelAggregates: one whole per part, no cycle */
    refused("one whole per part", REL(NEW1, "IfcRelAggregates", C1, "\"" B1 "\""),
            B1 " is a part of two wholes (relations " NEW1 " and " AGG ")");
    refused("aggregate cycle of two",
            "[" ADD_ASM(ASM2) ",{\"op\":\"add_relation\",\"id\":\"" NEW1 "\",\"kind\":\"IfcRelAggregates\",\"relating\":\"" ASM2
            "\",\"related\":[\"" ASM "\"]},{\"op\":\"add_relation\",\"id\":\"" NEW2 "\",\"kind\":\"IfcRelAggregates\","
            "\"relating\":\"" ASM "\",\"related\":[\"" ASM2 "\"]}]",
            "invalid: IfcRelAggregates makes a cycle through ");
    refused("aggregate cycle of three",
            "[" ADD_ASM(ASM2) "," ADD_ASM(ASM3) ",{\"op\":\"add_relation\",\"id\":\"" NEW1
            "\",\"kind\":\"IfcRelAggregates\",\"relating\":\"" ASM2 "\",\"related\":[\"" ASM "\"]},"
            "{\"op\":\"add_relation\",\"id\":\"" NEW2 "\",\"kind\":\"IfcRelAggregates\",\"relating\":\"" ASM3
            "\",\"related\":[\"" ASM2 "\"]},{\"op\":\"set_relation\",\"id\":\"" AGG "\",\"kind\":\"IfcRelAggregates\","
            "\"relating\":\"" ASM "\",\"related\":[\"" ASM3 "\",\"" HEB "\",\"" GOUSSET "\",\"" B1 "\"]}]",
            "IfcRelAggregates makes a cycle through ");
    accepted("nested assemblies (a tree, no cycle)",
             "[" ADD_ASM(ASM2) ",{\"op\":\"add_relation\",\"id\":\"" NEW1 "\",\"kind\":\"IfcRelAggregates\",\"relating\":\"" ASM2
             "\",\"related\":[\"" C1 "\",\"" ASM "\"]}]");
    /* related: sorted, no duplicates, 1..10000; exactly one for a connection */
    refused("related not sorted", REL(NEW1, "IfcRelAggregates", C1, "\"" W1 "\",\"" D1 "\""),
            "related elements must be sorted, without duplicates");
    refused("related twice", REL(NEW1, "IfcRelAggregates", C1, "\"" D1 "\",\"" D1 "\""),
            "related elements must be sorted, without duplicates");
    refused("related empty", REL(NEW1, "IfcRelAggregates", C1, ""), "no related element");
    refused("connection relates one", REL(NEW1, "IfcRelConnectsElements", C1, "\"" D1 "\",\"" W1 "\""),
            "relates exactly one element, got 2");
    refused("realizing on a plain connection",
            "[{\"op\":\"add_relation\",\"id\":\"" NEW1 "\",\"kind\":\"IfcRelConnectsElements\",\"relating\":\"" W1
            "\",\"related\":[\"" D1 "\"],\"realizing\":[\"" C1 "\"]}]",
            "only IfcRelConnectsWithRealizingElements has realizing elements");
    refused("realizing missing", REL(NEW1, "IfcRelConnectsWithRealizingElements", W1, "\"" D1 "\""),
            "no realizing element");
    {
        /* 10,001 related ids: refused on the count before any lookup */
        Buf j;
        buf_init(&j);
        buf_puts(&j, "[{\"op\":\"add_relation\",\"id\":\"" NEW1 "\",\"kind\":\"IfcRelAggregates\",\"relating\":\"" C1
                     "\",\"related\":[");
        for (uint64_t k = 0; k < 10001; k++) {
            cc_guid g = cc_guid_from_u128(1, k);
            buf_puts(&j, k ? ",\"" : "\"");
            buf_append(&j, g.c, 22);
            buf_putc(&j, '"');
        }
        buf_puts(&j, "]}]");
        refused("at most 10000 related", buf_cstr(&j), "at most 10000 related elements");
        buf_free(&j);
    }
    /* kinds */
    refused("unknown kind", REL(NEW1, "IfcRelVoidsElement", W1, "\"" D1 "\""),
            "invalid: relation " NEW1 ": unknown kind \"IfcRelVoidsElement\", expected one of IfcRelAggregates, "
            "IfcRelConnectsElements, IfcRelConnectsWithRealizingElements, IfcRelDefinesByType");
    refused("IfcRelDefinesByType waits for type objects", REL(NEW1, "IfcRelDefinesByType", W1, "\"" D1 "\""),
            "needs a type object, and the model holds no types yet");
    accepted("IfcRelConnectsElements", REL(NEW1, "IfcRelConnectsElements", W1, "\"" D1 "\""));
    /* ids: unique across the model */
    refused("relation id of an element", REL(W1, "IfcRelConnectsElements", C1, "\"" D1 "\""),
            "the id is already used by another object");
    refused("relation id of a storey", REL(RDC, "IfcRelConnectsElements", C1, "\"" D1 "\""),
            "the id is already used by another object");
    refused("element id of a relation",
            "[{\"op\":\"add_element\",\"id\":\"" AGG "\",\"class\":\"IfcColumn\",\"name\":\"X\",\"storey\":\"" RDC
            "\",\"material\":\"" S355 "\"," PLACEMENT ",\"generator\":{\"kind\":\"rect_column\",\"width\":300,"
            "\"depth\":300,\"height\":3000}}]",
            "object already exists: " AGG);
    refused("add an existing relation", REL(AGG, "IfcRelConnectsElements", W1, "\"" D1 "\""),
            "object already exists: " AGG);
    refused("set a missing relation",
            "[{\"op\":\"set_relation\",\"id\":\"" NEW1 "\",\"kind\":\"IfcRelConnectsElements\",\"relating\":\"" W1
            "\",\"related\":[\"" D1 "\"]}]",
            "no such object: " NEW1);
    refused("remove a missing relation", "[{\"op\":\"remove_relation\",\"id\":\"" NEW1 "\"}]", "no such object: " NEW1);
    refused("empty property key",
            "[{\"op\":\"add_relation\",\"id\":\"" NEW1 "\",\"kind\":\"IfcRelConnectsElements\",\"relating\":\"" W1
            "\",\"related\":[\"" D1 "\"],\"properties\":{\"\":\"x\"}}]",
            "empty property key");
    /* RemoveElement of an element in a relation: refused (X2 decides what delete does) */
    refused("remove a part", "[{\"op\":\"remove_element\",\"id\":\"" B1 "\"}]",
            B1 " is still used by the relation " AGG " (IfcRelAggregates)");
    refused("remove the whole", "[{\"op\":\"remove_element\",\"id\":\"" ASM "\"}]",
            ASM " is still used by the relation " AGG " (IfcRelAggregates)");
    refused("remove a realizing element", "[{\"op\":\"remove_relation\",\"id\":\"" AGG "\"},{\"op\":\"remove_element\",\"id\":\"" GOUSSET "\"}]",
            GOUSSET " is still used by the relation " CON " (IfcRelConnectsWithRealizingElements)");
    accepted("remove the relations, then the elements",
             "[{\"op\":\"remove_relation\",\"id\":\"" AGG "\"},{\"op\":\"remove_relation\",\"id\":\"" CON "\"},"
             "{\"op\":\"remove_element\",\"id\":\"" B1 "\"},{\"op\":\"remove_element\",\"id\":\"" ASM "\"}]");
    /* IfcElementAssembly: no generator, no openings; others need one */
    refused("an assembly with a generator",
            "[{\"op\":\"add_element\",\"id\":\"" ASM2 "\",\"class\":\"IfcElementAssembly\",\"name\":\"X\",\"storey\":\"" RDC
            "\",\"material\":\"" S355 "\"," PLACEMENT ",\"generator\":{\"kind\":\"rect_column\",\"width\":300,"
            "\"depth\":300,\"height\":3000}}]",
            "an IfcElementAssembly has no generator: its shape is its parts");
    refused("a column without a generator",
            "[{\"op\":\"add_element\",\"id\":\"" ASM2 "\",\"class\":\"IfcColumn\",\"name\":\"X\",\"storey\":\"" RDC
            "\",\"material\":\"" S355 "\"," PLACEMENT "}]",
            "missing field `generator`");
    refused("set_generator on an assembly",
            "[{\"op\":\"set_generator\",\"id\":\"" ASM "\",\"generator\":{\"kind\":\"rect_column\",\"width\":300,"
            "\"depth\":300,\"height\":3000}}]",
            "an IfcElementAssembly has no generator: its shape is its parts");
}

/* add, set, remove: apply, undo, redo, labels, inverses; the command JSON round trip. */
static void commands(void) {
    cc_model m;
    cc_history h;
    Buf err, t0, t1, t2, labels_u, labels_r;
    buf_init(&err), buf_init(&t0), buf_init(&t1), buf_init(&t2), buf_init(&labels_u), buf_init(&labels_r);
    fresh(&m, &h);
    model_text(&m, &t0);
    static const char *const SET =
        "[{\"op\":\"set_relation\",\"id\":\"" CON "\",\"kind\":\"IfcRelConnectsWithRealizingElements\",\"relating\":\"" HEB
        "\",\"related\":[\"" B1 "\"],\"realizing\":[\"" GOUSSET "\"],\"properties\":{\"Bimble_Connection/Fastener\":"
        "\"6 x M20 10.9\"}}]";
    check_true("set_relation applies", apply(&h, &m, SET, &err) == 0);
    model_text(&m, &t1);
    check_true("set_relation changed the properties", strstr(buf_cstr(&t1), "6 x M20 10.9") && !strstr(buf_cstr(&t1), "Reversible"));
    check_true("add_relation applies", apply(&h, &m, REL(NEW1, "IfcRelConnectsElements", W1, "\"" D1 "\""), &err) == 0);
    check_true("remove_relation applies", apply(&h, &m, "[{\"op\":\"remove_relation\",\"id\":\"" AGG "\"}]", &err) == 0);
    model_text(&m, &t2);
    check_true("after remove: two relations, aggregate gone", m.nrelations == 2 && !cc_model_relation(&m, gid(AGG)));
    cc_history_labels(&h, 0, &labels_u, &labels_r);
    check_eq("undo labels", labels_u.p, labels_u.len,
             "[\"delete the IfcRelAggregates of Portique P1\",\"add the IfcRelConnectsElements of W1\","
             "\"change the IfcRelConnectsWithRealizingElements of HEB \\\"200\\\"\",\"add Portique P1 and 2 more changes\"]",
             strlen("[\"delete the IfcRelAggregates of Portique P1\",\"add the IfcRelConnectsElements of W1\","
                    "\"change the IfcRelConnectsWithRealizingElements of HEB \\\"200\\\"\",\"add Portique P1 and 2 more changes\"]"));
    Buf t;
    buf_init(&t);
    check_true("undo remove", cc_history_undo(&h, &m) == 1);
    check_true("undo add", cc_history_undo(&h, &m) == 1);
    model_text(&m, &t);
    check_eq("undo twice: back to after set", t.p, t.len, t1.p, t1.len);
    check_true("undo set", cc_history_undo(&h, &m) == 1);
    model_text(&m, &t);
    check_eq("undo set: the golden again", t.p, t.len, t0.p, t0.len);
    check_true("redo x3", cc_history_redo(&h, &m) == 1 && cc_history_redo(&h, &m) == 1 && cc_history_redo(&h, &m) == 1);
    model_text(&m, &t);
    check_eq("redo all: same model", t.p, t.len, t2.p, t2.len);

    /* JSON round trip of the three commands (and of add_element without a generator) */
    static const char *const ALL =
        "[{\"op\":\"add_element\",\"id\":\"" ASM "\",\"class\":\"IfcElementAssembly\",\"name\":\"Portique P1\",\"storey\":\""
        RDC "\",\"material\":\"" S355 "\",\"placement\":{\"origin\":[0.0,0.0,0.0],\"z_axis\":[0.0,0.0,1.0],\"x_axis\":[1.0,"
        "0.0,0.0]}},{\"op\":\"add_relation\",\"id\":\"" CON "\",\"kind\":\"IfcRelConnectsWithRealizingElements\","
        "\"relating\":\"" HEB "\",\"related\":[\"" B1 "\"],\"realizing\":[\"" GOUSSET "\"],\"properties\":{\"a/b\":\"c\"}},"
        "{\"op\":\"set_relation\",\"id\":\"" AGG "\",\"kind\":\"IfcRelAggregates\",\"relating\":\"" ASM "\",\"related\":[\""
        HEB "\"]},{\"op\":\"remove_relation\",\"id\":\"" AGG "\"}]";
    cc_cmds l;
    cc_cmds_init(&l);
    check_true("parse the command list", cc_commands_parse(ALL, strlen(ALL), &l, &err) == 0 && l.n == 4);
    buf_clear(&t);
    cc_commands_json(l.v, l.n, &t);
    check_eq("commands written back byte for byte", t.p, t.len, ALL, strlen(ALL));
    cc_cmds_free(&l);
    cc_cmds_init(&l);
    static const char *const NOKIND = "[{\"op\":\"add_relation\",\"id\":\"" NEW1 "\",\"relating\":\"" W1 "\",\"related\":[]}]";
    static const char *const BADID =
        "[{\"op\":\"add_relation\",\"id\":\"nope\",\"kind\":\"IfcRelAggregates\",\"relating\":\"" W1 "\",\"related\":[]}]";
    check_true("relation without kind: parse error",
               cc_commands_parse(NOKIND, strlen(NOKIND), &l, &err) != 0 && strstr(buf_cstr(&err), "missing field `kind`"));
    check_true("relation with a bad id: parse error", cc_commands_parse(BADID, strlen(BADID), &l, &err) != 0 &&
                                                          strstr(buf_cstr(&err), "invalid IFC GlobalId: \"nope\""));
    cc_cmds_free(&l);
    cc_model_free(&m);
    cc_history_free(&h);
    buf_free(&t);
    buf_free(&err), buf_free(&t0), buf_free(&t1), buf_free(&t2), buf_free(&labels_u), buf_free(&labels_r);
}

/* A file with bad relations: parse and validation errors, never a crash. */
static void file_errors(void) {
    size_t gn;
    char *gj = slurp(golden, "relations/assembly.json", &gn);
    strip_nl(gj, &gn);
    struct {
        const char *from, *to, *want;
    } cases[] = {
        {"\"relations\": {", "\"relations\": [", "parse: "},
        {"\"kind\": \"IfcRelAggregates\"", "\"kind\": 7", "invalid type: integer, expected a string"},
        {"\"kind\": \"IfcRelAggregates\"", "\"kinds\": \"IfcRelAggregates\"", "missing field `kind`"},
        {"\"kind\": \"IfcRelAggregates\"", "\"kind\": \"IfcRelNests\"", "unknown kind \"IfcRelNests\""},
        {"\"relations\": {\n      \"" AGG "\"", "\"relations\": {\n      \"" NEW1 "\"", "key does not match id"},
        {"\"relating\": \"" ASM "\"", "\"relating\": \"" ASM2 "\"", "unknown element " ASM2},
        {"\"related\": [\n          \"" B1 "\"", "\"related\": [\n          \"00000000000000000000ZZ\"", "unknown element 00000000000000000000ZZ"},
        {"\"Bimble_Connection/Fastener\"", "\"\"", "empty property key"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const char *at = strstr(gj, cases[i].from);
        char what[256];
        snprintf(what, sizeof what, "file error %zu: %s", i, cases[i].want);
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
        int rc = load_text(&m, t.p, t.len, &e);
        int ok = rc != 0 && strstr(e.msg, cases[i].want) != NULL;
        if (!ok) printf("  case %zu: rc %d, got \"%s\"\n", i, rc, e.msg);
        check_true(what, ok);
        cc_model_free(&m);
        buf_free(&t);
    }
    /* Truncations and byte flips of the v2 golden: errors, never a crash (ASan in the gate). */
    size_t bn;
    char *gb = slurp(golden, "relations/assembly.bimble", &bn);
    int loaded = 0;
    for (size_t k = 0; k < bn; k++) {
        cc_model m;
        cc_error e;
        if (load_text(&m, gb, k, &e) == 0) loaded++;
        cc_model_free(&m);
    }
    check_true("v2 truncations: none loads", loaded == 0);
    free(gb);
    free(gj);
}

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    golden = dir;
    goldens();
    old_files();
    rules();
    commands();
    file_errors();
    return report("relation");
}
