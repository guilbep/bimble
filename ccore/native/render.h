/* The native renderer (NAT1, NAT2, ticket NUI1): the C core's scenes (native.h) drawn through SDL3's
 * GPU API into offscreen targets (RGBA8 colour, R32 element id, D32 depth), so that the app can draw
 * its UI over the picture in the same command buffer (ccore/native/ui/), or read it back.
 *
 * NUI1: the picture is made of PARTS: one per model (the open model, each link), and a link's proxies
 * (its cache's HLOD) as a part of their own until its detail arrives. A part is uploaded on any thread
 * (nv_part_upload: its own command buffers; the loader's threads do it), then added to the renderer on
 * the thread that draws (nv_render_add); a part removed is freed after the GPU is idle.
 *
 * Use:
 *   SDL_GPUDevice *dev = nv_gpu_device(NULL, NULL);        (SPIR-V, first-instance indirect)
 *   nv_render *r = nv_render_new(dev, &opts, err, n);
 *   nv_part *p = nv_part_upload(dev, &scene, err, n);       (any thread; frees the scene's CPU geometry)
 *   nv_render_add(r, p);
 *   nv_render_targets(r, w, h);
 *   nv_render_record(r, cmd, &cam, pick, px, py);          (into a command buffer of the caller's)
 *   ... submit, wait ...
 *   nv_render_collect(r, &picked);                          (counters and the picked pixel)
 * Element numbers are global (native.h: NV_ELEM0 on); a part's are [elem0, elem0 + nelements). */
#ifndef BIMBLE_NATIVE_RENDER_H
#define BIMBLE_NATIVE_RENDER_H
#include <SDL3/SDL.h>
#include <stdint.h>
#include "native.h"
#include "tiles.h"

enum { NV_EDGES_NONE, NV_EDGES_LINES, NV_EDGES_SCREEN };
extern const char *const NV_EDGES_NAME[3];

#define NV_PI 3.14159265358979323846
#define NV_FOV_Y (35.0 * NV_PI / 180.0)

/* The lab's orbit camera (glviewport.ts): a target, yaw and pitch about it, a distance (mm). NUI1:
 * `ortho` 1 draws it orthographic (plans, sections, Top), `ortho_h` the half height of the view (mm). */
typedef struct {
    double target[3], yaw, pitch, dist;
    int ortho;
    double ortho_h;
} nv_camera;

typedef struct {
    int edges;       /* NV_EDGES_* */
    int hlod;        /* draw the parts' proxies (HLOD cells) for far cells */
    float cut_px;    /* the cull's screen-size cut */
    float hlod_px;   /* a cell whose error projects under this is drawn as its proxy */
    float crease;    /* screen edges: 0 = the default */
} nv_render_opts;

typedef struct nv_render nv_render;
typedef struct nv_part nv_part;

typedef struct {
    uint32_t draws, commands, proxies, parts, tiles;
    uint64_t tris;      /* triangles drawn in the last frame */
    uint32_t tiles_resident, tests, pass1, wanted; /* NUI2: tile sets' resident tiles, boxes tested, drawn in pass 1, still wanted */
    uint64_t tile_bytes;
    int budget_full; /* tiles wanted but the GPU budget is full of tiles in use: their ancestors stand in */
    uint64_t gpu_bytes; /* buffers and targets of the renderer and its parts */
    uint32_t w, h;
} nv_render_stats;

/* The GPU device: Vulkan through SDL (SPIR-V shaders). NULL with SDL's error. */
SDL_GPUDevice *nv_gpu_device(const char **name, const char **driver);
nv_render *nv_render_new(SDL_GPUDevice *dev, const nv_render_opts *o, char *err, size_t errn);
void nv_render_free(nv_render *r);

/* Uploads `sc` as a part (any thread). The scene is moved into the part (`sc` is left empty) and its
 * CPU geometry freed; the part keeps its ids, colours, cells and counts. NULL when the GPU refuses a
 * buffer (`err`; `sc` freed). */
