/* The native renderer (render.h): Bimble's look (the page's fragment shader), the cull pass,
 * instancing, HLOD proxies, line and screen-space edges, the id buffer; NUI1: the picture made of
 * parts (a model, a link, a link's proxies), each uploaded on its own (any thread) and drawn in one
 * render pass. The program around it (a window, a UI, a benchmark) is the caller's. */
#define _POSIX_C_SOURCE 200809L
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"
#include "render_int.h"

/* SPIR-V, compiled by glslc at build time (native/build.sh), embedded. */
static const uint32_t SPV_FACE_WORLD[] =
#include "face_world.vert.h"
    ;
static const uint32_t SPV_FACE_INST[] =
#include "face_inst.vert.h"
    ;
static const uint32_t SPV_EDGE_WORLD[] =
#include "edge_world.vert.h"
    ;
static const uint32_t SPV_EDGE_INST[] =
#include "edge_inst.vert.h"
    ;
static const uint32_t SPV_FACE_ID_FRAG[] =
#include "face_id.frag.h"
    ;
static const uint32_t SPV_CULL[] =
#include "cull.comp.h"
    ;
static const uint32_t SPV_RESET[] =
#include "reset.comp.h"
    ;
static const uint32_t SPV_SCREEN[] =
#include "screen.vert.h"
    ;
static const uint32_t SPV_SCREEN_EDGES[] =
#include "screen_edges.frag.h"
    ;

const char *const NV_EDGES_NAME[3] = {"none", "lines", "screen"};
static const float EDGE_SRGB[3] = {26 / 255.f, 26 / 255.f, 28 / 255.f};
static const uint8_t SELECTION_SRGB[3] = {215, 172, 182}; /* glviewport.ts SELECTION_SRGB */

typedef struct nv_render R;
typedef struct nv_part P;

/* ---- math (column-major 4x4, as GLSL) ---- */
static void mat_mul(float r[16], const float a[16], const float b[16]) {
    float t[16];
    for (int c = 0; c < 4; c++)
        for (int i = 0; i < 4; i++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + i] * b[c * 4 + k];
            t[c * 4 + i] = s;
        }
    memcpy(r, t, sizeof t);
}

void nv_cam_eye(const nv_camera *c, double e[3]) {
    double cp = cos(c->pitch);
    e[0] = c->target[0] + c->dist * cp * cos(c->yaw);
    e[1] = c->target[1] + c->dist * cp * sin(c->yaw);
    e[2] = c->target[2] + c->dist * sin(c->pitch);
}

/* The view's axes: f forward, s right, u up. */
static void cam_axes(const nv_camera *c, const double eye[3], double f[3], double s[3], double u[3]) {
    for (int i = 0; i < 3; i++) f[i] = c->target[i] - eye[i];
    double fl = sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-12) f[0] = 1, f[1] = 0, f[2] = 0, fl = 1;
    for (int i = 0; i < 3; i++) f[i] /= fl;
    /* s = f x up(0,0,1); at a pitch of +-90 degrees (Top) the yaw gives it */
    s[0] = f[1], s[1] = -f[0], s[2] = 0;
    double sl = sqrt(s[0] * s[0] + s[1] * s[1]);
    if (sl < 1e-9) s[0] = -sin(c->yaw), s[1] = cos(c->yaw), sl = 1;
    for (int i = 0; i < 3; i++) s[i] /= sl;
    u[0] = s[1] * f[2] - s[2] * f[1], u[1] = s[2] * f[0] - s[0] * f[2], u[2] = s[0] * f[1] - s[1] * f[0];
}

/* View-projection, reverse Z: perspective with an infinite far plane (depth 1 at NEAR_MM, 0 at
 * infinity); orthographic from 1 mm to 2 distances + 100 m (depth 1 near, 0 far). */
