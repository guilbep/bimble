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
