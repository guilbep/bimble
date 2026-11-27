/* A tile pyramid per link, baked at conversion (ticket TILE1, ADR 0078 amendment 6): "draw like a game".
 *
 * A loose octree over a scene's elements (the link's own frame: its scene mm), cut so that a LEAF tile holds
 * about CC_TILES_LEAF_TRIS triangles (copies counted). An element belongs to the smallest cell whose loose cube
 * (twice the cell, same centre) holds its box: large elements (slabs, facades, long walls) stay high in the
 * tree, in "own" leaves of that cell (flag OWN), chunked by Morton order of their centres. Every element with
 * geometry is in exactly one leaf. An INNER tile's content is a PROXY: one simplified mesh standing for
 * everything under it (vertex clustering on one lattice, Rossignac & Borrel, the code of hlod.c), its geometric
 * error the grid cell's diagonal (every proxy point lies within it of the detail it stands for; parts thinner
 * than the error in two directions may vanish: they project under the threshold), at most a quarter of its
 * children's triangles and CC_TILES_PROXY_TRIS. Above the octree's root a chain of coarser proxies of the
 * whole (each a quarter of the one below) ends at CC_TILES_ROOT_TRIS triangles or fewer: tile 0.
 *
 * Leaves are not copied: a leaf's geometry is its elements' meshes in the view file's ELEM section (the
 * source of truth), served gathered (cc_tiles_leaf_answer). The section holds the table of contents, the
 * element list, the occluders and the proxies (raw, quantised, uploadable as they are).
 *
 * ---- Section TILE (little-endian; offsets from the section's first byte) ----
 *   0  char[4] "TIL1"            4  u32 version (1)       8  u32 nT tiles          12 u32 nE elements (rows)
 *   16 u32 nL element list       20 u32 nO occluder tris  24 u32 toc bytes (48 + 80 nT + 4 nL + pad4(nL)): the
 *                                                            table of contents; the occluders follow, then the blobs
 *                                                            at pad8(toc + 36 nO)
 *   28 u32 leaf target (tris)    32 u64 blob bytes        40 u32 levels (deepest level + 1)
 *   44 u32 bake flags: 1 the exterior shell computed (reserved, SHELL1; 0: not computed, unknown, never "empty")
 *   48 nT tiles of 80 bytes, tile 0 the root; ids breadth first (a tile's children are consecutive ids,
 *      each after its parent):
 *      0  u32 parent (0xFFFFFFFF: the root)   4 u32 first child   8 u16 children   10 u8 level (root 0)
 *      11 u8 flags: 1 LEAF (content: its elements' detail), 2 PROXY (content: a TPX1 blob), 4 OWN (a leaf of
 *         the large elements of its parent's cell), 8 has occluders, 16 has exterior elements (reserved: SHELL1)
 *      (a proxy may later be built from a subset of its elements, e.g. the exterior shell: its element range is what it
 *      stands for, not necessarily what it depicts)
 *      12 f32[6] box (min xyz, max xyz, link scene mm): holds its content and every descendant's box
 *      36 f32 error (mm): 0 for a leaf; the proxy's deviation; +inf for an inner tile without a proxy (descend)
 *      40 u32 el0   44 u32 nel: its subtree's elements are list[el0 .. el0 + nel) (a leaf: its own)
 *      48 u64 blob offset (from the blobs' start, 4-aligned)   56 u32 blob bytes (0 for a leaf)
 *      60 u32 triangles its content draws (a leaf: its elements', copies counted; a proxy: the proxy's)
 *      64 u32 vertices its content holds (a leaf: own meshes' plus each distinct shape's once)
 *      68 u32 first occluder   72 u16 occluders   74 u16 0
 *      76 u32 detail triangles under it, copies counted (saturates at 0xFFFFFFFF)
 *   then u32 x nL elements (row indices: the link's BMSH rows, the view file's order), each once
 *   then u8 x nL per list entry: 1 closed and outward, 2 opaque occluder class; padded to 4 (the table ends here)
 *   then nO occluder triangles, f32[9] each (three corners, link scene mm), outside the table of contents: real triangles of opaque large
 *      elements (walls, slabs, roofs, footings, columns, beams, coverings; not glass), the largest by area,
 *      at most CC_TILES_OCC per tile (an inner tile: its children's largest): drawing only these into a
 *      depth buffer never hides what is in front of a real surface (a subset of the real geometry)
 *   then the blobs: per PROXY tile a TPX1 blob:
 *      0 "TPX1"   4 u32 nv   8 u32 ni (3 per triangle)   12 u32 nparts
 *      16 nparts x {u32 category (cc_category: concrete 0 .. other 7), u32 first index, u32 index count}
 *      then u16[3] x nv positions: p = lo + q (hi - lo) / 65535, lo, hi the tile's box
 *      then padded to 4, indices: u16 x ni when nv <= 65536, else u32 x ni
 *
 * ---- Answers (session/mesh.h cc_link_tiles, GET /mesh?link=k&tiles=toc | &tile=a,b,…) ----
 *   tiles=toc: the section's bytes [0, toc bytes) (header, tiles, list, flags) as stored; tiles=occ: the occluders
 *   (36 nO bytes, tile i's are [occ0, occ0 + nocc)).
 *   tile=…: "TILS" u32 count, then per tile asked: u32 id, u32 kind (0 none, 1 leaf TLF1, 2 proxy TPX1),
 *      u32 bytes, the bytes, padded to 4. A proxy: its TPX1 blob as stored. A leaf, TLF1:
 *      0 "TLF1"  4 u32 id  8 u32 nel  12 u32 nb blocks
 *      16 nel x {u32 row, u32 block, f32[12] placement: columns x, y, z then t (identity for an own mesh)}
 *      then u32 x nb block bytes, then the blocks: BMG1 meshes (meshcodec.h; the cache's own bytes: own
 *      meshes in scene mm, shapes in their own frame, edges included), each shape once per leaf.
 *
 * ---- Compact vertices for the GPU (the readers' choice; tiles.ts and cc_tiles_pack21 implement it) ----
 *   Proxy: the stored u16[3] (6 bytes), no normal; quantum (hi - lo) / 65535 per axis, far under the error.
 *   Leaf: the link's lattice of 1/16 mm (BMG1's step), 21 bits per axis in two u32 (8 bytes):
 *      k = the least shift with (ceil(16 hi) - floor(16 lo)) >> k < 2^21 (k = 0 for a tile under 131 m),
 *      o = (floor(16 lo) >> k) << k, q = (round(16 p) - o) >> k (rounded at 2^k), p = (o + q 2^k) / 16;
 *      word0 = qx | qy << 21 (low 11 bits), word1 = qy >> 11 | qz << 10. Two leaves with the same k put a
 *      shared vertex on the same lattice point: no crack (k > 0 only for tiles over 131 m: then <= 2^(k-5) mm).
 *
 * Untrusted input: cc_tiles_check refuses a section whose counts, offsets, tree (parents, children, levels,
 * boxes, element ranges, flags), floats or blobs do not hold; O(n) plus a bitmap of nE bits. The builder is
 * bounded (depth, cells, each proxy's triangles), streamed (post-order: at most the open path's clusters),
 * deterministic (same scene, same bytes), cancellable between steps. Pure: no files, threads or clock. */
