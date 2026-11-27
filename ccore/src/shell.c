/* The exterior shell (shell.h; ticket SHELL1, ADR 0078 amendment 7). */
#ifdef __clang__
#pragma clang fp contract(off) /* the same bytes from every build (native, wasm): no fused multiply-add */
#endif
#include <stdlib.h>
#include <string.h>

#include "jobs.h"
#include "math.h"
#include "shell.h"

static int fail(cc_error *err, const char *msg) {
    if (!err) return -1;
    size_t n = strlen(msg);
    err->offset = CC_NO_OFFSET;
    if (n >= sizeof err->msg) n = sizeof err->msg - 1;
    memcpy(err->msg, msg, n);
    err->msg[n] = 0;
    return -1;
}

static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const double *a, const double *b, double *o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}
static void norm3(double *a) {
    double l = cc_sqrt(dot3(a, a));
    if (l > 0) a[0] /= l, a[1] /= l, a[2] /= l;
}
static void basis(cc_shell_cam *c, const double f[3]) {
    double up[3] = {0, 0, 1};
    memcpy(c->f, f, sizeof c->f);
    norm3(c->f);
    if (c->f[2] > 0.999 || c->f[2] < -0.999) up[0] = 0, up[1] = 1, up[2] = 0;
    cross3(c->f, up, c->r), norm3(c->r), cross3(c->r, c->f, c->u);
}

void cc_shell_cam_ortho(cc_shell_cam *c, const double dir[3], const double centre[3], double radius, int res) {
    memset(c, 0, sizeof *c);
    basis(c, dir);
    memcpy(c->o, centre, sizeof c->o);
    if (!(radius > 1.0)) radius = 1.0;
    c->w = c->h = res;
    c->scale = (double)res / (2.0 * radius * 1.0001);
}

void cc_shell_cam_persp(cc_shell_cam *c, const double eye[3], const double at[3], double tan_half, int w, int h) {
    memset(c, 0, sizeof *c);
    double f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    if (!(dot3(f, f) > 0)) f[0] = 1, f[1] = f[2] = 0;
    basis(c, f);
    memcpy(c->o, eye, sizeof c->o);
    c->persp = 1, c->w = w, c->h = h, c->near_mm = 10.0;
    c->scale = (double)h / (2.0 * (tan_half > 1e-6 ? tan_half : 1e-6));
}

int cc_shell_zb_init(cc_shell_zb *b, const cc_shell_cam *c) {
    memset(b, 0, sizeof *b);
    b->cam = *c;
    if (c->w <= 0 || c->h <= 0 || c->w > 16384 || c->h > 16384) return -1;
    b->bw = (c->w + 7) / 8, b->bh = (c->h + 7) / 8;
    b->z = malloc((size_t)c->w * (size_t)c->h * sizeof *b->z);
    b->bm = malloc((size_t)b->bw * (size_t)b->bh * sizeof *b->bm);
    if (!b->z || !b->bm) return cc_shell_zb_free(b), -1;
    cc_shell_zb_clear(b);
    return 0;
}

void cc_shell_zb_clear(cc_shell_zb *b) {
    size_t n = (size_t)b->cam.w * (size_t)b->cam.h;
    for (size_t i = 0; i < n; i++) b->z[i] = 1.0f / 0.0f;
    for (size_t i = 0; i < (size_t)b->bw * (size_t)b->bh; i++) b->bm[i] = 1.0f / 0.0f;
}

void cc_shell_zb_free(cc_shell_zb *b) {
    free(b->z), free(b->bm);
    b->z = b->bm = NULL;
}

void cc_shell_zb_blocks(cc_shell_zb *b) {
    int w = b->cam.w, h = b->cam.h;
    for (int by = 0; by < b->bh; by++)
        for (int bx = 0; bx < b->bw; bx++) {
            float m = -1.0f / 0.0f;
            for (int y = by * 8; y < by * 8 + 8 && y < h; y++)
                for (int x = bx * 8; x < bx * 8 + 8 && x < w; x++) {
                    float v = b->z[(size_t)y * w + x];
                    if (!(v <= m)) m = v; /* +inf wins */
                }
            b->bm[(size_t)by * b->bw + bx] = m;
        }
}

/* ---- projection: up to 4 screen points (a triangle clipped by the near plane) ---- */

typedef struct {
    double x, y, k;
} SP;

