/* The native renderer's tile path (ticket NUI2, render.h "tile pyramids"; ADR 0078 amendment 6's contract, ADR 0084).
 *
 * A tile set is a pyramid (a link's section TILE): its table of contents (copied at open) and, per tile, its GPU
 * content once loaded (nv_gtile: a leaf's elements, merged and instanced as the parts do, or a proxy's TPX1 raw: u16
 * positions in the tile's box decoded by the vertex shader). Each frame, per set: the walker picks the tiles
 * (nv_tiles_walk, the one call of cc_tiles_select: its parameters are a one-line change), then:
 *   - a loaded tile is drawn; with occlusion (settings draw_visible) in pass 1 when it was visible last frame, else in
 *     pass 2, gated by its own test;
 *   - a tile not loaded is covered by its nearest loaded ancestor's proxy (never a hole, never a box), drawn in pass 2
 *     (with occlusion: when the test says one of the tiles it covers is visible), never in pass 1 (a stand-in deviates
 *     from the detail by its error: it must not hide the tiles it stands for);
 *   - every selected tile's box is tested against the depth pyramid built from pass 1's depth (each level the
 *     farthest of four: a box is hidden when its nearest point is behind the farthest depth over the cells it covers):
 *     what pass 1 drew stays exact, what pass 2 draws is decided on the GPU in the same frame; the answers read back
 *     after the frame's fence say which tiles were visible (pass 1 next frame) and which tiles to load (the visible).
 * No depth pre-pass and no sorting (a tile-based GPU removes hidden surfaces itself; the research note).
 * Elements under still_px pixels are skipped (per element, CPU, the tiles drawn only), back faces of closed outward
 * elements culled (cull_back; a mirrored placement's triangles reversed at load). Untrusted input: TLF1 and TPX1 are
 * checked before use; a damaged tile fails alone. */
#define _POSIX_C_SOURCE 200809L
#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"
#include "render_int.h"
#include "arena.h"
#include "meshcodec.h"

static const uint32_t SPV_PROXY[] =
#include "face_proxy.vert.h"
    ;
static const uint32_t SPV_FACE_WORLD2[] =
#include "face_world.vert.h"
    ;
static const uint32_t SPV_FACE_INST2[] =
#include "face_inst.vert.h"
    ;
static const uint32_t SPV_FACE_ID_FRAG2[] =
#include "face_id.frag.h"
    ;
static const uint32_t SPV_FACE_WORLD_C[] =
#include "face_world_clip.vert.h"
    ;
static const uint32_t SPV_FACE_INST_C[] =
#include "face_inst_clip.vert.h"
    ;
static const uint32_t SPV_PROXY_C[] =
#include "face_proxy_clip.vert.h"
    ;
static const uint32_t SPV_FACE_ND[] =
#include "face_id_nodiscard.frag.h"
    ;
static const uint32_t SPV_HIZ0[] =
#include "hiz0.comp.h"
    ;
static const uint32_t SPV_HIZN[] =
#include "hizn.comp.h"
    ;
static const uint32_t SPV_HIZN2[] =
#include "hizn2.comp.h"
    ;
static const uint32_t SPV_TTEST[] =
#include "ttest.comp.h"
    ;
static const uint32_t SPV_SEEN[] =
#include "seen.comp.h"
    ;
static const uint32_t SPV_GATE[] =
#include "gate.comp.h"
    ;

typedef struct nv_render R;
static uint32_t *seen_cpu; /* per element: the last frame that showed it (seen.comp, read back) */
static uint32_t nseen_cpu;
static uint32_t ngrec[2]; /* the surplus records (below) */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

/* ---- GPU tiles ---- */
/* NUI3: each element's test box (ob: centre, then three half axes), the tighter of its world box and its own placed box
 * (a long diagonal member's world box is much larger than itself) */
typedef struct { float c[3], r; uint32_t first, n, elem; float lo[3], hi[3]; float ob[12]; } GElem; /* a merged element: its sphere, its indices, its number, its box */
typedef struct { uint32_t first, n, firstf, copy0, ncopy, nmir; uint8_t closed; } GShape; /* firstf: reversed indices (~0 none) */
typedef struct { float c[3], r; uint32_t plc, elem; float lo[3], hi[3]; float ob[12]; } GCopy;
/* the test box: the placed local box (m: columns x, y, z, t; llo/lhi: the local box) when its volume is under 0.9 of the
 * world box's, else the world box; 1 mm wider either way (f32 slack) */
static void test_box(float ob[12], const float m[12], const float llo[3], const float lhi[3], const float wlo[3], const float whi[3]) {
    double vw = 1;
    for (int d = 0; d < 3; d++) vw *= (double)(whi[d] - wlo[d]) + 2;
    double det = m[0] * ((double)m[4] * m[8] - (double)m[5] * m[7]) - m[3] * ((double)m[1] * m[8] - (double)m[2] * m[7]) + m[6] * ((double)m[1] * m[5] - (double)m[2] * m[4]);
    double vo = fabs(det);
    float lc[3], le[3], len[3];
    for (int d = 0; d < 3; d++) lc[d] = (llo[d] + lhi[d]) / 2, le[d] = (lhi[d] - llo[d]) / 2, vo *= 2.0 * le[d] + 2;
    for (int k = 0; k < 3; k++) len[k] = sqrtf(m[3 * k] * m[3 * k] + m[3 * k + 1] * m[3 * k + 1] + m[3 * k + 2] * m[3 * k + 2]);
    if (vo < 0.9 * vw && len[0] > 0 && len[1] > 0 && len[2] > 0 && isfinite(vo)) {
        for (int d = 0; d < 3; d++) ob[d] = m[d] * lc[0] + m[3 + d] * lc[1] + m[6 + d] * lc[2] + m[9 + d];
        for (int k = 0; k < 3; k++)
            for (int d = 0; d < 3; d++) ob[3 + 3 * k + d] = m[3 * k + d] * (le[k] + 1.0f / len[k]);
        return;
    }
    for (int d = 0; d < 3; d++) ob[d] = (wlo[d] + whi[d]) / 2;
    memset(ob + 3, 0, 36);
    for (int d = 0; d < 3; d++) ob[3 + 4 * d] = (whi[d] - wlo[d]) / 2 + 1;
}
/* Tiles live in shared PAGES (one draw covers every tile of a page): per page a buffer of merged vertices (V), indices
 * (I, every tile's), shapes' vertices (SV), placements (PL), proxies' u16 positions (P, two vertices a unit) and the
 * proxies' table (TT: box, placement, colour); a tile holds one range of each in one page. Pages grow (the first small:
 * a small model reserves little), never move; ranges are freed when a tile is released. */
enum { B_V, B_I, B_SV, B_PL, B_P, B_TT, NBUF };
static const uint32_t UNIT[NBUF] = {16, 4, 12, 64, 12, 64};
/* page 0: about 26 MB, in the measured ratio of a leaf (0.5 vertex and 3 indices a triangle) and a proxy */
static const uint32_t CAP0[NBUF] = {1u << 19, 3u << 20, 1u << 17, 1u << 15, 1u << 17, 1u << 12};
/* NUI3: page_grow: a page at most 2^page_grow times the first (26 MB): a 202 MB page made while tiles arrive cost the GPU
 * tens of ms that frame (a real project: the spikes fell on the frames where pages were made); 1 (51 MB pages) measured no
 * better there (more GPU memory reserved, still p95 within the noise): 3 kept */
static uint32_t page_grow = 3;
void nv_tiles_page_grow(uint32_t g) { page_grow = g > 3 ? 3 : g; }
static uint64_t page_limit; /* the pages' bytes at most (the GPU budget's 1.25: ranges are not always contiguous), 0 none */
static const SDL_GPUBufferUsageFlags PUSE[NBUF] = {SDL_GPU_BUFFERUSAGE_VERTEX, SDL_GPU_BUFFERUSAGE_INDEX, SDL_GPU_BUFFERUSAGE_VERTEX,
                                                   SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
                                                   SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ};
#define NPAGE 48
typedef struct { uint32_t off, len; } Rg;
typedef struct {
    SDL_GPUBuffer *b[NBUF];
    uint32_t cap[NBUF];
    Rg *fr[NBUF];
    uint32_t nfr[NBUF], cfr[NBUF];
} Page;
static Page pg[NPAGE];
static int npg;
static pthread_mutex_t pmu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t pg_bytes; /* the pages' buffers (reserved) */

static uint32_t rg_alloc(Page *p, int b, uint32_t n) {
    if (!n) return 0;
    for (uint32_t i = 0; i < p->nfr[b]; i++)
        if (p->fr[b][i].len >= n) {
            uint32_t o = p->fr[b][i].off;
            p->fr[b][i].off += n, p->fr[b][i].len -= n;
            if (!p->fr[b][i].len) memmove(&p->fr[b][i], &p->fr[b][i + 1], (p->nfr[b] - i - 1) * sizeof(Rg)), p->nfr[b]--;
            return o;
        }
    return UINT32_MAX;
}
static void rg_free(Page *p, int b, uint32_t off, uint32_t n) {
    if (!n) return;
    uint32_t i = 0;
    while (i < p->nfr[b] && p->fr[b][i].off < off) i++;
    int prev = i > 0 && p->fr[b][i - 1].off + p->fr[b][i - 1].len == off, next = i < p->nfr[b] && off + n == p->fr[b][i].off;
    if (prev && next) p->fr[b][i - 1].len += n + p->fr[b][i].len, memmove(&p->fr[b][i], &p->fr[b][i + 1], (p->nfr[b] - i - 1) * sizeof(Rg)), p->nfr[b]--;
    else if (prev) p->fr[b][i - 1].len += n;
    else if (next) p->fr[b][i].off = off, p->fr[b][i].len += n;
    else {
        if (p->nfr[b] == p->cfr[b]) {
            uint32_t c = p->cfr[b] ? p->cfr[b] * 2 : 64;
            Rg *nr = realloc(p->fr[b], c * sizeof(Rg));
            if (!nr) return; /* lost to the page (out of memory) */
            p->fr[b] = nr, p->cfr[b] = c;
        }
        memmove(&p->fr[b][i + 1], &p->fr[b][i], (p->nfr[b] - i) * sizeof(Rg));
        p->fr[b][i] = (Rg){off, n}, p->nfr[b]++;
    }
}
/* ranges of n[b] units in one page (made when none has room): its index, off[b]; -1 when the GPU refuses a page */
static int page_alloc(SDL_GPUDevice *dev, const uint32_t n[NBUF], uint32_t off[NBUF]) {
    pthread_mutex_lock(&pmu);
    for (int k = 0; k <= npg && k < NPAGE; k++) {
        Page *p = &pg[k];
        if (k == npg) { /* a new page, twice the last (to page_grow times the first), or the tile's own size */
            uint32_t f = 1u << (k < (int)page_grow ? k : (int)page_grow);
            uint64_t want = 0;
            for (int b = 0; b < NBUF; b++) want += (uint64_t)CAP0[b] * f * UNIT[b];
            if (page_limit && pg_bytes + want > page_limit && npg > 0) { pthread_mutex_unlock(&pmu); return -2; } /* the budget's pages full */
            for (int b = 0; b < NBUF; b++) {
                uint64_t c = (uint64_t)CAP0[b] * f;
                if (c < n[b] * 2ull) c = n[b] * 2ull;
                if (c * UNIT[b] > 0x7FFFFFFFull) c = 0x7FFFFFFFull / UNIT[b];
                p->cap[b] = (uint32_t)c;
                SDL_GPUBufferCreateInfo ci = {.usage = PUSE[b], .size = p->cap[b] * UNIT[b]};
                p->b[b] = SDL_CreateGPUBuffer(dev, &ci);
                p->fr[b] = malloc(64 * sizeof(Rg)), p->cfr[b] = 64, p->nfr[b] = 1;
                if (!p->b[b] || !p->fr[b]) {
                    for (int q = 0; q <= b; q++) {
                        if (p->b[q]) SDL_ReleaseGPUBuffer(dev, p->b[q]);
                        free(p->fr[q]);
                    }
                    memset(p, 0, sizeof *p);
                    pthread_mutex_unlock(&pmu);
                    return -1;
                }
                p->fr[b][0] = (Rg){0, p->cap[b]};
                pg_bytes += (uint64_t)p->cap[b] * UNIT[b];
            }
            npg++;
            if (getenv("BIMBLE_TILES_DEBUG")) {
                uint64_t pb = 0;
                for (int b2 = 0; b2 < NBUF; b2++) pb += (uint64_t)p->cap[b2] * UNIT[b2];
                fprintf(stderr, "tiles: page %d made, %.0f MB, at %.1f ms\n", k, pb / 1048576.0, SDL_GetTicksNS() / 1e6);
            }
        }
        int b = 0;
        for (; b < NBUF; b++)
            if ((off[b] = rg_alloc(p, b, n[b])) == UINT32_MAX) break;
        if (b == NBUF) { pthread_mutex_unlock(&pmu); return k; }
        while (b-- > 0) rg_free(p, b, off[b], n[b]); /* not all here: the next page */
    }
    pthread_mutex_unlock(&pmu);
    return -1;
}

struct nv_gtile {
    int kind; /* 1 leaf, 2 proxy */
    int page;
    uint32_t off[NBUF], n[NBUF];
    uint64_t bytes;
    uint64_t tris;
    uint32_t nel, nclosed; /* merged elements (a large one as its clusters, NUI3); the first nclosed closed and outward */
    uint32_t nclu;          /* elements cut in clusters */
    GElem *el;
    uint32_t nshape;
    GShape *sh;
    GCopy *cp;
    uint32_t nparts;
    uint32_t part[8][3]; /* cat, first index, count */
};

struct nv_tileset {
    int refs, shown;
    uint8_t *tocb;
    size_t tocn;
    cc_tiles_toc toc;
    float *occ;
    uint32_t nocc, elem0, nE, nT;
    double c, s, off[3];
    uint32_t colour;
    uint8_t *xmask, *xtile; /* NUI3: the cross-link shell's marks, a tile's "holds one" */
    nv_gtile **g;
    uint8_t *st;            /* 0 none, 1 queued, 3 failed */
    uint64_t *used, *vis, *tested, *standin_at;
    uint32_t *standin_gate;
    float *sbox;            /* per tile: its box in the scene (6 floats) */
    uint8_t *descend;
    uint64_t desc_gen;
    uint32_t *sel, nsel;
    uint64_t bytes;
    uint32_t resident;
    uint8_t *sec_all;
    size_t sec_n;
    /* the last walk's inputs (a frame with the same ones reuses its tiles) */
    cc_tiles_view last_view;
    double last_err;
    uint64_t last_budget, last_gen;
    int last_lod, last_ok;
};
int nv_tileset_set_xmask(nv_tileset *ts, const uint8_t *marks, uint32_t n) {
    free(ts->xmask), free(ts->xtile), ts->xmask = ts->xtile = NULL;
    ts->toc.xmask = ts->toc.xtile = NULL, ts->last_ok = 0;
    if (!marks) return 0;
    if (n != ts->toc.nlist) return -1;
    ts->xmask = malloc(n + 1ull), ts->xtile = malloc(ts->nT + 1ull);
    if (!ts->xmask || !ts->xtile) { free(ts->xmask), free(ts->xtile), ts->xmask = ts->xtile = NULL; return -1; }
    memcpy(ts->xmask, marks, n);
    for (uint32_t t = 0; t < ts->nT; t++) {
        cc_tile x;
        cc_tiles_get(&ts->toc, t, &x);
        uint8_t any = 0;
        for (uint32_t k = 0; k < x.nel && x.el0 + (uint64_t)k < n && !any; k++) any = marks[x.el0 + k] != 0;
        ts->xtile[t] = any;
    }
    ts->toc.xmask = ts->xmask, ts->toc.xtile = ts->xtile;
    return 0;
}
void nv_tileset_keep_section(nv_tileset *ts, uint8_t *sec, size_t n) { free(ts->sec_all), ts->sec_all = sec, ts->sec_n = n; }
void nv_tileset_section(const nv_tileset *ts, const uint8_t **sec, size_t *n) { *sec = ts->sec_all, *n = ts->sec_n; }

typedef struct nv_ttest_map { nv_tileset *ts; uint32_t tile, gate; } TMap;

