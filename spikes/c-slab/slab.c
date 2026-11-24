/* C port of bimble-geom's slab mesh: corners_only, earcut (convex quads only), extrude_tris,
 * mesh_from_tris (local) or Scene::from_model + MeshBuilder::build (scene), and
 * crease_edges_with_normals. No malloc: static arrays, capped at SLAB_MAX_PTS outline points. */
#include "slab.h"
#include <math.h>
#include <string.h>

typedef double Tri[3][3];

static double pts[SLAB_MAX_PTS][2];
static Tri tris[SLAB_MAX_TRIS];
static double wpos[SLAB_MAX_VERTS][3];

static uint64_t bits64(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static uint32_t bits32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* Drop repeated and collinear points (lib.rs corners_only). Returns the new count. */
static int corners_only(double v[][2], int n) {
    for (;;) {
        int i, drop = -1;
        if (n < 3) return n;
        for (i = 0; i < n && drop < 0; i++) {
            const double *a = v[(i + n - 1) % n], *b = v[i], *c = v[(i + 1) % n];
            double ux = c[0] - a[0], uy = c[1] - a[1], wx = b[0] - a[0], wy = b[1] - a[1];
            double len = sqrt(ux * ux + uy * uy);
            int same = fabs(b[0] - a[0]) < 1e-9 && fabs(b[1] - a[1]) < 1e-9;
            double l2 = len * len, along = (wx * ux + wy * uy) / (l2 > 1e-30 ? l2 : 1e-30);
            if (same || (len > 0.0 && fabs(ux * wy - uy * wx) / len < 1e-6
                         && along >= 0.0 && along <= 1.0))
                drop = i;
        }
        if (drop < 0) return n;
        memmove(v[drop], v[drop + 1], (size_t)(n - drop - 1) * sizeof v[0]);
        n--;
    }
}

/* Cap triangulation. NOT a general earcut: for a CCW convex quad, earcutr returns
 * [2,3,0, 0,1,2], reproduced here. Other polygons get a fan from vertex 0, which is
 * only valid for convex outlines and does not match earcut's order. */
static int triangulate(int n, int *t) {
    int i, k = 0;
    if (n == 4) {
        static const int q[6] = {2, 3, 0, 0, 1, 2};
        memcpy(t, q, sizeof q);
        return 2;
    }
    for (i = 1; i + 1 < n; i++) { t[k++] = 0; t[k++] = i; t[k++] = i + 1; }
    return n - 2;
}

static void set3(double *p, const double *q, double z) { p[0] = q[0]; p[1] = q[1]; p[2] = z; }

/* extrude_tris: bottom cap reversed, top cap, then two triangles per side. */
static int extrude(int n, double depth) {
    int t[3 * SLAB_MAX_PTS], nt = triangulate(n, t), m = 0, pass, i;
    for (pass = 0; pass < 2; pass++) {
        double z = pass ? depth : 0.0;
        for (i = 0; i < nt; i++) {
            int a = t[3 * i], b = t[3 * i + 1], c = t[3 * i + 2];
            if (!pass) { int s = b; b = c; c = s; }
            set3(tris[m][0], pts[a], z); set3(tris[m][1], pts[b], z); set3(tris[m][2], pts[c], z);
            m++;
        }
    }
    for (i = 0; i < n; i++) {
        int j = (i + 1) % n;
        set3(tris[m][0], pts[i], 0.0); set3(tris[m][1], pts[j], 0.0); set3(tris[m][2], pts[j], depth);
        m++;
        set3(tris[m][0], pts[i], 0.0); set3(tris[m][1], pts[j], depth); set3(tris[m][2], pts[i], depth);
        m++;
    }
    return m;
}

/* ---- crease_edges_with_normals (scene.rs) ---- */
typedef struct {
    int64_t k[6];
    int count, crease;
    float normal[3], other[3], seg[6];
} EdgeUse;
static EdgeUse uses[SLAB_MAX_EDGES];

static int8_t q8(float c) {
    float x = c < -1.0f ? -1.0f : c > 1.0f ? 1.0f : c;
    return (int8_t)roundf(x * 127.0f);
}
static void pack(const float n[3], float sign, int8_t *o) {
    o[0] = q8(sign * n[0]); o[1] = q8(sign * n[1]); o[2] = q8(sign * n[2]); o[3] = 0;
}
static int keycmp(const int64_t *a, const int64_t *b, int len) {
    int i;
    for (i = 0; i < len; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}
static int segless(const float *a, const float *b) {
    int i;
    for (i = 0; i < 6; i++) if (a[i] != b[i]) return a[i] < b[i];
    return 0;
}

static void crease_edges(SlabMesh *m, float angle_deg, int want_normals) {
    float cos_limit = cosf(angle_deg * (3.14159265358979323846f / 180.0f));
    int nu = 0, t, k, i, j;
    for (t = 0; t < m->nt; t++) {
        const uint32_t *tr = &m->idx[3 * t];
        const float *a0 = m->pos[tr[0]], *b0 = m->pos[tr[1]], *c0 = m->pos[tr[2]];
        float u[3], v[3], cr[3], l, n[3];
        for (k = 0; k < 3; k++) { u[k] = b0[k] - a0[k]; v[k] = c0[k] - a0[k]; }
        cr[0] = u[1] * v[2] - u[2] * v[1];
        cr[1] = u[2] * v[0] - u[0] * v[2];
        cr[2] = u[0] * v[1] - u[1] * v[0];
        l = sqrtf(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
        if (l > 1e-12f) { n[0] = cr[0] / l; n[1] = cr[1] / l; n[2] = cr[2] / l; }
        else { n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f; }
        for (k = 0; k < 3; k++) {
            const float *a = m->pos[tr[k]], *b = m->pos[tr[(k + 1) % 3]];
            int64_t ka[3], kb[3], key[6];
            EdgeUse *e = 0;
            for (i = 0; i < 3; i++) {
                ka[i] = (int64_t)roundf(a[i] * 100.0f);
                kb[i] = (int64_t)roundf(b[i] * 100.0f);
            }
            if (!keycmp(ka, kb, 3)) continue;
            if (keycmp(ka, kb, 3) < 0) { memcpy(key, ka, 24); memcpy(key + 3, kb, 24); }
            else { memcpy(key, kb, 24); memcpy(key + 3, ka, 24); }
            for (i = 0; i < nu; i++) if (!keycmp(uses[i].k, key, 6)) { e = &uses[i]; break; }
            if (!e) {
                e = &uses[nu++];
                memcpy(e->k, key, 48);
                e->count = 0; e->crease = 0;
                memcpy(e->normal, n, 12); memcpy(e->other, n, 12);
                memcpy(e->seg, a, 12); memcpy(e->seg + 3, b, 12);
            }
            if (e->count > 0) {
                float c = e->normal[0] * n[0] + e->normal[1] * n[1] + e->normal[2] * n[2];
                if (c < cos_limit) e->crease = 1;
                memcpy(e->other, n, 12);
            }
            e->count++;
        }
    }
    m->ne = 0;
    for (i = 0; i < nu; i++) {
        EdgeUse *e = &uses[i];
        int8_t *o;
        if (!(e->count == 1 || e->crease)) continue;
        /* insertion sort by the 6 flattened coordinates (Rust sorts after a HashMap) */
        for (j = m->ne; j > 0 && segless(e->seg, m->edges[j - 1]); j--) {
            memcpy(m->edges[j], m->edges[j - 1], 24);
            memcpy(m->normals[j], m->normals[j - 1], 8);
        }
        memcpy(m->edges[j], e->seg, 24);
        o = m->normals[j];
        memset(o, 0, 8);
        if (want_normals) {
            if (e->count == 1) { pack(e->normal, 1.0f, o); pack(e->normal, -1.0f, o + 4); }
            else if (e->count == 2) { pack(e->normal, 1.0f, o); pack(e->other, 1.0f, o + 4); }
        }
        m->ne++;
    }
}

/* mesh_from_tris: weld by f64 bits after +0.0, then positions to f32. */
static void weld_local(int ntri, SlabMesh *m) {
    int t, k, i, nv = 0;
    for (t = 0; t < ntri; t++)
        for (k = 0; k < 3; k++) {
            double p[3];
            for (i = 0; i < 3; i++) p[i] = tris[t][k][i] + 0.0;
            for (i = 0; i < nv; i++)
                if (bits64(wpos[i][0]) == bits64(p[0]) && bits64(wpos[i][1]) == bits64(p[1])
                    && bits64(wpos[i][2]) == bits64(p[2])) break;
            if (i == nv) { memcpy(wpos[nv], p, 24); nv++; }
            m->idx[3 * t + k] = (uint32_t)i;
        }
    for (i = 0; i < nv; i++)
        for (k = 0; k < 3; k++) m->pos[i][k] = (float)wpos[i][k];
    m->nv = nv;
    m->nt = ntri;
}

int slab_mesh(const double outline[][2], int n, double thickness,
              const double origin_placement[3], SlabMesh *out, int scene) {
    int ntri, t, k, r;
    if (n < 3 || n > SLAB_MAX_PTS || !(thickness > 0.0) || !isfinite(thickness)) return -1;
    memcpy(pts, outline, (size_t)n * sizeof pts[0]);
    n = corners_only(pts, n);
    if (n < 3) return -1;
    ntri = extrude(n, thickness);
    memset(out, 0, sizeof *out);
    weld_local(ntri, out);
    if (scene) {
        /* placement_matrix with default axes, storey elevation 0; y = z x x */
        static const double z[3] = {0.0, 0.0, 1.0}, x[3] = {1.0, 0.0, 0.0};
        double y[3], mat[4][3], lo[3], hi[3], org[3];
        int nt = 0, nv = 0, i;
        static Tri world[SLAB_MAX_TRIS];
        static float spos[SLAB_MAX_VERTS][3];
        y[0] = z[1] * x[2] - z[2] * x[1];
        y[1] = z[2] * x[0] - z[0] * x[2];
        y[2] = z[0] * x[1] - z[1] * x[0];
        for (k = 0; k < 3; k++) {
            mat[0][k] = x[k]; mat[1][k] = y[k]; mat[2][k] = z[k];
            mat[3][k] = origin_placement[k];
        }
        mat[3][2] = origin_placement[2] + 0.0; /* + storey elevation (0) */
        for (t = 0; t < out->nt; t++) {
            int finite = 1;
            for (k = 0; k < 3; k++) {
                const float *lp = out->pos[out->idx[3 * t + k]];
                double p[3] = {lp[0], lp[1], lp[2]};
                for (r = 0; r < 3; r++) {
                    world[nt][k][r] = mat[0][r] * p[0] + mat[1][r] * p[1] + mat[2][r] * p[2] + mat[3][r];
                    if (!isfinite(world[nt][k][r])) finite = 0;
                }
            }
            if (finite) nt++;
        }
        for (k = 0; k < 3; k++) { lo[k] = INFINITY; hi[k] = -INFINITY; }
        for (t = 0; t < nt; t++)
            for (k = 0; k < 3; k++)
                for (r = 0; r < 3; r++) {
                    lo[r] = fmin(lo[r], world[t][k][r]);
                    hi[r] = fmax(hi[r], world[t][k][r]);
                }
        for (r = 0; r < 3; r++) org[r] = isfinite(lo[0]) ? round((lo[r] + hi[r]) / 2.0) : 0.0;
        /* MeshBuilder::build: skip degenerate triangles, weld by f32 bits */
        out->nt = 0;
        for (t = 0; t < nt; t++) {
            double u[3], v[3], c[3], l;
            for (r = 0; r < 3; r++) { u[r] = world[t][1][r] - world[t][0][r]; v[r] = world[t][2][r] - world[t][0][r]; }
            c[0] = u[1] * v[2] - u[2] * v[1];
            c[1] = u[2] * v[0] - u[0] * v[2];
            c[2] = u[0] * v[1] - u[1] * v[0];
            l = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            if (isnan(l) || l <= 0.0) continue;
            for (k = 0; k < 3; k++) {
                float p[3];
                for (r = 0; r < 3; r++) p[r] = (float)(world[t][k][r] - org[r]);
                for (i = 0; i < nv; i++)
                    if (bits32(spos[i][0]) == bits32(p[0]) && bits32(spos[i][1]) == bits32(p[1])
                        && bits32(spos[i][2]) == bits32(p[2])) break;
                if (i == nv) { memcpy(spos[nv], p, 12); nv++; }
                out->idx[3 * out->nt + k] = (uint32_t)i;
            }
            out->nt++;
        }
        memcpy(out->pos, spos, (size_t)nv * 12);
        out->nv = nv;
        memcpy(out->origin, org, 24);
    }
    crease_edges(out, 20.0f, scene);
    return 0;
}