#ifndef CCORE_TILES_H
#define CCORE_TILES_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "buf.h"
#include "scene.h"
#include "viewfile.h"

#define CC_TILES_VERSION 1u
#define CC_TILES_HEAD 48u
#define CC_TILES_REC 80u
#define CC_TILES_LEAF_TRIS 40000u   /* a leaf's target (copies counted); 20-60 k */
#define CC_TILES_PROXY_TRIS 16384u  /* most triangles of one proxy */
#define CC_TILES_ROOT_TRIS 512u     /* the coarsest proxy (tile 0) has at most this */
#define CC_TILES_RES 64u            /* grid cells along an inner cell's side before the proxy is fit */
#define CC_TILES_MAX_DEPTH 16u      /* octree depth */
#define CC_TILES_MAX_LEVELS 40u     /* levels in the file (octree, own leaves, the coarse chain) */
#define CC_TILES_MAX_TILES (1u << 22)
#define CC_TILES_OCC 64u            /* occluder triangles per tile (the default; cc_tiles_opts.occ, at most CC_TILES_OCC_MAX) */
#define CC_TILES_OCC_MAX 256u
#define CC_TILES_NONE 0xFFFFFFFFu

/* 16 reserved (ticket SHELL1): the tile holds exterior elements. */
enum { CC_TILE_LEAF = 1, CC_TILE_PROXY = 2, CC_TILE_OWN = 4, CC_TILE_OCC = 8, CC_TILE_EXTERIOR = 16 };
/* 4 reserved (SHELL1): the element is seen from outside (0 with CC_TILES_BAKE_SHELL unset: unknown) */
enum { CC_TILES_EL_CLOSED = 1, CC_TILES_EL_OPAQUE = 2, CC_TILES_EL_EXTERIOR = 4 };
/* the header's bake flags (offset 44): 1 the exterior shell was computed (SHELL1; 0 today: not computed, not "none") */
enum { CC_TILES_BAKE_SHELL = 1 };