nv_tileset *nv_tileset_new(const uint8_t *toc, size_t n, uint32_t nelements, const void *occ, uint32_t nocc, uint32_t elem0, double c, double s,
                           const double off[3], uint32_t colour, char *err, size_t errn) {
    nv_tileset *t = calloc(1, sizeof *t);
    if (!t) { snprintf(err, errn, "out of memory"); return NULL; }
    t->refs = 1, t->shown = 1;
    t->tocb = malloc(n ? n : 1);
    if (!t->tocb) goto oom;
    memcpy(t->tocb, toc, n), t->tocn = n;
    cc_error e;
    if (cc_tiles_check(t->tocb, n, 0, nelements, &t->toc, &e)) {
        snprintf(err, errn, "tiles: %s", e.msg);
        free(t->tocb), free(t);
        return NULL;
    }
    t->nT = t->toc.ntiles, t->nE = nelements, t->elem0 = elem0, t->c = c, t->s = s, t->colour = colour;
    memcpy(t->off, off, sizeof t->off);
    if (occ && nocc) {
        t->occ = malloc(36ull * nocc);
        if (!t->occ) goto oom;
        memcpy(t->occ, occ, 36ull * nocc), t->nocc = nocc;
    }
    size_t N = t->nT + 1ull;
    t->g = calloc(N, sizeof *t->g), t->st = calloc(N, 1), t->used = calloc(N, 8), t->vis = calloc(N, 8), t->tested = calloc(N, 8);
    t->standin_at = calloc(N, 8), t->standin_gate = calloc(N, 4), t->sbox = malloc(N * 24), t->descend = calloc(N, 1), t->sel = malloc(N * 4);
    if (!t->g || !t->st || !t->used || !t->vis || !t->tested || !t->standin_at || !t->standin_gate || !t->sbox || !t->descend || !t->sel) goto oom;
    double ac = fabs(c), as = fabs(s);
    for (uint32_t i = 0; i < t->nT; i++) { /* each tile's box in the scene: its centre placed, its half extent turned */
        cc_tile x;
        cc_tiles_get(&t->toc, i, &x);
        double m[3], h[3];
        for (int k = 0; k < 3; k++) m[k] = (x.box[k] + x.box[3 + k]) / 2.0, h[k] = (x.box[3 + k] - x.box[k]) / 2.0;
        double cx = c * m[0] - s * m[1] + off[0], cy = s * m[0] + c * m[1] + off[1], cz = m[2] + off[2];
        double ex = ac * h[0] + as * h[1], ey = as * h[0] + ac * h[1], ez = h[2];
        ex = ex * (1 + 1e-6) + 0.5, ey = ey * (1 + 1e-6) + 0.5, ez = ez * (1 + 1e-6) + 0.5; /* f32 placement's slack: never smaller */
        float *b = &t->sbox[6ull * i];
        b[0] = (float)(cx - ex), b[1] = (float)(cy - ey), b[2] = (float)(cz - ez), b[3] = (float)(cx + ex), b[4] = (float)(cy + ey), b[5] = (float)(cz + ez);
    }
    t->desc_gen = ~0ull;
    return t;
oom:
    snprintf(err, errn, "out of memory");
    nv_tileset_unref(NULL, t);
    return NULL;
}

void nv_gtile_free(SDL_GPUDevice *dev, nv_gtile *g) {
    (void)dev;
    if (!g) return;
    if (g->page >= 0 && g->page < npg) {
        pthread_mutex_lock(&pmu);
        for (int b = 0; b < NBUF; b++) rg_free(&pg[g->page], b, g->off[b], g->n[b]);
        pthread_mutex_unlock(&pmu);
    }
    free(g->el), free(g->sh), free(g->cp);
    free(g);
}
uint64_t nv_gtile_bytes(const nv_gtile *g) { return g ? g->bytes : 0; }
uint64_t nv_tiles_pages_bytes(void) { return pg_bytes; }

void nv_tileset_ref(nv_tileset *ts) { if (ts) __atomic_add_fetch(&ts->refs, 1, __ATOMIC_ACQ_REL); }
void nv_tileset_unref(SDL_GPUDevice *dev, nv_tileset *t) {
    if (!t || __atomic_sub_fetch(&t->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    if (t->g)
        for (uint32_t i = 0; i < t->nT; i++) nv_gtile_free(dev, t->g[i]);
    free(t->g), free(t->st), free(t->used), free(t->vis), free(t->tested), free(t->standin_at), free(t->standin_gate), free(t->sbox);
    free(t->descend), free(t->sel), free(t->tocb), free(t->occ), free(t->sec_all), free(t->xmask), free(t->xtile);
    free(t);
}
const cc_tiles_toc *nv_tileset_toc(const nv_tileset *ts) { return &ts->toc; }
void nv_tileset_set_shown(nv_tileset *ts, int shown) { ts->shown = shown != 0; }
uint64_t nv_tileset_gpu_bytes(const nv_tileset *ts) { return ts->bytes; }
uint32_t nv_tileset_resident(const nv_tileset *ts) { return ts->resident; }
void nv_tileset_box(const nv_tileset *ts, double lo[3], double hi[3]) {
    for (int k = 0; k < 3; k++) lo[k] = ts->nT ? ts->sbox[k] : 0, hi[k] = ts->nT ? ts->sbox[3 + k] : -1;
}
/* a tile's GPU bytes guessed before it is loaded (the budget counts the tiles on their way): measured 20-30 a triangle */
static uint64_t guess_bytes(const nv_tileset *ts, uint32_t tile) {
    cc_tile x;
    cc_tiles_get(&ts->toc, tile, &x);
    return (uint64_t)x.tris * ((x.flags & CC_TILE_LEAF) ? 28 : 9) + 4096;
}
static uint64_t inflight_bytes;
void nv_tileset_queued(nv_tileset *ts, uint32_t tile, int q) {
    if (tile >= ts->nT || ts->st[tile] == 3) return;
    if (q && ts->st[tile] != 1) inflight_bytes += guess_bytes(ts, tile);
    if (!q && ts->st[tile] == 1) inflight_bytes -= guess_bytes(ts, tile);
    ts->st[tile] = q ? 1 : 0;
}

/* ---- loading (any thread) ---- */
#define USE_V SDL_GPU_BUFFERUSAGE_VERTEX
#define USE_I SDL_GPU_BUFFERUSAGE_INDEX
#define USE_S SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ

/* a tile's arrays into its page's ranges: one transfer buffer, one command buffer (any thread). 0 ok. */
static uint64_t up_bytes, up_n; /* NUI3: bytes uploaded since the last frame collected (BIMBLE_TILES_DEBUG) */
static int upload(SDL_GPUDevice *dev, nv_gtile *g, const void *data[NBUF], const size_t bytes[NBUF]) {
    for (int b = 0; b < NBUF; b++) g->n[b] = (uint32_t)((bytes[b] + UNIT[b] - 1) / UNIT[b]);
    g->page = page_alloc(dev, g->n, g->off);
    if (g->page < 0) return g->page;
    size_t tot = 0;
    for (int b = 0; b < NBUF; b++) tot += (bytes[b] + 15) & ~(size_t)15, g->bytes += (uint64_t)g->n[b] * UNIT[b];
    if (!tot) return 0;
    if (tot > 0xF0000000u) return -1;
    __atomic_add_fetch(&up_bytes, tot, __ATOMIC_RELAXED), __atomic_add_fetch(&up_n, 1, __ATOMIC_RELAXED);
    SDL_GPUTransferBufferCreateInfo tci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = (Uint32)tot};
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tci);
    if (!tb) return -1;
    uint8_t *m = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (!m) { SDL_ReleaseGPUTransferBuffer(dev, tb); return -1; }
    size_t at = 0;
    for (int b = 0; b < NBUF; b++) {
        if (bytes[b]) memcpy(m + at, data[b], bytes[b]);
        at += (bytes[b] + 15) & ~(size_t)15;
    }
    SDL_UnmapGPUTransferBuffer(dev, tb);
    int ok = 0;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (cmd) {
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        at = 0;
        for (int b = 0; b < NBUF; b++) {
            if (bytes[b]) {
                SDL_GPUTransferBufferLocation src = {.transfer_buffer = tb, .offset = (Uint32)at};
                SDL_GPUBufferRegion dst = {.buffer = pg[g->page].b[b], .offset = g->off[b] * UNIT[b], .size = (Uint32)bytes[b]};
                SDL_UploadToGPUBuffer(cp, &src, &dst, false);
            }
            at += (bytes[b] + 15) & ~(size_t)15;
        }
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence *f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (f) SDL_WaitForGPUFences(dev, true, &f, 1), SDL_ReleaseGPUFence(dev, f);
        ok = 1;
    }
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return ok ? 0 : -1;
}

nv_gtile *nv_gtile_proxy(SDL_GPUDevice *dev, const nv_tileset *ts, uint32_t tile, const uint8_t *p, size_t n, char *err, size_t errn) {
    cc_error e;
    if (tile >= ts->nT) { snprintf(err, errn, "tiles: no such tile"); return NULL; }
    if (cc_tiles_proxy_check(p, n, &e)) { snprintf(err, errn, "%s", e.msg); return NULL; }
    nv_gtile *g = calloc(1, sizeof *g);
    if (!g) { snprintf(err, errn, "out of memory"); return NULL; }
    g->kind = 2, g->page = -1;
    uint32_t nv = rd32(p + 4), ni = rd32(p + 8), np = rd32(p + 12);
    cc_tile x;
    cc_tiles_get(&ts->toc, tile, &x);
    g->nparts = np > 8 ? 8 : np;
    for (uint32_t k = 0; k < g->nparts; k++) {
        const uint8_t *q = p + 16 + 16ull * k;
        g->part[k][0] = rd32(q), g->part[k][1] = rd32(q + 8), g->part[k][2] = rd32(q + 12);
    }
    size_t at = 16 + 16ull * np, ix = ((at + 6ull * nv + 3) & ~(size_t)3);
    int idx16 = nv <= 65536;
    g->tris = ni / 3;
    uint8_t *pos = calloc(((nv + 1) / 2) * 12ull + 12, 1); /* two vertices a unit of the page */
    uint32_t *idx = malloc(4ull * ni + 4);
    float row[16];
    if (!pos || !idx) { free(pos), free(idx), free(g); snprintf(err, errn, "out of memory"); return NULL; }
    memcpy(pos, p + at, 6ull * nv);
    for (uint32_t k = 0; k < ni; k++) idx[k] = idx16 ? (uint32_t)(p[ix + 2ull * k] | p[ix + 2ull * k + 1] << 8) : rd32(p + ix + 4ull * k);
    memcpy(row, x.box, 12), memcpy(&row[3], &ts->colour, 4), memcpy(&row[4], x.box + 3, 12), row[7] = 0;
    row[8] = (float)ts->c, row[9] = (float)ts->s, row[10] = row[11] = 0;
    for (int k = 0; k < 3; k++) row[12 + k] = (float)ts->off[k];
    row[15] = 0;
    const void *data[NBUF] = {NULL, idx, NULL, NULL, pos, row};
    size_t bytes[NBUF] = {0, 4ull * ni, 0, 0, ((nv + 1) / 2) * 12ull, 64};
    int rc = upload(dev, g, data, bytes);
    free(pos), free(idx);
    if (rc) { snprintf(err, errn, rc == -2 ? "no room: the GPU budget's pages are full" : "the GPU refused a page (%s)", SDL_GetError()); nv_gtile_free(dev, g); return NULL; }
    return g;
}

/* NUI3, clusters (cluster_tris): a merged element of more than clu_size triangles drawn and tested as clusters of
 * clu_size triangles in space order (one huge mesh's box covers the building: the test cannot hide it); 0 off */
