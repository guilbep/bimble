/* Suite viewfile: the .bimbleview export (golden/view/, see golden/README.md) byte for byte with
 * Rust's raw stream, its gzip inflating to the same, Rust's files read back to the C scene,
 * robustness of the reader (prefixes, flipped bytes, lying lengths, huge varints, many
 * sections), sharing a read-only scene (cc_viewfile_write_meta: an IFC read as it is, read back
 * C -> C as Rust has no golden of a read scene's view file; an opened view file shared again gives
 * its raw bytes), and timings (the frame; the 20k synthetic model when out/t16-large-20000.bimble
 * exists: python3 bench/large_model.py). */
#define _POSIX_C_SOURCE 199309L /* clock_gettime under -std=c11 */
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../src/model.h"
#include "../src/file.h"
#include "../src/scene.h"
#include "../src/inflate.h"
#include "../src/meshcodec.h"
#include "../src/viewfile.h"
#include "../src/ifc_read.h"
#include "check.h"
#include "../session/link_cache.h" /* LINKF */

static const char *golden;

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static double dabs(double x) { return x < 0 ? -x : x; }

/* The decoded mesh `d` is `m` through the codec: same counts, indices and normals (xyz), every
 * point within the tolerance. */
static int same_mesh(const cc_mesh *m, const cc_mesh *d) {
    if (m->nv != d->nv || m->ni != d->ni || m->ne != d->ne) return 0;
    uint32_t nn = m->nn < m->ne ? m->nn : m->ne;
    if (d->nn != nn) return 0;
    if (m->ni && memcmp(m->idx, d->idx, m->ni * sizeof(uint32_t)) != 0) return 0;
    for (uint32_t i = 0; i < m->nv; i++)
        for (int k = 0; k < 3; k++)
            if (dabs((double)m->pos[i][k] - (double)d->pos[i][k]) > CC_MESHCODEC_TOLERANCE_MM) return 0;
    for (uint32_t i = 0; i < m->ne; i++)
        for (int k = 0; k < 6; k++)
            if (dabs((double)m->edges[i][k] - (double)d->edges[i][k]) > CC_MESHCODEC_TOLERANCE_MM) return 0;
    for (uint32_t i = 0; i < nn; i++)
        for (int k = 0; k < 8; k++)
            if ((k % 4 == 3 ? 0 : m->normals[i][k]) != d->normals[i][k]) return 0;
    return 1;
}

static int same_str(cc_str a, const char *p, size_t n) { return a.n == n && memcmp(a.p, p, n) == 0; }

/* A view file read back against the C scene it was made from. */
static void check_against_scene(const char *what, const cc_viewfile *v, const cc_model *m, const cc_scene *s) {
    char label[256];
    snprintf(label, sizeof label, "%s: %zu elements, %zu shapes (C scene: %zu, %zu)", what, v->nelements,
             v->nshapes, s->nelements, s->nshapes);
    check_true(label, v->nelements == s->nelements && v->nshapes == s->nshapes);
    if (v->nelements != s->nelements) return;
    int ok_meta = 1, ok_mesh = 1, ok_place = 1;
    for (size_t k = 0; k < s->nelements; k++) {
        const cc_scene_element *e = &s->elements[k];
        const cc_view_element *x = &v->elements[k];
        const cc_element *me = &m->elements[e->model_index];
        const cc_storey *st = cc_model_storey(m, &me->storey);
        const cc_material *mat = cc_model_material(m, &me->material);
        if (!same_str(x->global_id, e->id.c, 22) || !same_str(x->name, me->name.p, me->name.n) ||
            !same_str(x->class_name, e->class_name.p, e->class_name.n) ||
            !same_str(x->storey, st->name.p, st->name.n) || !same_str(x->material, mat->name.p, mat->name.n) ||
            x->category != e->category)
            ok_meta = 0;
        if ((x->shape >= 0) != (e->shape >= 0)) {
            ok_mesh = 0;
            continue;
        }
        if (!same_mesh(cc_scene_local_mesh(s, k), cc_viewfile_local_mesh(v, k))) ok_mesh = 0;
        if (e->shape >= 0 && memcmp(&e->to_scene, &x->to_scene, sizeof e->to_scene) != 0) {
            /* -0.0 is written as 0.0 */
            const float *a = &e->to_scene.x[0], *b = &x->to_scene.x[0];
            for (int i = 0; i < 12; i++)
                if (a[i] != b[i]) ok_place = 0;
        }
    }
    snprintf(label, sizeof label, "%s: ids, names, classes, storeys, materials, families", what);
    check_true(label, ok_meta);
    snprintf(label, sizeof label, "%s: meshes (indices, normals exact; points within 0.05 mm)", what);
    check_true(label, ok_mesh);
    snprintf(label, sizeof label, "%s: placements", what);
    check_true(label, ok_place);
    snprintf(label, sizeof label, "%s: origin", what);
    check_true(label, v->origin[0] == s->origin[0] && v->origin[1] == s->origin[1] && v->origin[2] == s->origin[2]);
    snprintf(label, sizeof label, "%s: storeys", what);
    check_true(label, v->nstoreys == m->nstoreys);
}

