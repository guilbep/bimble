/* The exterior shell of a link (ticket SHELL1, ADR 0078 amendment 7): which elements can be seen from outside at all.
 *
 * A software depth buffer, no GPU. For each of N directions (the six sides, the eight corner diagonals, then a Fibonacci
 * sphere), an orthographic view of the link's whole bounding sphere at res x res pixels:
 *   pass 1: the OPAQUE elements (the tile pyramid's element flag 2: the occluder classes, IfcWall*, IfcSlab, IfcRoof,
 *           IfcFooting, IfcColumn, IfcBeam, IfcCovering, never the glass family) write their depth at the pixels whose
 *           centre they cover (both faces);
 *   pass 2: every element not yet exterior is tested: its box first (hidden when every 8 x 8 block it covers holds only
 *           surfaces nearer than the box by more than `tol`), then its triangles with CONSERVATIVE coverage (a pixel
 *           touched at all counts: a pipe thinner than a pixel is tested) at their depth clamped to the triangle's range:
 *           visible when nearer than the buffer plus `tol` mm (an element lying on a surface, a mullion, a handle, counts).
 * Non-opaque elements (windows, curtain walls' plates and members, doors, railings, furniture, MEP...) never write depth:
 * what is behind a window is exterior. An element seen in at least one view is EXTERIOR. Optionally (`touch`) an element
 * whose box reaches the link's box faces (within tol) is exterior whatever the views say (a sampling safeguard).
 *
 * Orthographic views over every direction are complete for an eye outside the link's box: a ray from such an eye to a
 * point inside meets the box's hull only once, so the whole line in front of the point is outside the hull and seen by
 * the view along that direction. What can be missed: a direction between the sampled ones (a narrow slot seen at one
 * angle only) and a sliver narrower than a pixel behind an edge. The walker recovers nothing by itself: the renderers'
 * run-time occlusion is the safety net (tiles.h).
 *
 * Bounded: memory is a batch of views' buffers (at most CC_SHELL_BATCH_BYTES) plus a run of leaves' placed triangles
 * (the pyramid's leaves read in order, CC_SHELL_RUN_TRIS at a time; an element larger than that alone), work is views x
 * triangles. Deterministic: each view is drawn by one worker in the elements' order; marks are or-ed: the same bytes for
 * any thread count. Untrusted input: non-finite points and degenerate triangles skipped. Pure: no files, clock or I/O. */
#ifndef CCORE_SHELL_H
#define CCORE_SHELL_H
#include <stddef.h>
#include <stdint.h>
#include "tiles.h"

#define CC_SHELL_MAX_VIEWS 4096u
#define CC_SHELL_MAX_RES 4096u
#define CC_SHELL_BATCH_BYTES ((size_t)64 << 20) /* the views' buffers drawn at once */
#define CC_SHELL_RUN_TRIS 400000u              /* placed triangles held at once */

/* ---- the rasteriser (also the truth renders of the tools: a perspective camera) ---- */
typedef struct {
    int persp;           /* 0 orthographic */
    double o[3];         /* ortho: the image centre's point; persp: the eye (mm) */
    double r[3], u[3], f[3]; /* right, up, forward (unit) */
    double scale;        /* ortho: pixels a mm; persp: pixels a unit of tan (h / (2 tan(fov_y / 2))) */
    double near_mm;      /* persp */
    int w, h;
} cc_shell_cam;

/* An orthographic camera looking along `dir` (unit) at a sphere (centre, radius mm) filling res x res pixels. */
void cc_shell_cam_ortho(cc_shell_cam *c, const double dir[3], const double centre[3], double radius, int res);
/* A perspective camera (z up), w x h pixels, near plane 10 mm. */
void cc_shell_cam_persp(cc_shell_cam *c, const double eye[3], const double at[3], double tan_half, int w, int h);