static uint32_t clu_size = 512;
#define CLU_ON(nt) (clu_size && (nt) > clu_size)
void nv_tiles_cluster_tris(uint32_t n) { clu_size = n ? (n < 64 ? 64 : n) : 0; }
static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static int cmp_used(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

nv_gtile *nv_gtile_leaf(SDL_GPUDevice *dev, const nv_tileset *ts, uint32_t tile, const uint8_t *p, size_t n, char *err, size_t errn) {
    if (tile >= ts->nT) { snprintf(err, errn, "tiles: no such tile"); return NULL; }
    if (n < 16 || memcmp(p, "TLF1", 4)) { snprintf(err, errn, "tiles: not a TLF1 leaf"); return NULL; }
    uint64_t nel = rd32(p + 8), nb = rd32(p + 12);
    if (nel > n / 56 || nb > nel || 16 + 56 * nel + 4 * nb > n) { snprintf(err, errn, "tiles: a leaf's counts"); return NULL; }
    cc_tile x;
    cc_tiles_get(&ts->toc, tile, &x);
    const uint8_t *els = p + 16, *lens = els + 56 * nel, *bl = lens + 4 * nb;
    Arena a;
    arena_init(&a, 0);
    nv_gtile *g = calloc(1, sizeof *g);
    cc_mesh *bm = arena_alloc_n(&a, nb + 1, sizeof *bm, 8);
    uint32_t *uses = calloc(nb + 1, 4), *shape_of = malloc((nb + 1) * 4), *mirn = calloc(nb + 1, 4);
    uint8_t *closed_b = calloc(nb + 1, 1);
    struct { float m[12]; uint32_t elem, b; int mir, closed; } *E = calloc(nel + 1, sizeof *E);
    nv_vec wv = {0}, wi = {0}, sv = {0}, si = {0}, plc = {0};
    uint64_t *ord = NULL, *tk = NULL;
    int rc = -1;
    if (!g || !bm || !uses || !shape_of || !mirn || !closed_b || !E) { snprintf(err, errn, "out of memory"); goto done; }
    g->kind = 1, g->page = -1;
    uint64_t at = 0;
    for (uint64_t i = 0; i < nb; i++) {
        uint32_t ln = rd32(lens + 4 * i);
        const char *why = "";
        if (ln > n - (size_t)(bl - p) - at || cc_meshcodec_decode(bl + at, ln, &a, &bm[i], &why) != CC_MC_OK) { snprintf(err, errn, "tiles: a leaf's block (%s)", why); goto done; }
        for (uint32_t k = 0; k < bm[i].ni; k++)
            if (bm[i].idx[k] >= bm[i].nv) { snprintf(err, errn, "tiles: a leaf's index"); goto done; }
        at += ln;
    }
    double c = ts->c, s = ts->s;
    for (uint64_t i = 0; i < nel; i++) {
        const uint8_t *e = els + 56 * i;
        uint32_t row = rd32(e), b = rd32(e + 4);
        if (b >= nb || row >= ts->nE) { snprintf(err, errn, "tiles: a leaf's element"); goto done; }
        float f[12];
        for (int k = 0; k < 12; k++) f[k] = rdf(e + 8 + 4 * k);
        /* the link's placement after the element's: R(c, s) f + off (columns x, y, z, then t) */
        for (int col = 0; col < 4; col++) {
            double X = f[3 * col], Y = f[3 * col + 1];
            E[i].m[3 * col] = (float)(c * X - s * Y), E[i].m[3 * col + 1] = (float)(s * X + c * Y), E[i].m[3 * col + 2] = f[3 * col + 2];
        }
        for (int k = 0; k < 3; k++) E[i].m[9 + k] = (float)(E[i].m[9 + k] + ts->off[k]);
        double det = f[0] * ((double)f[4] * f[8] - (double)f[5] * f[7]) - f[3] * ((double)f[1] * f[8] - (double)f[2] * f[7]) + f[6] * ((double)f[1] * f[5] - (double)f[2] * f[4]);
        E[i].elem = ts->elem0 + row, E[i].b = b, E[i].mir = det < 0;
        uint32_t li = x.el0 + (uint32_t)i; /* the list's entry: its flags (closed and outward) */
        E[i].closed = li < ts->toc.nlist && cc_tiles_list(&ts->toc, li) == row && (cc_tiles_el_flags(&ts->toc, li) & CC_TILES_EL_CLOSED);
        uses[b]++;
        if (E[i].mir) mirn[b]++;
        if (E[i].closed) closed_b[b] = 1;
    }
    /* the elements in space order (Morton order of their boxes' centres in the tile's box): the groups of occlusion_group
     * consecutive elements are then compact, and their box tests tight (in list order a group of 32 spans the tile) */
    ord = malloc((nel + 1) * 8);
    if (!ord) { snprintf(err, errn, "out of memory"); goto done; }
    {
        float tl[3], ts3[3];
        for (int d = 0; d < 3; d++) tl[d] = x.box[d], ts3[d] = x.box[3 + d] - x.box[d] > 1 ? x.box[3 + d] - x.box[d] : 1;
        for (uint64_t i = 0; i < nel; i++) {
            const cc_mesh *me = &bm[E[i].b];
            float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
            for (uint32_t k = 0; k < me->nv; k++)
                for (int d = 0; d < 3; d++) {
                    float v = me->pos[k][d];
                    if (!k || v < lo[d]) lo[d] = v;
                    if (!k || v > hi[d]) hi[d] = v;
                }
            float c[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
            const float *m = E[i].m;
            uint64_t key = 0;
            uint32_t q[3];
            /* the centre back in the link frame (the tile's box is there): R^T (p - off) */
            float wx = m[0] * c[0] + m[3] * c[1] + m[6] * c[2] + m[9] - (float)ts->off[0], wy = m[1] * c[0] + m[4] * c[1] + m[7] * c[2] + m[10] - (float)ts->off[1],
                  wz = m[2] * c[0] + m[5] * c[1] + m[8] * c[2] + m[11] - (float)ts->off[2];
            float lp[3] = {(float)(ts->c * wx + ts->s * wy), (float)(-ts->s * wx + ts->c * wy), wz};
            for (int d = 0; d < 3; d++) {
                float f = (lp[d] - tl[d]) / ts3[d];
                q[d] = f <= 0 ? 0 : f >= 1 ? 1023 : (uint32_t)(f * 1023);
            }
            for (int bit = 9; bit >= 0; bit--)
                for (int d = 0; d < 3; d++) key = key << 1 | ((q[d] >> bit) & 1);
            ord[i] = key << 32 | (uint64_t)i;
        }
        qsort(ord, nel, 8, cmp_u64);
        for (uint64_t i = 0; i < nel; i++) ord[i] &= 0xFFFFFFFFull;
    }
    /* merged: elements whose block is used fewer than 4 times, placed here; closed ones first (their own draw) */
    uint32_t nm = 0, maxt = 0;
    for (uint64_t i = 0; i < nel; i++)
        if (uses[E[i].b] < 4) {
            uint32_t nt = bm[E[i].b].ni / 3;
            nm += CLU_ON(nt) ? (nt + clu_size - 1) / clu_size : 1;
            if (CLU_ON(nt) && nt > maxt) maxt = nt;
        }
    tk = maxt ? malloc(8ull * maxt) : NULL; /* NUI3: a large element's triangles in space order (its clusters) */
    if (maxt && !tk) { snprintf(err, errn, "out of memory"); goto done; }
    g->el = calloc(nm + 1, sizeof *g->el);
    if (!g->el) { snprintf(err, errn, "out of memory"); goto done; }
    for (int grp = 0; grp < 2; grp++)
        for (uint64_t o = 0; o < nel; o++) {
            uint64_t i = ord[o];
            if (uses[E[i].b] >= 4 || E[i].closed != (grp == 0)) continue;
            const cc_mesh *me = &bm[E[i].b];
            uint32_t vb = (uint32_t)wv.n;
            nv_vtx *v = nv_push(&wv, sizeof(nv_vtx), me->nv);
            uint32_t *ix = nv_push(&wi, 4, me->ni);
            if (!v || !ix) { snprintf(err, errn, "out of memory"); goto done; }
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            const float *m = E[i].m;
            for (uint32_t k = 0; k < me->nv; k++) {
                const float *q = me->pos[k];
                for (int d = 0; d < 3; d++) {
                    v[k].p[d] = m[d] * q[0] + m[3 + d] * q[1] + m[6 + d] * q[2] + m[9 + d];
                    if (v[k].p[d] < lo[d]) lo[d] = v[k].p[d];
                    if (v[k].p[d] > hi[d]) hi[d] = v[k].p[d];
                }
                v[k].elem = E[i].elem;
            }
            uint32_t ntri = me->ni / 3;
            int clu = CLU_ON(ntri) && tk;
            if (clu) { /* NUI3: the triangles in Morton order of their centres in the element's box, cut in clusters of clu_size */
                float ext[3];
                for (int d = 0; d < 3; d++) ext[d] = hi[d] - lo[d] > 1 ? hi[d] - lo[d] : 1;
                for (uint32_t t = 0; t < ntri; t++) {
                    uint32_t q[3];
                    uint64_t key = 0;
                    for (int d = 0; d < 3; d++) {
                        float f = ((v[me->idx[3 * t]].p[d] + v[me->idx[3 * t + 1]].p[d] + v[me->idx[3 * t + 2]].p[d]) / 3 - lo[d]) / ext[d];
                        q[d] = f <= 0 ? 0 : f >= 1 ? 1023 : (uint32_t)(f * 1023);
                    }
                    for (int bit = 9; bit >= 0; bit--)
                        for (int d = 0; d < 3; d++) key = key << 1 | ((q[d] >> bit) & 1);
                    tk[t] = key << 32 | t;
                }
                qsort(tk, ntri, 8, cmp_u64);
            }
            for (uint32_t t = 0; t < ntri; t++) { /* a mirrored placement's triangles reversed: outward stays front */
                uint32_t k = 3 * t, s0 = clu ? 3 * (uint32_t)(tk[t] & 0xFFFFFFFFu) : k;
                ix[k] = vb + me->idx[s0];
                ix[k + 1] = vb + me->idx[E[i].mir ? s0 + 2 : s0 + 1];
                ix[k + 2] = vb + me->idx[E[i].mir ? s0 + 1 : s0 + 2];
            }
            for (uint32_t k = 3 * ntri; k < me->ni; k++) ix[k] = vb + me->idx[k];
            float ec[3], r2 = 0;
            for (int d = 0; d < 3; d++) ec[d] = me->nv ? (lo[d] + hi[d]) / 2 : 0, r2 += me->nv ? (hi[d] - lo[d]) * (hi[d] - lo[d]) / 4 : 0;
            if (clu) { /* the clusters: the element's sphere (still_px decides by the element), each its own box */
                uint32_t base = (uint32_t)(wi.n - me->ni);
                for (uint32_t t0 = 0; t0 < ntri; t0 += clu_size) {
                    uint32_t t1 = t0 + clu_size < ntri ? t0 + clu_size : ntri;
                    GElem *ce = &g->el[g->nel++];
                    memcpy(ce->c, ec, 12), ce->r = sqrtf(r2), ce->first = base + 3 * t0, ce->n = 3 * (t1 - t0), ce->elem = E[i].elem;
                    for (int d = 0; d < 3; d++) ce->lo[d] = 3e38f, ce->hi[d] = -3e38f;
                    for (uint32_t k = 3 * t0; k < 3 * t1; k++)
                        for (int d = 0; d < 3; d++) {
                            float p = wv.n ? ((nv_vtx *)wv.p)[ix[k]].p[d] : 0;
                            ce->lo[d] = fminf(ce->lo[d], p), ce->hi[d] = fmaxf(ce->hi[d], p);
                        }
                    for (int d = 0; d < 3; d++) ce->ob[d] = (ce->lo[d] + ce->hi[d]) / 2;
                    memset(ce->ob + 3, 0, 36);
                    for (int d = 0; d < 3; d++) ce->ob[3 + 4 * d] = (ce->hi[d] - ce->lo[d]) / 2 + 1;
                    if (grp == 0) g->nclosed++;
                }
                g->nclu++;
                g->tris += me->ni / 3;
                continue;
            }
            GElem *ge = &g->el[g->nel++];
            memcpy(ge->c, ec, 12);
            ge->r = sqrtf(r2), ge->first = (uint32_t)(wi.n - me->ni), ge->n = me->ni, ge->elem = E[i].elem;
            for (int d = 0; d < 3; d++) ge->lo[d] = me->nv ? lo[d] : 0, ge->hi[d] = me->nv ? hi[d] : 0;
            {
                float llo[3] = {0, 0, 0}, lhi[3] = {0, 0, 0};
                for (uint32_t k = 0; k < me->nv; k++)
                    for (int d = 0; d < 3; d++) {
                        if (!k || me->pos[k][d] < llo[d]) llo[d] = me->pos[k][d];
                        if (!k || me->pos[k][d] > lhi[d]) lhi[d] = me->pos[k][d];
                    }
                test_box(ge->ob, m, llo, lhi, ge->lo, ge->hi);
            }
            if (grp == 0) g->nclosed++;
            g->tris += me->ni / 3;
        }
    /* shapes: blocks used 4 times or more, drawn instanced; copies grouped by shape, mirrored ones last */
    uint32_t ns = 0, ncp = 0;
    for (uint64_t b = 0; b < nb; b++)
        if (uses[b] >= 4) shape_of[b] = ns++, ncp += uses[b];
        else shape_of[b] = UINT32_MAX;
    g->sh = calloc(ns + 1, sizeof *g->sh), g->cp = calloc(ncp + 1, sizeof *g->cp);
    if (!g->sh || !g->cp) { snprintf(err, errn, "out of memory"); goto done; }
    g->nshape = ns;
    uint32_t idx_base = (uint32_t)wi.n; /* shape indices follow the merged ones in the one index buffer */
    {
        uint32_t cacc = 0;
        for (uint64_t b = 0; b < nb; b++) {
            if (shape_of[b] == UINT32_MAX) continue;
            GShape *sh = &g->sh[shape_of[b]];
            const cc_mesh *me = &bm[b];
            uint32_t vb = (uint32_t)sv.n;
            float *v = nv_push(&sv, 12, me->nv);
            uint32_t *ix = nv_push(&si, 4, me->ni * (mirn[b] ? 2u : 1u));
            if (!v || !ix) { snprintf(err, errn, "out of memory"); goto done; }
            if (me->nv) memcpy(v, me->pos, 12ull * me->nv);
            for (uint32_t k = 0; k < me->ni; k++) ix[k] = vb + me->idx[k];
            sh->first = idx_base + (uint32_t)(si.n - me->ni * (mirn[b] ? 2u : 1u)), sh->n = me->ni, sh->firstf = UINT32_MAX;
            if (mirn[b]) {
                for (uint32_t k = 0; k + 2 < me->ni; k += 3) ix[me->ni + k] = ix[k], ix[me->ni + k + 1] = ix[k + 2], ix[me->ni + k + 2] = ix[k + 1];
                sh->firstf = sh->first + me->ni;
            }
            sh->copy0 = cacc, sh->ncopy = uses[b], sh->nmir = mirn[b], sh->closed = closed_b[b];
            cacc += uses[b];
        }
        uint32_t *fill = calloc(ns + 1, 4), *fillm = calloc(ns + 1, 4);
        if (!fill || !fillm) { free(fill), free(fillm); snprintf(err, errn, "out of memory"); goto done; }
        float lo0[3], hi0[3];
        for (uint64_t o = 0; o < nel; o++) {
            uint64_t i = ord[o];
            uint32_t sidx = shape_of[E[i].b];
            if (sidx == UINT32_MAX) continue;
            GShape *sh = &g->sh[sidx];
            uint32_t slot = E[i].mir ? sh->copy0 + sh->ncopy - sh->nmir + fillm[sidx]++ : sh->copy0 + fill[sidx]++;
            GCopy *cp = &g->cp[slot];
            nv_plc *pc = nv_push(&plc, sizeof(nv_plc), 1);
            if (!pc) { free(fill), free(fillm); snprintf(err, errn, "out of memory"); goto done; }
            const float *m = E[i].m;
            for (int d = 0; d < 3; d++) pc->x[d] = m[d], pc->y[d] = m[3 + d], pc->z[d] = m[6 + d], pc->t[d] = m[9 + d];
            pc->x[3] = pc->y[3] = pc->z[3] = 0;
            memcpy(&pc->t[3], &E[i].elem, 4);
            cp->plc = (uint32_t)(plc.n - 1), cp->elem = E[i].elem;
            const cc_mesh *me = &bm[E[i].b]; /* the copy's sphere: its shape's box placed */
            for (int d = 0; d < 3; d++) lo0[d] = 1e30f, hi0[d] = -1e30f;
            for (uint32_t k = 0; k < me->nv; k++)
                for (int d = 0; d < 3; d++) lo0[d] = fminf(lo0[d], me->pos[k][d]), hi0[d] = fmaxf(hi0[d], me->pos[k][d]);
            float r2 = 0;
            for (int d = 0; d < 3; d++) {
                float cc0 = me->nv ? (lo0[d] + hi0[d]) / 2 : 0, e0 = me->nv ? (hi0[d] - lo0[d]) / 2 : 0;
                (void)cc0, r2 += e0 * e0;
            }
            float lc[3];
            for (int d = 0; d < 3; d++) lc[d] = me->nv ? (lo0[d] + hi0[d]) / 2 : 0;
            for (int d = 0; d < 3; d++) cp->c[d] = m[d] * lc[0] + m[3 + d] * lc[1] + m[6 + d] * lc[2] + m[9 + d];
            float sc = fmaxf(fmaxf(sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), sqrtf(m[3] * m[3] + m[4] * m[4] + m[5] * m[5])), sqrtf(m[6] * m[6] + m[7] * m[7] + m[8] * m[8]));
            cp->r = sqrtf(r2) * sc;
            for (int d = 0; d < 3; d++) { /* the copy's box: its shape's box placed (the corners' extent along each axis) */
                float e0 = me->nv ? (hi0[0] - lo0[0]) / 2 : 0, e1 = me->nv ? (hi0[1] - lo0[1]) / 2 : 0, e2 = me->nv ? (hi0[2] - lo0[2]) / 2 : 0;
                float ex = fabsf(m[d]) * e0 + fabsf(m[3 + d]) * e1 + fabsf(m[6 + d]) * e2;
                cp->lo[d] = cp->c[d] - ex, cp->hi[d] = cp->c[d] + ex;
            }
            if (me->nv) test_box(cp->ob, m, lo0, hi0, cp->lo, cp->hi);
            else test_box(cp->ob, m, (float[3]){0, 0, 0}, (float[3]){0, 0, 0}, cp->lo, cp->hi);
            g->tris += me->ni / 3;
        }
        free(fill), free(fillm);
    }
    /* the one index buffer: merged, then the shapes' */
    uint32_t *allix = nv_push(&wi, 4, si.n);
    if (!allix && si.n) { snprintf(err, errn, "out of memory"); goto done; }
    if (si.n) memcpy(allix, si.p, 4 * si.n);
    const void *data[NBUF] = {wv.p, wi.p, sv.p, plc.p, NULL, NULL};
    size_t bytes[NBUF] = {wv.n * sizeof(nv_vtx), wi.n * 4, sv.n * 12, plc.n * sizeof(nv_plc), 0, 0};
    int urc = upload(dev, g, data, bytes);
    if (urc) { snprintf(err, errn, urc == -2 ? "no room: the GPU budget's pages are full" : "the GPU refused a page (%s)", SDL_GetError()); goto done; }
    g->bytes += (uint64_t)g->nel * sizeof(GElem) + (uint64_t)g->nshape * sizeof(GShape) + (uint64_t)ncp * sizeof(GCopy); /* its CPU tables too */
    rc = 0;
    (void)cmp_u32;
done:
    nv_vec_free(&wv), nv_vec_free(&wi), nv_vec_free(&sv), nv_vec_free(&si), nv_vec_free(&plc);
    free(uses), free(shape_of), free(mirn), free(closed_b), free(E), free(ord), free(tk);
    arena_free(&a);
    if (rc) { nv_gtile_free(dev, g); return NULL; }
    return g;
}

/* ---- the renderer's side (the drawing thread) ---- */
void nv_tileset_attach(R *r, nv_tileset *ts, uint32_t tile, nv_gtile *g) {
    if (tile >= ts->nT) { nv_gtile_free(r->dev, g); return; }
    nv_tileset_queued(ts, tile, 0);
    if (!g) { ts->st[tile] = 3; return; }
    if (ts->g[tile]) { nv_gtile_free(r->dev, g); return; }
    ts->g[tile] = g, ts->st[tile] = 0;
    ts->bytes += g->bytes, ts->resident++;
    r->tl.gpu_bytes += g->bytes;
}

/* a tile released while a frame that draws it may still run (frames in flight): its ranges freed at the next collect */
static nv_gtile **dead;
static uint32_t ndead, cdead;
static void drop_tile(R *r, nv_tileset *ts, uint32_t t) {
    nv_gtile *g = ts->g[t];
    if (!g) return;
    ts->bytes -= g->bytes, ts->resident--;
    r->tl.gpu_bytes -= g->bytes;
    ts->g[t] = NULL;
    if (ndead == cdead) {
        uint32_t c = cdead ? cdead * 2 : 256;
        nv_gtile **n2 = realloc(dead, c * sizeof *n2);
        if (!n2) { SDL_WaitForGPUIdle(r->dev), nv_gtile_free(r->dev, g); return; }
        dead = n2, cdead = c;
    }
    dead[ndead++] = g;
}
static void bury(R *r) {
    for (uint32_t i = 0; i < ndead; i++) nv_gtile_free(r->dev, dead[i]);
    ndead = 0;
}
/* what a frame's collect needs, kept per slot (the next frame's prepare overwrites the frame state) */
static struct Snap {
    int read_vis, seen_planned;
    uint64_t frame, tris1;
    uint32_t tiles1, ngates, nmap, cg, cm, nseen; /* nseen: the elements the frame's seen read-back holds */
    uint64_t *gate_tris;
    uint32_t *gate_tiles;
    TMap *map;
} snp[2];

void nv_render_tiles_drop_all(R *r) {
    nv_tiles_r *T = &r->tl;
    for (uint32_t si = 0; si < T->nsets; si++)
        for (uint32_t t = 0; t < T->sets[si]->nT; t++) drop_tile(r, T->sets[si], t);
    T->want_n = 0;
}
int nv_render_add_tileset(R *r, nv_tileset *ts) {
    nv_tiles_r *T = &r->tl;
    if (T->nsets == T->csets) {
        uint32_t c = T->csets ? T->csets * 2 : 32;
        nv_tileset **n = realloc(T->sets, c * sizeof *n);
        if (!n) return -1;
        T->sets = n, T->csets = c;
    }
    nv_tileset_ref(ts);
    T->sets[T->nsets++] = ts;
    return 0;
}
void nv_render_remove_tileset(R *r, nv_tileset *ts) {
    nv_tiles_r *T = &r->tl;
    for (uint32_t k = 0; k < T->nsets; k++)
        if (T->sets[k] == ts) {
            memmove(&T->sets[k], &T->sets[k + 1], (T->nsets - k - 1) * sizeof *T->sets);
            T->nsets--;
            r->tl.gpu_bytes -= ts->bytes;
            SDL_WaitForGPUIdle(r->dev);
            ngrec[0] = ngrec[1] = 0; /* NUI3: the surplus records point at tiles freed here (BIMBLE_TILES_SURPLUS crashed on a re-indexed link) */
            for (uint32_t i = 0; i < ts->nT; i++) if (ts->g[i]) ts->bytes -= ts->g[i]->bytes, nv_gtile_free(r->dev, ts->g[i]), ts->g[i] = NULL;
            ts->resident = 0;
            nv_tileset_unref(r->dev, ts);
            break;
        }
    T->want_n = 0;
}
void nv_render_tiles_opts(R *r, const nv_tiles_opts *o) { r->tl.o = *o, r->tl.groups = o->groups, page_limit = o->budget ? o->budget + o->budget / 4 : 0; }
/* a tile that found no room: the least recently used released at the next frame, down to 80 % of the budget */
void nv_render_tiles_squeeze(R *r) { r->tl.squeeze = 1, r->tl.noroom_frame = r->tl.frame ? r->tl.frame : 1; }
nv_tiles_opts nv_render_get_tiles_opts(const R *r) { return r->tl.o; }

/* ---- pipelines ---- */
static SDL_GPUShader *shader(SDL_GPUDevice *dev, const uint32_t *code, size_t bytes, SDL_GPUShaderStage st, uint32_t samplers, uint32_t storage,
                             uint32_t uniforms) {
    SDL_GPUShaderCreateInfo ci = {.code_size = bytes, .code = (const Uint8 *)code, .entrypoint = "main", .format = SDL_GPU_SHADERFORMAT_SPIRV,
                                  .stage = st, .num_samplers = samplers, .num_storage_buffers = storage, .num_uniform_buffers = uniforms};
    return SDL_CreateGPUShader(dev, &ci);
}
static SDL_GPUGraphicsPipeline *gpipe(R *r, SDL_GPUShader *vs, SDL_GPUShader *fs, int pitch, int nattr, SDL_GPUCullMode cull) {
    if (!vs || !fs) return NULL;
    SDL_GPUVertexBufferDescription vb = {.slot = 0, .pitch = (Uint32)pitch, .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX};
    SDL_GPUVertexAttribute va[2] = {
        {.location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = 0},
        {.location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_UINT, .offset = 12},
    };
    SDL_GPUColorTargetDescription ct[2] = {{.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM}, {.format = SDL_GPU_TEXTUREFORMAT_R32_UINT}};
    SDL_GPUGraphicsPipelineCreateInfo ci = {
        .vertex_shader = vs,
        .fragment_shader = fs,
        .vertex_input_state = {.vertex_buffer_descriptions = &vb, .num_vertex_buffers = nattr ? 1 : 0, .vertex_attributes = va, .num_vertex_attributes = (Uint32)nattr},
        .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
        .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = cull, .front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE,
                             .enable_depth_bias = true, .depth_bias_constant_factor = -1.0f, .depth_bias_slope_factor = -1.5f},
        .depth_stencil_state = {.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, .enable_depth_test = true, .enable_depth_write = true},
        .target_info = {.color_target_descriptions = ct, .num_color_targets = 2, .depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT, .has_depth_stencil_target = true},
    };
    return SDL_CreateGPUGraphicsPipeline(r->dev, &ci);
}
static SDL_GPUComputePipeline *cpipe(R *r, const uint32_t *code, size_t bytes, int samplers, int ro_tex, int ro_buf, int rw_tex, int rw_buf, int tx, int ty) {
    SDL_GPUComputePipelineCreateInfo ci = {.code_size = bytes, .code = (const Uint8 *)code, .entrypoint = "main", .format = SDL_GPU_SHADERFORMAT_SPIRV,
                                           .num_samplers = (Uint32)samplers, .num_readonly_storage_textures = (Uint32)ro_tex,
                                           .num_readonly_storage_buffers = (Uint32)ro_buf, .num_readwrite_storage_textures = (Uint32)rw_tex,
                                           .num_readwrite_storage_buffers = (Uint32)rw_buf, .num_uniform_buffers = 1,
                                           .threadcount_x = (Uint32)tx, .threadcount_y = (Uint32)ty, .threadcount_z = 1};
    return SDL_CreateGPUComputePipeline(r->dev, &ci);
}