static int finite9(const float t[9]) {
    for (int i = 0; i < 9; i++)
        if (!(t[i] - t[i] == 0.0f)) return 0;
    return 1;
}

/* the triangle's screen points; 0 (nothing in front), 3 or 4 (a quad: two triangles 0 1 2, 0 2 3) */
static int project(const cc_shell_cam *c, const float t[9], SP out[4]) {
    double q[3][3];
    for (int i = 0; i < 3; i++) {
        double d[3] = {(double)t[3 * i] - c->o[0], (double)t[3 * i + 1] - c->o[1], (double)t[3 * i + 2] - c->o[2]};
        q[i][0] = dot3(d, c->r), q[i][1] = dot3(d, c->u), q[i][2] = dot3(d, c->f);
    }
    double hw = c->w / 2.0, hh = c->h / 2.0;
    if (!c->persp) {
        for (int i = 0; i < 3; i++) out[i].x = hw + q[i][0] * c->scale, out[i].y = hh - q[i][1] * c->scale, out[i].k = q[i][2];
        return 3;
    }
    double p[4][3];
    int n = 0;
    for (int i = 0; i < 3; i++) { /* Sutherland-Hodgman against z >= near */
        const double *a = q[i], *b = q[(i + 1) % 3];
        int ia = a[2] >= c->near_mm, ib = b[2] >= c->near_mm;
        if (ia) memcpy(p[n++], a, sizeof p[0]);
        if (ia != ib) {
            double s = (c->near_mm - a[2]) / (b[2] - a[2]);
            for (int k = 0; k < 3; k++) p[n][k] = a[k] + s * (b[k] - a[k]);
            p[n][2] = c->near_mm;
            n++;
        }
    }
    if (n < 3) return 0;
    for (int i = 0; i < n; i++) out[i].x = hw + c->scale * p[i][0] / p[i][2], out[i].y = hh - c->scale * p[i][1] / p[i][2], out[i].k = -1.0 / p[i][2];
    return n;
}

/* within [0, n - 1] */
static int ceil_clamp(double v, int n) {
    if (!(v > 0)) return 0;
    if (v >= n - 1) return n - 1;
    int i = (int)v;
    return (double)i < v ? i + 1 : i;
}
static int floor_clamp(double v, int n) {
    if (!(v > 0)) return 0;
    if (v >= n - 1) return n - 1;
    return (int)v;
}

/* the depth key of the triangle's point nearest (x, y) on screen (Ericson 5.1.5 in two dimensions) */
static double nearest_key(const SP v[3], double x, double y) {
    double abx = v[1].x - v[0].x, aby = v[1].y - v[0].y, acx = v[2].x - v[0].x, acy = v[2].y - v[0].y;
    double apx = x - v[0].x, apy = y - v[0].y, d1 = abx * apx + aby * apy, d2 = acx * apx + acy * apy;
    if (d1 <= 0 && d2 <= 0) return v[0].k;
    double bpx = x - v[1].x, bpy = y - v[1].y, d3 = abx * bpx + aby * bpy, d4 = acx * bpx + acy * bpy;
    if (d3 >= 0 && d4 <= d3) return v[1].k;
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        double t = d1 / (d1 - d3);
        return v[0].k + t * (v[1].k - v[0].k);
    }
    double cpx = x - v[2].x, cpy = y - v[2].y, d5 = abx * cpx + aby * cpy, d6 = acx * cpx + acy * cpy;
    if (d6 >= 0 && d5 <= d6) return v[2].k;
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        double t = d2 / (d2 - d6);
        return v[0].k + t * (v[2].k - v[0].k);
    }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        double t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return v[1].k + t * (v[2].k - v[1].k);
    }
    double den = va + vb + vc;
    if (!(den != 0)) return (v[0].k + v[1].k + v[2].k) / 3.0;
    double sv = vb / den, sw = vc / den;
    return v[0].k + sv * (v[1].k - v[0].k) + sw * (v[2].k - v[0].k);
}

