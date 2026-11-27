/* Mission 1 across links (ticket M1LINKS, ADR 0081): the property sets read beside the display
 * (src/ifc_psets.h), their table as a link cache's section PROP (src/viewfile.c, kept on disk),
 * hostile tables and sections, the notes of a link entry in the .bimble (src/file.c). Fails before
 * M1LINKS: no ifc_psets.h, no cc_step_parse_only, no notes in a link entry.
 * SIZE2: the packed table (section PRPZ: each distinct set once, gzip blocks): every element's entry read
 * back through the packed reader is the table's, the shared sets stored once, the section smaller; a
 * table of 3,000 elements sharing 3 sets packs to a few % of it; hostile packed sections (every byte
 * flipped, cuts, a block that inflates past its declared size) are refused or read as JSON-free errors, no
 * crash, no read past the section. Fails before SIZE2: no cc_psets_pack. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "file.h"
#include "ifc_psets.h"
#include "ifc_read.h"
#include "viewfile.h"

static const char *golden;

/* SIZE2: the packed reader over bytes in memory (a cache's section) */
typedef struct { const uint8_t *p; uint64_t n; } Mem;
static int mem_read(void *ctx, uint64_t off, size_t n, void *dst) {
    const Mem *m = ctx;
    if (off > m->n || n > m->n - off) return -1;
    memcpy(dst, m->p + off, n);
    return 0;
}

static void packed(const char *what, const Buf *t, int shared_expected) {
    Buf pk;
    buf_init(&pk);
    char msg[200];
    snprintf(msg, sizeof msg, "%s: packed", what);
    check_true(msg, cc_psets_pack((const uint8_t *)t->p, t->len, &pk) == 0 && !pk.oom);
    uint32_t ne = 0, ns = 0, nr = 0;
    memcpy(&ne, pk.p, 4), memcpy(&ns, pk.p + 4, 4), memcpy(&nr, pk.p + 8, 4);
    Mem m = {(const uint8_t *)pk.p, pk.len};
    int same = cc_psets_packed_check((const uint8_t *)pk.p, pk.len, pk.len, ne) == 0;
    Buf e;
    buf_init(&e);
    for (size_t i = 0; same && i < ne; i++) {
        const uint8_t *p;
        size_t len;
        buf_clear(&e);
        same = cc_psets_entry((const uint8_t *)t->p, t->len, i, &p, &len) == 0 && cc_psets_packed_entry(mem_read, &m, pk.len, i, &e) == 0 &&
               e.len == len && (!len || !memcmp(e.p, p, len));
    }
    snprintf(msg, sizeof msg, "%s: every entry read back from the packed table is the table's (%u elements, %u references, %u distinct sets; %zu -> %zu bytes)",
             what, ne, nr, ns, t->len, pk.len);
    check_true(msg, same);
    if (shared_expected) {
        snprintf(msg, sizeof msg, "%s: shared sets stored once, the packed table under a tenth of the table", what);
        check_true(msg, ns < nr && pk.len * 10 < t->len);
    }
    /* hostile: every byte flipped (a sample), every cut: refused or read, never a crash or a read past */
    size_t step = pk.len / 500 + 1, reads = 0;
    uint8_t *d = malloc(pk.len);
    for (size_t at = 0; d && at < pk.len; at += step) {
        memcpy(d, pk.p, pk.len);
        d[at] ^= 0x5a;
        Mem h = {d, pk.len};
        for (size_t i = 0; i < ne && i < 40; i++) {
            buf_clear(&e);
            reads += cc_psets_packed_entry(mem_read, &h, pk.len, i, &e) == 0;
        }
    }
    for (size_t cut = 0; cut < pk.len; cut += step) {
        Mem h = {(const uint8_t *)pk.p, cut};
        buf_clear(&e);
        if (cc_psets_packed_entry(mem_read, &h, cut, ne ? ne - 1 : 0, &e) == 0 && cut < pk.len && e.len) reads++;
    }
    snprintf(msg, sizeof msg, "%s: damaged packed tables never crash (%zu entries still read)", what, reads);
    check_true(msg, 1);
    free(d);
    buf_free(&e), buf_free(&pk);
}

/* SIZE2: a table of `n` elements, each with one of 3 sets (as IfcRelDefinesByProperties shares them). */
static void shared_table(Buf *t, size_t n) {
    static const char *const SETS[3] = {"[{\"name\":\"Pset_WallCommon\",\"properties\":[{\"name\":\"IsExternal\",\"value\":\"true\"},{\"name\":\"LoadBearing\",\"value\":\"false\"}],\"type\":\"IfcPropertySet\"}]",
                                        "[{\"name\":\"Pset_SlabCommon\",\"properties\":[{\"name\":\"IsExternal\",\"value\":\"false\"}],\"type\":\"IfcPropertySet\"},{\"name\":\"Qto\",\"properties\":[{\"name\":\"Width\",\"value\":\"200\"}],\"type\":\"IfcElementQuantity\"}]",
                                        ""};
    uint32_t c = (uint32_t)n, end = 0;
    buf_append(t, &c, 4);
    size_t head = t->len;
    for (size_t i = 0; i < n; i++) buf_append(t, &end, 4);
    for (size_t i = 0; i < n; i++) {
        buf_puts(t, SETS[i % 3]);
        end = (uint32_t)(t->len - head - 4 * n);
        memcpy(t->p + head + 4 * i, &end, 4);
    }
}