/* ---- the builder's input: a scene (the open model) or a lazy view file (a link's cache, read in place) ---- */
typedef struct {
    const cc_scene *scene;  /* one of the two */
    const cc_viewfile *vf;  /* lazy (cc_viewfile_read_mapped) or decoded */
} cc_tiles_src;

/* The source's elements, for other bakers (shell.h): its rows; its blocks (shapes, then each element's own mesh); element
 * k's block (SIZE_MAX: damaged); block b decoded into `a` when lazy (0 ok); element k's placement (NULL: its own mesh, in
 * scene mm); its box (empty when no finite point; 0 ok). */
size_t cc_tiles_src_count(const cc_tiles_src *s);
size_t cc_tiles_src_nblocks(const cc_tiles_src *s);
size_t cc_tiles_src_el_block(const cc_tiles_src *s, size_t k);
int cc_tiles_src_block(const cc_tiles_src *s, size_t b, Arena *a, cc_mesh *m);
const cc_placement32 *cc_tiles_src_place(const cc_tiles_src *s, size_t k);
int cc_tiles_src_box(const cc_tiles_src *s, size_t k, float b[6], Arena *scratch);

typedef struct cc_tiles_work cc_tiles_work;

/* The bake-time settings (the owner's rule: every optimisation a setting with a default and an off value). A cache records
 * them (its SRCE "tiles": cc_tiles_opts_text) and is indexed again when they change. occ 0: no occluders. */
typedef struct {
    uint32_t leaf_tris, proxy_tris, root_tris, res, occ;
    /* SHELL1 (shell.h): the exterior shell's views (0: none), their resolution and the depth tolerance d (mm) */
    uint32_t shell_views, shell_res, shell_tol;
} cc_tiles_opts;
#define CC_TILES_SHELL_VIEWS 64u
#define CC_TILES_SHELL_RES 1024u
#define CC_TILES_SHELL_TOL 100u
#define CC_SHELL_MAX_VIEWS_T 4096u
#define CC_SHELL_MAX_RES_T 4096u
void cc_tiles_default(cc_tiles_opts *o);
/* "leaf=40000 proxy=16384 root=512 res=64 occ=64 shell=64x1024 shell_d=100" into out[0..n) ("shell=0": none) */
void cc_tiles_opts_text(const cc_tiles_opts *o, char *out, size_t n);
/* the defaults, then each "key=value" of `t` (space separated; unknown keys and bad values ignored, each value bounded) */
void cc_tiles_opts_parse(cc_tiles_opts *o, const char *t);

typedef struct cc_tiles {
    Buf out;            /* the TILE section's payload once built */
    cc_tiles_work *work;
    uint64_t done, total; /* triangles clustered so far, of `total` (progress) */
    /* numbers for the report */
    uint32_t ntiles, nleaves, nproxies, levels, nocc;
    uint64_t proxy_tris, leaf_tris;
} cc_tiles;