static void check_bytes(const char *what, const Buf *got, const char *want, size_t wn) {
    if (got->len == wn && memcmp(got->p, want, wn) == 0) {
        check_true(what, 1);
        return;
    }
    size_t i = 0;
    while (i < got->len && i < wn && got->p[i] == want[i]) i++;
    char label[256];
    snprintf(label, sizeof label, "%s (got %zu bytes, want %zu; first difference at %zu)", what, got->len, wn, i);
    check_true(label, 0);
}

static int read_ok(const void *p, size_t n) {
    cc_viewfile v;
    cc_error err;
    int rc = cc_viewfile_read(p, n, &v, &err);
    cc_viewfile_free(&v);
    return rc == 0;
}

static int read_err(const void *p, size_t n, const char *msg) {
    cc_viewfile v;
    cc_error err;
    int rc = cc_viewfile_read(p, n, &v, &err);
    cc_viewfile_free(&v);
    if (rc == 0) return 0;
    return !msg || strstr(err.msg, msg) != NULL;
}

static void one_model(const char *name) {
    char path[256], label[256];
    size_t n;
    snprintf(path, sizeof path, "%s.json", name);
    char *text = slurp(golden, path, &n);
    cc_model m;
    cc_error err;
    cc_scene s;
    if (cc_model_load(&m, text, n, &err)) {
        printf("%s does not load: %s\n", path, err.msg);
        failed++;
        free(text);
        return;
    }
    if (cc_scene_build(&s, &m, &err)) {
        printf("%s: scene: %s\n", name, err.msg);
        failed++;
        cc_scene_free(&s), cc_model_free(&m), free(text);
        return;
    }
    Buf raw, gz, inflated;
    buf_init(&raw), buf_init(&gz), buf_init(&inflated);

    double t0 = now_ms();
    int rc = cc_viewfile_write(&m, &s, 0, &raw);
    double t_raw = now_ms() - t0;
    snprintf(path, sizeof path, "view/%s.bimbleview.raw", name);
    size_t wn;
    char *want = slurp(golden, path, &wn);
    snprintf(label, sizeof label, "%s: raw stream == golden %s", name, path);
    check_true(label, rc == 0);
    check_bytes(label, &raw, want, wn);

    t0 = now_ms();
    rc = cc_viewfile_write(&m, &s, 1, &gz);
    double t_gz = now_ms() - t0;
    const char *why = NULL;
    snprintf(label, sizeof label, "%s: gzip output inflates to the golden raw stream", name);
    check_true(label, rc == 0 && cc_gunzip((const unsigned char *)gz.p, gz.len, 1u << 30, &inflated, &why) == 0);
    check_bytes(label, &inflated, want, wn);
    printf("  %s: raw %zu bytes, C gzip %zu bytes; write raw %.3f ms, write gzip %.3f ms\n", name, raw.len, gz.len,
           t_raw, t_gz);

    /* Rust's gzipped file and raw stream, read back against the C scene. */
    size_t rn;
    snprintf(path, sizeof path, "view/%s.bimbleview", name);
    char *rust = slurp(golden, path, &rn);
    cc_viewfile v;
    t0 = now_ms();
    rc = cc_viewfile_read((const uint8_t *)rust, rn, &v, &err);
    double t_read = now_ms() - t0;
    snprintf(label, sizeof label, "%s: Rust's %s reads (%s)", name, path, rc ? err.msg : "ok");
    check_true(label, rc == 0);
    if (rc == 0) {
        snprintf(label, sizeof label, "%s: Rust's .bimbleview", name);
        check_against_scene(label, &v, &m, &s);
        printf("  %s: read Rust's .bimbleview (%zu bytes gzip) %.3f ms\n", name, rn, t_read);
    }
    cc_viewfile_free(&v);
    rc = cc_viewfile_read((const uint8_t *)want, wn, &v, &err);
    snprintf(label, sizeof label, "%s: Rust's raw stream reads (%s)", name, rc ? err.msg : "ok");
    check_true(label, rc == 0);
    if (rc == 0) {
        snprintf(label, sizeof label, "%s: Rust's raw stream", name);
        check_against_scene(label, &v, &m, &s);
    }
    cc_viewfile_free(&v);
    rc = cc_viewfile_read((const uint8_t *)gz.p, gz.len, &v, &err);
    snprintf(label, sizeof label, "%s: C's gzip reads back", name);
    check_true(label, rc == 0);
    if (rc == 0) check_against_scene(label, &v, &m, &s);
    cc_viewfile_free(&v);

    snprintf(label, sizeof label, "%s: sniff raw and gzip", name);
    check_true(label, cc_viewfile_sniff((const uint8_t *)raw.p, raw.len) &&
                          cc_viewfile_sniff((const uint8_t *)gz.p, gz.len) &&
                          !cc_viewfile_sniff((const uint8_t *)"ISO-10303-21;", 13));

    free(rust), free(want);
    buf_free(&raw), buf_free(&gz), buf_free(&inflated);
    cc_scene_free(&s), cc_model_free(&m), free(text);
}

