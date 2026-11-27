/* BMSH version 3 (scene_bmsh.h): shapes once, a placement per element (ticket 3D1, ADR 0078). */
#include <string.h>

#include "scene_bmsh.h"
#include "meshcodec.h"

static void put_u32(Buf *b, uint32_t v) { buf_append(b, &v, 4); }

static int fail(cc_error *err, const char *msg) {
    size_t n = strlen(msg);
    err->offset = CC_NO_OFFSET;
    if (n >= sizeof err->msg) n = sizeof err->msg - 1;
    memcpy(err->msg, msg, n);
    err->msg[n] = 0;
    return -1;
}

/* The mesh element k's row points at: its own, or its shape. */
static const cc_mesh *row_mesh(const cc_scene *s, size_t k) {
    const cc_scene_element *e = &s->elements[k];
    return e->shape >= 0 ? &s->shapes[e->shape] : &e->mesh;
}

int cc_scene_bmsh3(const cc_scene *s, cc_bmsh_id_fn id, const void *ctx, uint32_t rev, Buf *out, cc_error *err) {
    size_t ne = s->nelements, nsh = s->nshapes;
    /* Per element its block (first vertex, index, segment); per shape the same, once (UINT32_MAX:
     * not written yet). The blocks in first-use order: `order` holds element indices (an own mesh) or
     * ne + shape (a shape). */
    Arena tmp;
    arena_init(&tmp, 0);
    int rc = -1;
    uint32_t(*first)[3] = arena_alloc(&tmp, (ne + nsh + 1) * sizeof *first, 4);
    size_t *order = arena_alloc(&tmp, (ne + nsh + 1) * sizeof *order, 8);
    if (!first || !order) { fail(err, "out of memory"); goto done; }
    for (size_t k = 0; k < nsh; k++) first[ne + k][0] = UINT32_MAX;
    uint64_t nv = 0, ni = 0, ns = 0;
    size_t nb = 0;
    for (size_t k = 0; k < ne; k++) {
        const cc_scene_element *e = &s->elements[k];
        if (e->shape >= 0 && (size_t)e->shape >= nsh) { fail(err, "shape out of range"); goto done; }
        size_t b = e->shape >= 0 ? ne + (size_t)e->shape : k;
        if (b >= ne && first[b][0] != UINT32_MAX) {
            memcpy(first[k], first[b], sizeof first[k]);
            continue;
        }
        const cc_mesh *m = row_mesh(s, k);
        if (nv > UINT32_MAX || ni > UINT32_MAX || ns > UINT32_MAX) break;
        first[b][0] = (uint32_t)nv, first[b][1] = (uint32_t)ni, first[b][2] = (uint32_t)ns;
        if (b != k) memcpy(first[k], first[b], sizeof first[k]);
        order[nb++] = b;
        nv += m->nv, ni += m->ni, ns += m->ne;
    }
    if (ne > UINT32_MAX || nv > UINT32_MAX || ni > UINT32_MAX || ns > UINT32_MAX) {
        fail(err, "scene too large for BMSH");
        goto done;
    }
    if (buf_reserve(out, 52 + 48 * ne + 12 * nv + 4 * ni + 32 * ns + 52 * ne) != 0) { fail(err, "out of memory"); goto done; }
    buf_append(out, "BMSH", 4);
    put_u32(out, CC_BMSH_SHAPES_VERSION);
    put_u32(out, rev);
    put_u32(out, (uint32_t)ne), put_u32(out, (uint32_t)nv), put_u32(out, (uint32_t)ni), put_u32(out, (uint32_t)ns);
    buf_append(out, s->origin, 24);
    for (size_t k = 0; k < ne; k++) {
        const cc_scene_element *e = &s->elements[k];
        const cc_mesh *m = row_mesh(s, k);
        char t[22];
        if (id) id(ctx, s, k, t);
        else memcpy(t, e->id.c, 22);
        buf_append(out, t, 22);
        buf_putc(out, (char)(unsigned char)e->category);
        buf_putc(out, 0);
        put_u32(out, first[k][0]), put_u32(out, m->nv), put_u32(out, first[k][1]), put_u32(out, m->ni);
        put_u32(out, first[k][2]), put_u32(out, m->ne);
    }
#define BLOCK_MESH(b) ((b) >= ne ? &s->shapes[(b) - ne] : &s->elements[(b)].mesh)
    for (size_t j = 0; j < nb; j++) { /* SIZE3: a mesh with counts and no arrays (a scene not decoded) is refused */
        const cc_mesh *m = BLOCK_MESH(order[j]);
        if ((m->nv && !m->pos) || (m->ni && !m->idx) || (m->ne && !m->edges) || (m->nn && !m->normals)) { fail(err, "mesh arrays missing"); goto done; }
    }
    for (size_t j = 0; j < nb; j++) {
        const cc_mesh *m = BLOCK_MESH(order[j]);
        buf_append(out, m->pos, (size_t)m->nv * 12);
    }
    for (size_t j = 0; j < nb; j++) {
        const cc_mesh *m = BLOCK_MESH(order[j]);
        uint32_t fv = first[order[j]][0];
        for (uint32_t i = 0; i < m->ni; i++) {
            if (m->idx[i] >= m->nv) { fail(err, "mesh index out of range"); goto done; }
            put_u32(out, fv + m->idx[i]);
        }
    }
    for (size_t j = 0; j < nb; j++) {
        const cc_mesh *m = BLOCK_MESH(order[j]);
        buf_append(out, m->edges, (size_t)m->ne * 24);
    }
    static const int8_t zero[8];
    for (size_t j = 0; j < nb; j++) {
        const cc_mesh *m = BLOCK_MESH(order[j]);
        for (uint32_t i = 0; i < m->ne; i++) buf_append(out, i < m->nn ? m->normals[i] : zero, 8);
    }
#undef BLOCK_MESH
    static const float z12[12];
    for (size_t k = 0; k < ne; k++) {
        const cc_scene_element *e = &s->elements[k];
        put_u32(out, e->shape >= 0 ? (uint32_t)e->shape + 1u : 0u);
        if (e->shape >= 0) {
            buf_append(out, e->to_scene.x, 12), buf_append(out, e->to_scene.y, 12);
            buf_append(out, e->to_scene.z, 12), buf_append(out, e->to_scene.t, 12);
        } else buf_append(out, z12, 48);
    }
    if (out->oom) { fail(err, "out of memory"); goto done; }
    rc = 0;
done:
    arena_free(&tmp);
    return rc;
}