void nv_cam_vp(const nv_camera *c, double aspect, float vp[16], double eye[3]) {
    nv_cam_eye(c, eye);
    double f[3], s[3], u[3];
    cam_axes(c, eye, f, s, u);
    float v[16] = {0};
    for (int i = 0; i < 3; i++) v[i * 4 + 0] = (float)s[i], v[i * 4 + 1] = (float)u[i], v[i * 4 + 2] = (float)-f[i];
    v[12] = (float)-(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    v[13] = (float)-(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    v[14] = (float)(f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
    v[15] = 1;
    float p[16] = {0};
    if (c->ortho) {
        double h = c->ortho_h > 1e-6 ? c->ortho_h : 1, n = 1, fa = 2 * c->dist + 1e5;
        p[0] = (float)(1 / (h * aspect)), p[5] = (float)(1 / h);
        p[10] = (float)(1 / (fa - n)), p[14] = (float)(fa / (fa - n)), p[15] = 1;
    } else {
        double fy = 1.0 / tan(NV_FOV_Y / 2);
        p[0] = (float)(fy / aspect), p[5] = (float)fy, p[11] = -1, p[14] = (float)NEAR_MM;
    }
    mat_mul(vp, p, v);
}

/* The planes of the view (left, right, bottom, top, near), normalized: inside when n.p + d >= 0. */
static void frustum(const float m[16], float pl[5][4]) {
    float row[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) row[r][c] = m[c * 4 + r];
    for (int k = 0; k < 4; k++) {
        int axis = k / 2;
        float sgn = (k % 2) ? -1.f : 1.f;
        for (int c = 0; c < 4; c++) pl[k][c] = row[3][c] + sgn * row[axis][c];
    }
    for (int c = 0; c < 4; c++) pl[4][c] = row[3][c] - row[2][c]; /* z_ndc <= 1: not before the near plane */
    for (int k = 0; k < 5; k++) {
        float l = sqrtf(pl[k][0] * pl[k][0] + pl[k][1] * pl[k][1] + pl[k][2] * pl[k][2]);
        if (l > 0)
            for (int c = 0; c < 4; c++) pl[k][c] /= l;
    }
}

static int box_out(const float pl[5][4], const float b[6]) {
    for (int k = 0; k < 5; k++) {
        float c = 0, e = 0;
        for (int i = 0; i < 3; i++) {
            float ci = (b[i] + b[3 + i]) / 2, ei = (b[3 + i] - b[i]) / 2;
            c += pl[k][i] * ci, e += fabsf(pl[k][i]) * ei;
        }
        if (c + e + pl[k][3] < 0) return 1;
    }
    return 0;
}

void nv_cam_fit_box(nv_camera *c, const double lo[3], const double hi[3]) {
    double r2 = 0;
    for (int i = 0; i < 3; i++) {
        c->target[i] = (lo[i] + hi[i]) / 2;
        double h = (hi[i] - lo[i]) / 2;
        r2 += h * h;
    }
    double r = sqrt(r2);
    if (r < 1) r = 1000;
    c->dist = r / sin(NV_FOV_Y / 2) * 1.02;
    c->ortho_h = r * 1.05;
}

/* ---- GPU objects ---- */
static SDL_GPUShader *shader(SDL_GPUDevice *dev, const uint32_t *code, size_t bytes, SDL_GPUShaderStage st, uint32_t samplers,
                             uint32_t storage, uint32_t uniforms) {
    SDL_GPUShaderCreateInfo ci = {.code_size = bytes, .code = (const Uint8 *)code, .entrypoint = "main",
                                  .format = SDL_GPU_SHADERFORMAT_SPIRV, .stage = st, .num_samplers = samplers,
                                  .num_storage_buffers = storage, .num_uniform_buffers = uniforms};
    return SDL_CreateGPUShader(dev, &ci);
}

static SDL_GPUGraphicsPipeline *pipeline(R *r, SDL_GPUShader *vs, SDL_GPUShader *fs, int world, int lines, int with_id, int depth) {
    if (!vs || !fs) return NULL;
    SDL_GPUVertexBufferDescription vb = {.slot = 0, .pitch = world ? 16 : 12, .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
    SDL_GPUVertexAttribute va[2] = {
        {.location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = 0},
        {.location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT, .offset = 12},
    };
    SDL_GPUColorTargetDescription ct[2] = {
        {.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM},
        {.format = SDL_GPU_TEXTUREFORMAT_R32_UINT},
    };
    SDL_GPUGraphicsPipelineCreateInfo ci = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .vertex_input_state = {.vertex_buffer_descriptions = &vb, .num_vertex_buffers = depth ? 1 : 0,
                               .vertex_attributes = va, .num_vertex_attributes = depth ? (world ? 2 : 1) : 0},
        .primitive_type = lines ? SDL_GPU_PRIMITIVETYPE_LINELIST : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE,
                             .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
                             /* faces pushed back (reverse Z: smaller), the page's polygonOffset, so the edges win */
                             .enable_depth_bias = !lines && depth, .depth_bias_constant_factor = -1.0f,
                             .depth_bias_slope_factor = -1.5f},
        .depth_stencil_state = {.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, .enable_depth_test = depth != 0,
                                .enable_depth_write = depth != 0},
        .target_info = {.color_target_descriptions = ct, .num_color_targets = with_id ? 2 : 1,
                        .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT, .has_depth_stencil_target = depth != 0},
    };
    return SDL_CreateGPUGraphicsPipeline(r->dev, &ci);
}

/* A GPU buffer with `data` uploaded (in 64 MB chunks through one transfer buffer, its own command
 * buffers: any thread). NULL when refused. */
static SDL_GPUBuffer *buffer(SDL_GPUDevice *dev, SDL_GPUBufferUsageFlags usage, const void *data, size_t bytes, uint64_t *acc) {
    if (bytes == 0) bytes = 16, data = NULL;
    if (bytes > 0xffffffffu) return NULL;
    SDL_GPUBufferCreateInfo ci = {.usage = usage, .size = (Uint32)bytes};
    SDL_GPUBuffer *b = SDL_CreateGPUBuffer(dev, &ci);
    if (!b) return NULL;
    *acc += bytes;
    if (!data) return b;
    const size_t CHUNK = 64u << 20;
    size_t tsz = bytes < CHUNK ? bytes : CHUNK;
    SDL_GPUTransferBufferCreateInfo tci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (Uint32)tsz};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tci);
    if (!tb) { SDL_ReleaseGPUBuffer(dev, b); return NULL; }
    for (size_t off = 0; off < bytes; off += tsz) {
        size_t n = bytes - off < tsz ? bytes - off : tsz;
        void *m = SDL_MapGPUTransferBuffer(dev, tb, false);
        if (!m) { SDL_ReleaseGPUTransferBuffer(dev, tb), SDL_ReleaseGPUBuffer(dev, b); return NULL; }
        memcpy(m, (const char *)data + off, n);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (!cmd) { SDL_ReleaseGPUTransferBuffer(dev, tb), SDL_ReleaseGPUBuffer(dev, b); return NULL; }
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src = {.transfer_buffer = tb, .offset = 0};
        SDL_GPUBufferRegion dst = {.buffer = b, .offset = (Uint32)off, .size = (Uint32)n};
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (f) SDL_WaitForGPUFences(dev, true, &f, 1), SDL_ReleaseGPUFence(dev, f);
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return b;
}

static int targets(R *r, uint32_t w, uint32_t h) {
    if (r->color) {
        SDL_ReleaseGPUTexture(r->dev, r->color), SDL_ReleaseGPUTexture(r->dev, r->ids), SDL_ReleaseGPUTexture(r->dev, r->depth);
        r->gpu_bytes -= (uint64_t)r->W * r->H * 12;
        r->color = r->ids = r->depth = NULL;
    }
    r->W = w, r->H = h;
    SDL_GPUTextureCreateInfo ci = {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                   .usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                   .width = w, .height = h, .layer_count_or_depth = 1, .num_levels = 1};
    r->color = SDL_CreateGPUTexture(r->dev, &ci);
    ci.format = SDL_GPU_TEXTUREFORMAT_R32_UINT;
    r->ids = SDL_CreateGPUTexture(r->dev, &ci);
    ci.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    r->depth = SDL_CreateGPUTexture(r->dev, &ci);
    if (!r->color || !r->ids || !r->depth) return -1;
    r->gpu_bytes += (uint64_t)w * h * 12;
    if (nv_tiles_targets(r, w, h)) fprintf(stderr, "bimble: no depth pyramid (%s): tiles drawn without occlusion\n", SDL_GetError());
    return 0;
}

static int make_pipelines(R *r) {
    SDL_GPUDevice *d = r->dev;
    SDL_GPUShader *fw = shader(d, SPV_FACE_WORLD, sizeof SPV_FACE_WORLD, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1);
    SDL_GPUShader *fi = shader(d, SPV_FACE_INST, sizeof SPV_FACE_INST, SDL_GPU_SHADERSTAGE_VERTEX, 0, 3, 1);
    SDL_GPUShader *ew = shader(d, SPV_EDGE_WORLD, sizeof SPV_EDGE_WORLD, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1);
    SDL_GPUShader *ei = shader(d, SPV_EDGE_INST, sizeof SPV_EDGE_INST, SDL_GPU_SHADERSTAGE_VERTEX, 0, 3, 1);
    SDL_GPUShader *ff = shader(d, SPV_FACE_ID_FRAG, sizeof SPV_FACE_ID_FRAG, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 1);
    SDL_GPUShader *sv = shader(d, SPV_SCREEN, sizeof SPV_SCREEN, SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 0);
    SDL_GPUShader *sf = shader(d, SPV_SCREEN_EDGES, sizeof SPV_SCREEN_EDGES, SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 0, 1);
    r->face_w = pipeline(r, fw, ff, 1, 0, 1, 1);
    r->face_i = pipeline(r, fi, ff, 0, 0, 1, 1);
    r->edge_w = pipeline(r, ew, ff, 1, 1, 1, 1);
    r->edge_i = pipeline(r, ei, ff, 0, 1, 1, 1);
    r->screen = pipeline(r, sv, sf, 0, 0, 0, 0);
    SDL_GPUShader *all[] = {fw, fi, ew, ei, ff, sv, sf};
    for (size_t k = 0; k < 7; k++) if (all[k]) SDL_ReleaseGPUShader(d, all[k]);
    SDL_GPUComputePipelineCreateInfo cc = {.code_size = sizeof SPV_CULL, .code = (const Uint8 *)SPV_CULL, .entrypoint = "main",
                                           .format = SDL_GPU_SHADERFORMAT_SPIRV, .num_readonly_storage_buffers = 2,
                                           .num_readwrite_storage_buffers = 4, .num_uniform_buffers = 1,
                                           .threadcount_x = 64, .threadcount_y = 1, .threadcount_z = 1};
    r->cull = SDL_CreateGPUComputePipeline(d, &cc);
    SDL_GPUComputePipelineCreateInfo rc = {.code_size = sizeof SPV_RESET, .code = (const Uint8 *)SPV_RESET, .entrypoint = "main",
                                           .format = SDL_GPU_SHADERFORMAT_SPIRV, .num_readwrite_storage_buffers = 3,
                                           .num_uniform_buffers = 1, .threadcount_x = 64, .threadcount_y = 1, .threadcount_z = 1};
    r->reset = SDL_CreateGPUComputePipeline(d, &rc);
    SDL_GPUSamplerCreateInfo si = {.min_filter = SDL_GPU_FILTER_NEAREST, .mag_filter = SDL_GPU_FILTER_NEAREST,
                                   .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
                                   .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                   .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
                                   .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE};
    r->nearest = SDL_CreateGPUSampler(d, &si);
    return r->face_w && r->face_i && r->edge_w && r->edge_i && r->screen && r->cull && r->reset && r->nearest ? 0 : -1;
}

#define USE_V SDL_GPU_BUFFERUSAGE_VERTEX
#define USE_I SDL_GPU_BUFFERUSAGE_INDEX
#define USE_S SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ
#define USE_CR SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ
#define USE_CW SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE
#define USE_IND SDL_GPU_BUFFERUSAGE_INDIRECT

static void part_release(SDL_GPUDevice *d, P *p) {
    SDL_GPUBuffer *bufs[] = {p->wv, p->wi, p->we, p->sv, p->si, p->se, p->plc, p->visible, p->items, p->hidden,
                             p->fcmd, p->ecmd, p->counters, p->pv, p->pi, p->pe, p->pfcmd, p->pecmd};
    for (size_t k = 0; k < sizeof bufs / sizeof *bufs; k++) if (bufs[k]) SDL_ReleaseGPUBuffer(d, bufs[k]);
    if (p->up) SDL_ReleaseGPUTransferBuffer(d, p->up);
    free(p->hidden_cpu), free(p->pf_cpu), free(p->pe_cpu);
    nv_scene_free(&p->sc);
    free(p);
}

void nv_part_free(SDL_GPUDevice *dev, nv_part *p) { if (p) part_release(dev, p); }

nv_part *nv_part_upload(SDL_GPUDevice *dev, nv_scene *sc, char *err, size_t errn) {
    P *p = calloc(1, sizeof *p);
    if (!p) { snprintf(err, errn, "out of memory"); nv_scene_free(sc); return NULL; }
    p->sc = *sc;
    memset(sc, 0, sizeof *sc);
    nv_scene *s = &p->sc;
    p->shown = 1;
    p->nw = (uint32_t)s->welems.n, p->nshape = (uint32_t)s->shapes.n, p->ncmd = p->nw + p->nshape;
    p->nitems = (uint32_t)s->items.n, p->ncells = (uint32_t)s->cells.n;
    uint64_t *g = &p->gpu_bytes;
    int ok = 1;
#define B(field, usage, data, bytes) ok = ok && (p->field = buffer(dev, usage, data, bytes, g)) != NULL
    B(wv, USE_V, s->wv.p, s->wv.n * sizeof(nv_vtx));
    B(wi, USE_I, s->wi.p, s->wi.n * 4);
    B(we, USE_V, s->we.p, s->we.n * sizeof(nv_vtx));
    B(sv, USE_V, s->sv.p, s->sv.n * 12);
    B(si, USE_I, s->si.p, s->si.n * 4);
    B(se, USE_V, s->se.p, s->se.n * 12);
    B(plc, USE_S, s->plc.p, s->plc.n * sizeof(nv_plc));
    B(visible, USE_S | USE_CW, NULL, s->plc.n * 4);
    { /* the draw commands: one per merged element, then one per shape; the cull sets the counts */
        SDL_GPUIndexedIndirectDrawCommand *fc = calloc(p->ncmd + 1, sizeof *fc);
        SDL_GPUIndirectDrawCommand *ec = calloc(p->ncmd + 1, sizeof *ec);
        if (!fc || !ec) ok = 0;
        const nv_welem *we = s->welems.p;
        const nv_shape *sh = s->shapes.p;
        for (uint32_t k = 0; ok && k < p->nw; k++) {
            fc[k].num_indices = we[k].nidx, fc[k].first_index = we[k].first_index;
            ec[k].num_vertices = we[k].nedge, ec[k].first_vertex = we[k].first_edge;
        }
        for (uint32_t k = 0; ok && k < p->nshape; k++) {
            SDL_GPUIndexedIndirectDrawCommand *f = &fc[p->nw + k];
            f->num_indices = sh[k].nidx, f->first_index = sh[k].first_index, f->first_instance = sh[k].first_inst;
            SDL_GPUIndirectDrawCommand *e = &ec[p->nw + k];
            e->num_vertices = sh[k].nedge, e->first_vertex = sh[k].first_edge, e->first_instance = sh[k].first_inst;
        }
        B(fcmd, USE_IND | USE_CW | USE_CR, fc, p->ncmd * sizeof *fc);
        B(ecmd, USE_IND | USE_CW | USE_CR, ec, p->ncmd * sizeof *ec);
        free(fc), free(ec);
    }
    B(items, USE_CR, s->items.p, s->items.n * sizeof(nv_item));
    B(counters, USE_CW | USE_CR, NULL, 16);
    B(hidden, USE_CR, NULL, (p->ncells + 1) * 4ull);
    if (p->ncells) {
        B(pv, USE_V, s->pv.p, s->pv.n * sizeof(nv_vtx));
        B(pi, USE_I, s->pi.p, s->pi.n * 4);
        B(pe, USE_V, s->pe.p, s->pe.n * sizeof(nv_vtx));
        B(pfcmd, USE_IND, NULL, (p->ncells + 1) * sizeof(SDL_GPUIndexedIndirectDrawCommand));
        B(pecmd, USE_IND, NULL, (p->ncells + 1) * sizeof(SDL_GPUIndirectDrawCommand));
        p->hidden_cpu = calloc(p->ncells + 1, 4);
        p->pf_cpu = calloc(p->ncells + 1, sizeof *p->pf_cpu);
        p->pe_cpu = calloc(p->ncells + 1, sizeof *p->pe_cpu);
        p->up_size = (p->ncells + 1) * (uint32_t)(4 + sizeof(SDL_GPUIndexedIndirectDrawCommand) + sizeof(SDL_GPUIndirectDrawCommand));
        SDL_GPUTransferBufferCreateInfo tci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = p->up_size};
        p->up = SDL_CreateGPUTransferBuffer(dev, &tci);
        ok = ok && p->hidden_cpu && p->pf_cpu && p->pe_cpu && p->up;
    }
#undef B
    /* the GPU holds the geometry now: the CPU copies go (the counts stay) */
    size_t nwi = s->wi.n, nwe = s->we.n, nplc = s->plc.n;
    nv_vec_free(&s->wv), nv_vec_free(&s->wi), nv_vec_free(&s->we), nv_vec_free(&s->sv), nv_vec_free(&s->si);
    nv_vec_free(&s->se), nv_vec_free(&s->plc), nv_vec_free(&s->pv), nv_vec_free(&s->pi), nv_vec_free(&s->pe), nv_vec_free(&s->items);
    nv_vec_free(&s->welems);
    s->wi.n = nwi, s->we.n = nwe, s->plc.n = nplc;
    if (!ok) {
        snprintf(err, errn, "the GPU refused a buffer (%s)", SDL_GetError());
        part_release(dev, p);
        return NULL;
    }
    return p;
}