/* Splits the tree (one pass over the elements' boxes; nothing decoded). `src` must outlive the steps. Always
 * initializes `t` (free it with cc_tiles_free). 0 ok, -1 with `err`. */
int cc_tiles_start(cc_tiles *t, const cc_tiles_src *src, const cc_tiles_opts *o, cc_error *err);
/* About `budget` triangles more (at least one element). 1 done (t->out holds the section), 0 more, -1 err. */
int cc_tiles_step(cc_tiles *t, uint64_t budget, cc_error *err);
int cc_tiles_build(cc_tiles *t, const cc_tiles_src *src, const cc_tiles_opts *o, cc_error *err);
void cc_tiles_free(cc_tiles *t);

/* ---- reading (untrusted) ---- */
typedef struct {
    uint32_t parent, skip, nchildren, level, flags; /* skip: tiles in its subtree (itself too): its first child is id + 1, the next sibling id + skip */
    float box[6], error;
    uint32_t el0, nel;
    uint64_t off;
    uint32_t len, tris, verts, occ0, nocc, detail;
} cc_tile;

typedef struct {
    const uint8_t *p;   /* the section (or its table of contents alone: blobs absent) */
    size_t n;
    uint32_t ntiles, nelements, nlist, nocc, toc, levels, leaf_target, bake;
    uint64_t blobs;
    uint32_t blob_at;   /* the blobs' offset in the section: pad8(toc + 36 nO) */
    const uint8_t *list, *eflags, *occ; /* occ NULL when only the table of contents was given */
    const uint8_t *shell; /* SHELL1: the shell table (TSH1) when baked and given (the whole section, or cc_tiles_shell_check) */
    uint32_t shell_n;
} cc_tiles_toc;

/* The table of contents at p[0..n) checked (n may stop at the toc bytes: `with_blobs` 0), every rule above. 0 ok,
 * -1 with `err`. `nelements`: the link's rows (CC_TILES_NONE: not checked). */
int cc_tiles_check(const uint8_t *p, size_t n, int with_blobs, uint32_t nelements, cc_tiles_toc *toc, cc_error *err);
void cc_tiles_get(const cc_tiles_toc *toc, uint32_t i, cc_tile *out);
uint32_t cc_tiles_list(const cc_tiles_toc *toc, uint32_t i); /* list[i] */
uint32_t cc_tiles_el_flags(const cc_tiles_toc *toc, uint32_t i); /* the list's i-th element's CC_TILES_EL_* */
/* A TPX1 blob checked: counts, sizes, parts within the indices, indices within the vertices. 0 ok. */
int cc_tiles_proxy_check(const uint8_t *p, size_t n, cc_error *err);
/* A TPX1 blob decoded into `a`: positions in scene mm (from the tile's box), u32 indices; parts' category per
 * triangle in `cat` (one byte a triangle). 0 ok, -1 damaged or out of memory. */
int cc_tiles_proxy_mesh(const uint8_t *p, size_t n, const float box[6], Arena *a, cc_mesh *m, uint8_t **cat);

/* ---- SHELL1 (ADR 0078 amendment 7, shell.h): the exterior shell, baked after the pyramid ----
 * With the header's bake flag 1: element flag 4 marks the elements seen from outside, tile flag 16 the tiles holding one,
 * and the blobs end with the SHELL TABLE and a trailer (the section's last 16 bytes: "TSHT", u32 table bytes, u64 the
 * table's offset from the blobs' start; 8-aligned). Readers of the layout without it are unchanged: the blobs are only
 * longer. The table, "TSH1" (asked apart: tiles=shell):
 *   0 "TSH1"  4 u32 version 1  8 u32 nT  12 u32 views  16 u32 resolution  20 u32 depth tolerance mm
 *   24 u32 exterior elements  28 u32 0;  then nT records of 32 bytes, in tile order:
 *   0 u32 exterior detail triangles under it, copies counted (a leaf: its exterior elements'; saturating, <= detail)
 *   4 u32 flags: 1 a shell proxy: its exterior elements' proxy, the TPX1 blob below (the tile's own when all its
 *     elements are exterior; a proxy of the exterior ones only, error measured against them, when it has both kinds)
 *   8 u64 the blob's offset from the blobs' start   16 u32 its bytes   20 u32 its triangles (a leaf: = the first field)
 *   24 u32 its vertices   28 f32 its error mm (0 a leaf; +inf no shell proxy: the outside walk descends)
 * The outside walk (CC_TILES_SEL_SHELL): tiles without flag 16 are skipped, leaves draw their exterior elements
 * (leaf answer with CC_TILES_ANS_SHELL, GET ...&tile=a,b&shell=1), proxies their shell proxy. The full walk is unchanged
 * (the tiles' own proxies depict all their elements). */