/* mode 0: write the depth at covered pixel centres; 1: test at pixel centres; 2: test, conservative coverage. 1 on a hit. */
static int raster(const cc_shell_zb *b, const SP *a0, const SP *a1, const SP *a2, int mode, double tol, int *centres) {
    SP v[3] = {*a0, *a1, *a2};
    double area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
    if (!(area > 1e-4 || area < -1e-4)) return 0; /* degenerate (under a ten-thousandth of a pixel), or not finite */
    if (area < 0) {
        SP t = v[1];
        v[1] = v[2], v[2] = t;
        area = -area;
    }
    double minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < 3; i++) {
        if (v[i].x < minx) minx = v[i].x;
        if (v[i].x > maxx) maxx = v[i].x;
        if (v[i].y < miny) miny = v[i].y;
        if (v[i].y > maxy) maxy = v[i].y;
    }
    int w = b->cam.w, h = b->cam.h;
    /* the pixels: centres inside the box (x + 0.5 in [min, max]), or (conservative) squares [x, x + 1] meeting it */
    double s0 = mode == 2 ? 1.0 : 0.5, s1 = mode == 2 ? 0.0 : 0.5;
    if (!(maxx - s1 >= 0) || !(maxy - s1 >= 0) || !(minx - s0 < w - 1 + 1e-9) || !(miny - s0 < h - 1 + 1e-9)) return 0;
    int x0 = ceil_clamp(minx - s0, w), x1 = floor_clamp(maxx - s1, w), y0 = ceil_clamp(miny - s0, h), y1 = floor_clamp(maxy - s1, h);
    if (x0 > x1 || y0 > y1) return 0;
    /* edge i opposite vertex i: E_i(p) = (b.x - a.x)(p.y - a.y) - (b.y - a.y)(p.x - a.x), a = v[i+1], b = v[i+2]; inside: all >= 0 */
    double A[3], B[3], C[3], slack[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        const SP *p = &v[(i + 1) % 3], *q = &v[(i + 2) % 3];
        A[i] = -(q->y - p->y), B[i] = q->x - p->x, C[i] = -(A[i] * p->x + B[i] * p->y);
        if (mode == 2) slack[i] = 0.5 * ((A[i] < 0 ? -A[i] : A[i]) + (B[i] < 0 ? -B[i] : B[i]));
    }
    /* sum E_i = area for every p: E_i is area times the barycentric weight of vertex i */
    double ka = v[0].k / area, kb = v[1].k / area, kc = v[2].k / area;
    for (int y = y0; y <= y1; y++) {
        double py = y + 0.5, px = x0 + 0.5;
        double e0 = A[0] * px + B[0] * py + C[0], e1 = A[1] * px + B[1] * py + C[1], e2 = A[2] * px + B[2] * py + C[2];
        float *row = b->z + (size_t)y * w;
        for (int x = x0; x <= x1; x++, e0 += A[0], e1 += A[1], e2 += A[2]) {
            if (e0 < -slack[0] || e1 < -slack[1] || e2 < -slack[2]) continue;
            double k = e0 * ka + e1 * kb + e2 * kc;
            if (mode == 2 && (e0 < 0 || e1 < 0 || e2 < 0)) { /* a pixel the triangle only touches: the depth of the triangle's
                                                              * point nearest its centre (not the plane's extrapolation: a steep
                                                              * face would reach far in front) */
                double cx = x + 0.5, cy = y + 0.5;
                k = nearest_key(v, cx, cy);
            }
            if (mode == 0) {
                if ((float)k < row[x]) row[x] = (float)k;
            } else {
                if (centres) ++*centres;
                if (k <= (double)row[x] + tol) return 1;
            }
        }
    }
    return 0;
}

void cc_shell_draw(cc_shell_zb *b, const float t[9]) {
    SP p[4];
    if (!finite9(t)) return;
    int n = project(&b->cam, t, p);
    if (n >= 3) raster(b, &p[0], &p[1], &p[2], 0, 0, NULL);
    if (n == 4) raster(b, &p[0], &p[2], &p[3], 0, 0, NULL);
}