const nv_scene *nv_part_scene(const nv_part *p) { return &p->sc; }
uint64_t nv_part_gpu_bytes(const nv_part *p) { return p->gpu_bytes; }
void nv_part_set_shown(nv_part *p, int shown) { p->shown = shown != 0; }
int nv_part_shown(const nv_part *p) { return p->shown; }
uint32_t nv_render_nparts(const nv_render *r) { return r->nparts; }
nv_part *nv_render_part(const nv_render *r, uint32_t i) { return i < r->nparts ? r->parts[i] : NULL; }

nv_part *nv_render_part_of(const nv_render *r, uint32_t elem, uint32_t *local) {
    for (uint32_t k = 0; k < r->nparts; k++) {
        const nv_scene *s = &r->parts[k]->sc;
        if (s->nelements && elem >= s->elem0 && elem < s->elem0 + s->nelements) {
            if (local) *local = elem - s->elem0;
            return r->parts[k];
        }
    }
    return NULL;
}

/* The colour table holds `n` entries at least (CPU and GPU). 0 ok. */
static int colour_room(R *r, uint32_t n) {
    if (n > r->ncolour) {
        uint32_t *b = realloc(r->colour_base, (size_t)n * 4), *c = b ? realloc(r->colour_cpu, (size_t)n * 4) : NULL;
        if (b) r->colour_base = b;
        if (!b || !c) return -1;
        r->colour_cpu = c;
        for (uint32_t k = r->ncolour; k < n; k++) r->colour_base[k] = r->colour_cpu[k] = nv_category_colour(7);
        r->ncolour = n;
        r->colour_dirty = 1;
    }
    if (n > r->colour_cap) {
        uint32_t cap = r->colour_cap ? r->colour_cap : 65536;
        while (cap < n) cap *= 2;
        SDL_GPUBuffer *nb = buffer(r->dev, USE_S, NULL, (size_t)cap * 4, &r->gpu_bytes);
        if (!nb) return -1;
        if (r->colour) SDL_WaitForGPUIdle(r->dev), SDL_ReleaseGPUBuffer(r->dev, r->colour), r->gpu_bytes -= (uint64_t)r->colour_cap * 4;
        r->colour = nb, r->colour_cap = cap, r->colour_dirty = 1;
    }
    return 0;
}

