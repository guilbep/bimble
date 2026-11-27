/* The native app (ticket NUI1; spike NAT2): the ribbon's icons, from the SAME SVG strings the Vue ribbon draws
 * (ribbon-tabs.ts `icon`: the inner SVG of a 24-unit stroke icon), rasterised once by nanosvg into
 * one atlas texture (white strokes on transparent: tinted when drawn). */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare" /* nanosvg.h:3211, not ours */
#endif
#include "nanosvg.h"
#include "nanosvgrast.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include "ui.h"

int icons_build(App *a) {
    if (!a->icon_n) return 0;
    int px = (int)ceilf(22 * a->scale * (a->offscreen ? 1 : SDL_GetWindowPixelDensity(a->win))); /* RibbonTool.vue: 22 px */
    if (px < 16) px = 16;
    int cols = 16, rows = (a->icon_n + cols - 1) / cols;
    int W = cols * px, H = rows * px;
    uint8_t *img = calloc((size_t)W * H, 4), *tile = malloc((size_t)px * px * 4);
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!img || !tile || !rast) { free(img), free(tile); return -1; }
    char svg[4096];
    for (int i = 0; i < a->icon_n; i++) {
        /* RibbonTool.vue's <svg>: viewBox 0 0 24 24, no fill, stroke 1.5, round caps and joins */
        snprintf(svg, sizeof svg, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" fill=\"none\" "
                 "stroke=\"#ffffff\" stroke-width=\"1.5\" stroke-linecap=\"round\" stroke-linejoin=\"round\">%s</svg>", a->icon_src[i]);
        NSVGimage *im = nsvgParse(svg, "px", 96); /* nanosvg parses in place */
        if (!im) continue;
        memset(tile, 0, (size_t)px * px * 4);
        nsvgRasterize(rast, im, 0, 0, px / 24.0f, tile, px, px, px * 4);
        nsvgDelete(im);
        int cx = (i % cols) * px, cy = (i / cols) * px;
        for (int y = 0; y < px; y++) memcpy(img + ((size_t)(cy + y) * W + cx) * 4, tile + (size_t)y * px * 4, (size_t)px * 4);
    }
    nsvgDeleteRasterizer(rast);
    free(tile);
    SDL_GPUTextureCreateInfo ci = {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                   .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER, .width = (Uint32)W, .height = (Uint32)H,
                                   .layer_count_or_depth = 1, .num_levels = 1};
    a->icon_tex = SDL_CreateGPUTexture(a->dev, &ci);
    SDL_GPUTransferBufferCreateInfo tci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (Uint32)(W * H * 4)};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(a->dev, &tci);
    if (!a->icon_tex || !tb) { free(img); return -1; }
    memcpy(SDL_MapGPUTransferBuffer(a->dev, tb, false), img, (size_t)W * H * 4);
    SDL_UnmapGPUTransferBuffer(a->dev, tb);
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(a->dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {.transfer_buffer = tb};
    SDL_GPUTextureRegion dst = {.texture = a->icon_tex, .w = (Uint32)W, .h = (Uint32)H, .d = 1};
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(a->dev, tb);
    free(img);
    a->icon_cols = cols, a->icon_px = px;
    return 0;
}

void icon_draw(App *a, ImDrawList *dl, int icon, float x, float y, float size, ImU32 tint) {
    if (icon < 0 || icon >= a->icon_n || !a->icon_tex) return;
    int rows = (a->icon_n + a->icon_cols - 1) / a->icon_cols;
    float u0 = (float)(icon % a->icon_cols) / a->icon_cols, v0 = (float)(icon / a->icon_cols) / rows;
    ImTextureRef_c ref = {NULL, (ImTextureID)(uintptr_t)a->icon_tex};
    ImDrawList_AddImage(dl, ref, V2(x, y), V2(x + size, y + size), V2(u0, v0), V2(u0 + 1.0f / a->icon_cols, v0 + 1.0f / rows), tint);
}

/* The icon of the ribbon tool that runs `action` (-1 none). */
int icon_for(App *a, const char *action) {
    for (int t = 0; t < a->ntabs; t++)
        for (int g = 0; g < a->tabs[t].ngroups; g++)
            for (int k = 0; k < a->tabs[t].groups[g].ntools; k++) {
                const RTool *x = &a->tabs[t].groups[g].tools[k];
                if (x->action && !strcmp(x->action, action)) return x->icon;
            }
    return -1;
}