nv_part *nv_part_upload(SDL_GPUDevice *dev, nv_scene *sc, char *err, size_t errn);
void nv_part_free(SDL_GPUDevice *dev, nv_part *p); /* a part never added */
const nv_scene *nv_part_scene(const nv_part *p);
uint64_t nv_part_gpu_bytes(const nv_part *p);
/* Adds a part (the drawing thread). Its elements' colours join the colour table. 0 ok. */
int nv_render_add(nv_render *r, nv_part *p);
/* Removes a part and frees it (after waiting for the GPU). */
void nv_render_remove(nv_render *r, nv_part *p);
/* Shown or not (a link's eye). */
void nv_part_set_shown(nv_part *p, int shown);
int nv_part_shown(const nv_part *p);
/* The part an element belongs to (NULL none), and the element's index in it. */
nv_part *nv_render_part_of(const nv_render *r, uint32_t elem, uint32_t *local);
uint32_t nv_render_nparts(const nv_render *r);
nv_part *nv_render_part(const nv_render *r, uint32_t i);

/* (Re)makes the targets at w x h pixels. 0 ok. */
int nv_render_targets(nv_render *r, uint32_t w, uint32_t h);
void nv_render_set_edges(nv_render *r, int edges);
/* NUI2 bench (the setting bench_skip, native.json / --set): passes left out to find where a frame's time goes */
enum { NV_SKIP_CULL = 1, NV_SKIP_DRAW = 2, NV_SKIP_EDGES = 4, NV_SKIP_READBACK = 8, NV_SKIP_UI = 16, NV_SKIP_WALK = 32 };
void nv_render_set_skip(nv_render *r, unsigned bits);
int nv_render_edges(const nv_render *r);
void nv_render_set_cut(nv_render *r, float px);
void nv_render_set_hlod(nv_render *r, int on, float px);
/* An element's look: 0 its colour, 1 selected (the lab's selection colour), 2 hidden. Uploaded with
 * the next frame. */
void nv_render_set_state(nv_render *r, uint32_t elem, int state);
void nv_render_clear_states(nv_render *r);
/* The section box (scene mm): faces and line edges outside it are not drawn. */
void nv_render_set_clip(nv_render *r, int on, const double mn[3], const double mx[3]);
void nv_render_set_background(nv_render *r, const float rgb[3]);
/* Records the cull, the scene, the edges and (pick) a read-back of pixel (px, py) of the id target
 * into `cmd`. The picture is then in nv_render_color() (RGBA8, w x h). */
void nv_render_record(nv_render *r, SDL_GPUCommandBuffer *cmd, const nv_camera *cam, int pick, int px, int py);
/* After the command buffer's fence: the counters, and the picked element (0: none; else the global
 * element index + 1, as the id target holds it). */
void nv_render_collect(nv_render *r, uint32_t *picked);
SDL_GPUTexture *nv_render_color(const nv_render *r);
nv_render_stats nv_render_get_stats(const nv_render *r);
/* Writes `rgba` (w x h, RGBA8) as a PNG (SDL) or a PPM (by extension). */
int nv_write_image(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);

/* ---- NUI2: tile pyramids (tiles_gpu.c). A link with a section TILE is a tile set: its table of contents read at
 * open; each frame the walker (cc_tiles_select, through nv_tiles_walk: ONE place for its parameters) picks its
 * tiles; a tile's content (a leaf's elements, a proxy) is uploaded on a loader thread (nv_gtile_*), attached on the
 * drawing thread, released least recently used past the budget; a tile not yet there is covered by its nearest
 * uploaded ancestor's proxy. Occlusion: two passes over this frame's depth (the tiles visible last frame, a depth
 * pyramid, every other tile's box tested on the GPU, the newly visible drawn in the same frame). ---- */