int nv_render_add(R *r, nv_part *p) {
    if (r->nparts >= MAX_PARTS) return -1;
    if (r->nparts == r->cparts) {
        uint32_t c = r->cparts ? r->cparts * 2 : 32;
        nv_part **np = realloc(r->parts, c * sizeof *np);
        if (!np) return -1;
        r->parts = np, r->cparts = c;
    }
    const nv_scene *s = &p->sc;
    if (s->nelements) {
        if (colour_room(r, s->elem0 + s->nelements)) return -1;
        memcpy(r->colour_base + s->elem0, s->colour.p, s->nelements * 4ull);
        memcpy(r->colour_cpu + s->elem0, s->colour.p, s->nelements * 4ull);
        r->colour_dirty = 1;
    }
    r->parts[r->nparts++] = p;
    return 0;
}

void nv_render_remove(R *r, nv_part *p) {
    for (uint32_t k = 0; k < r->nparts; k++)
        if (r->parts[k] == p) {
            memmove(&r->parts[k], &r->parts[k + 1], (r->nparts - k - 1) * sizeof *r->parts);
            r->nparts--;
            SDL_WaitForGPUIdle(r->dev);
            part_release(r->dev, p);
            return;
        }
}

/* HLOD (the page's rule, gl/proxies.ts): from each root, a cell in view whose proxy's error projects
 * under hlod_px is drawn as its proxy and its leaves' elements are left out; else descend. A part of
 * proxies alone draws, in each branch, the first cell small enough or the last one with a proxy. */
static void hlod_walk(R *r, P *p, const float pl[5][4], const double eye[3], float px_per, int ortho) {
    const nv_cell *cells = p->sc.cells.p;
    const uint32_t *kids = p->sc.kids.p, *roots = p->sc.roots.p;
    memset(p->hidden_cpu, 0, p->ncells * 4ull);
    p->npcmd = 0;
    uint32_t stack[4096];
    int alone = p->sc.proxies_only;
    for (size_t k = 0; k < p->sc.roots.n; k++) {
        int top = 0;
        stack[top++] = roots[k];
        while (top) {
            uint32_t c = stack[--top];
            if (c >= p->ncells) continue;
            const nv_cell *cl = &cells[c];
            if (box_out(pl, cl->bounds)) continue;
            double d = 1;
            if (!ortho) {
                double d2 = 0;
                for (int i = 0; i < 3; i++) {
                    double v = eye[i] < cl->bounds[i] ? cl->bounds[i] - eye[i] : eye[i] > cl->bounds[3 + i] ? eye[i] - cl->bounds[3 + i] : 0;
                    d2 += v * v;
                }
                d = sqrt(d2);
            }
            int small = d > 0 && cl->error_mm * px_per / d < r->hlod_px;
            int finer = 0;
            for (uint32_t j = 0; j < cl->nchildren; j++) {
                uint32_t x = kids[cl->first_child + j];
                if (x < p->ncells && cells[x].has_proxy) finer = 1;
            }
            if (cl->has_proxy && (small || (alone && !finer))) {
                SDL_GPUIndexedIndirectDrawCommand *f = &p->pf_cpu[p->npcmd];
                f->num_indices = cl->pnidx, f->num_instances = 1, f->first_index = cl->pfirst_index;
                f->vertex_offset = 0, f->first_instance = 0;
                SDL_GPUIndirectDrawCommand *e = &p->pe_cpu[p->npcmd++];
                e->num_vertices = cl->pnedge, e->num_instances = 1, e->first_vertex = cl->pfirst_edge, e->first_instance = 0;
                uint32_t st2[4096]; /* every leaf under it: hidden */
                int t2 = 0;
                st2[t2++] = c;
                while (t2) {
                    uint32_t x = st2[--t2];
                    if (x >= p->ncells) continue;
                    if (cells[x].leaf) p->hidden_cpu[x] = 1;
                    for (uint32_t j = 0; j < cells[x].nchildren && t2 < 4096; j++) st2[t2++] = kids[cells[x].first_child + j];
                }
                continue;
            }
            for (uint32_t j = 0; j < cl->nchildren && top < 4096; j++) stack[top++] = kids[cl->first_child + j];
        }
    }
}

