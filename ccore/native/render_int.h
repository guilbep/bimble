/* The native renderer's insides, shared by render.c (parts) and tiles_gpu.c (tile pyramids, NUI2). */
#ifndef BIMBLE_NATIVE_RENDER_INT_H
#define BIMBLE_NATIVE_RENDER_INT_H
#include "render.h"
#define NEAR_MM 10.0
#define MAX_PARTS 1024

/* MACAPP (ADR 0088): a shader's code in each format the build carries: SPIR-V (Vulkan) always; on macOS also the MSL
 * translated from that SPIR-V (shaders/gen/<name>.msl.h, SDL_shadercross). The device's own format is taken: MSL when
 * it takes MSL (Metal), else SPIR-V. NV_SHADER(X) names SPV_X and, on macOS, MSL_X. */
typedef struct {
    const uint32_t *spv;
    size_t spv_bytes;
    const char *msl; /* NULL: none in this build */
    size_t msl_bytes;
} nv_shader_code;
#ifdef __APPLE__
#define NV_SHADER(X) ((nv_shader_code){SPV_##X, sizeof SPV_##X, MSL_##X, sizeof MSL_##X - 1})
#else
#define NV_SHADER(X) ((nv_shader_code){SPV_##X, sizeof SPV_##X, NULL, 0})
#endif
/* the code, its size, its format and its entry point for this device (SPIRV-Cross names MSL's `main` main0) */
static inline SDL_GPUShaderFormat nv_shader_pick(SDL_GPUDevice *d, nv_shader_code c, const Uint8 **code, size_t *bytes, const char **entry) {
    if (c.msl && (SDL_GetGPUShaderFormats(d) & SDL_GPU_SHADERFORMAT_MSL)) {
        *code = (const Uint8 *)c.msl, *bytes = c.msl_bytes, *entry = "main0";
        return SDL_GPU_SHADERFORMAT_MSL;
    }
    *code = (const Uint8 *)c.spv, *bytes = c.spv_bytes, *entry = "main";
    return SDL_GPU_SHADERFORMAT_SPIRV;
}

/* NUI2: the tile path's state in the renderer (tiles_gpu.c) */
typedef struct {
    SDL_GPUGraphicsPipeline *face_w_back, *face_i_back, *proxy;
    SDL_GPUGraphicsPipeline *c_face_w, *c_face_w_back, *c_face_i, *c_face_i_back, *c_proxy; /* the clip-distance variants */
    SDL_GPUComputePipeline *hiz0, *hizn, *test, *gate, *hizn2;
    SDL_GPUTexture *hiz;          /* the depth pyramid: its levels side by side (R32F) */
    uint32_t hiz_w, hiz_h, hiz_levels, lv[16][3]; /* per level: x offset, w, h */
    /* per frame, grown as needed */
    SDL_GPUBuffer *cmd_a, *cmd_b, *gidx, *gates, *tests, *vis, *inst;
    uint32_t cap_cmd, cap_gates, cap_tests, cap_inst;
    SDL_GPUTransferBuffer *up, *down;
    uint32_t up_cap, down_cap;
    nv_tileset **sets;
    uint32_t nsets, csets;
    uint64_t frame;
    nv_tiles_opts o;
    /* the last frame */
    uint32_t ntests, test_cap_cpu;
    struct nv_ttest_map *map; /* per test: its set and tile, the triangles it would draw */
    uint64_t tris_pass1, tris_standin_always;
    uint32_t tiles_drawn, tiles_pass1, ntests_read;
    int read_vis, read_vis_planned, budget_full, squeeze, groups, seen_planned;
    SDL_GPUTransferBuffer *downs[2], *seen_downs[2];
    uint32_t down_caps[2], seen_down_caps[2];
    uint64_t noroom_frame;
    uint32_t nmap;
    SDL_GPUComputePipeline *seen;
    SDL_GPUBuffer *seen_buf;
    uint32_t seen_cap;
    SDL_GPUTransferBuffer *seen_down;
    double ms_prepare, ms_draw, ms_walk; /* the last frame's CPU: the walk and commands, the draws' recording */
    uint64_t gpu_bytes; /* resident tiles */
    uint32_t want_n;
    nv_tile_want want[256];
} nv_tiles_r;