int cc_shell_test(const cc_shell_zb *b, const float t[9], double tol, int conservative) {
    SP p[4];
    if (!finite9(t)) return 0;
    int n = project(&b->cam, t, p), c = 0;
    /* the pixel centres first; conservative coverage only for a triangle that holds none (a sliver, a part thinner than a
     * pixel): a face that holds centres is judged there (its edge pixels would compare a steep face's depth with the
     * buffer's at a neighbouring point) */
    if (n >= 3 && raster(b, &p[0], &p[1], &p[2], 1, tol, &c)) return 1;
    if (n == 4 && raster(b, &p[0], &p[2], &p[3], 1, tol, &c)) return 1;
    if (!conservative || c) return 0;
    { /* a face seen edge on is not a part thinner than a pixel: its neighbours show (or not) */
        double u[3], w[3], nn[3];
        for (int k = 0; k < 3; k++) u[k] = (double)t[3 + k] - t[k], w[k] = (double)t[6 + k] - t[k];
        cross3(u, w, nn);
        double l = cc_sqrt(dot3(nn, nn)), f[3];
        if (b->cam.persp) {
            for (int k = 0; k < 3; k++) f[k] = ((double)t[k] + t[3 + k] + t[6 + k]) / 3.0 - b->cam.o[k];
            norm3(f);
        } else memcpy(f, b->cam.f, sizeof f);
        double cs = l > 0 ? dot3(nn, f) / l : 0;
        if (cs < 0.05 && cs > -0.05) return 0;
    }
    if (n >= 3 && raster(b, &p[0], &p[1], &p[2], 2, tol, NULL)) return 1;
    return n == 4 && raster(b, &p[0], &p[2], &p[3], 2, tol, NULL);
}

int cc_shell_box_hidden(const cc_shell_zb *b, const float box[6], double tol) {
    const cc_shell_cam *c = &b->cam;
    double minx = 1e300, maxx = -1e300, miny = 1e300, maxy = -1e300, kn = 1e300;
    for (int i = 0; i < 6; i++)
        if (!(box[i] - box[i] == 0.0f)) return 0;
    for (int k = 0; k < 8; k++) {
        double d[3] = {(double)(k & 1 ? box[3] : box[0]) - c->o[0], (double)(k & 2 ? box[4] : box[1]) - c->o[1], (double)(k & 4 ? box[5] : box[2]) - c->o[2]};
        double x = dot3(d, c->r), y = dot3(d, c->u), z = dot3(d, c->f), sx, sy, key;
        if (c->persp) {
            if (z < c->near_mm) return 0;
            sx = c->w / 2.0 + c->scale * x / z, sy = c->h / 2.0 - c->scale * y / z, key = -1.0 / z;
        } else sx = c->w / 2.0 + c->scale * x, sy = c->h / 2.0 - c->scale * y, key = z;
        if (sx < minx) minx = sx;
        if (sx > maxx) maxx = sx;
        if (sy < miny) miny = sy;
        if (sy > maxy) maxy = sy;
        if (key < kn) kn = key;
    }
    if (maxx < 0 || maxy < 0 || minx >= c->w || miny >= c->h) return 1; /* off screen */
    int bx0 = minx <= 0 ? 0 : (int)minx / 8, by0 = miny <= 0 ? 0 : (int)miny / 8;
    int bx1 = maxx >= c->w - 1 ? b->bw - 1 : (int)maxx / 8, by1 = maxy >= c->h - 1 ? b->bh - 1 : (int)maxy / 8;
    for (int y = by0; y <= by1; y++)
        for (int x = bx0; x <= bx1; x++)
            if (!((double)b->bm[(size_t)y * b->bw + x] < kn - tol)) return 0;
    return 1;
}

/* ---- the directions ---- */

void cc_shell_dirs(uint32_t n, double *out) {
    static const double FIX[14][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
                                      {1, 1, 1}, {-1, 1, 1}, {1, -1, 1}, {-1, -1, 1}, {1, 1, -1}, {-1, 1, -1}, {1, -1, -1}, {-1, -1, -1}};
    uint32_t i = 0;
    for (; i < n && i < 14; i++) {
        double d[3] = {FIX[i][0], FIX[i][1], FIX[i][2]};
        norm3(d);
        memcpy(out + 3 * i, d, sizeof d);
    }
    if (n <= 14) return;
    uint32_t m = n - 14;
    /* the golden angle's rotation by recurrence (no libm: the same bits in every build) */
    const double gc = -0.73736887807831963, gs = 0.67549029426152362;
    double c = 1.0, s = 0.0;
    for (uint32_t j = 0; j < m; j++, i++) {
        double z = 1.0 - (2.0 * j + 1.0) / m, r = cc_sqrt(1.0 - z * z > 0 ? 1.0 - z * z : 0);
        double d[3] = {r * c, r * s, z};
        norm3(d);
        memcpy(out + 3 * i, d, sizeof d);
        double c2 = c * gc - s * gs, s2 = s * gc + c * gs;
        c = c2, s = s2;
    }
}

/* ---- the classification ---- */