typedef struct {
    float vp[16];
    float bias[4];
    float cmin[4], cmax[4]; /* NUI2: the section box for the clip-distance variants */
} VUni;
typedef struct {
    float light[4];
    float edge[4];
    float bmin[4], bmax[4]; /* the section box (bmin[3] 1: on) */
} FUni;
typedef struct {
    float plane[5][4];
    float eye[4];
    uint32_t n[4];
    float cut[4];
} CUni;

static int rb_room(R *r, uint32_t parts) {
    if (r->rb && r->rb_parts >= parts) return 0;
    uint32_t c = parts < 64 ? 64 : parts * 2;
    SDL_GPUTransferBufferCreateInfo rci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = 16 * c + 64};
    SDL_GPUTransferBuffer *t = SDL_CreateGPUTransferBuffer(r->dev, &rci);
    if (!t) return -1;
    if (r->rb) SDL_ReleaseGPUTransferBuffer(r->dev, r->rb);
    r->rb = t, r->rb_parts = c;
    return 0;
}

void nv_render_record(R *r, SDL_GPUCommandBuffer *cmd, const nv_camera *cam, int pick, int pick_x, int pick_y) {
    float vp[16];
    double eye[3];
    nv_cam_vp(cam, (double)r->W / r->H, vp, eye);
    float pl[5][4];
    frustum(vp, pl);
    float px_per = cam->ortho ? (float)(r->H / 2.0 / (cam->ortho_h > 1e-6 ? cam->ortho_h : 1)) : (float)(r->H / 2.0 / tan(NV_FOV_Y / 2));
    r->draws = 0, r->commands = 0, r->proxies_drawn = 0;
    if (r->colour_dirty && r->colour && r->ncolour) { /* element looks changed: the table again */
        uint32_t bytes = r->ncolour * 4;
        SDL_GPUTransferBufferCreateInfo tci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = bytes};
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(r->dev, &tci);
        void *m = tb ? SDL_MapGPUTransferBuffer(r->dev, tb, false) : NULL;
        if (m) {
            memcpy(m, r->colour_cpu, bytes), SDL_UnmapGPUTransferBuffer(r->dev, tb);
            SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTransferBufferLocation src = {.transfer_buffer = tb, .offset = 0};
            SDL_GPUBufferRegion dst = {.buffer = r->colour, .offset = 0, .size = bytes};
            SDL_UploadToGPUBuffer(cp, &src, &dst, false);
            SDL_EndGPUCopyPass(cp);
            r->colour_dirty = 0;
        }
        if (tb) SDL_ReleaseGPUTransferBuffer(r->dev, tb); /* released once the command buffer is done */
    }
    /* per part: the HLOD walk (CPU), then reset and cull (GPU) */
    for (uint32_t k = 0; k < r->nparts; k++) {
        P *p = r->parts[k];
        p->use_hlod = p->shown && p->ncells && (r->hlod || p->sc.proxies_only) && !(r->skip & NV_SKIP_WALK);
        if (!p->shown) continue;
        if (p->use_hlod) {
            hlod_walk(r, p, pl, eye, px_per, cam->ortho);
            r->proxies_drawn += p->npcmd;
            uint8_t *m = SDL_MapGPUTransferBuffer(r->dev, p->up, true);
            if (m) {
                memcpy(m, p->hidden_cpu, p->ncells * 4ull);
                size_t o1 = p->ncells * 4ull, o2 = o1 + p->npcmd * sizeof *p->pf_cpu;
                memcpy(m + o1, p->pf_cpu, p->npcmd * sizeof *p->pf_cpu);
                memcpy(m + o2, p->pe_cpu, p->npcmd * sizeof *p->pe_cpu);
                SDL_UnmapGPUTransferBuffer(r->dev, p->up);
                SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
                SDL_GPUTransferBufferLocation src = {.transfer_buffer = p->up, .offset = 0};
                SDL_GPUBufferRegion dst = {.buffer = p->hidden, .offset = 0, .size = p->ncells * 4};
                SDL_UploadToGPUBuffer(cp, &src, &dst, true);
                if (p->npcmd) {
                    src.offset = (Uint32)o1;
                    dst = (SDL_GPUBufferRegion){.buffer = p->pfcmd, .offset = 0, .size = (Uint32)(p->npcmd * sizeof *p->pf_cpu)};
                    SDL_UploadToGPUBuffer(cp, &src, &dst, true);
                    src.offset = (Uint32)o2;
                    dst = (SDL_GPUBufferRegion){.buffer = p->pecmd, .offset = 0, .size = (Uint32)(p->npcmd * sizeof *p->pe_cpu)};
                    SDL_UploadToGPUBuffer(cp, &src, &dst, true);
                }
                SDL_EndGPUCopyPass(cp);
            } else p->npcmd = 0, p->use_hlod = 0;
        }
        if (!p->nitems || (r->skip & NV_SKIP_CULL)) continue;
        SDL_GPUStorageBufferReadWriteBinding rw[3] = {{.buffer = p->fcmd}, {.buffer = p->ecmd}, {.buffer = p->counters}};
        uint32_t rn[4] = {p->ncmd, p->ncmd, 0, 0};
        SDL_PushGPUComputeUniformData(cmd, 0, rn, sizeof rn);
        SDL_GPUComputePass *c1 = SDL_BeginGPUComputePass(cmd, NULL, 0, rw, 3);
        SDL_BindGPUComputePipeline(c1, r->reset);
        SDL_DispatchGPUCompute(c1, (p->ncmd + 63) / 64 + 1, 1, 1);
        SDL_EndGPUComputePass(c1);
        CUni cu;
        memcpy(cu.plane, pl, sizeof pl);
        cu.eye[0] = (float)eye[0], cu.eye[1] = (float)eye[1], cu.eye[2] = (float)eye[2], cu.eye[3] = px_per;
        cu.n[0] = p->nitems, cu.n[1] = r->edges == NV_EDGES_LINES, cu.n[2] = p->use_hlod, cu.n[3] = cam->ortho ? 1 : 0;
        cu.cut[0] = r->cut_px, cu.cut[1] = cu.cut[2] = cu.cut[3] = 0;
        SDL_PushGPUComputeUniformData(cmd, 0, &cu, sizeof cu);
        SDL_GPUStorageBufferReadWriteBinding rw2[4] = {{.buffer = p->fcmd}, {.buffer = p->ecmd}, {.buffer = p->visible}, {.buffer = p->counters}};
        SDL_GPUComputePass *c2 = SDL_BeginGPUComputePass(cmd, NULL, 0, rw2, 4);
        SDL_BindGPUComputePipeline(c2, r->cull);
        SDL_GPUBuffer *ro[2] = {p->items, p->hidden};
        SDL_BindGPUComputeStorageBuffers(c2, 0, ro, 2);
        SDL_DispatchGPUCompute(c2, (p->nitems + 63) / 64, 1, 1);
        SDL_EndGPUComputePass(c2);
    }
    memcpy(r->last_vp, vp, sizeof r->last_vp);
    double tp0 = nv_now_ms();
    int tiles = nv_tiles_prepare(r, cmd, vp, eye, pl, cam->ortho ? px_per : r->H / 2.0 / tan(NV_FOV_Y / 2), cam->ortho, r->cut_px);
    r->tl.ms_prepare = nv_now_ms() - tp0;
    VUni vu;
    memcpy(vu.vp, vp, sizeof vp);
    vu.bias[0] = 1e-4f, vu.bias[1] = vu.bias[2] = vu.bias[3] = 0;
    memcpy(vu.cmin, r->clip_min, sizeof vu.cmin), memcpy(vu.cmax, r->clip_max, sizeof vu.cmax);
    SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof vu);
    FUni fu;
    double l[3] = {eye[0] - cam->target[0], eye[1] - cam->target[1], eye[2] - cam->target[2]};
    double ll = sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (ll < 1e-9) ll = 1, l[2] = 1;
    double lv[3] = {l[0] / ll - 0.3, l[1] / ll - 0.2, l[2] / ll + 0.9};
    ll = sqrt(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]);
    for (int i = 0; i < 3; i++) fu.light[i] = (float)(lv[i] / ll), fu.edge[i] = EDGE_SRGB[i];
    fu.light[3] = 0.62f, fu.edge[3] = 1;
    memcpy(fu.bmin, r->clip_min, sizeof fu.bmin), memcpy(fu.bmax, r->clip_max, sizeof fu.bmax);
    SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof fu);

    SDL_GPUColorTargetInfo ct[2] = {
        {.texture = r->color, .clear_color = {r->bg[0], r->bg[1], r->bg[2], 1}, .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE},
        {.texture = r->ids, .clear_color = {0, 0, 0, 0}, .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE},
    };
    SDL_GPUDepthStencilTargetInfo dt = {.texture = r->depth, .clear_depth = 0, .load_op = SDL_GPU_LOADOP_CLEAR,
                                        .store_op = SDL_GPU_STOREOP_STORE, .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
                                        .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE};
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, ct, 2, &dt);
    SDL_GPUBuffer *st1[1] = {r->colour};
    for (int pass = 0; pass < 2 && r->colour && !(r->skip & NV_SKIP_DRAW); pass++) { /* faces, then edges */
        int edges = pass == 1;
        if (edges && r->edges != NV_EDGES_LINES) break;
        for (uint32_t k = 0; k < r->nparts; k++) {
            P *p = r->parts[k];
            if (!p->shown) continue;
            SDL_BindGPUGraphicsPipeline(rp, edges ? r->edge_w : r->face_w);
            SDL_BindGPUVertexStorageBuffers(rp, 0, st1, 1);
            if (p->nw) { /* merged */
                SDL_GPUBufferBinding vb = {.buffer = edges ? p->we : p->wv, .offset = 0};
                SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                if (edges) SDL_DrawGPUPrimitivesIndirect(rp, p->ecmd, 0, p->nw);
                else {
                    SDL_GPUBufferBinding ib = {.buffer = p->wi, .offset = 0};
                    SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                    SDL_DrawGPUIndexedPrimitivesIndirect(rp, p->fcmd, 0, p->nw);
                }
                r->draws++, r->commands += p->nw;
            }
            if (p->use_hlod && p->npcmd) { /* proxies (merged vertex layout) */
                SDL_GPUBufferBinding pb = {.buffer = edges ? p->pe : p->pv, .offset = 0};
                SDL_BindGPUVertexBuffers(rp, 0, &pb, 1);
                if (edges) SDL_DrawGPUPrimitivesIndirect(rp, p->pecmd, 0, p->npcmd);
                else {
                    SDL_GPUBufferBinding ib = {.buffer = p->pi, .offset = 0};
                    SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                    SDL_DrawGPUIndexedPrimitivesIndirect(rp, p->pfcmd, 0, p->npcmd);
                }
                r->draws++, r->commands += p->npcmd;
            }
            if (p->nshape && !(r->skip & NV_SKIP_DRAW)) { /* shapes, instanced */
                SDL_GPUBuffer *st3[3] = {r->colour, p->plc, p->visible};
                SDL_BindGPUGraphicsPipeline(rp, edges ? r->edge_i : r->face_i);
                SDL_BindGPUVertexStorageBuffers(rp, 0, st3, 3);
                SDL_GPUBufferBinding sb = {.buffer = edges ? p->se : p->sv, .offset = 0};
                SDL_BindGPUVertexBuffers(rp, 0, &sb, 1);
                if (edges) SDL_DrawGPUPrimitivesIndirect(rp, p->ecmd, p->nw * (Uint32)sizeof(SDL_GPUIndirectDrawCommand), p->nshape);
                else {
                    SDL_GPUBufferBinding ib = {.buffer = p->si, .offset = 0};
                    SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                    SDL_DrawGPUIndexedPrimitivesIndirect(rp, p->fcmd, p->nw * (Uint32)sizeof(SDL_GPUIndexedIndirectDrawCommand), p->nshape);
                }
                r->draws++, r->commands += p->nshape;
            }
        }
    }
    double td0 = nv_now_ms();
    if (tiles && !(r->skip & NV_SKIP_DRAW)) nv_tiles_draw(r, cmd, rp, 1);
    r->tl.ms_draw = nv_now_ms() - td0;
    SDL_EndGPURenderPass(rp);
    if (tiles && nv_tiles_has_pass2(r)) { /* occlusion: the depth pyramid, the tests, then what they found visible */
        nv_tiles_occlusion(r, cmd);
        SDL_GPUColorTargetInfo c2[2] = {
            {.texture = r->color, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE},
            {.texture = r->ids, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE},
        };
        SDL_GPUDepthStencilTargetInfo d2 = {.texture = r->depth, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE,
                                            .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE, .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE};
        SDL_GPURenderPass *rp2 = SDL_BeginGPURenderPass(cmd, c2, 2, &d2);
        SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof vu);
        SDL_PushGPUFragmentUniformData(cmd, 0, &fu, sizeof fu);
        double td1 = nv_now_ms();
        if (!(r->skip & NV_SKIP_DRAW)) nv_tiles_draw(r, cmd, rp2, 2);
        r->tl.ms_draw += nv_now_ms() - td1;
        SDL_EndGPURenderPass(rp2);
        nv_tiles_seen(r, cmd);
    }
    if (r->edges == NV_EDGES_SCREEN && !(r->skip & NV_SKIP_EDGES)) {
        SDL_GPUColorTargetInfo c2 = {.texture = r->color, .load_op = SDL_GPU_LOADOP_LOAD, .store_op = SDL_GPU_STOREOP_STORE};
        SDL_GPURenderPass *sp = SDL_BeginGPURenderPass(cmd, &c2, 1, NULL);
        SDL_BindGPUGraphicsPipeline(sp, r->screen);
        SDL_GPUTextureSamplerBinding tb[2] = {{.texture = r->ids, .sampler = r->nearest}, {.texture = r->depth, .sampler = r->nearest}};
        SDL_BindGPUFragmentSamplers(sp, 0, tb, 2);
        float eu[8] = {EDGE_SRGB[0], EDGE_SRGB[1], EDGE_SRGB[2], 1, r->crease, 0, 0, 0};
        SDL_PushGPUFragmentUniformData(cmd, 0, eu, sizeof eu);
        SDL_DrawGPUPrimitives(sp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(sp);
        r->draws++;
    }
    /* read-back: each part's counters, the picked pixel */
    r->readback = !(r->skip & NV_SKIP_READBACK) && rb_room(r, r->nparts) == 0;
    r->picking = 0;
    r->rb_count = 0;
    if (r->readback) {
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        for (uint32_t k = 0; k < r->nparts; k++) {
            P *p = r->parts[k];
            if (!p->shown || !p->nitems) continue;
            SDL_GPUBufferRegion src = {.buffer = p->counters, .offset = 0, .size = 16};
            SDL_GPUTransferBufferLocation dst = {.transfer_buffer = r->rb, .offset = 64 + 16 * r->rb_count++};
            SDL_DownloadFromGPUBuffer(cp, &src, &dst);
        }
        if (pick && pick_x >= 0 && pick_y >= 0 && (uint32_t)pick_x < r->W && (uint32_t)pick_y < r->H) {
            SDL_GPUTextureRegion src = {.texture = r->ids, .x = (Uint32)pick_x, .y = (Uint32)pick_y, .w = 1, .h = 1, .d = 1};
            SDL_GPUTextureTransferInfo dst = {.transfer_buffer = r->rb, .offset = 0};
            SDL_DownloadFromGPUTexture(cp, &src, &dst);
            r->picking = 1;
        }
        if (tiles) nv_tiles_readback(r, cp);
        SDL_EndGPUCopyPass(cp);
    }
}