void nv_tiles_init(R *r) {
    nv_tiles_r *T = &r->tl;
    SDL_GPUDevice *d = r->dev;
    SDL_GPUShader *fw = shader(d, SPV_FACE_WORLD2, sizeof SPV_FACE_WORLD2, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1);
    SDL_GPUShader *fi = shader(d, SPV_FACE_INST2, sizeof SPV_FACE_INST2, SDL_GPU_SHADERSTAGE_VERTEX, 0, 3, 1);
    SDL_GPUShader *fp = shader(d, SPV_PROXY, sizeof SPV_PROXY, SDL_GPU_SHADERSTAGE_VERTEX, 0, 3, 1);
    SDL_GPUShader *ff = shader(d, SPV_FACE_ID_FRAG2, sizeof SPV_FACE_ID_FRAG2, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 1);
    T->face_w_back = gpipe(r, fw, ff, 16, 2, SDL_GPU_CULLMODE_BACK);
    T->face_i_back = gpipe(r, fi, ff, 12, 1, SDL_GPU_CULLMODE_BACK);
    T->proxy = gpipe(r, fp, ff, 0, 0, SDL_GPU_CULLMODE_NONE);
    SDL_GPUShader *cw = shader(d, SPV_FACE_WORLD_C, sizeof SPV_FACE_WORLD_C, SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 1);
    SDL_GPUShader *ci = shader(d, SPV_FACE_INST_C, sizeof SPV_FACE_INST_C, SDL_GPU_SHADERSTAGE_VERTEX, 0, 3, 1);
    SDL_GPUShader *cp2 = shader(d, SPV_PROXY_C, sizeof SPV_PROXY_C, SDL_GPU_SHADERSTAGE_VERTEX, 0, 3, 1);
    SDL_GPUShader *fn = shader(d, SPV_FACE_ND, sizeof SPV_FACE_ND, SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0, 1);
    T->c_face_w = gpipe(r, cw, fn, 16, 2, SDL_GPU_CULLMODE_NONE), T->c_face_w_back = gpipe(r, cw, fn, 16, 2, SDL_GPU_CULLMODE_BACK);
    T->c_face_i = gpipe(r, ci, fn, 12, 1, SDL_GPU_CULLMODE_NONE), T->c_face_i_back = gpipe(r, ci, fn, 12, 1, SDL_GPU_CULLMODE_BACK);
    T->c_proxy = gpipe(r, cp2, fn, 0, 0, SDL_GPU_CULLMODE_NONE);
    if (!T->c_face_w || !T->c_face_w_back || !T->c_face_i || !T->c_face_i_back || !T->c_proxy)
        fprintf(stderr, "bimble: the clip-distance pipelines were refused (%s): the section box by discard\n", SDL_GetError());
    SDL_GPUShader *all[] = {fw, fi, fp, ff, cw, ci, cp2, fn};
    for (int k = 0; k < 8; k++) if (all[k]) SDL_ReleaseGPUShader(d, all[k]);
    T->hiz0 = cpipe(r, SPV_HIZ0, sizeof SPV_HIZ0, 1, 0, 0, 1, 0, 8, 8);
    T->hizn = cpipe(r, SPV_HIZN, sizeof SPV_HIZN, 0, 0, 0, 1, 0, 8, 8);
    T->hizn2 = cpipe(r, SPV_HIZN2, sizeof SPV_HIZN2, 0, 0, 0, 1, 0, 8, 8);
    T->test = cpipe(r, SPV_TTEST, sizeof SPV_TTEST, 1, 0, 1, 0, 1, 64, 1); /* NUI3: the pyramid by a sampler */
    T->gate = cpipe(r, SPV_GATE, sizeof SPV_GATE, 0, 0, 3, 0, 1, 64, 1);
    T->seen = cpipe(r, SPV_SEEN, sizeof SPV_SEEN, 1, 0, 0, 0, 1, 8, 8);
    if (!T->face_w_back || !T->face_i_back || !T->proxy || !T->hiz0 || !T->hizn || !T->test || !T->gate)
        fprintf(stderr, "bimble: a tile pipeline was refused (%s): tiles draw without it\n", SDL_GetError());
    T->o = (nv_tiles_opts){.occlusion = 1, .error_px = 1, .lod = 1};
}

void nv_tiles_release(R *r) {
    nv_tiles_r *T = &r->tl;
    SDL_GPUDevice *d = r->dev;
    for (uint32_t k = 0; k < T->nsets; k++) {
        nv_tileset *ts = T->sets[k];
        for (uint32_t i = 0; i < ts->nT; i++) if (ts->g[i]) nv_gtile_free(d, ts->g[i]), ts->g[i] = NULL;
        nv_tileset_unref(d, ts);
    }
    free(T->sets), free(T->map);
    SDL_GPUGraphicsPipeline *gp[] = {T->face_w_back, T->face_i_back, T->proxy, T->c_face_w, T->c_face_w_back, T->c_face_i, T->c_face_i_back, T->c_proxy};
    for (int k = 0; k < 8; k++) if (gp[k]) SDL_ReleaseGPUGraphicsPipeline(d, gp[k]);
    SDL_GPUComputePipeline *cp[] = {T->hiz0, T->hizn, T->test, T->gate, T->seen, T->hizn2};
    for (int k = 0; k < 6; k++) if (cp[k]) SDL_ReleaseGPUComputePipeline(d, cp[k]);
    if (T->seen_buf) SDL_ReleaseGPUBuffer(d, T->seen_buf);
    for (int k = 0; k < 2; k++) if (T->seen_downs[k]) SDL_ReleaseGPUTransferBuffer(d, T->seen_downs[k]);
    free(seen_cpu), seen_cpu = NULL, nseen_cpu = 0;
    SDL_GPUBuffer *b[] = {T->cmd_a, T->cmd_b, T->gidx, T->gates, T->tests, T->vis, T->inst};
    for (int k = 0; k < 7; k++) if (b[k]) SDL_ReleaseGPUBuffer(d, b[k]);
    if (T->hiz) SDL_ReleaseGPUTexture(d, T->hiz);
    for (int q = 0; q < npg; q++)
        for (int b = 0; b < NBUF; b++) {
            if (pg[q].b[b]) SDL_ReleaseGPUBuffer(d, pg[q].b[b]);
            free(pg[q].fr[b]);
        }
    memset(pg, 0, sizeof pg), npg = 0, pg_bytes = 0;
    if (T->up) SDL_ReleaseGPUTransferBuffer(d, T->up);
    for (int k = 0; k < 2; k++) if (T->downs[k]) SDL_ReleaseGPUTransferBuffer(d, T->downs[k]);
    memset(T, 0, sizeof *T);
}

static int hiz_pair = 1; /* NUI3 hiz_pair: two levels of the pyramid a dispatch (hizn2.comp); 0 one (hizn.comp) */
void nv_tiles_hiz_pair(int on) { hiz_pair = on != 0; }
static uint32_t hiz_block = 4; /* hiz_block: pixels a level-0 texel side (1, 2, 4) */
void nv_tiles_hiz_block(nv_render *r, uint32_t b) {
    b = b <= 1 ? 1 : b <= 2 ? 2 : 4;
    if (b != hiz_block) {
        hiz_block = b;
        if (r->W && r->H) SDL_WaitForGPUIdle(r->dev), nv_tiles_targets(r, r->W, r->H);
    }
}
/* the depth pyramid for a w x h depth: level 0 a quarter a side (each texel the farthest of its 4 x 4 pixels), then
 * halves (the farthest of 2 x 2), side by side in one R32F texture */
int nv_tiles_targets(R *r, uint32_t w, uint32_t h) {
    nv_tiles_r *T = &r->tl;
    if (T->hiz) SDL_ReleaseGPUTexture(r->dev, T->hiz), T->hiz = NULL, r->gpu_bytes -= (uint64_t)T->hiz_w * T->hiz_h * 4;
    uint32_t B = hiz_block, lw = (w + B - 1) / B, lh = (h + B - 1) / B, ox = 0, n = 0;
    while (n < 16) {
        T->lv[n][0] = ox, T->lv[n][1] = lw, T->lv[n][2] = lh;
        ox += lw, n++;
        if (lw == 1 && lh == 1) break;
        lw = (lw + 1) / 2, lh = (lh + 1) / 2;
    }
    T->hiz_levels = n, T->hiz_w = ox, T->hiz_h = T->lv[0][2];
    SDL_GPUTextureCreateInfo ci = {.type = SDL_GPU_TEXTURETYPE_2D, .format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
                                   .usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE |
                                            SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE,
                                   .width = T->hiz_w, .height = T->hiz_h, .layer_count_or_depth = 1, .num_levels = 1};
    T->hiz = SDL_CreateGPUTexture(r->dev, &ci);
    if (T->hiz) r->gpu_bytes += (uint64_t)T->hiz_w * T->hiz_h * 4;
    return T->hiz ? 0 : -1;
}

/* ---- one frame ---- */
typedef SDL_GPUIndexedIndirectDrawCommand Cmd;
/* the frame's commands in buckets: one draw each (pass, page, kind) */
enum { K_MERGED, K_MERGED_BACK, K_SHAPES, K_SHAPES_BACK, K_PROXY, NKIND };
typedef struct { Cmd *c; uint32_t *g; uint32_t n, cap, at; } Bucket;
typedef struct { float c[4], ax[4], ay[4], az[4]; uint32_t g[4]; } Test; /* NUI3: a box by its centre and half axes (ttest.comp) */
static Bucket bk[2][NPAGE][NKIND];