static void psets_of_frame(void) {
    size_t n;
    char *text = slurp(golden, "ifc/frame.ifc", &n);
    cc_ifc_scene s;
    cc_error err;
    check_true("frame.ifc read", cc_ifc_read(text, n, &s, &err) == 0);
    Buf t;
    buf_init(&t);
    check_true("its property sets read", cc_ifc_psets(text, n, &s, &t, &err) == 0 && !t.oom);
    size_t with = 0;
    for (size_t i = 0; i < s.scene.nelements; i++) {
        const uint8_t *p;
        size_t len;
        if (cc_psets_entry((const uint8_t *)t.p, t.len, i, &p, &len)) {
            check_true("every entry holds together", 0);
            break;
        }
        with += len && !memcmp(p, "[{\"name\":\"Bimble_Generator\"", 27);
    }
    check_true("every element of frame.ifc has its Bimble_Generator set", with == s.scene.nelements);
    /* the cache: the view file, its PROP last; read back, the section stays on disk */
    cc_viewfile_meta mt;
    Buf vb;
    buf_init(&vb);
    check_true("the cache written", !cc_ifc_scene_meta(&s, &mt) && !cc_viewfile_write_meta(&mt, &s.scene, 0, &vb) &&
                                        !cc_viewfile_append_psets(&vb, t.p, t.len));
    cc_viewfile_meta_free(&mt);
    cc_viewfile v;
    check_true("the cache read: PROP found, not copied", cc_viewfile_read((const uint8_t *)vb.p, vb.len, &v, &err) == 0 && v.has_psets &&
                                                          v.psets_off + v.psets_len == vb.len && v.psets_len == t.len);
    cc_viewfile_free(&v);
    /* hostile: the head's count, an end going back, an end past the section */
    for (int k = 0; k < 3; k++) {
        Buf h;
        buf_init(&h);
        buf_append(&h, vb.p, vb.len);
        char *pay = h.p + (h.len - t.len);
        if (k == 0) pay[0] ^= 1;                    /* entries != elements */
        if (k == 1) memset(pay + 8, 0, 4);          /* end[1] < end[0] when end[0] > 0 */
        if (k == 2) memset(pay + 4 + 4 * (s.scene.nelements - 1), 0xff, 4); /* the last end past the end */
        int rc = cc_viewfile_read((const uint8_t *)h.p, h.len, &v, &err);
        char what[96];
        snprintf(what, sizeof what, "a hostile PROP refused (%d): %s", k, rc ? err.msg : "read");
        check_true(what, rc != 0);
        cc_viewfile_free(&v);
        buf_free(&h);
    }
    /* hostile tables given to the entry reader */
    const uint8_t tiny[3] = {1, 0, 0}, big[8] = {0xff, 0xff, 0xff, 0x7f, 0, 0, 0, 0};
    const uint8_t *p;
    size_t len;
    check_true("a table shorter than its head", cc_psets_entry(tiny, 3, 0, &p, &len) != 0);
    check_true("a table counting more entries than it holds", cc_psets_entry(big, 8, 0, &p, &len) != 0);
    check_true("an entry past the count", cc_psets_entry((const uint8_t *)t.p, t.len, s.scene.nelements, &p, &len) != 0);
    packed("frame.ifc", &t, 0); /* SIZE2 */
    { /* SIZE2: the packed section in a cache: PRPZ found, kept on disk, its head checked */
        Buf pk, vb2;
        buf_init(&pk), buf_init(&vb2);
        cc_viewfile_meta mt2;
        int ok = !cc_psets_pack((const uint8_t *)t.p, t.len, &pk) && !cc_ifc_scene_meta(&s, &mt2) && !cc_viewfile_write_meta(&mt2, &s.scene, 0, &vb2) &&
                 !cc_viewfile_append_psets_packed(&vb2, pk.p, pk.len);
        cc_viewfile_meta_free(&mt2);
        ok = ok && cc_viewfile_read((const uint8_t *)vb2.p, vb2.len, &v, &err) == 0 && v.has_psets && v.psets_packed &&
             v.psets_off + v.psets_len == vb2.len && v.psets_len == pk.len;
        check_true("the cache read: PRPZ found, packed, not copied", ok);
        cc_viewfile_free(&v);
        vb2.p[vb2.len - pk.len] ^= 1; /* its element count */
        check_true("a PRPZ of another element count refused", cc_viewfile_read((const uint8_t *)vb2.p, vb2.len, &v, &err) != 0);
        cc_viewfile_free(&v);
        buf_free(&pk), buf_free(&vb2);
    }
    {
        Buf big;
        buf_init(&big);
        shared_table(&big, 3000);
        packed("3,000 elements sharing 3 sets", &big, 1);
        /* a block that inflates past its declared size: the declared size shrunk (send[] of the last set) */
        Buf pk;
        buf_init(&pk);
        cc_psets_pack((const uint8_t *)big.p, big.len, &pk);
        uint32_t ne, ns, nr;
        memcpy(&ne, pk.p, 4), memcpy(&ns, pk.p + 4, 4), memcpy(&nr, pk.p + 8, 4);
        size_t send_last = 16 + 4 * ((size_t)ne + 1) + 4 * (size_t)nr + 4 * ((size_t)ns - 1);
        uint32_t small = 3;
        memcpy(pk.p + send_last, &small, 4);
        Mem m = {(const uint8_t *)pk.p, pk.len};
        Buf e;
        buf_init(&e);
        check_true("a block inflating past its declared size refused (zip-bomb guard)", cc_psets_packed_entry(mem_read, &m, pk.len, 1, &e) != 0);
        buf_free(&e), buf_free(&pk), buf_free(&big);
    }
    buf_free(&vb), buf_free(&t);
    cc_ifc_scene_free(&s);
    free(text);
}