void nv_render_collect(R *r, uint32_t *picked) {
    if (picked) *picked = 0;
    r->tris = 0;
    if (r->readback) {
        uint32_t *m = SDL_MapGPUTransferBuffer(r->dev, r->rb, false);
        if (m) {
            for (uint32_t k = 0; k < r->rb_count; k++) r->tris += m[16 + 4 * k + 1];
            if (r->picking && picked) *picked = m[0];
            SDL_UnmapGPUTransferBuffer(r->dev, r->rb);
        }
    }
    for (uint32_t k = 0; k < r->nparts; k++) {
        P *p = r->parts[k];
        if (!p->shown || !p->use_hlod) continue;
        for (uint32_t c = 0; c < p->npcmd; c++) r->tris += p->pf_cpu[c].num_indices / 3;
    }
    r->tris += nv_tiles_collect(r);
}

/* ---- the library's entry points ---- */
SDL_GPUDevice *nv_gpu_device(const char **name, const char **driver) {
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(p, SDL_PROP_GPU_DEVICE_CREATE_FEATURE_INDIRECT_DRAW_FIRST_INSTANCE_BOOLEAN, true);
    SDL_GPUDevice *dev = SDL_CreateGPUDeviceWithProperties(p);
    SDL_DestroyProperties(p);
    if (!dev) return NULL;
    SDL_PropertiesID dp = SDL_GetGPUDeviceProperties(dev);
    if (name) *name = SDL_GetStringProperty(dp, SDL_PROP_GPU_DEVICE_NAME_STRING, "?");
    if (driver) *driver = SDL_GetStringProperty(dp, SDL_PROP_GPU_DEVICE_DRIVER_INFO_STRING, "?");
    return dev;
}