static struct {
    Cmd *cmd;
    uint32_t *gidx, ncmd;
    uint32_t *inst, ninst, cinst;
    Test *test;
    uint32_t ntest, ctest;
    uint32_t ngates;
    uint64_t *gate_tris;
    uint32_t *gate_tiles, cgates;
    uint64_t tris1;
    uint32_t tiles1, tiles_cold;
    int oom;
} F;

#define GROW(arr, n, cap, more)                                                                                          \
    do {                                                                                                                 \
        if ((n) + (more) > (cap)) {                                                                                      \
            uint32_t nc = (cap) ? (cap) : 1024;                                                                          \
            while (nc < (n) + (more)) nc *= 2;                                                                           \
            void *np_ = realloc((arr), (size_t)nc * sizeof *(arr));                                                      \
            if (!np_) { F.oom = 1; break; }                                                                              \
            (arr) = np_, (cap) = nc;                                                                                     \
        }                                                                                                                \
    } while (0)

static uint32_t new_gate(void) {
    if (F.ngates >= F.cgates) {
        uint32_t nc = F.cgates ? F.cgates * 2 : 1024;
        uint64_t *a = realloc(F.gate_tris, nc * 8ull);
        if (a) F.gate_tris = a;
        uint32_t *b = a ? realloc(F.gate_tiles, nc * 4ull) : NULL;
        if (!a || !b) { F.oom = 1; return 0; }
        F.gate_tiles = b, F.cgates = nc;
    }
    F.gate_tris[F.ngates] = 0, F.gate_tiles[F.ngates] = 0;
    return F.ngates++;
}

static void add_cmd(int pass, int page, int kind, uint32_t nidx, uint32_t ninst, uint32_t first, int32_t voff, uint32_t first_inst, uint32_t gate) {
    Bucket *b = &bk[pass - 1][page][kind];
    if (b->n == b->cap) {
        uint32_t c = b->cap ? b->cap * 2 : 256;
        Cmd *nc = realloc(b->c, c * sizeof *nc);
        uint32_t *ng = nc ? realloc(b->g, c * 4ull) : NULL;
        if (nc) b->c = nc;
        if (!nc || !ng) { F.oom = 1; return; }
        b->g = ng, b->cap = c;
    }
    b->c[b->n] = (Cmd){.num_indices = nidx, .num_instances = ninst, .first_index = first, .vertex_offset = voff, .first_instance = first_inst};
    b->g[b->n++] = gate;
}

/* an element's size on screen (px) from its sphere; cut 0: everything */
typedef struct { double eye[3], k; int ortho; float cut; } Cut;
static int small(const Cut *c, const float ctr[3], float r) { /* r k / (dist - r) < cut, without a square root */
    if (c->cut <= 0) return 0;
    if (c->ortho) return r * c->k < c->cut;
    float dx = ctr[0] - (float)c->eye[0], dy = ctr[1] - (float)c->eye[1], dz = ctr[2] - (float)c->eye[2];
    float lim = r + r * (float)c->k / c->cut; /* small when farther than this (and farther than r + 1) */
    float d2 = dx * dx + dy * dy + dz * dz;
    return d2 > lim * lim && d2 > (r + 1) * (r + 1);
}

/* tile t's commands (pass 1 or 2, under `gate`): its triangles */
static uint64_t emit(nv_tiles_r *T, nv_tileset *ts, nv_gtile *g, int pass, uint32_t gate, const Cut *cut) {
    (void)ts;
    uint64_t tris = 0;
    int back = T->o.cull_back && !T->o.reference, pgi = g->page;
    if (pgi < 0) return 0;
    if (g->kind == 2) {
        for (uint32_t k = 0; k < g->nparts; k++)
            add_cmd(pass, pgi, K_PROXY, g->part[k][2], 1, g->off[B_I] + g->part[k][1], (int32_t)(g->off[B_P] * 2), g->off[B_TT] << 3 | (g->part[k][0] < 8 ? g->part[k][0] : 7), gate),
                tris += g->part[k][2] / 3;
        return tris;
    }
    /* merged: runs of kept elements (closed ones apart when their back faces are culled: their own pipeline) */
    for (int grp = 0; grp < 2; grp++) {
        uint32_t a = back ? (grp ? g->nclosed : 0) : (grp ? g->nel : 0), b = back ? (grp ? g->nel : g->nclosed) : g->nel;
        int kind = back && grp == 0 ? K_MERGED_BACK : K_MERGED;
        uint32_t run0 = 0, runn = 0;
        for (uint32_t i = a; i < b; i++) {
            const GElem *e = &g->el[i];
            if (small(cut, e->c, e->r)) {
                if (runn) add_cmd(pass, pgi, kind, runn, 1, g->off[B_I] + run0, (int32_t)g->off[B_V], 0, gate), tris += runn / 3, runn = 0;
                continue;
            }
            if (runn && run0 + runn == e->first) runn += e->n;
            else {
                if (runn) add_cmd(pass, pgi, kind, runn, 1, g->off[B_I] + run0, (int32_t)g->off[B_V], 0, gate), tris += runn / 3;
                run0 = e->first, runn = e->n;
            }
        }
        if (runn) add_cmd(pass, pgi, kind, runn, 1, g->off[B_I] + run0, (int32_t)g->off[B_V], 0, gate), tris += runn / 3;
    }
    /* shapes: the kept copies' placements listed (the instanced shader reads placement[inst[i]], the page's) */
    for (uint32_t s2 = 0; s2 < g->nshape; s2++) {
        const GShape *sh = &g->sh[s2];
        int kind = back && sh->closed ? K_SHAPES_BACK : K_SHAPES;
        for (int mir = 0; mir < 2; mir++) {
            uint32_t a = sh->copy0 + (mir ? sh->ncopy - sh->nmir : 0), b = mir ? sh->copy0 + sh->ncopy : sh->copy0 + sh->ncopy - sh->nmir;
            if (a >= b) continue;
            GROW(F.inst, F.ninst, F.cinst, b - a);
            if (F.oom) return tris;
            uint32_t i0 = F.ninst;
            for (uint32_t k = a; k < b; k++)
                if (!small(cut, g->cp[k].c, g->cp[k].r)) F.inst[F.ninst++] = g->off[B_PL] + g->cp[k].plc;
            uint32_t kept = F.ninst - i0;
            if (!kept) continue;
            uint32_t first = mir && sh->firstf != UINT32_MAX ? sh->firstf : sh->first;
            add_cmd(pass, pgi, kind, sh->n, kept, g->off[B_I] + first, (int32_t)g->off[B_SV], i0, gate), tris += (uint64_t)sh->n / 3 * kept;
        }
    }
    return tris;
}

/* NUI2, occlusion to the element: a loaded leaf's elements in groups of CL (index order: neighbours in the leaf), each
 * group drawn in pass 1 when one of its elements was seen last frame (the id buffer: seen.comp), else tested on its own
 * box (its members' spheres) against pass 1's depth and drawn in pass 2 when visible. */
#define CL 8
static uint64_t lastc; /* the frame last collected (serial: this one's previous; frames in flight: the one before) */
/* NUI2, where the triangles go (BIMBLE_TILES_SURPLUS=1 or nv_tiles_surplus_on): each group drawn recorded, its elements
 * checked against the elements the frame showed (seen) when the frame is collected */
typedef struct { nv_gtile *g; uint32_t a, b, gate; int32_t shape; uint8_t pass; } GRec;
static GRec *grec[2];
static uint32_t ngrec[2], cgrec[2];
static int surplus_on = -1;
static int surplus_top;
static struct { double t; uint32_t e; float ob[12]; } top[8];
void nv_tiles_surplus_top(void) {
    for (int j = 0; j < 8; j++)
        if (top[j].t > 0) {
            const float *o = top[j].ob;
            fprintf(stderr, "surplus: element %u, %.0f triangles, box centre %.0f %.0f %.0f, half axes |%.0f| |%.0f| |%.0f|\n", top[j].e, top[j].t, o[0], o[1], o[2],
                    sqrtf(o[3] * o[3] + o[4] * o[4] + o[5] * o[5]), sqrtf(o[6] * o[6] + o[7] * o[7] + o[8] * o[8]), sqrtf(o[9] * o[9] + o[10] * o[10] + o[11] * o[11]));
        }
}
static double surplus[8]; /* need, pass 1, pass 2, not seen: same group as a seen one, box passed alone, drawn in pass 1 but hidden now; leaves' total; frames */
void nv_tiles_surplus(double out[8]) { memcpy(out, surplus, sizeof surplus); }
static void grec_add(int slot, nv_gtile *g, uint32_t a, uint32_t b, int32_t shape, int pass, uint32_t gate) {
    if (surplus_on < 0) surplus_on = getenv("BIMBLE_TILES_SURPLUS") != NULL;
    if (!surplus_on) return;
    if (ngrec[slot] == cgrec[slot]) {
        uint32_t c = cgrec[slot] ? cgrec[slot] * 2 : 4096;
        GRec *n2 = realloc(grec[slot], c * sizeof *n2);
        if (!n2) return;
        grec[slot] = n2, cgrec[slot] = c;
    }
    grec[slot][ngrec[slot]++] = (GRec){g, a, b, gate, shape, (uint8_t)pass};
}
static int cur_slot;
static Cut slot_cut[2];
static int seen_last(uint32_t e, uint64_t fr) { (void)fr; return e < nseen_cpu && lastc && seen_cpu[e] == (uint32_t)lastc; }
static Test *new_test(uint32_t gate) {
    GROW(F.test, F.ntest, F.ctest, 1);
    if (F.oom) return NULL;
    Test *te = &F.test[F.ntest++];
    memset(te, 0, sizeof *te);
    te->g[0] = gate, te->g[2] = 1;
    return te;
}
static void test_aabb(Test *te, const float lo[3], const float hi[3]) {
    for (int d = 0; d < 3; d++) te->c[d] = (lo[d] + hi[d]) / 2;
    te->ax[0] = (hi[0] - lo[0]) / 2, te->ay[1] = (hi[1] - lo[1]) / 2, te->az[2] = (hi[2] - lo[2]) / 2;
}
static void test_ob(uint32_t gate, const float ob[12]) {
    Test *te = new_test(gate);
    if (!te) return;
    memcpy(te->c, ob, 12), memcpy(te->ax, ob + 3, 12), memcpy(te->ay, ob + 6, 12), memcpy(te->az, ob + 9, 12);
}
static void test_world(uint32_t gate, const float lo[3], const float hi[3]) { /* 1 mm wider (f32 slack), as grow_box */
    Test *te = new_test(gate);
    if (!te) return;
    float l[3] = {lo[0] - 1, lo[1] - 1, lo[2] - 1}, h[3] = {hi[0] + 1, hi[1] + 1, hi[2] + 1};
    test_aabb(te, l, h);
}
static uint32_t group_test(float lo[3], float hi[3]) {
    uint32_t g = new_gate();
    Test *te = new_test(g);
    if (te) test_aabb(te, lo, hi);
    return g;
}
/* NUI3, the tighter test (occlusion_test): 0 a group's box (NUI2); 1 each element's world box, the group's gate shared
 * (pass 1 draws exactly the elements seen last frame, the others in groups); 2 as 1 with each element's own placed box
 * when tighter (test_box) */
static int TMODE = 2;
/* occlusion_split: pieces a side at most (1: none), about test_split_px pixels each; occlusion_span: texels a side read */
static uint32_t test_split = 1, test_split_px = 64, test_span = 4;
void nv_tiles_test_mode(int mode, int split, int span, int split_px) {
    TMODE = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    test_split_px = split_px < 8 ? 8 : split_px > 1024 ? 1024 : (uint32_t)split_px;
    test_split = split < 1 ? 1 : split > 8 ? 8 : (uint32_t)split;
    test_span = span < 2 ? 2 : span > 16 ? 16 : (uint32_t)span;
}
static void elem_test(uint32_t gate, const float lo[3], const float hi[3], const float ob[12]) {
    if (TMODE >= 2) test_ob(gate, ob);
    else test_world(gate, lo, hi);
}
static void grow_box(float lo[3], float hi[3], const float a[3], const float b[3]) { /* an element's box, 1 mm wider (f32 slack) */
    for (int d = 0; d < 3; d++) lo[d] = fminf(lo[d], a[d] - 1), hi[d] = fmaxf(hi[d], b[d] + 1);
}
/* NUI3 seen_step: the elements seen read from one pixel in seen_step a side (1 every pixel): one missed is tested in pass 2
 * (exact), only drawn a frame later in pass 1 */