#define CC_TILES_SHELL_HEAD 32u
#define CC_TILES_SHELL_REC 32u
enum { CC_TILE_SHELL_PROXY = 1 };
enum { CC_TILES_ANS_SHELL = 1 };
typedef struct {
    uint32_t ext, flags;
    uint64_t off;
    uint32_t len, tris, verts;
    float error;
} cc_tile_shell;
/* The table p[0..n) checked against `toc` (counts, flags, ranges; the proxies too when toc holds the blobs) and attached
 * (toc->shell). 0 ok, -1 with err (toc->shell NULL). */
int cc_tiles_shell_check(const uint8_t *p, size_t n, cc_tiles_toc *toc, cc_error *err);
void cc_tiles_shell_get(const cc_tiles_toc *toc, uint32_t i, cc_tile_shell *out); /* zeros without a table */
typedef struct {
    uint32_t exterior, elements, mixed; /* elements exterior, elements listed, tiles with both kinds */
    int shell_build_ok;                 /* the exterior proxies were built (the same tree) */
    uint64_t ext_tris, added, shell_proxy_tris;
} cc_tiles_shell_info;
/* The section sec[0..n) (built without a shell) with the shell of `ext` (per scene element, 1 exterior) into `out`: the
 * flags set, the mixed tiles' exterior proxies built (a build of the same tree from the exterior elements; `o` its
 * settings), the table and the trailer appended. Cancellable between steps (`cancel`, may be NULL). 0 ok, -1 err. */
int cc_tiles_shell_apply(const uint8_t *sec, size_t n, const cc_tiles_src *src, const cc_tiles_opts *o, const uint8_t *ext,
                         const int *cancel, Buf *out, cc_tiles_shell_info *info, cc_error *err);

/* The leaf answer TLF1 of tile `id` (list[el0..el0+nel)) from a scene or a view file (lazy: the blocks copied
 * from the mapping; else encoded). Appended to `out`. 0 ok, -1 err. */
int cc_tiles_leaf_answer(const cc_tiles_src *src, const cc_tiles_toc *toc, uint32_t id, Buf *out, cc_error *err);
/* the same, `flags` CC_TILES_ANS_SHELL: its exterior elements only (a section without the shell: all) */
int cc_tiles_leaf_answer_ex(const cc_tiles_src *src, const cc_tiles_toc *toc, uint32_t id, unsigned flags, Buf *out, cc_error *err);
/* A TLF1 answer read back (untrusted): each element's triangles placed in scene mm appended to `a` (tests,
 * tools). 0 ok. */
int cc_tiles_leaf_mesh(const uint8_t *p, size_t n, Arena *a, cc_mesh *m, uint32_t **tri_row);

/* The 21-bit lattice of a leaf (above): shift k and origin o (1/16 mm units) of a box. */
void cc_tiles_lattice(const float box[6], int *k, int64_t o[3]);
/* p (scene mm) on that lattice: q[3] (each < 2^21). */
void cc_tiles_pack21(const float p[3], int k, const int64_t o[3], uint32_t q[3]);