static void put_le(Buf *b, uint64_t v, int n) {
    for (int i = 0; i < n; i++) buf_putc(b, (char)(v >> (8 * i)));
}

/* FIXLIB: a hand-made view file of one element (a string table of one entry), with one thing
 * wrong at a time: the element's 4th string index, its shape index (kind 1, no shape stored), the
 * header's origin x (any f64). Each check is the only guard before an out-of-range read (the
 * table, v->shapes) or a non-finite origin. */
static void section(Buf *x, const char *kind, const Buf *payload) {
    buf_append(x, kind, 4);
    put_le(x, payload->len, 8);
    buf_append(x, payload->p, payload->len);
}
static void tiny_view(Buf *x, const char *magic12, uint64_t family_index, uint64_t shape_k, uint64_t origin_bits) {
    Buf s;
    buf_init(&s);
    buf_clear(x);
    buf_append(x, magic12, 12);
    cc_put_varint(&s, 1), cc_put_varint(&s, 0), cc_put_varint(&s, 0); /* 1 element, 0 shapes, 0 triangles */
    put_le(&s, origin_bits, 8), put_le(&s, 0, 8), put_le(&s, 0, 8);
    section(x, "HEAD", &s);
    buf_clear(&s);
    cc_put_varint(&s, 1), cc_put_varint(&s, 1), buf_putc(&s, 'x'); /* the table: "x" */
    for (int k = 0; k < 4; k++) cc_put_varint(&s, 0);             /* storeys, grids, densities, warnings */
    section(x, "META", &s);
    buf_clear(&s);
    cc_put_varint(&s, 0), cc_put_varint(&s, 1);                   /* 0 shapes, 1 element */
    cc_put_varint(&s, 0), cc_put_varint(&s, 0);                   /* global id, name: "" */
    cc_put_varint(&s, 0), cc_put_varint(&s, 0), cc_put_varint(&s, 0), cc_put_varint(&s, family_index);
    buf_putc(&s, 1), cc_put_varint(&s, shape_k);                  /* kind 1: shape k */
    for (int j = 0; j < 12; j++) put_le(&s, 0, 4);                /* the placement: zeros */
    section(x, "ELEM", &s);
    buf_free(&s);
}

static void index_checks(const char *raw) {
    Buf x;
    buf_init(&x);
    tiny_view(&x, raw, 0, 0, 0);
    check_true("tiny view: shape 0 of none: \"shape index\"", read_err(x.p, x.len, "shape index"));
    tiny_view(&x, raw, 1, 0, 0);
    check_true("tiny view: string 1 of a table of 1: \"string index\"", read_err(x.p, x.len, "string index"));
    tiny_view(&x, raw, 0, 0, 0x7ff8000000000000ull);
    check_true("tiny view: a NaN origin: \"number not finite\"", read_err(x.p, x.len, "number not finite"));
    tiny_view(&x, raw, 0, 0, 0xfff0000000000000ull);
    check_true("tiny view: a -inf origin: \"number not finite\"", read_err(x.p, x.len, "number not finite"));
    buf_free(&x);

    /* The mesh codec (raw mode, one vertex at 0): an index past the vertices, and an edge's
     * vertex past them (without the check: a read of pos[5] in a 2-vertex array). */
#define FIXLIB_V0 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 /* one raw vertex at (0, 0, 0) */
    static const uint8_t idx_bad[] = {'B', 'M', 'G', '1', 0, 1, 3, 0, 0, FIXLIB_V0, 0, 0, 2, 0};
    static const uint8_t idx_ok[] = {'B', 'M', 'G', '1', 0, 1, 3, 0, 0, FIXLIB_V0, 0, 0, 0, 0};
    static const uint8_t edge_bad[] = {'B', 'M', 'G', '1', 0, 1, 0, 1, 0, FIXLIB_V0, 0, 20, 0};
    static const uint8_t edge_neg[] = {'B', 'M', 'G', '1', 0, 1, 0, 1, 0, FIXLIB_V0, 0, 2, 0};
#undef FIXLIB_V0
    static const struct { const char *what; const uint8_t *p; size_t n; int rc; const char *why; } mc[] = {
        {"codec: index 1 of 1 vertex", idx_bad, sizeof idx_bad, CC_MC_INVALID, "index out of range"},
        {"codec: indices 0 0 0 of 1 vertex read", idx_ok, sizeof idx_ok, CC_MC_OK, ""},
        {"codec: an edge at vertex 5 of 1", edge_bad, sizeof edge_bad, CC_MC_INVALID, "edge vertex"},
        {"codec: an edge at vertex 0 then -1", edge_neg, sizeof edge_neg, CC_MC_INVALID, "edge vertex"},
    };
    Arena a;
    arena_init(&a, 0);
    for (size_t i = 0; i < sizeof mc / sizeof mc[0]; i++) {
        cc_mesh d;
        const char *why = "";
        int rc = cc_meshcodec_decode(mc[i].p, mc[i].n, &a, &d, &why);
        if (rc != mc[i].rc || strcmp(why, mc[i].why)) printf("  %s: rc %d \"%s\"\n", mc[i].what, rc, why);
        check_true(mc[i].what, rc == mc[i].rc && !strcmp(why, mc[i].why));
        arena_reset(&a);
    }
    arena_free(&a);
}