static uint32_t seen_step = 1;
void nv_tiles_seen_step(uint32_t s) { seen_step = s < 1 ? 1 : s > 4 ? 4 : s; }
static int tile_gate = 1; /* NUI3 occlusion_tiles: a loaded leaf hidden last frame tested by its box alone */
void nv_tiles_tile_gate(int on) { tile_gate = on != 0; }
static uint32_t CLN = 2; /* occlusion_group: elements a group (the A/B: 1, 2, 4, 8, 16, 32, 64) */
void nv_tiles_group_size(uint32_t n) { CLN = n < 1 ? 1 : n > 256 ? 256 : n; }
static void emit_leaf_elems(nv_tiles_r *T, nv_gtile *g, uint64_t fr, const Cut *cut) {
    int back = T->o.cull_back && !T->o.reference, pgi = g->page;
    for (int grp = 0; grp < 2; grp++) {
        uint32_t a = back ? (grp ? g->nclosed : 0) : (grp ? g->nel : 0), b = back ? (grp ? g->nel : g->nclosed) : g->nel;
        int kind = back && grp == 0 ? K_MERGED_BACK : K_MERGED;
        uint32_t p0 = 0, pn = 0, r0 = 0, rn = 0, gn = 0, gate = 0;
        uint64_t gt = 0;
        for (uint32_t i = a; i < b; i++) {
            const GElem *e = &g->el[i];
            if (small(cut, e->c, e->r)) continue;
            if (seen_last(e->elem, fr)) { /* pass 1, ungated: runs joined */
                grec_add(cur_slot, g, i, i + 1, -1, 1, 0);
                if (pn && p0 + pn == e->first) pn += e->n;
                else {
                    if (pn) add_cmd(1, pgi, kind, pn, 1, g->off[B_I] + p0, (int32_t)g->off[B_V], 0, 0), F.tris1 += pn / 3;
                    p0 = e->first, pn = e->n;
                }
                continue;
            }
            if (!gn) gate = new_gate(), gt = 0;
            elem_test(gate, e->lo, e->hi, e->ob);
            grec_add(cur_slot, g, i, i + 1, -1, 2, gate);
            if (rn && r0 + rn == e->first) rn += e->n;
            else {
                if (rn) add_cmd(2, pgi, kind, rn, 1, g->off[B_I] + r0, (int32_t)g->off[B_V], 0, gate), gt += rn / 3;
                r0 = e->first, rn = e->n;
            }
            if (++gn == CLN) {
                if (rn) add_cmd(2, pgi, kind, rn, 1, g->off[B_I] + r0, (int32_t)g->off[B_V], 0, gate), gt += rn / 3, rn = 0;
                F.gate_tris[gate] += gt, gn = 0;
            }
            if (F.oom) return;
        }
        if (pn) add_cmd(1, pgi, kind, pn, 1, g->off[B_I] + p0, (int32_t)g->off[B_V], 0, 0), F.tris1 += pn / 3;
        if (gn) {
            if (rn) add_cmd(2, pgi, kind, rn, 1, g->off[B_I] + r0, (int32_t)g->off[B_V], 0, gate), gt += rn / 3;
            F.gate_tris[gate] += gt;
        }
    }
    for (uint32_t s2 = 0; s2 < g->nshape; s2++) {
        const GShape *sh = &g->sh[s2];
        int kind = back && sh->closed ? K_SHAPES_BACK : K_SHAPES;
        for (int mir = 0; mir < 2; mir++) {
            uint32_t a = sh->copy0 + (mir ? sh->ncopy - sh->nmir : 0), b = mir ? sh->copy0 + sh->ncopy : sh->copy0 + sh->ncopy - sh->nmir;
            if (a >= b) continue;
            uint32_t first = mir && sh->firstf != UINT32_MAX ? sh->firstf : sh->first;
            GROW(F.inst, F.ninst, F.cinst, b - a);
            if (F.oom) return;
            uint32_t i0 = F.ninst; /* pass 1: the copies seen, one command */
            for (uint32_t k = a; k < b; k++)
                if (!small(cut, g->cp[k].c, g->cp[k].r) && seen_last(g->cp[k].elem, fr)) F.inst[F.ninst++] = g->off[B_PL] + g->cp[k].plc, grec_add(cur_slot, g, k, k + 1, (int32_t)s2, 1, 0);
            if (F.ninst > i0) add_cmd(1, pgi, kind, sh->n, F.ninst - i0, g->off[B_I] + first, (int32_t)g->off[B_SV], i0, 0), F.tris1 += (uint64_t)sh->n / 3 * (F.ninst - i0);
            uint32_t gn = 0, gate = 0;
            i0 = F.ninst;
            for (uint32_t k = a; k < b; k++) { /* pass 2: the others in groups, each copy tested, the group's gate shared */
                if (small(cut, g->cp[k].c, g->cp[k].r) || seen_last(g->cp[k].elem, fr)) continue;
                if (!gn) gate = new_gate(), i0 = F.ninst;
                F.inst[F.ninst++] = g->off[B_PL] + g->cp[k].plc;
                elem_test(gate, g->cp[k].lo, g->cp[k].hi, g->cp[k].ob);
                grec_add(cur_slot, g, k, k + 1, (int32_t)s2, 2, gate);
                if (++gn == CLN) add_cmd(2, pgi, kind, sh->n, gn, g->off[B_I] + first, (int32_t)g->off[B_SV], i0, gate), F.gate_tris[gate] += (uint64_t)sh->n / 3 * gn, gn = 0;
                if (F.oom) return;
            }
            if (gn) add_cmd(2, pgi, kind, sh->n, gn, g->off[B_I] + first, (int32_t)g->off[B_SV], i0, gate), F.gate_tris[gate] += (uint64_t)sh->n / 3 * gn;
        }
    }
}
static void emit_leaf_groups(nv_tiles_r *T, nv_gtile *g, uint64_t fr, const Cut *cut) {
    if (TMODE >= 1) { emit_leaf_elems(T, g, fr, cut); return; }
    int back = T->o.cull_back && !T->o.reference, pgi = g->page;
    for (int grp = 0; grp < 2; grp++) {
        uint32_t a = back ? (grp ? g->nclosed : 0) : (grp ? g->nel : 0), b = back ? (grp ? g->nel : g->nclosed) : g->nel;
        int kind = back && grp == 0 ? K_MERGED_BACK : K_MERGED;
        uint32_t p0 = 0, pn = 0; /* pass 1's run, carried across groups (one command for consecutive groups seen) */
        for (uint32_t c0 = a; c0 < b; c0 += CLN) {
            uint32_t c1 = c0 + CLN < b ? c0 + CLN : b;
            int seen = 0, any = 0;
            float lo[3] = {3e38f, 3e38f, 3e38f}, hi[3] = {-3e38f, -3e38f, -3e38f};
            for (uint32_t i = c0; i < c1; i++) {
                if (small(cut, g->el[i].c, g->el[i].r)) continue;
                any = 1, seen |= seen_last(g->el[i].elem, fr);
                grow_box(lo, hi, g->el[i].lo, g->el[i].hi);
            }
            if (!any) continue;
            grec_add(cur_slot, g, c0, c1, -1, seen ? 1 : 2, 0);
            if (seen) { /* pass 1, ungated: runs joined with the previous groups' */
                for (uint32_t i = c0; i < c1; i++) {
                    const GElem *e = &g->el[i];
                    if (small(cut, e->c, e->r)) {
                        if (pn) add_cmd(1, pgi, kind, pn, 1, g->off[B_I] + p0, (int32_t)g->off[B_V], 0, 0), F.tris1 += pn / 3, pn = 0;
                        continue;
                    }
                    if (pn && p0 + pn == e->first) pn += e->n;
                    else {
                        if (pn) add_cmd(1, pgi, kind, pn, 1, g->off[B_I] + p0, (int32_t)g->off[B_V], 0, 0), F.tris1 += pn / 3;
                        p0 = e->first, pn = e->n;
                    }
                }
                continue;
            }
            uint32_t gate = group_test(lo, hi);
            if (surplus_on > 0 && ngrec[cur_slot]) grec[cur_slot][ngrec[cur_slot] - 1].gate = gate;
            uint64_t tris = 0;
            uint32_t run0 = 0, runn = 0;
            for (uint32_t i = c0; i < c1; i++) {
                const GElem *e = &g->el[i];
                if (small(cut, e->c, e->r)) {
                    if (runn) add_cmd(2, pgi, kind, runn, 1, g->off[B_I] + run0, (int32_t)g->off[B_V], 0, gate), tris += runn / 3, runn = 0;
                    continue;
                }
                if (runn && run0 + runn == e->first) runn += e->n;
                else {
                    if (runn) add_cmd(2, pgi, kind, runn, 1, g->off[B_I] + run0, (int32_t)g->off[B_V], 0, gate), tris += runn / 3;
                    run0 = e->first, runn = e->n;
                }
            }
            if (runn) add_cmd(2, pgi, kind, runn, 1, g->off[B_I] + run0, (int32_t)g->off[B_V], 0, gate), tris += runn / 3;
            F.gate_tris[gate] += tris;
        }
        if (pn) add_cmd(1, pgi, kind, pn, 1, g->off[B_I] + p0, (int32_t)g->off[B_V], 0, 0), F.tris1 += pn / 3;
    }
    for (uint32_t s2 = 0; s2 < g->nshape; s2++) {
        const GShape *sh = &g->sh[s2];
        int kind = back && sh->closed ? K_SHAPES_BACK : K_SHAPES;
        for (int mir = 0; mir < 2; mir++) {
            uint32_t a = sh->copy0 + (mir ? sh->ncopy - sh->nmir : 0), b = mir ? sh->copy0 + sh->ncopy : sh->copy0 + sh->ncopy - sh->nmir;
            uint32_t first = mir && sh->firstf != UINT32_MAX ? sh->firstf : sh->first;
            for (uint32_t c0 = a; c0 < b; c0 += CLN) {
                uint32_t c1 = c0 + CLN < b ? c0 + CLN : b;
                GROW(F.inst, F.ninst, F.cinst, c1 - c0);
                if (F.oom) return;
                uint32_t i0 = F.ninst;
                int seen = 0;
                float lo[3] = {3e38f, 3e38f, 3e38f}, hi[3] = {-3e38f, -3e38f, -3e38f};
                for (uint32_t k = c0; k < c1; k++) {
                    if (small(cut, g->cp[k].c, g->cp[k].r)) continue;
                    F.inst[F.ninst++] = g->off[B_PL] + g->cp[k].plc;
                    seen |= seen_last(g->cp[k].elem, fr);
                    grow_box(lo, hi, g->cp[k].lo, g->cp[k].hi);
                }
                uint32_t kept = F.ninst - i0;
                if (!kept) continue;
                uint32_t gate = seen ? 0 : group_test(lo, hi);
                grec_add(cur_slot, g, c0, c1, (int32_t)s2, seen ? 1 : 2, gate);
                add_cmd(seen ? 1 : 2, pgi, kind, sh->n, kept, g->off[B_I] + first, (int32_t)g->off[B_SV], i0, gate);
                if (seen) F.tris1 += (uint64_t)sh->n / 3 * kept;
                else F.gate_tris[gate] += (uint64_t)sh->n / 3 * kept;
            }
        }
    }
}

/* a set's elements under the renderer's states (hidden, selected: colour_cpu differs from colour_base) mark the tiles
 * holding them: the walker descends there (a proxy has no element boundary) */
static void descend_marks(R *r, nv_tileset *ts) {
    uint64_t gen = r->state_gen;
    if (ts->desc_gen == gen) return;
    ts->desc_gen = gen;
    const cc_tiles_toc *toc = &ts->toc;
    uint32_t *pre = malloc((toc->nlist + 1ull) * 4);
    if (!pre) { memset(ts->descend, 1, ts->nT); return; }
    pre[0] = 0;
    for (uint32_t i = 0; i < toc->nlist; i++) {
        uint32_t e = ts->elem0 + cc_tiles_list(toc, i);
        int marked = e < r->ncolour && r->colour_cpu[e] != r->colour_base[e];
        pre[i + 1] = pre[i] + (uint32_t)marked;
    }
    for (uint32_t t = 0; t < ts->nT; t++) {
        cc_tile x;
        cc_tiles_get(toc, t, &x);
        ts->descend[t] = x.el0 + (uint64_t)x.nel <= toc->nlist && pre[x.el0 + x.nel] > pre[x.el0];
    }
    free(pre);
}

/* THE walker call (SHELL1 adds a parameter here) */
static uint32_t nv_tiles_walk(nv_tileset *ts, const cc_tiles_view *v, const nv_tiles_opts *o) {
    uint64_t tris = 0;
    unsigned fl = (o->lod ? 0u : (unsigned)CC_TILES_SEL_LEAVES) | (o->priority ? (unsigned)CC_TILES_SEL_PRIORITY : 0u); /* NUI3: exterior first */
    return cc_tiles_select(&ts->toc, v, o->error_px, o->tri_budget ? o->tri_budget : UINT64_MAX, fl, ts->descend, ts->sel, ts->nT, &tris);
}

static int buf_room(R *r, SDL_GPUBuffer **b, uint32_t *cap, uint32_t need, SDL_GPUBufferUsageFlags use) {
    if (*b && *cap >= need) return 0;
    uint32_t c = *cap ? *cap : 4096;
    while (c < need) c *= 2;
    SDL_GPUBufferCreateInfo ci = {.usage = use, .size = c};
    SDL_GPUBuffer *n = SDL_CreateGPUBuffer(r->dev, &ci);
    if (!n) return -1;
    if (*b) SDL_ReleaseGPUBuffer(r->dev, *b), r->gpu_bytes -= *cap;
    *b = n, *cap = c, r->gpu_bytes += c;
    return 0;
}