nv_render *nv_render_new(SDL_GPUDevice *dev, const nv_render_opts *o, char *err, size_t errn) {
    R *r = calloc(1, sizeof *r);
    if (!r) { snprintf(err, errn, "out of memory"); return NULL; }
    r->dev = dev;
    r->edges = o->edges, r->hlod = o->hlod;
    r->cut_px = o->cut_px, r->hlod_px = o->hlod_px > 0 ? o->hlod_px : 1;
    r->crease = o->crease > 0 ? o->crease : 0.0005f; /* a crease where 1/depth bends more than this share a pixel */
    r->bg[0] = 247 / 255.f, r->bg[1] = 247 / 255.f, r->bg[2] = 245 / 255.f;
    if (make_pipelines(r) || colour_room(r, NV_ELEM0)) {
        snprintf(err, errn, "the GPU refused a pipeline (%s)", SDL_GetError());
        nv_render_free(r);
        return NULL;
    }
    for (uint32_t k = 0; k < NV_ELEM0; k++) r->colour_base[k] = r->colour_cpu[k] = nv_category_colour((int)k);
    nv_tiles_init(r);
    return r;
}

void nv_render_free(R *r) {
    if (!r) return;
    SDL_GPUDevice *d = r->dev;
    SDL_WaitForGPUIdle(d);
    nv_tiles_release(r);
    for (uint32_t k = 0; k < r->nparts; k++) part_release(d, r->parts[k]);
    free(r->parts);
    if (r->colour) SDL_ReleaseGPUBuffer(d, r->colour);
    if (r->color) SDL_ReleaseGPUTexture(d, r->color), SDL_ReleaseGPUTexture(d, r->ids), SDL_ReleaseGPUTexture(d, r->depth);
    SDL_GPUGraphicsPipeline *gp[] = {r->face_w, r->face_i, r->edge_w, r->edge_i, r->screen};
    for (size_t k = 0; k < 5; k++) if (gp[k]) SDL_ReleaseGPUGraphicsPipeline(d, gp[k]);
    if (r->cull) SDL_ReleaseGPUComputePipeline(d, r->cull);
    if (r->reset) SDL_ReleaseGPUComputePipeline(d, r->reset);
    if (r->nearest) SDL_ReleaseGPUSampler(d, r->nearest);
    if (r->rb) SDL_ReleaseGPUTransferBuffer(d, r->rb);
    free(r->colour_base), free(r->colour_cpu);
    free(r);
}

int nv_render_targets(R *r, uint32_t w, uint32_t h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (r->color && r->W == w && r->H == h) return 0;
    SDL_WaitForGPUIdle(r->dev);
    return targets(r, w, h);
}

void nv_render_set_edges(R *r, int edges) { r->edges = edges; }
void nv_render_set_skip(R *r, unsigned bits) { r->skip = bits; }
int nv_render_edges(const R *r) { return r->edges; }
void nv_render_set_cut(R *r, float px) { r->cut_px = px < 0 ? 0 : px; }
void nv_render_set_hlod(R *r, int on, float px) { r->hlod = on, r->hlod_px = px > 0 ? px : r->hlod_px; }
void nv_render_set_background(R *r, const float rgb[3]) { memcpy(r->bg, rgb, sizeof r->bg); }