/* Damaged files: errors, never a crash. */
static void robustness(void) {
    size_t wn, gn;
    char *raw = slurp(golden, "view/frame.bimbleview.raw", &wn);
    char *gz = slurp(golden, "view/frame.bimbleview", &gn);
    /* first, flushed: without the string-index check the random damage below crashes the suite */
    index_checks(raw);
    fflush(stdout);

    int all = 1;
    for (size_t n = 0; n < wn; n++)
        if (read_ok(raw, n)) all = 0;
    check_true("every prefix of the raw stream is an error", all);
    check_true("the whole raw stream reads", read_ok(raw, wn));
    all = 1;
    for (size_t n = 0; n < gn; n++)
        if (read_ok(gz, n)) all = 0;
    check_true("every prefix of the gzip file is an error", all);
    check_true("an 11-byte prefix: not a view file", read_err(raw, 11, "not a .bimbleview file"));
    check_true("a cut section: cut short", read_err(raw, 30, "cut short"));
    check_true("an IFC file: not a view file", read_err("ISO-10303-21;", 13, "not a .bimbleview file"));
    check_true("empty: not a view file", read_err("", 0, "not a .bimbleview file"));

    /* Flipped bytes (4 per try, xorshift), raw and gzip: any result but a crash. */
    uint64_t seed = 0x2545f4914f6cdd1dull;
    char *b = malloc(wn > gn ? wn : gn);
    int nerr = 0;
    for (int t = 0; t < 3000; t++) {
        const char *src = t % 3 == 2 ? gz : raw;
        size_t n = t % 3 == 2 ? gn : wn;
        memcpy(b, src, n);
        for (int j = 0; j < 4; j++) {
            seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
            b[seed % n] ^= (char)((seed >> 32) | 1);
        }
        nerr += !read_ok(b, n);
    }
    printf("  3000 damaged files: %d errors\n", nerr);
    check_true("3000 damaged files: no crash, most are errors", nerr > 1500);
    free(b);

    Buf x;
    buf_init(&x);
    /* A section that says it is huge with nothing behind it: cut short; over 1 GiB: too large. */
    buf_append(&x, raw, 12);
    buf_append(&x, "HEAD", 4);
    put_le(&x, (uint64_t)1 << 30, 8);
    check_true("a 1 GiB section with nothing behind: cut short", read_err(x.p, x.len, "cut short"));
    buf_clear(&x);
    buf_append(&x, raw, 12);
    buf_append(&x, "HEAD", 4);
    put_le(&x, UINT64_MAX, 8);
    check_true("a section over 1 GiB: too large", read_err(x.p, x.len, "section too large"));
    /* Huge varints: 11 continuation bytes; a count of 2^63 elements. */
    buf_clear(&x);
    buf_append(&x, raw, 12);
    buf_append(&x, "HEAD", 4);
    put_le(&x, 11, 8);
    buf_fill(&x, (char)0xff, 11);
    check_true("a varint of 11 bytes: error", read_err(x.p, x.len, "varint too long"));
    buf_clear(&x);
    buf_append(&x, raw, wn);
    {   /* an ELEM claiming 2^63 shapes */
        Buf p;
        buf_init(&p);
        cc_put_varint(&p, (uint64_t)1 << 63);
        buf_append(&x, "ELEM", 4);
        put_le(&x, p.len, 8);
        buf_append(&x, p.p, p.len);
        buf_free(&p);
    }
    check_true("an ELEM of 2^63 shapes: cut short", read_err(x.p, x.len, "cut short"));
    /* Version 2 says so; an unknown section is skipped; many (deep) unknown sections too. */
    buf_clear(&x);
    buf_append(&x, raw, wn);
    x.p[8] = 2;
    check_true("version 2: update Bimble", read_err(x.p, x.len, "format version 2; this Bimble reads version 1"));
    buf_clear(&x);
    buf_append(&x, raw, wn);
    buf_append(&x, "PSET", 4);
    put_le(&x, 5, 8);
    buf_append(&x, "later", 5);
    check_true("an unknown section is skipped", read_ok(x.p, x.len));
    for (int i = 0; i < 200000; i++) buf_append(&x, "PSET\0\0\0\0\0\0\0\0", 12);
    check_true("200,000 empty unknown sections are skipped", read_ok(x.p, x.len));
    buf_clear(&x);
    buf_append(&x, raw, 12);
    buf_append(&x, "PSET\0\0\0\0\0\0\0\0", 12);
    buf_append(&x, raw + 12, wn - 12);
    check_true("an unknown section before HEAD: no header first", read_err(x.p, x.len, "no header first"));
    /* HEAD repeated; trailing bytes in a section; HEAD only (no elements but 72 promised). */
    buf_clear(&x);
    buf_append(&x, raw, wn);
    {
        size_t hl = 12 + 12 + raw[16]; /* HEAD's payload length fits a byte here */
        buf_append(&x, raw + 12, hl - 12);
    }
    check_true("HEAD repeated: error", read_err(x.p, x.len, "section repeated"));
    buf_clear(&x);
    {
        size_t hl = 12 + 12 + (unsigned char)raw[16];
        buf_append(&x, raw, hl);
    }
    check_true("HEAD alone: cut short (elements promised)", read_err(x.p, x.len, "cut short"));
    buf_free(&x);

    /* The codec alone: every prefix of a mesh is an error. */
    {
        size_t n;
        char *text = slurp(golden, "frame2.json", &n);
        cc_model m;
        cc_scene s;
        cc_error err;
        if (cc_model_load(&m, text, n, &err) == 0 && cc_scene_build(&s, &m, &err) == 0 && s.nelements) {
            Buf mb;
            buf_init(&mb);
            cc_meshcodec_encode(cc_scene_local_mesh(&s, 0), &mb);
            Arena a;
            arena_init(&a, 0);
            int ok = 1;
            for (size_t k = 0; k < mb.len; k++) {
                cc_mesh d;
                const char *why;
                if (cc_meshcodec_decode((const uint8_t *)mb.p, k, &a, &d, &why) == 0) ok = 0;
                arena_reset(&a);
            }
            cc_mesh d;
            const char *why;
            int rt = cc_meshcodec_decode((const uint8_t *)mb.p, mb.len, &a, &d, &why) == 0 &&
                     same_mesh(cc_scene_local_mesh(&s, 0), &d);
            check_true("codec: every prefix of a mesh is an error", ok);
            check_true("codec: round trip", rt);
            arena_free(&a);
            buf_free(&mb);
            cc_scene_free(&s);
        } else {
            check_true("codec: frame2 loads", 0);
        }
        cc_model_free(&m);
        free(text);
    }
    {   /* raw mode: a non-finite coordinate */
        float pos[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1e30f, 0}};
        uint32_t u = 0x7fc00000u;
        memcpy(&pos[2][2], &u, 4);
        uint32_t idx[3] = {0, 1, 2};
        cc_mesh mm = {pos, 3, idx, 3, NULL, 0, NULL, 0};
        Buf mb;
        buf_init(&mb);
        cc_meshcodec_encode(&mm, &mb);
        Arena a;
        arena_init(&a, 0);
        cc_mesh d;
        const char *why;
        int rc = cc_meshcodec_decode((const uint8_t *)mb.p, mb.len, &a, &d, &why);
        check_true("codec: a NaN coordinate gives a raw mesh, read back bit for bit",
                   mb.len > 4 && mb.p[4] == 0 && rc == 0 && memcmp(d.pos, pos, sizeof pos) == 0);
        arena_free(&a);
        buf_free(&mb);
    }
    free(raw), free(gz);
}