typedef struct nv_tileset nv_tileset;
typedef struct nv_gtile nv_gtile;
typedef struct {
    int occlusion;       /* settings draw_visible: the two-pass test (1), or every selected tile drawn (0) */
    double error_px;     /* the walker's error in pixels (0: leaves only) */
    int lod;             /* levels of detail (0: every leaf in the frustum) */
    float min_px;        /* settings still_px: an element under this many pixels is skipped (0: off) */
    int cull_back;       /* settings cull_back: back faces of closed outward elements culled */
    int clip_planes;     /* section_clip: the section box by clip distances (1) or by discard in the fragment stage (0) */
    int groups;          /* occlusion_groups: occlusion down to groups of 8 elements (seen last frame: pass 1; else tested); 0: by tile */
    uint64_t budget;     /* the resident tiles' GPU bytes (0: none) */
    uint64_t tri_budget; /* the walker's triangle budget (0: none; the moving budget sets it) */
    int reference;       /* the truth test's reference frame: no occlusion, no state changed */
    int priority;        /* NUI3: shell_mode priority: the walk's tiles exterior first (CC_TILES_SEL_PRIORITY): fetched first */
} nv_tiles_opts;
typedef struct { nv_tileset *ts; uint32_t tile, rank; } nv_tile_want;

/* A set from a section's table of contents (its bytes [0, toc), copied) and occluders (nocc x 9 floats, copied; may be
 * NULL), placed by (c, s, off) like a part (nv_load_opts), its elements elem0.. (the link's rows), `colour` a link
 * colour (packed RGBA8) or 0. NULL with err. Its owner holds one reference. */
nv_tileset *nv_tileset_new(const uint8_t *toc, size_t n, uint32_t nelements, const void *occ, uint32_t nocc, uint32_t elem0, double c, double s,
                           const double off[3], uint32_t colour, char *err, size_t errn);
void nv_tileset_ref(nv_tileset *ts);   /* a loader job holds one */
/* the whole section kept with the set (the open model's: its proxies are read from it; a link's are read from its mapped
 * cache): taken (malloc'd, n bytes) */
void nv_tileset_keep_section(nv_tileset *ts, uint8_t *sec, size_t n);
/* NUI3: the cross-link shell's marks (session/xshell.h: a byte per entry of the element list, 1 exterior), copied; NULL
 * detaches. The priority walk orders the tiles holding a marked element first. 0 ok, -1 (n differs, no memory). */
