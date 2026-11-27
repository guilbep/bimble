/* The native app's scenes (NAT1, NAT2, ticket NUI1): a model's geometry as the renderer's CPU arrays,
 * built from what the C core gives. A carrier (ccore/README.md "Layers", L6): it may use files, the
 * clock, threads and the GPU; it calls ccore/src and ccore/session as a library, the way the host does.
 *
 * Sources:
 *   - the open model: BMSH version 3 bytes (cc_mesh_get_form 3: each shared shape once, a placement per
 *     copy; src/scene_bmsh.h), nv_load_bytes;
 *   - a link: its rows and meshes through an nv_src (scene_load.c: a link read in place decodes its
 *     cache's mesh blocks itself, one batch at a time, so the session never holds the link decoded),
 *     placed as the session places it (cc_link_placement: turned about Z, moved), nv_load_src;
 *   - a link's proxies (its cache's HLOD section, BPRX: src/hlod.h), nv_load_bprx (alone: the first
 *     picture; with the detail: far cells drawn as their proxies). */
#ifndef BIMBLE_NATIVE_H
#define BIMBLE_NATIVE_H
#include <stddef.h>
#include <stdint.h>
#include "scene.h" /* cc_mesh */

/* A vertex of the merged buffers: scene position (mm, relative to the scene origin) and the global
 * element index (0..7: the material families' colours, for the HLOD proxies; the elements from
 * NV_ELEM0 on). */
#define NV_ELEM0 8u
typedef struct { float p[3]; uint32_t elem; } nv_vtx;
/* A copy of a shared shape: the Placement32 columns, the element index in t[3] (uint bits). */
typedef struct { float x[4], y[4], z[4], t[4]; } nv_plc;
/* A draw item for the cull pass: centre (w radius), half extent, k = (command, copy or ~0, leaf
 * cell, triangles). */
typedef struct { float c[4], e[4]; uint32_t k[4]; } nv_item;
/* A shared shape drawn instanced: its triangles and edge vertices in the shape buffers, and its
 * copies' range in the placements. */
typedef struct { uint32_t first_index, nidx, first_edge, nedge, first_inst, ninst; } nv_shape;
/* An element of the merged buffers: its ranges. */
typedef struct { uint32_t first_index, nidx, first_edge, nedge; } nv_welem;
/* HLOD: a cell of the octree (BPRX) with its proxy's draw range in the proxy buffers. */
typedef struct {
    float bounds[6];
    float error_mm;
    uint32_t first_child, nchildren;
    uint32_t pfirst_index, pnidx, pfirst_edge, pnedge; /* proxy (merged over its parts) */
    int has_proxy;
    uint32_t leaf; /* 1 when it has no children */
} nv_cell;

typedef struct { void *p; size_t n, cap, sz; } nv_vec;

/* One part's scene (render.h). Element k of it is the global element elem0 + k. */
typedef struct {
    /* merged (scene coordinates) */
    nv_vec wv, wi, we;       /* nv_vtx, uint32_t, nv_vtx pairs */
    nv_vec welems;           /* nv_welem, one draw command each */
    /* shared shapes */
    nv_vec sv, si, se;       /* float[3], uint32_t (absolute in sv), float[3] pairs */
    nv_vec shapes;           /* nv_shape */
    nv_vec plc;              /* nv_plc, grouped by shape (nv_finish) */
    /* per element */
    nv_vec colour;           /* uint32_t packed sRGB, nelements */
    nv_vec ids;              /* char[22], nelements */
    nv_vec items;            /* nv_item: welems first (command = index), then the copies */
    /* HLOD (the proxies in scene coordinates as nv_vtx) */
    nv_vec cells;            /* nv_cell */
    nv_vec roots;            /* uint32_t: the root cells */
    nv_vec kids;             /* uint32_t: the cells' children */
    nv_vec pv, pi, pe;       /* proxy vertices, indices, edge vertex pairs */
    nv_vec item_of;          /* uint32_t per element: its item (UINT32_MAX none), until nv_finish */
    uint64_t tris_total;     /* triangles of the whole picture, copies counted */
    uint64_t tris_unique;    /* triangles stored */
    uint32_t elem0, nelements;
    double bmin[3], bmax[3]; /* its box, scene mm (bmin > bmax: empty) */
    double origin0[3];       /* the scene origin (world mm) every part is drawn relative to */
    int proxies_only;        /* a link's proxies alone: every cell drawn as its finest proxy in view */
} nv_scene;

typedef struct {
    uint32_t min_copies; /* a shape with fewer copies is placed into the merged buffers (ADR 0078: 4) */
    /* the placement of the source's own coordinates in the scene: a vertex p goes to
     * R(c, s) p + off, off = R(c, s) origin + t - origin0 (links.h LNKS version 2); c = 1, s = 0 none */
    double c, s, off[3];
    int colour;          /* -1 the elements' own colours; else a palette index 0..11 (a link's colour) */
} nv_load_opts;

/* A model's rows and meshes (scene_load.c fills one for a session's link). Called on the loader's
 * thread between lock and unlock (the session's lock: render data must not be read while the
 * session changes); meshes returned are valid until the next call or unlock. */
typedef struct nv_src {
    size_t nelements, nshapes;
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    int (*cancelled)(void *ctx);
    /* element i: its 22-char id, category (0..7), shape (-1 its own mesh) and placement (shape >= 0) */
    void (*row)(void *ctx, size_t i, char id[22], int *cat, int32_t *shape, float m[12]);
    /* the box of shape s in its own coordinates (lo, hi), from what is known without decoding when
     * possible; 0 ok */
    int (*shape_box)(void *ctx, size_t s, float bb[6]);
    /* meshes: 0 ok (`out` borrowed until the next call or unlock), -1 none or damaged */
    int (*shape_mesh)(void *ctx, size_t s, cc_mesh *out);
    int (*own_mesh)(void *ctx, size_t i, cc_mesh *out);
} nv_src;

void *nv_push(nv_vec *v, size_t sz, size_t count); /* room for `count` more, returned; NULL: out of memory */
void nv_vec_free(nv_vec *v);
void nv_scene_init(nv_scene *sc, uint32_t elem0, const double origin0[3]);
void nv_scene_free(nv_scene *sc);
/* BMSH version 3 bytes (untrusted: every range checked) into `sc` (elements from sc->elem0). */
int nv_load_bytes(nv_scene *sc, const char *bmsh3, size_t n, const nv_load_opts *o, char *err, size_t errn);
/* A source's elements into `sc`. -1 with `err` (out of memory, damaged, cancelled: "cancelled"). */
int nv_load_src(nv_scene *sc, const nv_src *src, const nv_load_opts *o, char *err, size_t errn);
/* BPRX bytes (untrusted, checked) into `sc`'s cells and proxy buffers, placed by `o`; the leaves' element
 * lists set the items' cells (when the scene holds the source's elements: nelements given). */
int nv_load_bprx(nv_scene *sc, const char *bprx, size_t n, uint32_t nelements, const nv_load_opts *o, char *err, size_t errn);
/* After the last load: the copies grouped by shape (the indirect draws' instance ranges). -1 out of memory. */
int nv_finish(nv_scene *sc);
double nv_now_ms(void);
/* The lab's palettes (glviewport.ts CATEGORY_SRGB, LINK_SRGB), packed RGBA8. */
uint32_t nv_category_colour(int cat);
uint32_t nv_link_colour(int k);
#endif