static void large(void) {
    FILE *f = fopen("out/t16-large-20000.bimble", "rb");
    if (!f) {
        printf("  (no out/t16-large-20000.bimble: python3 bench/large_model.py makes it)\n");
        return;
    }
    fclose(f);
    size_t n;
    char *text = slurp("out", "t16-large-20000.bimble", &n);
    cc_model m;
    cc_scene s;
    cc_error err;
    if (cc_model_load(&m, text, n, &err) || cc_scene_build(&s, &m, &err)) {
        printf("  large model: %s\n", err.msg);
        free(text);
        return;
    }
    Buf raw, gz;
    buf_init(&raw), buf_init(&gz);
    double t0 = now_ms();
    int rc = cc_viewfile_write(&m, &s, 0, &raw);
    double t1 = now_ms();
    rc |= cc_viewfile_write(&m, &s, 1, &gz);
    double t2 = now_ms();
    cc_viewfile v;
    int rr = cc_viewfile_read((const uint8_t *)gz.p, gz.len, &v, &err);
    double t3 = now_ms();
    check_true("20k model: written and read back", rc == 0 && rr == 0 && v.nelements == s.nelements);
    if (rr == 0) check_against_scene("20k model", &v, &m, &s);
    printf("  20k model (%zu elements, %zu shapes): raw %zu bytes, gzip %zu bytes; write raw %.1f ms, "
           "write gzip %.1f ms, read gzip %.1f ms\n",
           s.nelements, s.nshapes, raw.len, gz.len, t1 - t0, t2 - t1, t3 - t2);
    cc_viewfile_free(&v);
    buf_free(&raw), buf_free(&gz);
    cc_scene_free(&s), cc_model_free(&m), free(text);
}

/* ---- sharing a read-only scene (cc_viewfile_write_meta) ---- */