/* ---- SIZE2: version 4, the blocks in the mesh codec (scene_bmsh.h) ---- */

static void put_u32_at(Buf *b, size_t at, uint32_t v) { memcpy(b->p + at, &v, 4); }

int cc_scene_bmsh4(const cc_scene *s, cc_bmsh_id_fn id, const void *ctx, uint32_t rev, Buf *out, cc_error *err) {
    return cc_scene_bmsh4_blocks(s, id, ctx, rev, NULL, NULL, out, err);
}

int cc_scene_bmsh4_blocks(const cc_scene *s, cc_bmsh_id_fn id, const void *ctx, uint32_t rev, cc_bmsh_block_fn blk, const void *bctx,
                          Buf *out, cc_error *err) {
    size_t ne = s->nelements, nsh = s->nshapes;
    Arena tmp;
    arena_init(&tmp, 0);
    int rc = -1;
    uint32_t(*first)[3] = arena_alloc(&tmp, (ne + nsh + 1) * sizeof *first, 4);
    size_t *order = arena_alloc(&tmp, (ne + nsh + 1) * sizeof *order, 8);
    if (!first || !order) { fail(err, "out of memory"); goto done; }
    for (size_t k = 0; k < nsh; k++) first[ne + k][0] = UINT32_MAX;
    uint64_t nv = 0, ni = 0, ns = 0;
    size_t nb = 0;
    for (size_t k = 0; k < ne; k++) { /* the blocks in version 3's first-use order */
        const cc_scene_element *e = &s->elements[k];
        if (e->shape >= 0 && (size_t)e->shape >= nsh) { fail(err, "shape out of range"); goto done; }
        size_t b = e->shape >= 0 ? ne + (size_t)e->shape : k;
        if (b >= ne && first[b][0] != UINT32_MAX) {
            memcpy(first[k], first[b], sizeof first[k]);
            continue;
        }
        const cc_mesh *m = row_mesh(s, k);
        if (nv > UINT32_MAX || ni > UINT32_MAX || ns > UINT32_MAX) break;
        first[b][0] = (uint32_t)nv, first[b][1] = (uint32_t)ni, first[b][2] = (uint32_t)ns;
        if (b != k) memcpy(first[k], first[b], sizeof first[k]);
        order[nb++] = b;
        nv += m->nv, ni += m->ni, ns += m->ne;
    }
    if (ne > UINT32_MAX || nv > UINT32_MAX || ni > UINT32_MAX || ns > UINT32_MAX || nb > UINT32_MAX) {
        fail(err, "scene too large for BMSH");
        goto done;
    }
    buf_append(out, "BMSH", 4);
    put_u32(out, CC_BMSH_COMPACT_VERSION);
    put_u32(out, rev);
    put_u32(out, (uint32_t)ne), put_u32(out, (uint32_t)nv), put_u32(out, (uint32_t)ni), put_u32(out, (uint32_t)ns);
    buf_append(out, s->origin, 24);
    for (size_t k = 0; k < ne; k++) {
        const cc_scene_element *e = &s->elements[k];
        const cc_mesh *m = row_mesh(s, k);
        char t[22];
        if (id) id(ctx, s, k, t);
        else memcpy(t, e->id.c, 22);
        buf_append(out, t, 22);
        buf_putc(out, (char)(unsigned char)e->category);
        buf_putc(out, 0);
        put_u32(out, first[k][0]), put_u32(out, m->nv), put_u32(out, first[k][1]), put_u32(out, m->ni);
        put_u32(out, first[k][2]), put_u32(out, m->ne);
    }
    for (size_t k = 0; k < ne; k++) put_u32(out, s->elements[k].shape >= 0 ? (uint32_t)s->elements[k].shape + 1u : 0u);
    for (size_t k = 0; k < ne; k++) {
        const cc_scene_element *e = &s->elements[k];
        if (e->shape < 0) continue;
        buf_append(out, e->to_scene.x, 12), buf_append(out, e->to_scene.y, 12);
        buf_append(out, e->to_scene.z, 12), buf_append(out, e->to_scene.t, 12);
    }
    put_u32(out, (uint32_t)nb);
    for (size_t j = 0; j < nb && !out->oom; j++) {
        const cc_mesh *m = order[j] >= ne ? &s->shapes[order[j] - ne] : &s->elements[order[j]].mesh;
        size_t at = out->len;
        if (blk) { /* SIZE3: the block's codec bytes as stored (a mapped cache), not encoded again */
            put_u32(out, 0);
            if (blk(bctx, order[j] >= ne, order[j] >= ne ? order[j] - ne : order[j], out)) { fail(err, "mesh bytes not available"); goto done; }
        } else {
            if ((m->nv && !m->pos) || (m->ni && !m->idx) || (m->ne && !m->edges) || (m->nn && !m->normals)) { fail(err, "mesh arrays missing"); goto done; } /* SIZE3 */
            for (uint32_t i = 0; i < m->ni; i++)
                if (m->idx[i] >= m->nv) { fail(err, "mesh index out of range"); goto done; }
            put_u32(out, 0);
            if (cc_meshcodec_encode(m, out)) break;
        }
        if (out->len - at - 4 > UINT32_MAX) { fail(err, "block too large for BMSH"); goto done; }
        if (!out->oom) put_u32_at(out, at, (uint32_t)(out->len - at - 4));
    }
    if (out->oom) { fail(err, "out of memory"); goto done; }
    rc = 0;
done:
    arena_free(&tmp);
    return rc;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int cc_bmsh4_to_bmsh3(const uint8_t *p, size_t n, size_t max_out, Buf *out, cc_error *err) {
    if (n < 52 || memcmp(p, "BMSH", 4) || rd32(p + 4) != CC_BMSH_COMPACT_VERSION) return fail(err, "not BMSH version 4");
    uint64_t ne = rd32(p + 12), nv = rd32(p + 16), ni = rd32(p + 20), ns = rd32(p + 24);
    uint64_t rows = 52 + 52 * ne, nplaced = 0; /* the rows and the shapes; then the placements */
    if (rows + 4 > n) return fail(err, "BMSH 4: cut short");
    for (uint64_t k = 0; k < ne; k++) nplaced += rd32(p + 52 + 48 * ne + 4 * k) != 0;
    rows += 48 * nplaced;
    if (rows + 4 > n) return fail(err, "BMSH 4: cut short");
    uint64_t want = 52 + 48 * ne + 12 * nv + 4 * ni + 32 * ns + 52 * ne;
    if (want > max_out) return fail(err, "BMSH 4: too large");
    size_t base = out->len;
    if (buf_reserve(out, out->len + (size_t)want)) return fail(err, "out of memory");
    buf_append(out, p, 52);
    put_u32_at(out, base + 4, CC_BMSH_SHAPES_VERSION);
    buf_append(out, p + 52, (size_t)(48 * ne));
    size_t P = base + 52 + (size_t)(48 * ne), I = P + (size_t)(12 * nv), E = I + (size_t)(4 * ni), N = E + (size_t)(24 * ns),
           T = N + (size_t)(8 * ns);
    /* the blocks' room, zeroed (an unknown normal is 0), then the placements */
    size_t room = (size_t)(T - P);
    memset(out->p + out->len, 0, room);
    out->len += room;
    { /* the placements, 52 bytes a row as version 3 (zeros for an own mesh) */
        static const uint8_t z12[48];
        const uint8_t *sh = p + 52 + 48 * ne, *pl = sh + 4 * ne;
        for (uint64_t k = 0; k < ne; k++) {
            buf_append(out, sh + 4 * k, 4);
            if (rd32(sh + 4 * k)) buf_append(out, pl, 48), pl += 48;
            else buf_append(out, z12, 48);
        }
    }
    if (out->oom) return fail(err, "out of memory");
    size_t at = (size_t)rows;
    uint32_t nb = rd32(p + at);
    at += 4;
    uint64_t fv = 0, fi = 0, fs = 0;
    int rc = 0;
    for (uint32_t j = 0; j < nb && !rc; j++) {
        if (n - at < 4) { rc = fail(err, "BMSH 4: cut short"); break; }
        uint32_t len = rd32(p + at);
        at += 4;
        if (len > n - at) { rc = fail(err, "BMSH 4: cut short"); break; }
        Arena a;
        arena_init(&a, 0);
        cc_mesh m;
        const char *why = "";
        int d = cc_meshcodec_decode(p + at, len, &a, &m, &why);
        if (d) rc = fail(err, d == CC_MC_OOM ? "out of memory" : "BMSH 4: a damaged block");
        else if (fv + m.nv > nv || fi + m.ni > ni || fs + m.ne > ns) rc = fail(err, "BMSH 4: blocks past the counts");
        else {
            memcpy(out->p + P + 12 * fv, m.pos, 12 * (size_t)m.nv);
            for (uint32_t i = 0; i < m.ni; i++) {
                uint32_t x = (uint32_t)fv + m.idx[i];
                memcpy(out->p + I + 4 * (fi + i), &x, 4);
            }
            memcpy(out->p + E + 24 * fs, m.edges, 24 * (size_t)m.ne);
            for (uint32_t i = 0; i < m.nn && i < m.ne; i++) memcpy(out->p + N + 8 * (fs + i), m.normals[i], 8);
            fv += m.nv, fi += m.ni, fs += m.ne;
        }
        arena_free(&a);
        at += len;
    }
    if (!rc && (fv != nv || fi != ni || fs != ns)) rc = fail(err, "BMSH 4: blocks short of the counts");
    if (!rc && at != n) rc = fail(err, "BMSH 4: trailing bytes");
    for (uint64_t k = 0; !rc && k < ne; k++) { /* every row within the counts */
        const uint8_t *r = p + 52 + 48 * k;
        if ((uint64_t)rd32(r + 24) + rd32(r + 28) > nv || (uint64_t)rd32(r + 32) + rd32(r + 36) > ni || (uint64_t)rd32(r + 40) + rd32(r + 44) > ns)
            rc = fail(err, "BMSH 4: a row out of range");
    }
    if (rc) out->len = base;
    return rc;
}