int nv_tileset_set_xmask(nv_tileset *ts, const uint8_t *marks, uint32_t n);
/* NUI3: every tile released from the GPU (the measure of the time to the first correct picture after a jump) */
void nv_render_tiles_drop_all(nv_render *r);
void nv_tileset_section(const nv_tileset *ts, const uint8_t **sec, size_t *n);
void nv_tileset_unref(SDL_GPUDevice *dev, nv_tileset *ts);
const cc_tiles_toc *nv_tileset_toc(const nv_tileset *ts);
void nv_tileset_set_shown(nv_tileset *ts, int shown);
void nv_tileset_box(const nv_tileset *ts, double lo[3], double hi[3]); /* its root's box, scene mm */
uint64_t nv_tileset_gpu_bytes(const nv_tileset *ts);
uint32_t nv_tileset_resident(const nv_tileset *ts);
int nv_render_add_tileset(nv_render *r, nv_tileset *ts);    /* takes a reference */
void nv_render_remove_tileset(nv_render *r, nv_tileset *ts); /* drops it (its tiles freed with the last reference) */
/* a tile's content made into GPU buffers (any thread): a TLF1 leaf answer or a TPX1 proxy blob (untrusted, checked) */
nv_gtile *nv_gtile_leaf(SDL_GPUDevice *dev, const nv_tileset *ts, uint32_t tile, const uint8_t *tlf1, size_t n, char *err, size_t errn);
nv_gtile *nv_gtile_proxy(SDL_GPUDevice *dev, const nv_tileset *ts, uint32_t tile, const uint8_t *tpx1, size_t n, char *err, size_t errn);
void nv_gtile_free(SDL_GPUDevice *dev, nv_gtile *g);
uint64_t nv_gtile_bytes(const nv_gtile *g);
/* the drawing thread: a loaded tile in (NULL: it failed, not asked again) */
void nv_tileset_attach(nv_render *r, nv_tileset *ts, uint32_t tile, nv_gtile *g);
void nv_tileset_queued(nv_tileset *ts, uint32_t tile, int queued);
void nv_render_tiles_opts(nv_render *r, const nv_tiles_opts *o);
void nv_render_tiles_squeeze(nv_render *r);
void nv_tiles_group_size(uint32_t n);
void nv_tiles_surplus_top(void); /* NUI3: BIMBLE_TILES_SURPLUS=2, the largest elements drawn but not shown */
void nv_tiles_page_grow(uint32_t g); /* NUI3: page_grow, pages at most 2^g times the first (26 MB) */
void nv_tiles_tile_gate(int on);
void nv_tiles_cold(int on); /* NUI3: occlusion_cold, the elements hidden last frame under one gate a leaf (1 still frames, 2 all) */
void nv_tiles_hiz_pair(int on); /* NUI3: hiz_pair, two levels of the depth pyramid a dispatch */
void nv_tiles_seen_step(uint32_t s); /* NUI3: seen_step, the elements seen from one pixel in s a side */ /* NUI3: occlusion_tiles, a loaded leaf hidden last frame tested by its box alone */
void nv_tiles_cluster_tris(uint32_t n); /* NUI3: cluster_tris, a large element's triangles a cluster (0 off); read when a tile loads */
void nv_tiles_test_mode(int mode, int split, int span, int split_px); /* NUI3: occlusion_test (0 group box, 1 element boxes, 2 + own placed box), occlusion_split, occlusion_span, occlusion_split_px */
void nv_tiles_hiz_block(nv_render *r, uint32_t b); /* hiz_block: the depth pyramid's level 0, pixels a texel side (1, 2, 4) */
/* BIMBLE_TILES_SURPLUS=1: the last frame's leaf triangles: [0] of elements shown (the need), [1] pass 1, [2] pass 2, of elements
 * not shown: [3] in a group with one shown, [4] a group passing the box test alone, [5] drawn in pass 1 (seen before); [6] all */
void nv_tiles_surplus(double out[8]); /* occlusion_group: elements a group (default 16) */ /* a tile found no room in the budget's pages: release the unused */
nv_tiles_opts nv_render_get_tiles_opts(const nv_render *r);
/* after nv_render_collect: the tiles to load next, nearest and coarsest first (at most cap) */
uint32_t nv_render_tiles_want(nv_render *r, nv_tile_want *out, uint32_t cap);
/* element colours (packed RGBA8) for elements elem0.. of a tile set (its link's) */
int nv_render_add_colours(nv_render *r, uint32_t elem0, uint32_t n, const uint32_t *colours);
SDL_GPUTexture *nv_render_ids(const nv_render *r);
SDL_GPUTexture *nv_render_depth(const nv_render *r); /* the depth target (D32 float, reverse Z) */
void nv_tiles_debug_elem(nv_render *r, uint32_t e); /* the id target (R32 uint, w x h) */

/* The camera (glviewport.ts's rules, NAT1's code). */
void nv_cam_fit_box(nv_camera *c, const double lo[3], const double hi[3]);
void nv_cam_vp(const nv_camera *c, double aspect, float vp[16], double eye[3]);
void nv_cam_eye(const nv_camera *c, double e[3]);
void nv_cam_orbit(nv_camera *c, double dx_px, double dy_px);
void nv_cam_pan(nv_camera *c, double dx_px, double dy_px, double view_h_px);
/* steps > 0 zooms in, about the point under (mx, my) of a w x h view. */
void nv_cam_zoom_at(nv_camera *c, double steps, double mx, double my, double w, double h);
/* The ray under the pixel (x, y) of a w x h view, scene coordinates: origin o, direction d. */
void nv_cam_ray(const nv_camera *c, double x, double y, double w, double h, double o[3], double d[3]);
/* A scene point to view pixels (x, y); 0 when it is behind the eye. */
int nv_cam_project(const nv_camera *c, const double p[3], double w, double h, double out[2]);
#endif