/* session.c view_scene: the cc_scene over a view file (meshes and shapes borrowed). */
static int scene_of_file(cc_scene *sc, const cc_viewfile *v) {
    memset(sc, 0, sizeof *sc);
    arena_init(&sc->arena, 0);
    memcpy(sc->origin, v->origin, sizeof sc->origin);
    sc->shapes = v->shapes, sc->nshapes = v->nshapes;
    sc->warnings = v->warnings, sc->nwarnings = v->nwarnings;
    sc->elements = arena_alloc(&sc->arena, (v->nelements + 1) * sizeof *sc->elements, 8);
    if (!sc->elements) return -1;
    for (size_t k = 0; k < v->nelements; k++) {
        const cc_view_element *x = &v->elements[k];
        cc_scene_element *e = &sc->elements[k];
        if (x->global_id.n == 22) memcpy(e->id.c, x->global_id.p, 22);
        e->cls = CC_IFC_PROXY;
        e->class_name = x->class_name;
        e->category = x->category;
        e->model_index = k;
        e->shape = x->shape;
        e->mesh = x->mesh;
        e->to_scene = x->to_scene;
    }
    sc->nelements = v->nelements;
    return 0;
}

/* An opened view file shared again: its raw bytes (Rust: read -> Scene -> write). */
static void reshare_file(const char *what, const uint8_t *p, size_t n, const char *want, size_t wn) {
    char label[256];
    cc_viewfile v;
    cc_error err;
    cc_scene sc;
    cc_viewfile_meta mt;
    Buf raw, gz, inflated;
    buf_init(&raw), buf_init(&gz), buf_init(&inflated);
    memset(&sc, 0, sizeof sc);
    cc_viewfile_meta_init(&mt);
    int rc = cc_viewfile_read(p, n, &v, &err);
    snprintf(label, sizeof label, "reshare %s: reads (%s)", what, rc ? err.msg : "ok");
    check_true(label, rc == 0);
    if (rc == 0) {
        cc_viewfile_meta_free(&mt);
        rc = scene_of_file(&sc, &v) || cc_viewfile_meta_from_file(&mt, &v) ||
             cc_viewfile_write_meta(&mt, &sc, 0, &raw) || cc_viewfile_write_meta(&mt, &sc, 1, &gz);
        snprintf(label, sizeof label, "reshare %s: written", what);
        check_true(label, rc == 0);
        snprintf(label, sizeof label, "reshare %s: raw stream == the file's", what);
        check_bytes(label, &raw, want, wn);
        const char *why = NULL;
        snprintf(label, sizeof label, "reshare %s: gzip inflates to the file's raw stream", what);
        check_true(label, cc_gunzip((const unsigned char *)gz.p, gz.len, 1u << 30, &inflated, &why) == 0);
        check_bytes(label, &inflated, want, wn);
    }
    cc_viewfile_meta_free(&mt);
    cc_scene_free(&sc);
    cc_viewfile_free(&v);
    buf_free(&raw), buf_free(&gz), buf_free(&inflated);
}

static int eq_str(cc_str a, cc_str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }

/* A view file read back against the IFC read scene it was shared from. */
static void check_against_ifc(const char *what, const cc_viewfile *v, const cc_ifc_scene *is, const cc_viewfile_meta *mt) {
    char label[256];
    const cc_scene *s = &is->scene;
    snprintf(label, sizeof label, "%s: %zu elements, %zu shapes (read scene: %zu, %zu)", what, v->nelements,
             v->nshapes, s->nelements, s->nshapes);
    check_true(label, v->nelements == s->nelements && v->nshapes == s->nshapes);
    if (v->nelements != s->nelements) return;
    int ok_meta = 1, ok_mesh = 1, ok_place = 1;
    for (size_t k = 0; k < s->nelements; k++) {
        const cc_scene_element *e = &s->elements[k];
        const cc_view_element *x = &v->elements[k];
        const cc_ifc_info *in = &is->info[e->model_index];
        if (!eq_str(x->global_id, in->global_id) || !eq_str(x->name, in->name) ||
            !eq_str(x->class_name, e->class_name) || !eq_str(x->class_name, in->class_name) ||
            !eq_str(x->storey, in->storey) || !eq_str(x->material, in->material) || x->category != e->category)
            ok_meta = 0;
        if ((x->shape >= 0) != (e->shape >= 0)) {
            ok_mesh = 0;
            continue;
        }
        if (!same_mesh(cc_scene_local_mesh(s, k), cc_viewfile_local_mesh(v, k))) ok_mesh = 0;
        if (e->shape >= 0) {
            const float *a = &e->to_scene.x[0], *b = &x->to_scene.x[0];
            for (int i = 0; i < 12; i++)
                if (a[i] != b[i]) ok_place = 0;
        }
    }
    snprintf(label, sizeof label, "%s: ids, names, classes, storeys, materials, families", what);
    check_true(label, ok_meta);
    snprintf(label, sizeof label, "%s: meshes (indices, normals exact; points within 0.05 mm)", what);
    check_true(label, ok_mesh);
    snprintf(label, sizeof label, "%s: placements", what);
    check_true(label, ok_place);
    snprintf(label, sizeof label, "%s: origin", what);
    check_true(label, v->origin[0] == s->origin[0] && v->origin[1] == s->origin[1] && v->origin[2] == s->origin[2]);
    int ok = v->nstoreys == mt->nstoreys && v->ngrids == mt->ngrids && v->nwarnings == s->nwarnings &&
             v->ndensities == 0 && v->has_georef == mt->has_georef;
    for (size_t i = 0; ok && i < v->nstoreys; i++)
        ok = eq_str(v->storeys[i].name, mt->storeys[i].name) && eq_str(v->storeys[i].id, mt->storeys[i].id) &&
             v->storeys[i].elevation == mt->storeys[i].elevation;
    for (size_t i = 0; ok && i < v->ngrids; i++)
        ok = eq_str(v->grids[i].tag, mt->grids[i].tag) && !memcmp(v->grids[i].start, mt->grids[i].start, 16) &&
             !memcmp(v->grids[i].end, mt->grids[i].end, 16);
    for (size_t i = 0; ok && i < v->nwarnings; i++) ok = eq_str(v->warnings[i], s->warnings[i]);
    if (ok && v->has_georef) ok = eq_str(v->georef, mt->georef);
    snprintf(label, sizeof label, "%s: storeys, grids, warnings, georef, no densities", what);
    check_true(label, ok);
}