typedef struct {
    float (*t)[9];       /* placed triangles */
    uint32_t *te;        /* their element (index into the run's elements) */
    size_t nt, ct;
} Soup;

struct cc_shell {
    cc_tiles_src src;
    cc_tiles_toc toc;
    cc_shell_opts o;
    uint32_t ne;
    uint8_t *ext, *opq, *in;  /* per scene element: exterior, occluder, listed with geometry */
    float (*ebox)[6];
    double ctr[3], radius;
    double *dirs;
    uint32_t *leaf, nleaf;    /* the pyramid's leaves, in id order */
    uint32_t b0, nb, bcap;    /* the batch of views: [b0, b0 + nb) */
    cc_shell_zb *zb;
    int pass;                 /* 1 depth, 2 tests */
    uint32_t at;              /* next leaf */
    Soup soup;
    uint32_t *rel, nrel, crel; /* the run's elements (scene rows) */
    uint32_t (*rng)[2];       /* their triangles in the soup */
    uint8_t *vm;              /* per view of the batch x run element: 1 the box may show (pass 2), then 2 seen */
    Arena dec;
    cc_mesh *shapes;          /* per block decoded in this run (nv = UINT32_MAX: not yet) */
    uint32_t *touched, ntouched;
    size_t nblocks;
    int done, need_batch;
    cc_shell_stats st;
};

void cc_shell_free(cc_shell *s) {
    if (!s) return;
    for (uint32_t i = 0; s->zb && i < s->bcap; i++) cc_shell_zb_free(&s->zb[i]);
    free(s->zb), free(s->ext), free(s->opq), free(s->in), free(s->ebox), free(s->dirs), free(s->leaf);
    free(s->soup.t), free(s->soup.te), free(s->rel), free(s->rng), free(s->vm), free(s->shapes), free(s->touched);
    arena_free(&s->dec);
    free(s);
}

const uint8_t *cc_shell_exterior(const cc_shell *s) { return s->ext; }
void cc_shell_get_stats(const cc_shell *s, cc_shell_stats *st) { *st = s->st; }