/* Before the render pass: the walk, the commands, their upload. Returns 1 when tiles draw this frame. */
int nv_tiles_prepare(R *r, SDL_GPUCommandBuffer *cmd, const float vp[16], const double eye[3], const float pl[5][4], double kpx, int ortho, float cut_px) {
    nv_tiles_r *T = &r->tl;
    F.ncmd = F.ninst = F.ntest = 0, F.ngates = 0, F.tris1 = 0, F.tiles1 = 0, F.tiles_cold = 0, F.oom = 0;
    for (int ps = 0; ps < 2; ps++)
        for (int q = 0; q < NPAGE; q++)
            for (int k = 0; k < NKIND; k++) bk[ps][q][k].n = 0;
    T->ntests = 0, T->read_vis = 0, T->read_vis_planned = 0, T->nmap = 0, T->seen_planned = 0;
    if (!T->nsets) return 0;
    int ref = T->o.reference, occl = T->o.occlusion && !ref && T->test && T->gate && T->hiz0 && T->hiz;
    if (!ref) T->frame++;
    uint64_t fr = T->frame;
    new_gate(); /* gate 0: always */
    T->ms_walk = 0;
    if (!ref) cur_slot = r->slot, ngrec[cur_slot] = 0, slot_cut[cur_slot] = (Cut){{eye[0], eye[1], eye[2]}, kpx, ortho, cut_px};
    Cut cut = {{eye[0], eye[1], eye[2]}, kpx, ortho, cut_px};
    uint32_t nmap = 0;
    uint32_t want_n = 0;
    for (uint32_t si = 0; si < T->nsets; si++) {
        nv_tileset *ts = T->sets[si];
        if (!ts->shown || !ts->nT) continue;
        descend_marks(r, ts);
        /* the view in the set's frame: p_scene = R p + off */
        cc_tiles_view v;
        memset(&v, 0, sizeof v);
        double c = ts->c, s = ts->s, e0 = eye[0] - ts->off[0], e1 = eye[1] - ts->off[1];
        v.eye[0] = c * e0 + s * e1, v.eye[1] = -s * e0 + c * e1, v.eye[2] = eye[2] - ts->off[2];
        for (int k = 0; k < 5; k++) {
            double n0 = pl[k][0], n1 = pl[k][1], n2 = pl[k][2];
            v.planes[k][0] = c * n0 + s * n1, v.planes[k][1] = -s * n0 + c * n1, v.planes[k][2] = n2;
            v.planes[k][3] = pl[k][3] + n0 * ts->off[0] + n1 * ts->off[1] + n2 * ts->off[2];
        }
        v.nplanes = 5;
        if (ortho) v.px_per_mm = kpx;
        else v.k = kpx;
        double tw = nv_now_ms();
        /* the same view, options and marks as this set's last walk: the same tiles (still frames while tiles arrive) */
        int same = ts->last_ok && !memcmp(&ts->last_view, &v, sizeof v) && ts->last_err == T->o.error_px && ts->last_budget == T->o.tri_budget &&
                   ts->last_lod == T->o.lod && ts->last_gen == ts->desc_gen;
        if (!same) {
            ts->nsel = nv_tiles_walk(ts, &v, &T->o);
            ts->last_view = v, ts->last_err = T->o.error_px, ts->last_budget = T->o.tri_budget, ts->last_lod = T->o.lod, ts->last_gen = ts->desc_gen, ts->last_ok = 1;
        }
        T->ms_walk += nv_now_ms() - tw;
        for (uint32_t k = 0; k < ts->nsel; k++) {
            uint32_t t = ts->sel[k];
            if (!ref) ts->used[t] = fr;
            nv_gtile *g = ts->g[t];
            uint32_t own = 0;
            int leafg = g && g->kind == 1 && T->groups; /* a loaded leaf drawn by its elements */
            if (occl && (!leafg || tile_gate)) { /* NUI3 (occlusion_tiles): a loaded leaf's box tested too: hidden, its elements wait */
                own = new_gate();
                if (nmap >= T->test_cap_cpu) {
                    uint32_t nc = T->test_cap_cpu ? T->test_cap_cpu * 2 : 1024;
                    TMap *nm2 = realloc(T->map, nc * sizeof *nm2);
                    if (!nm2) F.oom = 1;
                    else T->map = nm2, T->test_cap_cpu = nc;
                }
                if (F.oom) break;
                Test *te = new_test(own);
                if (!te) break;
                test_aabb(te, &ts->sbox[6ull * t], &ts->sbox[6ull * t + 3]);
                te->g[2] = g != NULL;
                T->map[nmap++] = (TMap){ts, t, own}, T->nmap = nmap;
            }
            if (g && occl && leafg) { /* a loaded leaf: its elements' groups decide (above) */
                if (tile_gate && own && lastc && ts->tested[t] == lastc && ts->vis[t] != lastc) {
                    /* NUI3: its box was hidden last frame: the whole leaf under its box's test (one gate, a few commands, no
                     * test an element: the CPU and the tests of a frame scale with what is near the visible, not with what is
                     * loaded); found visible, its elements decide from the next frame on. Exact: the box holds the leaf. */
                    Cut all = cut;
                    all.cut = cut.cut > 0 ? cut.cut : 0;
                    uint64_t tr = emit(T, ts, g, 2, own, &all);
                    F.gate_tris[own] += tr, F.gate_tiles[own]++;
                    F.tiles_cold++;
                    continue;
                }
                emit_leaf_groups(T, g, fr, &cut);
                F.tiles1++;
                continue;
            }
            if (g) {
                int pass = !occl || (lastc && ts->vis[t] == lastc) ? 1 : 2;
                uint64_t tr = emit(T, ts, g, pass, pass == 1 ? 0 : own, &cut);
                if (pass == 1) F.tris1 += tr, F.tiles1++;
                else F.gate_tris[own] += tr, F.gate_tiles[own]++;
                continue;
            }
            /* not loaded: wanted (with occlusion: when its box was visible last frame), its nearest loaded ancestor's proxy stands in */
            if (!ref && ts->st[t] == 0 && (!occl || (lastc && ts->tested[t] == lastc && ts->vis[t] == lastc)) && want_n < 256)
                T->want[want_n++] = (nv_tile_want){ts, t, k};
            uint32_t a = t;
            cc_tile x;
            for (int up = 0; up < 64; up++) {
                cc_tiles_get(&ts->toc, a, &x);
                if (x.parent == CC_TILES_NONE || x.parent >= ts->nT) { a = CC_TILES_NONE; break; }
                a = x.parent;
                if (ts->g[a] && ts->g[a]->kind == 2) break;
            }
            if (a == CC_TILES_NONE || !ts->g[a]) continue;
            if (ts->standin_at[a] != fr + (ref ? 1ull << 62 : 0)) { /* its first use this frame: its own gate (or drawn at once) */
                ts->standin_at[a] = fr + (ref ? 1ull << 62 : 0);
                if (!ref) ts->used[a] = fr;
                uint32_t sg = occl ? new_gate() : 0;
                ts->standin_gate[a] = sg;
                uint64_t tr = emit(T, ts, ts->g[a], 2, sg, &cut);
                if (occl) F.gate_tris[sg] += tr, F.gate_tiles[sg]++;
                else F.tris1 += tr, F.tiles1++;
            }
            if (occl) F.test[F.ntest - 1].g[1] = ts->standin_gate[a];
        }
        if (!ref && ts->st[0] == 0 && !ts->g[0] && want_n < 256) T->want[want_n++] = (nv_tile_want){ts, 0, 0}; /* the root first: the first picture */
        if (F.oom) break;
    }
    if (!ref) {
        T->want_n = want_n;
        /* the budget: tiles not used this frame released, least recently used first; never a set's root */
        uint64_t budget = T->o.budget;
        if (budget && T->squeeze) budget = budget / 5 * 4, T->squeeze = 0;
        if (budget && T->gpu_bytes > budget) { /* the resident tiles not used this frame, least recently used first */
            static struct { uint64_t used; nv_tileset *ts; uint32_t t; } *cand;
            static uint32_t ccand;
            uint32_t nc = 0;
            for (uint32_t si = 0; si < T->nsets; si++) {
                nv_tileset *ts = T->sets[si];
                for (uint32_t t = 1; t < ts->nT; t++) {
                    if (!ts->g[t] || ts->used[t] >= fr) continue;
                    if (nc == ccand) {
                        uint32_t c = ccand ? ccand * 2 : 1024;
                        void *n2 = realloc(cand, c * sizeof *cand);
                        if (!n2) break;
                        cand = n2, ccand = c;
                    }
                    cand[nc].used = ts->used[t], cand[nc].ts = ts, cand[nc].t = t, nc++;
                }
            }
            qsort(cand, nc, sizeof *cand, cmp_used);
            for (uint32_t i = 0; i < nc && T->gpu_bytes > budget; i++) drop_tile(r, cand[i].ts, cand[i].t);
        }
    }
    T->ntests = F.ntest;
    T->read_vis_planned = occl && F.ntest > 0;
    if (F.oom) { fprintf(stderr, "bimble: tiles: out of memory this frame\n"); return 0; }
    /* the buckets laid end to end: pass 1's, then pass 2's (the gate pass copies them all) */
    uint32_t tot = 0;
    for (int ps = 0; ps < 2; ps++)
        for (int q = 0; q < npg; q++)
            for (int k = 0; k < NKIND; k++) bk[ps][q][k].at = tot, tot += bk[ps][q][k].n;
    static uint32_t ccmd;
    if (tot > ccmd) {
        uint32_t c = ccmd ? ccmd : 4096;
        while (c < tot) c *= 2;
        Cmd *nc = realloc(F.cmd, c * sizeof *nc);
        uint32_t *ng = nc ? realloc(F.gidx, c * 4ull) : NULL;
        if (nc) F.cmd = nc;
        if (!nc || !ng) return 0;
        F.gidx = ng, ccmd = c;
    }
    for (int ps = 0; ps < 2; ps++)
        for (int q = 0; q < npg; q++)
            for (int k = 0; k < NKIND; k++) {
                Bucket *b = &bk[ps][q][k];
                if (b->n) memcpy(F.cmd + b->at, b->c, b->n * sizeof(Cmd)), memcpy(F.gidx + b->at, b->g, b->n * 4ull);
            }
    F.ncmd = tot;
    if (!F.ncmd && !F.ntest) return 0;
    /* the upload: commands (A), their gates, the gates' first values, the tests, the copies' list */
    uint32_t bc = F.ncmd * (uint32_t)sizeof(Cmd), bg = F.ncmd * 4, bga = F.ngates * 4, bt = F.ntest * (uint32_t)sizeof(Test), bi = F.ninst * 4;
    SDL_GPUBufferUsageFlags CR = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, CW = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, IND = SDL_GPU_BUFFERUSAGE_INDIRECT;
    if (buf_room(r, &T->cmd_a, &T->cap_cmd, bc + 20, IND | CR) || buf_room(r, &T->gidx, &T->cap_gates, bg + 4, CR) ||
        buf_room(r, &T->gates, &T->cap_tests, bga + 4, CR | CW) || buf_room(r, &T->tests, &T->cap_inst, bt + 48, CR)) return 0;
    static uint32_t cap_b, cap_vis, cap_in;
    if (buf_room(r, &T->cmd_b, &cap_b, bc + 20, IND | CW) || buf_room(r, &T->inst, &cap_in, bi + 4, USE_S)) return 0;
    (void)cap_vis;
    uint32_t total = bc + bg + bga + bt + bi + 80;
    if (!T->up || T->up_cap < total) {
        if (T->up) SDL_ReleaseGPUTransferBuffer(r->dev, T->up);
        T->up_cap = total * 2;
        SDL_GPUTransferBufferCreateInfo tci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, .size = T->up_cap};
        T->up = SDL_CreateGPUTransferBuffer(r->dev, &tci);
        if (!T->up) { T->up_cap = 0; return 0; }
    }
    uint8_t *m = SDL_MapGPUTransferBuffer(r->dev, T->up, true);
    if (!m) return 0;
    uint32_t o1 = 0, o2 = (o1 + bc + 15) & ~15u, o3 = (o2 + bg + 15) & ~15u, o4 = (o3 + bga + 15) & ~15u, o5 = (o4 + bt + 15) & ~15u;
    if (bc) memcpy(m + o1, F.cmd, bc), memcpy(m + o2, F.gidx, bg);
    uint32_t *gz = (uint32_t *)(m + o3);
    memset(gz, 0, bga), gz[0] = 1;
    if (bt) memcpy(m + o4, F.test, bt);
    if (bi) memcpy(m + o5, F.inst, bi);
    SDL_UnmapGPUTransferBuffer(r->dev, T->up);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    struct { uint32_t off, n; SDL_GPUBuffer *b; } u[5] = {{o1, bc, T->cmd_a}, {o2, bg, T->gidx}, {o3, bga, T->gates}, {o4, bt, T->tests}, {o5, bi, T->inst}};
    for (int k = 0; k < 5; k++) {
        if (!u[k].n) continue;
        SDL_GPUTransferBufferLocation src = {.transfer_buffer = T->up, .offset = u[k].off};
        SDL_GPUBufferRegion dst = {.buffer = u[k].b, .offset = 0, .size = u[k].n};
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
    SDL_EndGPUCopyPass(cp);
    T->tiles_pass1 = F.tiles1;
    return 1;
}

static int npg_draw(void) { return npg; }
static int cs_skipped(int bit);

/* the draws of `pass` into the open render pass (pass 1: commands A as written; pass 2: B as gated): one a bucket */
void nv_tiles_draw(R *r, SDL_GPUCommandBuffer *cmd, SDL_GPURenderPass *rp, int pass) {
    nv_tiles_r *T = &r->tl;
    (void)cmd;
    int gated = T->read_vis_planned;
    /* the section box by clip planes when it is on and asked (section_clip), the pipelines there */
    int cl = T->o.clip_planes && r->clip_min[3] > 0.5f && T->c_face_w && T->c_face_w_back && T->c_face_i && T->c_face_i_back && T->c_proxy;
    SDL_GPUGraphicsPipeline *P_W = cl ? T->c_face_w : r->face_w, *P_WB = cl ? T->c_face_w_back : T->face_w_back, *P_I = cl ? T->c_face_i : r->face_i,
                            *P_IB = cl ? T->c_face_i_back : T->face_i_back, *P_P = cl ? T->c_proxy : T->proxy;
    for (int ps = 1; ps <= 2; ps++) {
        if (gated ? ps != pass : pass != 1) continue; /* without the test, everything in one pass */
        SDL_GPUBuffer *cb = ps == 1 || !gated || cs_skipped(8) ? T->cmd_a : T->cmd_b;
        for (int q = 0; q < npg_draw(); q++)
            for (int k = 0; k < NKIND; k++) {
                const Bucket *b = &bk[ps - 1][q][k];
                if (!b->n) continue;
                const Page *pp = &pg[q];
                SDL_GPUBufferBinding ib = {.buffer = pp->b[B_I], .offset = 0};
                if (k == K_PROXY) {
                    if (!P_P) continue;
                    SDL_BindGPUGraphicsPipeline(rp, P_P);
                    SDL_GPUBuffer *st[3] = {r->colour, pp->b[B_P], pp->b[B_TT]};
                    SDL_BindGPUVertexStorageBuffers(rp, 0, st, 3);
                } else if (k == K_MERGED || k == K_MERGED_BACK) {
                    SDL_BindGPUGraphicsPipeline(rp, k == K_MERGED_BACK && P_WB ? P_WB : P_W);
                    SDL_GPUBuffer *st[1] = {r->colour};
                    SDL_BindGPUVertexStorageBuffers(rp, 0, st, 1);
                    SDL_GPUBufferBinding vb = {.buffer = pp->b[B_V], .offset = 0};
                    SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                } else {
                    SDL_BindGPUGraphicsPipeline(rp, k == K_SHAPES_BACK && P_IB ? P_IB : P_I);
                    SDL_GPUBuffer *st[3] = {r->colour, pp->b[B_PL], T->inst};
                    SDL_BindGPUVertexStorageBuffers(rp, 0, st, 3);
                    SDL_GPUBufferBinding vb = {.buffer = pp->b[B_SV], .offset = 0};
                    SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                }
                SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_DrawGPUIndexedPrimitivesIndirect(rp, cb, b->at * (Uint32)sizeof(Cmd), b->n);
                r->draws++, r->commands += b->n;
            }
    }
}

int nv_tiles_has_pass2(const R *r) { return r->tl.read_vis_planned && F.ntest > 0; }

/* Between the passes (occlusion): the depth pyramid from pass 1's depth, the tests, the gates into B. */
/* NUI3: BIMBLE_SKIP_CS (a bit a compute pass: 1 hiz0, 2 hizn, 4 the tests, 8 the gates, 16 seen) leaves passes out, to find
 * which one a driver fails on (lavapipe 25.2 crashed in this path); a debugging aid, wrong pictures by design */
static int skip_cs = -1;
static int cs_skipped(int bit) {
    if (skip_cs < 0) skip_cs = getenv("BIMBLE_SKIP_CS") ? atoi(getenv("BIMBLE_SKIP_CS")) : 0;
    return (skip_cs & bit) != 0;
}
void nv_tiles_occlusion(R *r, SDL_GPUCommandBuffer *cmd) {
    nv_tiles_r *T = &r->tl;
    if (!T->read_vis_planned) return;
    struct { int32_t a[4], b[4]; } u0 = {{(int)r->W, (int)r->H, (int)T->lv[0][1], (int)T->lv[0][2]}, {(int)hiz_block, 0, 0, 0}};
    SDL_GPUStorageTextureReadWriteBinding tw = {.texture = T->hiz, .mip_level = 0, .layer = 0, .cycle = false};
    SDL_GPUComputePass *p;
    if (!cs_skipped(1)) {
        p = SDL_BeginGPUComputePass(cmd, &tw, 1, NULL, 0);
        SDL_BindGPUComputePipeline(p, T->hiz0);
        SDL_GPUTextureSamplerBinding sb = {.texture = r->depth, .sampler = r->nearest};
        SDL_BindGPUComputeSamplers(p, 0, &sb, 1);
        SDL_PushGPUComputeUniformData(cmd, 0, &u0, sizeof u0);
        SDL_DispatchGPUCompute(p, (T->lv[0][1] + 7) / 8, (T->lv[0][2] + 7) / 8, 1);
        SDL_EndGPUComputePass(p);
    }
    for (uint32_t l = 1; l < T->hiz_levels && !cs_skipped(2) && hiz_pair && T->hizn2; l += 2) { /* NUI3: two levels a pass */
        int nx = l + 1 < T->hiz_levels;
        struct { int32_t a[4], b[4], c[4]; } u = {{(int)T->lv[l - 1][0], (int)T->lv[l - 1][1], (int)T->lv[l - 1][2], 0}, {(int)T->lv[l][0], (int)T->lv[l][1], (int)T->lv[l][2], 0},
                                                 {nx ? (int)T->lv[l + 1][0] : 0, nx ? (int)T->lv[l + 1][1] : 0, nx ? (int)T->lv[l + 1][2] : 0, 0}};
        p = SDL_BeginGPUComputePass(cmd, &tw, 1, NULL, 0);
        SDL_BindGPUComputePipeline(p, T->hizn2);
        SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof u);
        SDL_DispatchGPUCompute(p, (T->lv[l][1] + 7) / 8, (T->lv[l][2] + 7) / 8, 1);
        SDL_EndGPUComputePass(p);
    }
    for (uint32_t l = 1; l < T->hiz_levels && !cs_skipped(2) && !(hiz_pair && T->hizn2); l++) {
        struct { int32_t a[4], b[4]; } u = {{(int)T->lv[l - 1][0], (int)T->lv[l - 1][1], (int)T->lv[l - 1][2], 0}, {(int)T->lv[l][0], (int)T->lv[l][1], (int)T->lv[l][2], 0}};
        p = SDL_BeginGPUComputePass(cmd, &tw, 1, NULL, 0);
        SDL_BindGPUComputePipeline(p, T->hizn);
        SDL_PushGPUComputeUniformData(cmd, 0, &u, sizeof u);
        SDL_DispatchGPUCompute(p, (T->lv[l][1] + 7) / 8, (T->lv[l][2] + 7) / 8, 1);
        SDL_EndGPUComputePass(p);
    }
    /* the tests: every selected tile's box */
    struct { float vp[16]; float px[4]; int32_t n[4]; int32_t o[4]; int32_t lv[16][4]; } tu;
    memcpy(tu.vp, r->last_vp, sizeof tu.vp);
    tu.px[0] = (float)r->W, tu.px[1] = (float)r->H, tu.px[2] = (float)NEAR_MM, tu.px[3] = (float)hiz_block;
    tu.n[0] = (int)F.ntest, tu.n[1] = (int)T->hiz_levels, tu.n[2] = (int)T->lv[0][1], tu.n[3] = (int)T->lv[0][2];
    tu.o[0] = (int)test_split, tu.o[1] = (int)test_split_px, tu.o[2] = (int)test_span, tu.o[3] = 0;
    for (int l = 0; l < 16; l++) tu.lv[l][0] = (int)T->lv[l][0], tu.lv[l][1] = (int)T->lv[l][1], tu.lv[l][2] = (int)T->lv[l][2], tu.lv[l][3] = 0;
    SDL_GPUStorageBufferReadWriteBinding gw = {.buffer = T->gates, .cycle = false};
    if (!cs_skipped(4)) {
    p = SDL_BeginGPUComputePass(cmd, NULL, 0, &gw, 1);
    SDL_BindGPUComputePipeline(p, T->test);
    SDL_GPUTextureSamplerBinding hb = {.texture = T->hiz, .sampler = r->nearest};
    SDL_BindGPUComputeSamplers(p, 0, &hb, 1);
    SDL_GPUBuffer *rb[1] = {T->tests};
    SDL_BindGPUComputeStorageBuffers(p, 0, rb, 1);
    SDL_PushGPUComputeUniformData(cmd, 0, &tu, sizeof tu);
    SDL_DispatchGPUCompute(p, (F.ntest + 63) / 64, 1, 1);
    SDL_EndGPUComputePass(p);
    }
    /* the gates into B (every command: pass 1's are gate 0, always) */
    if (F.ncmd && !cs_skipped(8)) {
        uint32_t gu[4] = {0, F.ncmd, 0, 0};
        SDL_GPUStorageBufferReadWriteBinding bw = {.buffer = T->cmd_b, .cycle = false};
        p = SDL_BeginGPUComputePass(cmd, NULL, 0, &bw, 1);
        SDL_BindGPUComputePipeline(p, T->gate);
        SDL_GPUBuffer *rb3[3] = {T->cmd_a, T->gidx, T->gates};
        SDL_BindGPUComputeStorageBuffers(p, 0, rb3, 3);
        SDL_PushGPUComputeUniformData(cmd, 0, gu, sizeof gu);
        SDL_DispatchGPUCompute(p, (F.ncmd + 63) / 64, 1, 1);
        SDL_EndGPUComputePass(p);
    }
    T->read_vis = 1;
}