/* Read an IFC as it is, share it, read it back; share the read-back file again: same bytes.
 * `extra`: add two grids and a georeference to the meta (no fixture has them) to cover GRID
 * and GEOR. Checked C -> C: Rust has no golden view file of a read scene. */
static void share_ifc(const char *name, const char *file, size_t want_elements, int extra) {
    char label[256];
    size_t n;
    char *text = slurp(golden, file, &n);
    cc_ifc_scene is;
    cc_error err;
    int rc = cc_ifc_read(text, n, &is, &err);
    snprintf(label, sizeof label, "share %s: reads (%s)", name, rc ? err.msg : "ok");
    check_true(label, rc == 0);
    cc_viewfile_meta mt;
    cc_viewfile_meta_init(&mt);
    Buf raw, gz;
    buf_init(&raw), buf_init(&gz);
    if (rc == 0) {
        cc_viewfile_meta_free(&mt);
        rc = cc_ifc_scene_meta(&is, &mt);
        static const cc_view_grid G2[2] = {{{"A", 1}, {-1000.5, 0}, {9000, 0}}, {{"1", 1}, {0, -250}, {0, 7000.25}}};
        static const char GEO[] = "{\"rotation_deg\":12.5,\"crs\":\"EPSG:2056\",\"east_m\":2600000.0}";
        if (extra) {
            mt.grids = G2, mt.ngrids = 2;
            mt.has_georef = 1, mt.georef.p = GEO, mt.georef.n = sizeof GEO - 1;
        }
        if (rc == 0) rc = cc_viewfile_write_meta(&mt, &is.scene, 0, &raw);
        if (rc == 0) rc = cc_viewfile_write_meta(&mt, &is.scene, 1, &gz);
        snprintf(label, sizeof label, "share %s: written (raw %zu bytes, gzip %zu)", name, raw.len, gz.len);
        check_true(label, rc == 0);
        snprintf(label, sizeof label, "share %s: %zu elements", name, want_elements);
        check_true(label, is.scene.nelements == want_elements);
    }
    if (rc == 0) {
        cc_viewfile v;
        rc = cc_viewfile_read((const uint8_t *)gz.p, gz.len, &v, &err);
        snprintf(label, sizeof label, "share %s: the gzip file reads back (%s)", name, rc ? err.msg : "ok");
        check_true(label, rc == 0);
        if (rc == 0) {
            snprintf(label, sizeof label, "share %s%s", name, extra ? " + grids, georef" : "");
            check_against_ifc(label, &v, &is, &mt);
        }
        cc_viewfile_free(&v);
        snprintf(label, sizeof label, "%s%s (shared from IFC)", name, extra ? " + grids, georef" : "");
        reshare_file(label, (const uint8_t *)gz.p, gz.len, raw.p, raw.len);
    }
    cc_viewfile_meta_free(&mt);
    buf_free(&raw), buf_free(&gz);
    cc_ifc_scene_free(&is);
    free(text);
}

/* The model wrapper equals the meta writer fed cc_viewfile_meta_from_model (same bytes). */
static void reshare_golden(const char *name) {
    char path[256];
    size_t n, wn;
    snprintf(path, sizeof path, "view/%s.bimbleview", name);
    char *gz = slurp(golden, path, &n);
    snprintf(path, sizeof path, "view/%s.bimbleview.raw", name);
    char *want = slurp(golden, path, &wn);
    snprintf(path, sizeof path, "golden/view/%s.bimbleview (Rust's)", name);
    reshare_file(path, (const uint8_t *)gz, n, want, wn);
    snprintf(path, sizeof path, "golden/view/%s.bimbleview.raw (Rust's)", name);
    reshare_file(path, (const uint8_t *)want, wn, want, wn);
    free(gz), free(want);
}

