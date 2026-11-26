/* BMSH version 3 (scene_bmsh.h): shapes once, a placement per element (ticket 3D1, ADR 0078). */
#include <string.h>

#include "scene_bmsh.h"

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