struct nv_part {
    nv_scene sc; /* moved in: counts, ids, colours, cells (the geometry is on the GPU) */
    SDL_GPUBuffer *wv, *wi, *we, *sv, *si, *se, *plc, *visible, *items, *hidden, *fcmd, *ecmd, *counters;
    SDL_GPUBuffer *pv, *pi, *pe, *pfcmd, *pecmd;
    SDL_GPUTransferBuffer *up; /* per frame: the hidden cells and the proxies' commands */
    uint32_t up_size;
    uint32_t nw, nshape, ncmd, nitems, ncells;
    uint64_t gpu_bytes;
    uint32_t *hidden_cpu;
    SDL_GPUIndexedIndirectDrawCommand *pf_cpu;
    SDL_GPUIndirectDrawCommand *pe_cpu;
    uint32_t npcmd;
    int shown, use_hlod;
};

struct nv_render {
    SDL_GPUDevice *dev;
    int edges, hlod;
    float cut_px, hlod_px, crease;
    float bg[3];
    uint32_t W, H;
    SDL_GPUGraphicsPipeline *face_w, *face_i, *edge_w, *edge_i, *screen;
    SDL_GPUComputePipeline *cull, *reset;
    SDL_GPUBuffer *colour; /* the colour table: palette slots 0..7, then every element */
    uint32_t colour_cap;   /* entries in the GPU buffer */
    uint32_t ncolour;      /* entries used */
    uint32_t *colour_base, *colour_cpu;
    int colour_dirty;
    SDL_GPUTexture *color, *ids, *depth;
    SDL_GPUSampler *nearest;
    SDL_GPUTransferBuffer *rb; /* read-back: 16 bytes of counters per part, then the picked pixel */
    uint32_t rb_parts;
    uint64_t gpu_bytes; /* the renderer's own (targets, colour table) */
    nv_part **parts;
    uint32_t nparts, cparts;
    /* the last frame */
    uint32_t draws, commands, proxies_drawn;
    uint64_t tris;
    int readback, picking;
    uint32_t rb_count; /* parts read back */
    float clip_min[4], clip_max[4];
    unsigned skip; /* NUI2 bench: passes left out (NV_SKIP_*) to find where a frame's time goes */
    nv_tiles_r tl; /* NUI2: the tile pyramids (tiles_gpu.c) */
    /* NUI2, frames in flight: each frame recorded in a slot (its read-back buffers and what collect needs), collected in
     * order after its fence (the app may record the next frame before it waits for this one) */
    SDL_GPUTransferBuffer *rbs[2];
    uint32_t rbs_parts[2];
    struct { int readback, picking; uint32_t rb_count; } snap[2];
    int slot, pend[2], npend;
    uint64_t state_gen; /* bumped when an element's look changes (the walker descends under hidden or selected elements) */
    float last_vp[16];
};

void nv_tiles_init(struct nv_render *r);
int nv_tiles_targets(struct nv_render *r, uint32_t w, uint32_t h);
int nv_tiles_prepare(struct nv_render *r, SDL_GPUCommandBuffer *cmd, const float vp[16], const double eye[3], const float pl[5][4], double kpx, int ortho, float cut_px);
void nv_tiles_draw(struct nv_render *r, SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int pass);
int nv_tiles_has_pass2(const struct nv_render *r);
void nv_tiles_occlusion(struct nv_render *r, SDL_GPUCommandBuffer *cmd);
void nv_tiles_readback(struct nv_render *r, SDL_GPUCopyPass *cp);
void nv_tiles_seen(struct nv_render *r, SDL_GPUCommandBuffer *cmd);
uint64_t nv_tiles_collect(struct nv_render *r, int slot);
void nv_tiles_snapshot(struct nv_render *r, int slot);
uint64_t nv_tiles_pages_bytes(void);
void nv_tiles_release(struct nv_render *r);
/* records the tiles' passes: pass 1 inside `rp` (open), then the rest after it ends; returns the render pass for what follows */
#endif