static void notes_in_the_file(void) {
    static const char *const base =
        "{\"format\":\"bimble\",\"version\":1,\"model\":{\"project_id\":\"1ECFGZP4n20ADaVDzRQc6J\",\"project_name\":\"P\",\"storeys\":{},"
        "\"materials\":{},\"elements\":{},\"links\":[{\"name\":\"a.ifc\",\"notes\":%s,\"path\":\"/x/a.ifc\"}]}}";
    char text[8192];
    cc_model m;
    cc_error err;
    snprintf(text, sizeof text, base, "[{\"at\":[1.5,-2.0,3000.0],\"element\":\"0svT5hRin94BdWGBOHxd1z\",\"text\":\"Check\"}]");
    int rc = cc_model_load(&m, text, strlen(text), &err);
    if (rc) printf("  %s\n", err.msg);
    check_true("a link's note read", rc == 0 && m.nlinks == 1 && m.links[0].nnotes == 1 && m.links[0].notes[0].at[2] == 3000.0);
    if (!rc) {
        Buf b;
        buf_init(&b);
        check_true("written back with its keys sorted", !cc_model_save(&m, &b) && strstr(buf_cstr(&b), "\"notes\": [") != NULL);
        buf_free(&b);
        cc_model_free(&m);
    }
    static const char *const bad[] = {
        "[{\"at\":[1,2],\"element\":\"0svT5hRin94BdWGBOHxd1z\",\"text\":\"x\"}]",      /* two numbers */
        "[{\"at\":[1,2,3],\"element\":\"not a GlobalId\",\"text\":\"x\"}]",            /* no GlobalId */
        "[{\"at\":[1,2,1e300],\"element\":\"0svT5hRin94BdWGBOHxd1z\",\"text\":\"x\"}]", /* too far */
        "[{\"at\":[1,2,3],\"element\":\"0svT5hRin94BdWGBOHxd1z\",\"text\":\"a\\u0000b\"}]", /* a NUL */
        "[{\"at\":[1,2,3],\"element\":\"0svT5hRin94BdWGBOHxd1z\",\"text\":\"x\",\"z\":1}]", /* an unknown key */
        "{}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        snprintf(text, sizeof text, base, bad[i]);
        rc = cc_model_load(&m, text, strlen(text), &err);
        char what[64];
        snprintf(what, sizeof what, "a hostile note refused (%zu)", i);
        check_true(what, rc != 0);
        if (!rc) cc_model_free(&m);
    }
    /* 257 notes: refused */
    Buf many;
    buf_init(&many);
    buf_putc(&many, '[');
    for (int i = 0; i < 257; i++) buf_puts(&many, i ? ",{\"at\":[0,0,0],\"element\":\"0svT5hRin94BdWGBOHxd1z\",\"text\":\"n\"}" : "{\"at\":[0,0,0],\"element\":\"0svT5hRin94BdWGBOHxd1z\",\"text\":\"n\"}");
    buf_putc(&many, ']');
    char *big = malloc(many.len + 4096);
    snprintf(big, many.len + 4096, base, buf_cstr(&many));
    rc = cc_model_load(&m, big, strlen(big), &err);
    check_true("257 notes on a link refused", rc != 0);
    if (!rc) cc_model_free(&m);
    free(big), buf_free(&many);
}

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    golden = dir;
    psets_of_frame();
    notes_in_the_file();
    return report("m1links");
}