/* After pass 2: the elements the id buffer shows (seen.comp), for the next frame's split; read back with the gates */
void nv_tiles_seen(R *r, SDL_GPUCommandBuffer *cmd) {
    nv_tiles_r *T = &r->tl;
    if (!T->read_vis_planned || !T->groups || !T->seen || !r->ncolour || cs_skipped(16)) return;
    uint32_t need = r->ncolour * 4;
    if (!T->seen_buf || T->seen_cap < need) {
        if (T->seen_buf) SDL_ReleaseGPUBuffer(r->dev, T->seen_buf), r->gpu_bytes -= T->seen_cap;
        T->seen_cap = need + need / 2;
        SDL_GPUBufferCreateInfo ci = {.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, .size = T->seen_cap};
        T->seen_buf = SDL_CreateGPUBuffer(r->dev, &ci);
        if (!T->seen_buf) { T->seen_cap = 0; return; }
        r->gpu_bytes += T->seen_cap;
    }
    if (!T->seen_down || T->seen_down_caps[r->slot] < need) { /* this slot's read-back (the other may still be read) */
        if (T->seen_down) SDL_ReleaseGPUTransferBuffer(r->dev, T->seen_down);
        SDL_GPUTransferBufferCreateInfo tci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = T->seen_cap};
        T->seen_down = SDL_CreateGPUTransferBuffer(r->dev, &tci);
        T->seen_down_caps[r->slot] = T->seen_down ? T->seen_cap : 0;
        if (!T->seen_down) return;
    }
    uint32_t u[8] = {r->W, r->H, (uint32_t)T->frame, r->ncolour, seen_step, 0, 0, 0};
    SDL_GPUStorageBufferReadWriteBinding bw = {.buffer = T->seen_buf, .cycle = false};
    SDL_GPUComputePass *p = SDL_BeginGPUComputePass(cmd, NULL, 0, &bw, 1);
    SDL_BindGPUComputePipeline(p, T->seen);
    SDL_GPUTextureSamplerBinding sb = {.texture = r->ids, .sampler = r->nearest};
    SDL_BindGPUComputeSamplers(p, 0, &sb, 1);
    SDL_PushGPUComputeUniformData(cmd, 0, u, sizeof u);
    SDL_DispatchGPUCompute(p, ((r->W + seen_step - 1) / seen_step + 7) / 8, ((r->H + seen_step - 1) / seen_step + 7) / 8, 1);
    SDL_EndGPUComputePass(p);
    T->seen_planned = 1;
}

/* The gates read back (in the frame's copy pass): which tests saw their tile */
void nv_tiles_readback(R *r, SDL_GPUCopyPass *cp) {
    nv_tiles_r *T = &r->tl;
    if (!T->read_vis || !F.ngates) return;
    uint32_t need = F.ngates * 4;
    if (!T->down || T->down_cap < need) {
        if (T->down) SDL_ReleaseGPUTransferBuffer(r->dev, T->down);
        T->down_cap = need * 2;
        SDL_GPUTransferBufferCreateInfo tci = {.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, .size = T->down_cap};
        T->down = SDL_CreateGPUTransferBuffer(r->dev, &tci);
        if (!T->down) { T->down_cap = 0, T->read_vis = 0; return; }
    }
    SDL_GPUBufferRegion src = {.buffer = T->gates, .offset = 0, .size = need};
    SDL_GPUTransferBufferLocation dst = {.transfer_buffer = T->down, .offset = 0};
    SDL_DownloadFromGPUBuffer(cp, &src, &dst);
    if (T->seen_planned) {
        SDL_GPUBufferRegion s2 = {.buffer = T->seen_buf, .offset = 0, .size = r->ncolour * 4};
        SDL_GPUTransferBufferLocation d2 = {.transfer_buffer = T->seen_down, .offset = 0};
        SDL_DownloadFromGPUBuffer(cp, &s2, &d2);
    }
}

/* After the fence: the tiles seen (pass 1 next frame, loaded if missing), the triangles drawn. */
void nv_tiles_snapshot(R *r, int slot) {
    nv_tiles_r *T = &r->tl;
    struct Snap *p = &snp[slot];
    p->read_vis = T->read_vis, p->seen_planned = T->seen_planned, p->frame = T->frame, p->tris1 = F.tris1, p->tiles1 = F.tiles1;
    p->nseen = r->ncolour; /* NUI3: the colour table may grow before this frame is collected (a link added): its read-back holds this many */
    p->ngates = 0, p->nmap = 0;
    if (F.ngates > p->cg) {
        uint64_t *a2 = realloc(p->gate_tris, F.ngates * 8ull);
        if (a2) p->gate_tris = a2;
        uint32_t *b2 = a2 ? realloc(p->gate_tiles, F.ngates * 4ull) : NULL;
        if (!a2 || !b2) { p->read_vis = 0; return; }
        p->gate_tiles = b2, p->cg = F.ngates;
    }
    if (T->nmap > p->cm) {
        TMap *m2 = realloc(p->map, T->nmap * sizeof *m2);
        if (!m2) { p->read_vis = 0; return; }
        p->map = m2, p->cm = T->nmap;
    }
    if (F.ngates) memcpy(p->gate_tris, F.gate_tris, F.ngates * 8ull), memcpy(p->gate_tiles, F.gate_tiles, F.ngates * 4ull);
    if (T->nmap) memcpy(p->map, T->map, T->nmap * sizeof *p->map);
    p->ngates = F.ngates, p->nmap = T->nmap;
}

static struct Snap *last_collected;
/* After slot's fence: the tiles seen (pass 1 next, loaded if missing), the triangles drawn; tiles released since freed. */
uint64_t nv_tiles_collect(R *r, int slot) {
    nv_tiles_r *T = &r->tl;
    struct Snap *p = &snp[slot];
    last_collected = p, lastc = p->frame;
    uint64_t tris = p->tris1;
    uint32_t tiles = p->tiles1;
    if (p->read_vis && T->downs[slot]) {
        const uint32_t *g = SDL_MapGPUTransferBuffer(r->dev, T->downs[slot], false);
        if (g) {
            for (uint32_t k = 1; k < p->ngates; k++)
                if (g[k]) tris += p->gate_tris[k], tiles += p->gate_tiles[k];
            for (uint32_t i = 0; i < p->nmap; i++) {
                TMap *m = &p->map[i];
                int live = 0; /* a set removed since (its slot freed): skipped */
                for (uint32_t si = 0; si < T->nsets && !live; si++) live = T->sets[si] == m->ts;
                if (!live || m->tile >= m->ts->nT) continue;
                m->ts->tested[m->tile] = p->frame;
                if (g[m->gate]) m->ts->vis[m->tile] = p->frame;
            }
            SDL_UnmapGPUTransferBuffer(r->dev, T->downs[slot]);
        }
    } else if (!T->o.reference)
        for (uint32_t si = 0; si < T->nsets; si++) { /* no test: every selected tile counts as seen */
            nv_tileset *ts = T->sets[si];
            for (uint32_t k = 0; k < ts->nsel; k++) ts->tested[ts->sel[k]] = ts->vis[ts->sel[k]] = p->frame;
        }
    if (p->seen_planned && T->seen_downs[slot]) { /* the elements seen: the next frames' pass 1 */
        const uint32_t *sv = SDL_MapGPUTransferBuffer(r->dev, T->seen_downs[slot], false);
        if (sv) {
            if (nseen_cpu < p->nseen) {
                uint32_t *n2 = realloc(seen_cpu, p->nseen * 4ull);
                if (n2) memset(n2 + nseen_cpu, 0, (p->nseen - nseen_cpu) * 4ull), seen_cpu = n2, nseen_cpu = p->nseen;
            }
            if (seen_cpu) memcpy(seen_cpu, sv, (nseen_cpu < p->nseen ? nseen_cpu : p->nseen) * 4ull);
            SDL_UnmapGPUTransferBuffer(r->dev, T->seen_downs[slot]);
        }
    }
    if (surplus_on > 0 && p->read_vis && seen_cpu) { /* where the triangles went (before the tiles released are freed) */
        const uint32_t *gv = T->downs[slot] ? SDL_MapGPUTransferBuffer(r->dev, T->downs[slot], false) : NULL;
        double s[8] = {0};
        const Cut *cut = &slot_cut[slot];
        surplus_top = getenv("BIMBLE_TILES_SURPLUS") && atoi(getenv("BIMBLE_TILES_SURPLUS")) >= 2;
        memset(top, 0, sizeof top);
        uint32_t fr32 = (uint32_t)p->frame;
        for (uint32_t i = 0; gv && i < ngrec[slot]; i++) {
            const GRec *q = &grec[slot][i];
            int drawn = q->pass == 1 || (q->gate < p->ngates && gv[q->gate]);
            if (!drawn) continue;
            int any_seen = 0;
            for (uint32_t k = q->a; k < q->b; k++) {
                uint32_t e = q->shape < 0 ? q->g->el[k].elem : q->g->cp[k].elem;
                any_seen |= e < nseen_cpu && seen_cpu[e] == fr32;
            }
            for (uint32_t k = q->a; k < q->b; k++) {
                const float *c = q->shape < 0 ? q->g->el[k].c : q->g->cp[k].c;
                float rr = q->shape < 0 ? q->g->el[k].r : q->g->cp[k].r;
                if (small(cut, c, rr)) continue;
                uint32_t e = q->shape < 0 ? q->g->el[k].elem : q->g->cp[k].elem;
                double t = q->shape < 0 ? q->g->el[k].n / 3.0 : q->g->sh[q->shape].n / 3.0;
                s[q->pass == 1 ? 1 : 2] += t, s[6] += t;
                if (e < nseen_cpu && seen_cpu[e] == fr32) s[0] += t;
                else if (q->pass == 1) s[5] += t;
                else if (any_seen) s[3] += t;
                else {
                    s[4] += t;
                    if (surplus_top) { /* BIMBLE_TILES_SURPLUS=2: the largest of these, printed at the shot */
                        const float *ob = q->shape < 0 ? q->g->el[k].ob : q->g->cp[k].ob;
                        int at = -1;
                        for (int j = 0; j < 8; j++) if (top[j].t < t && (at < 0 || top[j].t < top[at].t)) at = j;
                        if (at >= 0) top[at].t = t, top[at].e = e, memcpy(top[at].ob, ob, 48);
                    }
                }
            }
        }
        if (gv) SDL_UnmapGPUTransferBuffer(r->dev, T->downs[slot]);
        s[7] = 1;
        memcpy(surplus, s, sizeof s);
    }
    bury(r);
    T->tiles_drawn = tiles;
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("BIMBLE_TILES_DEBUG") != NULL;
    if (dbg && !T->o.reference) {
        uint32_t sel = 0, res = 0;
        for (uint32_t si = 0; si < T->nsets; si++) sel += T->sets[si]->nsel, res += T->sets[si]->resident;
        fprintf(stderr, "tiles: prepare %.2f ms (walk %.2f), draws %.2f ms; frame %llu: %u sets, %u selected, %u resident, %u tests, %u pass 1, %u drawn, %llu tris, %u gates, %u commands, want %u, read %d, leaves under their box %u, uploads %llu (%.1f MB), at %.1f ms\n",
                T->ms_prepare, T->ms_walk, T->ms_draw, (unsigned long long)p->frame, T->nsets, sel, res, T->ntests, p->tiles1, tiles, (unsigned long long)tris, p->ngates, F.ncmd, T->want_n, p->read_vis, F.tiles_cold, (unsigned long long)__atomic_exchange_n(&up_n, 0, __ATOMIC_RELAXED),
                __atomic_exchange_n(&up_bytes, 0, __ATOMIC_RELAXED) / 1048576.0, SDL_GetTicksNS() / 1e6);
    }
    return tris;
}

uint32_t nv_render_tiles_want(R *r, nv_tile_want *out, uint32_t cap) {
    nv_tiles_r *T = &r->tl;
    uint32_t n = 0;
    T->budget_full = 0;
    /* with occlusion the wanted list is rebuilt from this frame's answers (a tile seen now, not loaded) */
    struct Snap *p = last_collected;
    if (T->o.occlusion && !T->o.reference && p && p->read_vis) {
        T->want_n = 0;
        for (uint32_t i = 0; i < p->nmap && T->want_n < 256; i++) {
            TMap *m = &p->map[i];
            int live = 0;
            for (uint32_t si = 0; si < T->nsets && !live; si++) live = T->sets[si] == m->ts;
            if (!live) continue;
            if (!m->ts->g[m->tile] && m->ts->st[m->tile] == 0 && m->ts->vis[m->tile] == p->frame) T->want[T->want_n++] = (nv_tile_want){m->ts, m->tile, i};
        }
        for (uint32_t si = 0; si < T->nsets && T->want_n < 256; si++)
            if (T->sets[si]->shown && T->sets[si]->nT && !T->sets[si]->g[0] && T->sets[si]->st[0] == 0)
                memmove(&T->want[1], &T->want[0], (T->want_n < 255 ? T->want_n : 255) * sizeof *T->want), T->want[0] = (nv_tile_want){T->sets[si], 0, 0},
                                                                                                   T->want_n += T->want_n < 256;
    }
    uint64_t room = T->o.budget ? (T->gpu_bytes + inflight_bytes < T->o.budget ? T->o.budget - T->gpu_bytes - inflight_bytes : 0) : UINT64_MAX;
    /* a tile found no room in the pages lately (their ranges are not contiguous): nothing more asked for 30 frames, the
     * budget said full (else the same tiles were asked, refused, released and asked again: never complete) */
    if (T->noroom_frame && T->frame - T->noroom_frame < 30) room = 0;
    for (uint32_t i = 0; i < T->want_n && n < cap; i++) {
        nv_tile_want *w = &T->want[i];
        if (w->ts->g[w->tile] || w->ts->st[w->tile] != 0) continue;
        uint64_t b = guess_bytes(w->ts, w->tile);
        if (b > room && w->tile != 0) { T->budget_full = 1; break; } /* the budget is full of tiles in use: their ancestors stand in */
        room -= b < room ? b : room;
        out[n++] = *w;
    }
    return n;
}

/* the truth test's diagnosis: an element's seen tag and where it was drawn this frame (-1 nowhere found) */
void nv_tiles_debug_elem(R *r, uint32_t e) {
    nv_tiles_r *T = &r->tl;
    int where = -1;
    uint32_t gate = 0;
    for (int ps = 0; ps < 2; ps++)
        for (int q = 0; q < npg; q++)
            for (int k = 0; k < NKIND; k++) (void)bk[ps][q][k];
    for (uint32_t si = 0; si < T->nsets && where < 0; si++) {
        nv_tileset *ts = T->sets[si];
        for (uint32_t k = 0; k < ts->nsel && where < 0; k++) {
            nv_gtile *g = ts->g[ts->sel[k]];
            if (!g || g->kind != 1) continue;
            for (uint32_t i = 0; i < g->nel; i++)
                if (g->el[i].elem == e) {
                    where = (int)ts->sel[k];
                    fprintf(stderr, "truth: element %u in tile %u (set %u), group of index %u, seen tag %u, frame %llu, sphere %.0f %.0f %.0f r %.0f\n", e, ts->sel[k], si, i / CLN,
                            e < nseen_cpu ? seen_cpu[e] : 0, (unsigned long long)T->frame, g->el[i].c[0], g->el[i].c[1], g->el[i].c[2], g->el[i].r);
                    break;
                }
        }
    }
    (void)gate;
    if (where < 0) fprintf(stderr, "truth: element %u not in a selected loaded leaf (seen tag %u, frame %llu)\n", e, e < nseen_cpu ? seen_cpu[e] : 0, (unsigned long long)T->frame);
}