/* ---- the reference walker (the page, the native viewer and the tests share its rule) ----
 * Top-down by projected error: the tiles' frontier starts at the root; the tile of largest projected error
 * (error mm x k / distance from the eye to its box, k = viewport height / (2 tan(fov / 2)); an orthographic
 * view: error mm x px_per_mm) is replaced by its children in view while it is over `error_px` and the
 * frontier's triangles stay within `budget` (a tile without a proxy, or marked in `descend`, is always
 * replaced: it cannot stand). Tiles out of the frustum (a box outside a plane) are dropped. The answer, front to
 * back (distance to the box): `out[0..ret)` (at most `cap`), `*tris` their triangles. */
typedef struct {
    double eye[3];        /* link scene mm */
    double planes[6][4];  /* a x + b y + c z + d >= 0 inside; nplanes of them */
    int nplanes;
    double k;             /* perspective: viewport height px / (2 tan(fov_y / 2)) */
    double px_per_mm;     /* > 0: orthographic */
} cc_tiles_view;
/* flags: CC_TILES_SEL_LEAVES: levels of detail off: every leaf in the frustum (error_px and budget ignored). The renderers'
 * settings: error_px (default 1 still), budget (default none), levels of detail (default on), occluders (cc_tiles_occ). */
/* CC_TILES_SEL_SHELL (SHELL1): the outside walk, for an eye outside the link's box (cc_tiles_shell_outside) when no section
 * cuts the link and no exterior element of it is hidden or transparent: tiles without exterior elements skipped, leaves
 * counted by their exterior triangles, proxies by their shell proxy (the table when attached); ignored without the shell. */
enum { CC_TILES_SEL_LEAVES = 1, CC_TILES_SEL_SHELL = 2 };
/* SHELL1: 1 when the shell is baked and `eye` (link scene mm) is outside the root's box grown by `margin` mm: the outside
 * walk applies (the caller also checks sections and hidden elements). The settings' shell_mode: off, outside, auto. */
int cc_tiles_shell_outside(const cc_tiles_toc *toc, const double eye[3], double margin);
#define CC_TILES_SHELL_MARGIN 2000.0 /* mm: interior tiles are asked before the eye reaches the box */
uint32_t cc_tiles_select(const cc_tiles_toc *toc, const cc_tiles_view *v, double error_px, uint64_t budget, unsigned flags,
                         const uint8_t *descend, uint32_t *out, uint32_t cap, uint64_t *tris);

/* ---- the reference occlusion: a small depth buffer of the tiles' occluders (conservative) ----
 * A camera in world mm (eye, forward, right, up unit vectors; k = pixels per unit of tan: height / (2 tan(fov/2))),
 * a buffer of w x h cells (a coarse copy of the screen, w x h of the viewport / cell), sampled at the cells' corners. An
 * occluder triangle (link frame, placed by R(c, s) about Z then o: world = R p + o) writes the corners it holds (edges
 * included: two triangles of one face leave no seam), its depth there; a cell is covered at the farthest of its four
 * corners; a box is hidden when every cell it covers is covered nearer than the box's nearest point. Conservative but
 * for a hole narrower than a cell between two occluders (the tools use cells of 3 px). A triangle or box reaching behind the near plane is never used to hide
 * and never hidden. Every tile drawn should put its occluders in first (a box is not hidden by its own occluders: they
 * lie inside it). */
typedef struct {
    double eye[3], fwd[3], right[3], up[3], k, near_mm;
    int w, h;          /* cells */
    double cell;       /* viewport pixels a cell */
    float *depth;      /* (w + 1) x (h + 1) corner samples, +inf: nothing */
} cc_tiles_occ;
int cc_tiles_occ_init(cc_tiles_occ *z, const double eye[3], const double at[3], double tan_half, double w_px, double h_px, int cells_w);
void cc_tiles_occ_free(cc_tiles_occ *z);
/* tile i's occluders (toc->occ must be there) drawn; c, s, o: the link's placement */
void cc_tiles_occ_draw(cc_tiles_occ *z, const cc_tiles_toc *toc, uint32_t i, double c, double s, const double o[3]);
/* 1 when box (link frame, placed so) is hidden */
int cc_tiles_occ_hidden(const cc_tiles_occ *z, const float box[6], double c, double s, const double o[3]);
#endif