typedef struct {
    cc_shell_cam cam;
    float *z;    /* w x h depth keys, smaller nearer (ortho: mm along f from o; persp: -1 / depth); +inf nothing */
    float *bm;   /* per 8 x 8 block, the farthest key (cc_shell_zb_blocks) */
    int bw, bh;
} cc_shell_zb;
int cc_shell_zb_init(cc_shell_zb *b, const cc_shell_cam *c); /* 0 ok, -1 out of memory */
void cc_shell_zb_clear(cc_shell_zb *b);
void cc_shell_zb_free(cc_shell_zb *b);
void cc_shell_zb_blocks(cc_shell_zb *b); /* after the opaque pass, before box tests */
/* One triangle (three points, mm, world = the camera's frame): its depth written at the pixel centres it covers. */
void cc_shell_draw(cc_shell_zb *b, const float t[9]);
/* 1 when the triangle shows at a pixel: conservative (any pixel it touches, at its depth clamped to its range) or exact
 * (pixel centres); within `tol` (keys' units: mm in ortho, 0 in perspective) of the buffer. */
int cc_shell_test(const cc_shell_zb *b, const float t[9], double tol, int conservative);
/* 1 when a box (lo, hi mm) is surely hidden: every block of its screen rectangle nearer than its nearest point by more
 * than tol; 0 when it may show (also when it reaches behind the near plane). An off-screen box is hidden. */
int cc_shell_box_hidden(const cc_shell_zb *b, const float box[6], double tol);

/* Back-face culling proven, not assumed (the pyramid's element flag 1, "closed and outward"): a mesh's triangles (placed,
 * their winding as a renderer would cull them: a mirrored copy's already flipped) drawn alone from `views` orthographic
 * directions of the Fibonacci sphere (no axis: a face exactly edge on is a sliver of rounding) at res x res, once whole and once without the faces turned away from the view; 1 when every view shows the
 * same pixels at the same depth (within a thousandth of the mesh's size): culling changes nothing. 0 when it does (or out of
 * memory). `out_view` (may be NULL): the first view that differs. */
int cc_shell_cull_proof(const float (*t)[9], uint32_t nt, uint32_t views, int res, uint32_t *out_view);

/* ---- the classification of a link ---- */
typedef struct {
    uint32_t views, res; /* views 0: no shell */
    uint32_t tol_mm;     /* depth tolerance */
    uint32_t touch;      /* 1: an element whose box reaches the link's box faces is exterior */
} cc_shell_opts;

/* The directions of `n` views (unit vectors, deterministic): the six sides, the eight corner diagonals, then a Fibonacci
 * sphere. `out` holds 3 n doubles. */
void cc_shell_dirs(uint32_t n, double *out);

typedef struct cc_shell cc_shell;
/* One pyramid of several classified together (the cross-link pass: the links of a project, each placed): world = R p + o,
 * R the turn about Z of cosine c and sine s. A part's elements are its src's rows. */
typedef struct {
    cc_tiles_src src;
    cc_tiles_toc toc;
    double c, s, o[3];
} cc_shell_part;
typedef struct {
    uint64_t drawn, tested;       /* triangles drawn (pass 1, per view) and tested (pass 2) */
    uint64_t box_tests, box_hidden;
    uint32_t exterior, elements;  /* elements found exterior, elements with geometry */
    uint32_t touched;             /* exterior by the touch rule alone */
    size_t peak_bytes;            /* buffers and runs at most */
} cc_shell_stats;

/* Starts the classification of the elements of `src` listed by the pyramid `toc` (its leaves give the order and the
 * runs; its element flag 2 the occluders). `toc` and `src` must outlive the steps. 0 ok, -1 with err (*out NULL). */
int cc_shell_start(cc_shell **out, const cc_tiles_src *src, const cc_tiles_toc *toc, const cc_shell_opts *o, cc_error *err);
/* About `budget` triangles more (drawn or tested, per view). 1 done, 0 more, -1 err (out of memory, a mesh). */
int cc_shell_step(cc_shell *s, uint64_t budget, cc_error *err);
/* Several pyramids together (each part's opaque elements hide the others'); the views frame the union. */
int cc_shell_start_parts(cc_shell **out, const cc_shell_part *parts, uint32_t nparts, const cc_shell_opts *o, cc_error *err);
/* Per scene element (the src's rows): 1 exterior. Complete after the last step. */
const uint8_t *cc_shell_exterior(const cc_shell *s);
const uint8_t *cc_shell_exterior_of(const cc_shell *s, uint32_t part); /* part k's rows */
void cc_shell_get_stats(const cc_shell *s, cc_shell_stats *st);
void cc_shell_free(cc_shell *s);
#endif