static void share_read_only(void) {
    reshare_golden("frame");
    reshare_golden("frame2");
    share_ifc("mapped_item", "ifc/fixtures/mapped_item.ifc", 5, 0);
    share_ifc("mapped_item", "ifc/fixtures/mapped_item.ifc", 5, 1);
    share_ifc("frame.ifc", "ifc/frame.ifc", 72, 0);
    share_ifc("triangulated", "ifc/fixtures/triangulated.ifc", 1, 0);
    share_ifc("chain18", "../test/fixtures/chain18.ifc", 0, 0);
}

/* LINKF: the SRCE section of a link cache (viewfile.h cc_viewfile_append_source, link_cache.h):
 * kept by this reader, untrusted (not JSON, not UTF-8, cut, repeated: errors or skipped, never a
 * crash), and the matching rule of the cache against its IFC. */
static void link_cache_source(void) {
    size_t wn;
    char *raw = slurp(golden, "view/frame.bimbleview.raw", &wn);
    char src[512];
    static const char head[] = "ISO-10303-21;\nHEADER;", tail[] = "ENDSEC;\nEND-ISO-10303-21;\n";
    check_true("source text", cc_link_cache_source(68059, 1795696594, head, sizeof head - 1, tail, sizeof tail - 1, src, sizeof src) == 0);
    Buf b;
    buf_init(&b);
    buf_append(&b, raw, wn);
    check_true("SRCE appended", cc_viewfile_append_source(&b, src, strlen(src)) == 0);
    cc_viewfile v;
    cc_error err;
    int rc = cc_viewfile_read((const uint8_t *)b.p, b.len, &v, &err);
    check_true("a cache with SRCE reads, its source kept", rc == 0 && v.has_source && v.source.n == strlen(src) && !memcmp(v.source.p, src, v.source.n));
    cc_viewfile_free(&v);
    check_true("without SRCE: no source (a shared view file)", cc_viewfile_read((const uint8_t *)raw, wn, &v, &err) == 0 && !v.has_source);
    cc_viewfile_free(&v);
    const char *ends = strstr(src, "\"ends\":\"") + 8;
    char hexs[65];
    memcpy(hexs, ends, 64), hexs[64] = 0;
    check_true("matches: same size, time, ends", cc_link_cache_matches(src, strlen(src), 68059, 1795696594, hexs, 1));
    check_true("another size: no", !cc_link_cache_matches(src, strlen(src), 68060, 1795696594, hexs, 1));
    check_true("another time: no", !cc_link_cache_matches(src, strlen(src), 68059, 1795696595, hexs, 1));
    hexs[0] = hexs[0] == '0' ? '1' : '0';
    check_true("other ends: no", !cc_link_cache_matches(src, strlen(src), 68059, 1795696594, hexs, 1));
    check_true("ends unknown (the IFC unreadable): size and time", cc_link_cache_matches(src, strlen(src), 68059, 1795696594, "", 0));
    check_true("not JSON: no", !cc_link_cache_matches("{bytes", 6, 68059, 1795696594, "", 0));
    static const char other[] = "{\"bytes\":1,\"ends\":\"\",\"format\":\"x\",\"mtime_s\":1}";
    check_true("another format: no", !cc_link_cache_matches(other, sizeof other - 1, 1, 1, "", 0));
    /* hostile SRCE sections */
    static const char *const bad[] = {"not json", "{\"a\":", "\xff\xfe"};
    for (int i = 0; i < 3; i++) {
        buf_clear(&b);
        buf_append(&b, raw, wn);
        cc_viewfile_append_source(&b, bad[i], strlen(bad[i]));
        check_true("a damaged SRCE: an error, no crash", read_err(b.p, b.len, "damaged"));
    }
    buf_clear(&b);
    buf_append(&b, raw, wn);
    cc_viewfile_append_source(&b, src, strlen(src));
    cc_viewfile_append_source(&b, "{}", 2);
    check_true("a second SRCE: skipped (the first kept)", cc_viewfile_read((const uint8_t *)b.p, b.len, &v, &err) == 0 && v.source.n == strlen(src));
    cc_viewfile_free(&v);
    int all = 1; /* every prefix that cuts into SRCE: an error */
    buf_clear(&b);
    buf_append(&b, raw, wn);
    cc_viewfile_append_source(&b, src, strlen(src));
    for (size_t n = wn + 1; n < b.len; n++)
        if (read_ok(b.p, n)) all = 0;
    check_true("every cut SRCE: an error", all);
    buf_free(&b);
    free(raw);
}

int main(int argc, char **argv) {
    static char dir[4096];
    snprintf(dir, sizeof dir, "%s/..", argc > 1 ? argv[1] : "ccore/golden/floats");
    golden = dir;
    one_model("frame");
    one_model("frame2");
    robustness();
    share_read_only();
    large();
    link_cache_source(); /* LINKF */
    return report("viewfile");
}