int cc_shell_start(cc_shell **out, const cc_tiles_src *src, const cc_tiles_toc *toc, const cc_shell_opts *o, cc_error *err) {
    *out = NULL;
    cc_shell *s = calloc(1, sizeof *s);
    if (!s) return fail(err, "out of memory");
    s->src = *src, s->toc = *toc, s->o = *o;
    if (s->o.views > CC_SHELL_MAX_VIEWS) s->o.views = CC_SHELL_MAX_VIEWS;
    if (s->o.res > CC_SHELL_MAX_RES) s->o.res = CC_SHELL_MAX_RES;
    if (s->o.res < 8) s->o.res = 8;
    arena_init(&s->dec, 0);
    size_t ne = cc_tiles_src_count(src);
    if (ne != toc->nelements || ne >= CC_TILES_MAX_TILES) return cc_shell_free(s), fail(err, "shell: the pyramid is of another scene");
    s->ne = (uint32_t)ne;
    s->nblocks = cc_tiles_src_nblocks(src);
    s->ext = calloc(ne + 1, 1), s->opq = calloc(ne + 1, 1), s->in = calloc(ne + 1, 1);
    s->ebox = malloc((ne + 1) * sizeof *s->ebox);
    s->dirs = malloc(3 * ((size_t)s->o.views + 1) * sizeof *s->dirs);
    s->leaf = malloc(((size_t)toc->ntiles + 1) * sizeof *s->leaf);
    s->shapes = malloc((s->nblocks + 1) * sizeof *s->shapes);
    s->touched = malloc((s->nblocks + 1) * sizeof *s->touched);
    if (!s->ext || !s->opq || !s->in || !s->ebox || !s->dirs || !s->leaf || !s->shapes || !s->touched) return cc_shell_free(s), fail(err, "out of memory");
    for (size_t b = 0; b < s->nblocks; b++) s->shapes[b].nv = UINT32_MAX;
    cc_shell_dirs(s->o.views, s->dirs);
    for (uint32_t i = 0; i < toc->nlist; i++) {
        uint32_t e = cc_tiles_list(toc, i);
        if (e >= ne) continue;
        s->in[e] = 1, s->st.elements++;
        s->opq[e] = (cc_tiles_el_flags(toc, i) & CC_TILES_EL_OPAQUE) != 0;
    }
    for (uint32_t i = 0; i < toc->ntiles; i++) {
        cc_tile t;
        cc_tiles_get(toc, i, &t);
        if (t.flags & CC_TILE_LEAF) s->leaf[s->nleaf++] = i;
    }
    /* the link's box: the elements' (finite) boxes */
    float all[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    Arena scratch;
    arena_init(&scratch, 0);
    for (uint32_t e = 0; e < s->ne; e++) {
        float *b = s->ebox[e];
        arena_trim(&scratch, (size_t)1 << 22);
        if (!s->in[e] || cc_tiles_src_box(src, e, b, &scratch)) {
            s->in[e] = 0;
            continue;
        }
        int ok = 1;
        for (int k = 0; k < 6; k++) ok &= b[k] - b[k] == 0.0f;
        if (!ok || !(b[0] <= b[3] && b[1] <= b[4] && b[2] <= b[5])) { s->in[e] = 0; continue; }
        for (int k = 0; k < 3; k++) {
            if (b[k] < all[k]) all[k] = b[k];
            if (b[3 + k] > all[3 + k]) all[3 + k] = b[3 + k];
        }
    }
    arena_free(&scratch);
    double r2 = 0;
    for (int k = 0; k < 3; k++) {
        s->ctr[k] = all[k] <= all[3 + k] ? ((double)all[k] + (double)all[3 + k]) / 2.0 : 0;
        double h = all[k] <= all[3 + k] ? ((double)all[3 + k] - (double)all[k]) / 2.0 : 0;
        r2 += h * h;
    }
    s->radius = cc_sqrt(r2) + 1.0;
    if (s->o.touch) /* the safeguard: an element reaching the link's box faces */
        for (uint32_t e = 0; e < s->ne; e++) {
            if (!s->in[e]) continue;
            const float *b = s->ebox[e];
            double t = s->o.tol_mm;
            int hit = 0;
            for (int k = 0; k < 3; k++) hit |= (double)b[k] <= (double)all[k] + t || (double)b[3 + k] >= (double)all[3 + k] - t;
            if (hit) s->ext[e] = 1, s->st.touched++;
        }
    /* the batch: as many views as fit CC_SHELL_BATCH_BYTES (at least one) */
    size_t per = (size_t)s->o.res * s->o.res * sizeof(float) + ((size_t)s->o.res / 8 + 1) * ((size_t)s->o.res / 8 + 1) * sizeof(float);
    size_t cap = CC_SHELL_BATCH_BYTES / per;
    if (cap < 1) cap = 1;
    if (cap > s->o.views) cap = s->o.views;
    s->bcap = (uint32_t)cap;
    s->zb = calloc(cap + 1, sizeof *s->zb);
    if (!s->zb) return cc_shell_free(s), fail(err, "out of memory");
    s->pass = 1, s->need_batch = 1;
    if (!s->o.views || !s->nleaf) s->done = 1;
    *out = s;
    return 0;
}

/* ---- a run of leaves' elements placed into the soup ---- */

static int soup_room(Soup *u, size_t more) {
    if (u->nt + more <= u->ct) return 0;
    size_t c = u->ct ? u->ct : 4096;
    while (c < u->nt + more) c *= 2;
    float(*t)[9] = realloc(u->t, c * sizeof *t);
    if (t) u->t = t;
    uint32_t *e = realloc(u->te, c * sizeof *e);
    if (e) u->te = e;
    if (!t || !e) return -1;
    u->ct = c;
    return 0;
}

static int rel_add(cc_shell *s, uint32_t e) {
    if (s->nrel == s->crel) {
        uint32_t c = s->crel ? 2 * s->crel : 1024;
        uint32_t *r = realloc(s->rel, (size_t)c * sizeof *r);
        if (r) s->rel = r;
        uint32_t(*g)[2] = realloc(s->rng, (size_t)c * sizeof *g);
        if (g) s->rng = g;
        if (!r || !g) return -1;
        s->crel = c;
    }
    s->rel[s->nrel++] = e;
    return 0;
}

/* element e's triangles (placed) appended to the soup as run element `ri` */
static int place_element(cc_shell *s, uint32_t e, uint32_t ri) {
    size_t b = cc_tiles_src_el_block(&s->src, e);
    if (b >= s->nblocks) return -1;
    cc_mesh *m = &s->shapes[b];
    if (m->nv == UINT32_MAX) {
        if (cc_tiles_src_block(&s->src, b, &s->dec, m)) return m->nv = UINT32_MAX, -1;
        s->touched[s->ntouched++] = (uint32_t)b;
    }
    const cc_placement32 *pl = cc_tiles_src_place(&s->src, e);
    uint32_t nt = m->ni / 3;
    if (soup_room(&s->soup, nt)) return -1;
    s->rng[ri][0] = (uint32_t)s->soup.nt;
    for (uint32_t t = 0; t < nt; t++) {
        uint32_t ix[3] = {m->idx[3 * t], m->idx[3 * t + 1], m->idx[3 * t + 2]};
        if (ix[0] >= m->nv || ix[1] >= m->nv || ix[2] >= m->nv) continue;
        float *o = s->soup.t[s->soup.nt];
        for (int c = 0; c < 3; c++) {
            const float *q = m->pos[ix[c]];
            if (pl)
                for (int j = 0; j < 3; j++) o[3 * c + j] = pl->x[j] * q[0] + pl->y[j] * q[1] + pl->z[j] * q[2] + pl->t[j];
            else memcpy(o + 3 * c, q, 12);
        }
        s->soup.te[s->soup.nt++] = ri;
    }
    s->rng[ri][1] = (uint32_t)s->soup.nt;
    return 0;
}

static void run_reset(cc_shell *s) {
    for (uint32_t i = 0; i < s->ntouched; i++) s->shapes[s->touched[i]].nv = UINT32_MAX;
    s->ntouched = 0;
    arena_reset(&s->dec);
    s->soup.nt = 0, s->nrel = 0;
}

/* the next leaves' elements wanted by the pass (1: opaque; 2: not exterior yet), up to CC_SHELL_RUN_TRIS (at least a leaf) */
static int gather(cc_shell *s, uint64_t *tris) {
    uint64_t acc = 0;
    while (s->at < s->nleaf) {
        cc_tile t;
        cc_tiles_get(&s->toc, s->leaf[s->at], &t);
        if (acc && acc + t.detail > CC_SHELL_RUN_TRIS) break;
        for (uint32_t i = 0; i < t.nel; i++) {
            uint32_t e = cc_tiles_list(&s->toc, t.el0 + i);
            if (e >= s->ne || !s->in[e]) continue;
            if (s->pass == 1 ? !s->opq[e] : s->ext[e]) continue;
            if (rel_add(s, e)) return -1;
        }
        acc += t.detail;
        s->at++;
    }
    *tris = acc;
    return 0;
}

typedef struct {
    cc_shell *s;
} Job;

static void draw_job(void *ctx, size_t v, size_t worker) {
    cc_shell *s = ((Job *)ctx)->s;
    (void)worker;
    for (size_t t = 0; t < s->soup.nt; t++) cc_shell_draw(&s->zb[v], s->soup.t[t]);
}

static void blocks_job(void *ctx, size_t v, size_t worker) {
    (void)worker;
    cc_shell_zb_blocks(&((Job *)ctx)->s->zb[v]);
}

static void box_job(void *ctx, size_t v, size_t worker) { /* pass 2, first: the run's elements' boxes in view v */
    cc_shell *s = ((Job *)ctx)->s;
    (void)worker;
    uint8_t *m = s->vm + v * (size_t)s->nrel;
    for (uint32_t i = 0; i < s->nrel; i++) m[i] = !cc_shell_box_hidden(&s->zb[v], s->ebox[s->rel[i]], (double)s->o.tol_mm);
}

static void test_job(void *ctx, size_t v, size_t worker) { /* pass 2, then: their triangles where the box may show */
    cc_shell *s = ((Job *)ctx)->s;
    (void)worker;
    uint8_t *m = s->vm + v * (size_t)s->nrel;
    for (uint32_t i = 0; i < s->nrel; i++) {
        if (m[i] != 1) continue;
        for (uint32_t t = s->rng[i][0]; t < s->rng[i][1]; t++)
            if (cc_shell_test(&s->zb[v], s->soup.t[t], (double)s->o.tol_mm, 1)) {
                m[i] = 2;
                break;
            }
    }
}

static int batch_start(cc_shell *s) {
    s->nb = s->o.views - s->b0 < s->bcap ? s->o.views - s->b0 : s->bcap;
    for (uint32_t v = 0; v < s->nb; v++) {
        cc_shell_cam c;
        cc_shell_cam_ortho(&c, s->dirs + 3 * (size_t)(s->b0 + v), s->ctr, s->radius, (int)s->o.res);
        if (!s->zb[v].z) {
            if (cc_shell_zb_init(&s->zb[v], &c)) return -1;
        } else {
            s->zb[v].cam = c;
            cc_shell_zb_clear(&s->zb[v]);
        }
    }
    size_t buf = (size_t)s->bcap * ((size_t)s->o.res * s->o.res * 4 + (size_t)s->zb[0].bw * s->zb[0].bh * 4);
    if (buf > s->st.peak_bytes) s->st.peak_bytes = buf;
    return 0;
}

int cc_shell_step(cc_shell *s, uint64_t budget, cc_error *err) {
    if (s->done) return 1;
    uint64_t used = 0;
    Job j = {s};
    while (!s->done) {
        if (s->need_batch) {
            if (batch_start(s)) return fail(err, "out of memory");
            s->need_batch = 0;
        }
        uint64_t tris;
        run_reset(s);
        if (gather(s, &tris)) return fail(err, "out of memory");
        if (s->nrel) {
            if (s->pass == 2) { /* boxes first: decode only what may show */
                free(s->vm);
                s->vm = malloc((size_t)s->nb * s->nrel + 1);
                if (!s->vm) return fail(err, "out of memory");
                cc_jobs_run(s->nb, box_job, &j);
                uint32_t keep = 0, old = s->nrel;
                for (uint32_t i = 0; i < old; i++) {
                    int any = 0;
                    for (uint32_t v = 0; v < s->nb; v++) any |= s->vm[(size_t)v * old + i];
                    s->st.box_tests += s->nb;
                    for (uint32_t v = 0; v < s->nb; v++) s->st.box_hidden += !s->vm[(size_t)v * old + i];
                    if (!any) continue;
                    for (uint32_t v = 0; v < s->nb; v++) s->vm[(size_t)v * old + keep] = s->vm[(size_t)v * old + i];
                    s->rel[keep++] = s->rel[i];
                }
                for (uint32_t v = 1; v < s->nb; v++) memmove(s->vm + (size_t)v * keep, s->vm + (size_t)v * old, keep); /* rows of width keep */
                s->nrel = keep;
            }
            for (uint32_t i = 0; i < s->nrel; i++)
                if (place_element(s, s->rel[i], i)) return fail(err, "shell: a mesh does not decode, or out of memory");
            {
                size_t pb = s->soup.ct * (sizeof *s->soup.t + sizeof *s->soup.te) + (size_t)s->nb * s->crel;
                size_t buf = (size_t)s->bcap * ((size_t)s->o.res * s->o.res * 4 + (size_t)s->zb[0].bw * s->zb[0].bh * 4);
                if (pb + buf > s->st.peak_bytes) s->st.peak_bytes = pb + buf;
            }
            if (s->pass == 1) {
                cc_jobs_run(s->nb, draw_job, &j);
                s->st.drawn += (uint64_t)s->soup.nt * s->nb;
                used += (uint64_t)s->soup.nt * s->nb;
            } else if (s->nrel) {
                cc_jobs_run(s->nb, test_job, &j);
                for (uint32_t i = 0; i < s->nrel; i++)
                    for (uint32_t v = 0; v < s->nb; v++)
                        if (s->vm[(size_t)v * s->nrel + i] == 2) s->ext[s->rel[i]] = 1;
                s->st.tested += (uint64_t)s->soup.nt * s->nb;
                used += (uint64_t)s->soup.nt * s->nb;
            }
        }
        (void)tris;
        if (s->at >= s->nleaf) { /* the pass is over */
            run_reset(s);
            if (s->pass == 1) {
                cc_jobs_run(s->nb, blocks_job, &j);
                s->pass = 2, s->at = 0;
            } else {
                s->b0 += s->nb, s->pass = 1, s->at = 0, s->need_batch = 1;
                if (s->b0 >= s->o.views) s->done = 1;
            }
        }
        if (used >= budget && !s->done) return 0;
    }
    run_reset(s);
    for (uint32_t i = 0; s->zb && i < s->bcap; i++) cc_shell_zb_free(&s->zb[i]);
    s->st.exterior = 0;
    for (uint32_t e = 0; e < s->ne; e++) s->st.exterior += s->ext[e] && s->in[e];
    return 1;
}
