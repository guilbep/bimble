/* Simplified stand-ins for what is far (HLOD, ADR 0036 #9; ticket 3D2, ADR 0078): a port of
 * bimble-geom hlod.rs. An octree over the scene, each cell with a proxy mesh standing for every
 * element under it and the error of that proxy in mm. The lab draws a cell's proxy instead of its
 * elements when the error projects under a pixel threshold, else descends; under a leaf it draws the
 * elements.
 *
 * Octree, not the box tree (hlod.rs:6): cubes halve per level, so the error does too. Simplification
 * is vertex clustering on a grid (Rossignac & Borrel 1993, hlod.rs:10): every vertex moves to the mean
 * of the vertices in its grid cell, triangles that lose a corner are dropped. All grids are one
 * lattice (the root cube's corner, power-of-two sizes), so a parent clusters its children's clusters
 * by halving their keys, which equals clustering the originals. The error is one-directional
 * (hlod.rs:17): every proxy point is within `error_mm` of the original. Proxies are kept per material
 * family (the category) and ignore what the viewer hides: a cell with something hidden, selected or
 * cut under it must be descended (the page's rule, proxies.rs `usable`).
 *
 * Built in steps (ADR 0074: opening is not slower, and the wasm core runs on the page's thread):
 * cc_hlod_start splits the tree (one pass over the elements' boxes), each cc_hlod_step reads about
 * `budget` triangles; cc_hlod_build does both at once. Differences from Rust: one thread (Rust built
 * each depth's cells in parallel); the steps; the clusters are sorted arrays merged on demand, not
 * hash maps; a vertex counts once per chunk of an element (as Rust's per-call map).
 *
 * Untrusted input: depth (CC_HLOD_MAX_DEPTH), each proxy's triangles (max_proxy_triangles, the grid
 * coarsened until it fits) and the cells (at most 2 per element, CC_HLOD_MAX_CELLS) are bounded;
 * non-finite vertices are skipped; no recursion; out of memory is an error value. */
#ifndef CCORE_HLOD_H
#define CCORE_HLOD_H
#include <stdint.h>
#include "arena.h"
#include "buf.h"
#include "scene.h"

/* hlod.rs:39 Options. */
typedef struct {
    uint64_t leaf_triangles;      /* a cell with more triangles (copies counted) is split: 65,536 */
    uint32_t resolution;          /* grid cells along a cell's side (error = side / resolution x sqrt 3): 64 */
    uint64_t max_proxy_triangles; /* most triangles in one proxy (the grid coarsens until it fits): 65,536 */
    double max_ratio;             /* a proxy is kept only under this share of what it stands for: 0.25 */
} cc_hlod_opts;

void cc_hlod_default(cc_hlod_opts *o);

#define CC_HLOD_MAX_DEPTH 16u            /* hlod.rs:64 */
#define CC_HLOD_MAX_CELLS (1u << 22)     /* 3D2: a tree of more cells is refused (2 per element at most) */

/* One material family's part of a proxy: the mesh in scene coordinates, with crease edges. */
typedef struct {
    cc_category cat;
    cc_mesh mesh;
} cc_hlod_part;

/* hlod.rs:67 Cell. `el0`, `nel`: the elements under the cell (every cell, inner ones too) are
 * `elements[el0 .. el0 + nel]` of the tree (indices into cc_scene.elements); a leaf has no children. */
typedef struct {
    uint32_t depth;
    float bounds[6];      /* min xyz, max xyz of its elements (they may reach out of its cube) */
    float error_mm;
    cc_hlod_part *proxy;  /* empty when not much smaller than what it stands for: descend */
    uint32_t nproxy;
    uint32_t *children;
    uint32_t nchildren;
    uint32_t el0, nel;
    uint64_t original_triangles; /* drawn without the proxy (copies counted) */
} cc_hlod_cell;

typedef struct cc_hlod_work cc_hlod_work; /* the build's state between steps (hlod.c) */

typedef struct {
    Arena arena;           /* cells, element lists, proxies */
    cc_hlod_cell *cells;   /* cells[0] is the root; none for an empty scene (heap) */
    uint32_t ncells;
    uint32_t *elements;    /* the elements, ordered so that each cell's are a range */
    uint32_t nelements;
    cc_hlod_work *work;    /* NULL once built */
    uint64_t read, total;  /* triangles read so far, of `total` (progress) */
} cc_hlod;

/* Splits the tree of `s` (which must outlive the steps). Always initializes `h` (free it with
 * cc_hlod_free). 0 ok, -1 with `err`. */
int cc_hlod_start(cc_hlod *h, const cc_scene *s, const cc_hlod_opts *o, cc_error *err);
/* Builds proxies for about `budget` triangles (at least one chunk). 1 done, 0 more to do, -1 with
 * `err` (out of memory). */
int cc_hlod_step(cc_hlod *h, uint64_t budget, cc_error *err);
/* hlod.rs:116 build: start, then every step. 0 ok, -1 with `err`. */
int cc_hlod_build(cc_hlod *h, const cc_scene *s, const cc_hlod_opts *o, cc_error *err);
void cc_hlod_free(cc_hlod *h);

uint64_t cc_hlod_proxy_triangles(const cc_hlod_cell *c);
/* Bytes of the proxy as stored (positions, indices, edges and their normals; hlod.rs:93). */
uint64_t cc_hlod_proxy_bytes(const cc_hlod_cell *c);

/* The built tree as BPRX bytes for the lab (appended to `out`), little-endian:
 *   0  char[4] "BPRX"   4 u32 version (1)   8 u32 rev   12 u32 nE (the scene's elements)
 *   16 u32 nC cells   20 u32 nK children   24 u32 nL element list   28 u32 nP parts
 *   32 u32 nV vertices   36 u32 nI indices   40 u32 nS edge segments   44 u32 0
 *   48 nC cells of 64 bytes: u32 depth, f32 error_mm, f32[6] bounds, u32 first child, u32 children,
 *      u32 el0, u32 nel, u32 first part, u32 parts, u64 original triangles
 *   then u32 x nK children, u32 x nL elements, nP parts of 28 bytes (u8 category, 3 pad, u32 first
 *   vertex, vertices, first index, indices, first segment, segments), f32[3] x nV positions (scene
 *   coordinates, as BMSH), u32 x nI indices (absolute), f32[6] x nS segments, i8[8] x nS normals.
 * 0 ok, -1 with `err` (not built, out of memory, a count over u32). */
int cc_hlod_bprx(const cc_hlod *h, uint32_t rev, uint32_t nelements, Buf *out, cc_error *err);
#endif