void nv_render_set_state(R *r, uint32_t elem, int state) {
    if (elem >= r->ncolour) return;
    uint32_t c = state == 1 ? (SELECTION_SRGB[0] | (uint32_t)SELECTION_SRGB[1] << 8 | (uint32_t)SELECTION_SRGB[2] << 16 | 0xff000000u)
               : state == 2 ? 0u : r->colour_base[elem];
    if (r->colour_cpu[elem] != c) r->colour_cpu[elem] = c, r->colour_dirty = 1, r->state_gen++;
}
void nv_render_clear_states(R *r) {
    if (r->ncolour && memcmp(r->colour_cpu, r->colour_base, r->ncolour * 4ull))
        memcpy(r->colour_cpu, r->colour_base, r->ncolour * 4ull), r->colour_dirty = 1, r->state_gen++;
}

int nv_render_add_colours(R *r, uint32_t elem0, uint32_t n, const uint32_t *colours) {
    if (!n) return 0;
    if (colour_room(r, elem0 + n)) return -1;
    memcpy(r->colour_base + elem0, colours, n * 4ull);
    memcpy(r->colour_cpu + elem0, colours, n * 4ull);
    r->colour_dirty = 1, r->state_gen++;
    return 0;
}
SDL_GPUTexture *nv_render_ids(const R *r) { return r->ids; }
SDL_GPUTexture *nv_render_depth(const R *r) { return r->depth; }

SDL_GPUTexture *nv_render_color(const R *r) { return r->color; }
nv_render_stats nv_render_get_stats(const R *r) {
    uint64_t g = r->gpu_bytes;
    for (uint32_t k = 0; k < r->nparts; k++) g += r->parts[k]->gpu_bytes;
    g += nv_tiles_pages_bytes(); /* the pages reserved (the tiles' share: tile_bytes) */
    nv_render_stats s = {.draws = r->draws, .commands = r->commands, .proxies = r->proxies_drawn, .parts = r->nparts, .tiles = r->tl.tiles_drawn, .tris = r->tris,
                         .tile_bytes = r->tl.gpu_bytes, .gpu_bytes = g, .w = r->W, .h = r->H};
    for (uint32_t k = 0; k < r->tl.nsets; k++) s.tiles_resident += nv_tileset_resident(r->tl.sets[k]);
    s.tests = r->tl.ntests, s.pass1 = r->tl.tiles_pass1, s.wanted = r->tl.budget_full ? 0 : r->tl.want_n, s.budget_full = r->tl.budget_full;
    return s;
}

int nv_write_image(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    size_t n = strlen(path);
    if (n > 4 && !strcmp(path + n - 4, ".ppm")) {
        FILE *fp = fopen(path, "wb");
        if (!fp) return -1;
        fprintf(fp, "P6\n%u %u\n255\n", w, h);
        for (size_t k = 0; k < (size_t)w * h; k++) fwrite(rgba + 4 * k, 1, 3, fp);
        return fclose(fp);
    }
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_RGBA32, (void *)rgba, (int)w * 4);
    if (!s) return -1;
    int ok = SDL_SavePNG(s, path);
    SDL_DestroySurface(s);
    return ok ? 0 : -1;
}

/* ---- the camera's moves ---- */
void nv_cam_orbit(nv_camera *c, double dx, double dy) {
    c->yaw -= dx * 0.006;
    c->pitch += dy * 0.006;
    if (c->pitch > 1.5707) c->pitch = 1.5707;
    if (c->pitch < -1.5707) c->pitch = -1.5707;
}
void nv_cam_pan(nv_camera *c, double dx, double dy, double view_h) {
    double e[3], f[3], s[3], u[3];
    nv_cam_eye(c, e);
    cam_axes(c, e, f, s, u);
    double half = c->ortho ? c->ortho_h : c->dist * tan(NV_FOV_Y / 2);
    double k = 2 * half / (view_h > 0 ? view_h : 1);
    for (int i = 0; i < 3; i++) c->target[i] += -dx * k * s[i] + dy * k * u[i];
}
void nv_cam_zoom_at(nv_camera *c, double steps, double mx, double my, double w, double h) {
    if (w < 1 || h < 1) return;
    double f = pow(0.85, steps);
    double e[3], fw[3], s[3], u[3];
    nv_cam_eye(c, e);
    cam_axes(c, e, fw, s, u);
    double hh = c->ortho ? c->ortho_h : c->dist * tan(NV_FOV_Y / 2);
    double ux = (2 * mx / w - 1) * hh * w / h, uy = (1 - 2 * my / h) * hh;
    for (int i = 0; i < 3; i++) c->target[i] += (s[i] * ux + u[i] * uy) * (1 - f);
    if (c->ortho) c->ortho_h *= f;
    else c->dist *= f;
}
void nv_cam_ray(const nv_camera *c, double x, double y, double w, double h, double o[3], double d[3]) {
    double e[3], f[3], s[3], u[3];
    nv_cam_eye(c, e);
    cam_axes(c, e, f, s, u);
    double a = w / (h > 0 ? h : 1);
    double nx = 2 * x / (w > 0 ? w : 1) - 1, ny = 1 - 2 * y / (h > 0 ? h : 1);
    if (c->ortho) {
        for (int i = 0; i < 3; i++) o[i] = e[i] + s[i] * nx * c->ortho_h * a + u[i] * ny * c->ortho_h, d[i] = f[i];
        return;
    }
    double t = tan(NV_FOV_Y / 2);
    double dl = 0;
    for (int i = 0; i < 3; i++) o[i] = e[i], d[i] = f[i] + s[i] * nx * t * a + u[i] * ny * t, dl += d[i] * d[i];
    dl = sqrt(dl);
    for (int i = 0; i < 3; i++) d[i] /= dl;
}
int nv_cam_project(const nv_camera *c, const double p[3], double w, double h, double out[2]) {
    float vp[16];
    double e[3];
    nv_cam_vp(c, w / (h > 0 ? h : 1), vp, e);
    double cx = vp[0] * p[0] + vp[4] * p[1] + vp[8] * p[2] + vp[12];
    double cy = vp[1] * p[0] + vp[5] * p[1] + vp[9] * p[2] + vp[13];
    double cw = vp[3] * p[0] + vp[7] * p[1] + vp[11] * p[2] + vp[15];
    if (cw <= 1e-9) return 0;
    out[0] = (cx / cw + 1) / 2 * w, out[1] = (1 - cy / cw) / 2 * h;
    return 1;
}

void nv_render_set_clip(R *r, int on, const double mn[3], const double mx[3]) {
    for (int i = 0; i < 3; i++) r->clip_min[i] = on && mn ? (float)mn[i] : 0, r->clip_max[i] = on && mx ? (float)mx[i] : 0;
    r->clip_min[3] = on ? 1.f : 0.f;
}
