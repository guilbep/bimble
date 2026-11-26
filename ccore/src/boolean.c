/* boolean.rs in C: apply, weld, snap (portable), and the manifold adapter (CC_HAVE_MANIFOLD). */
#include "boolean.h"
#include <stdlib.h>
#include <string.h>

/* ---- small helpers (no libm: wasm is freestanding) */

static double f_min(double a, double b) { return a != a ? b : b != b ? a : a < b ? a : b; } /* f64::min */
static double f_max(double a, double b) { return a != a ? b : b != b ? a : a > b ? a : b; } /* f64::max */
static double f_floor(double x) {
    double ax = x < 0 ? -x : x;
    if (!(ax < 4503599627370496.0)) return x; /* 2^52: an integer already (or NaN, inf) */
    double t = (double)(int64_t)x;
    if (t > x) t -= 1.0;
    return t == 0.0 && x < 0 ? -0.0 : t;
}
static double f_round(double x) { /* f64::round: half away from zero */
    double ax = x < 0 ? -x : x;
    if (!(ax < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x, d = x - t;
    if (d >= 0.5) t += 1.0;
    else if (d <= -0.5) t -= 1.0;
    if (t == 0.0) return x < 0 ? -0.0 : 0.0;
    return t;
}
static int64_t f_to_i64(double x) { /* `as i64`: saturating, NaN -> 0 */
    if (x != x) return 0;
    if (x >= 9223372036854775807.0) return INT64_MAX;
    if (x <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)x;
}
static uint64_t bits(double x) {
    uint64_t u;
    memcpy(&u, &x, 8);
    return u;
}
static uint64_t mix(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}
static void put_err(char *err, const char *a, const char *b) {
    if (!err) return;
    size_t n = 0;
    for (; a && *a && n + 1 < CC_BOOL_ERR; a++) err[n++] = *a;
    for (; b && *b && n + 1 < CC_BOOL_ERR; b++) err[n++] = *b;
    err[n] = 0;
}

/* ---- the kernel table */

#ifdef CC_HAVE_MANIFOLD
static const cc_boolean MANIFOLD_TABLE;
static const cc_boolean *g_kernel = &MANIFOLD_TABLE;
const cc_boolean *cc_boolean_manifold(void) { return &MANIFOLD_TABLE; }
#else
static const cc_boolean *g_kernel = NULL;
const cc_boolean *cc_boolean_manifold(void) { return NULL; }
#endif
const cc_boolean *cc_boolean_kernel(void) { return g_kernel; }
void cc_boolean_set(const cc_boolean *k) { g_kernel = k; }

/* ---- weld: shared vertices welded exactly (same bits after the shift to `origin`) */

static int weld(cc_tri_set t, const double origin[3], cc_bmesh *m) {
    memset(m, 0, sizeof *m);
    size_t np = 3 * t.n, cap = 16;
    while (cap < 2 * np) cap *= 2;
    uint32_t *slot = calloc(cap, sizeof *slot); /* vertex index + 1, 0 empty */
    m->pos = malloc((np ? np : 1) * 3 * sizeof *m->pos);
    m->idx = malloc((np ? np : 1) * sizeof *m->idx);
    if (!slot || !m->pos || !m->idx || np > 0xfffffffeu) {
        free(slot), free(m->pos), free(m->idx);
        memset(m, 0, sizeof *m);
        return -1;
    }
    for (size_t i = 0; i < np; i++) {
        const double *p = &t.t[i / 3][3 * (i % 3)];
        /* `+ 0.0` turns -0.0 into 0.0 so equal coordinates weld. */
        double q[3] = {p[0] - origin[0] + 0.0, p[1] - origin[1] + 0.0, p[2] - origin[2] + 0.0};
        uint64_t k[3] = {bits(q[0]), bits(q[1]), bits(q[2])};
        size_t h = (size_t)mix(k[0] ^ mix(k[1] ^ mix(k[2]))) & (cap - 1);
        for (;;) {
            uint32_t s = slot[h];
            if (!s) {
                memcpy(&m->pos[3 * m->nv], q, sizeof q);
                slot[h] = (uint32_t)(++m->nv);
                m->idx[i] = m->nv - 1;
                break;
            }
            const double *v = &m->pos[3 * (s - 1)];
            if (bits(v[0]) == k[0] && bits(v[1]) == k[1] && bits(v[2]) == k[2]) {
                m->idx[i] = s - 1;
                break;
            }
            h = (h + 1) & (cap - 1);
        }
    }
    m->nt = t.n;
    free(slot);
    return 0;
}
static void bmesh_free(cc_bmesh *m) {
    free(m->pos), free(m->idx);
    memset(m, 0, sizeof *m);
}

/* ---- snap */

int cc_boolean_snap(cc_tri_set t, double tol, double (**out)[9], size_t *nout) {
    *out = NULL, *nout = 0;
    size_t np = 3 * t.n, cap = 16;
    while (cap < 2 * np) cap *= 2;
    typedef struct {
        int64_t k[3];
        uint32_t head, tail; /* point index + 1 */
    } Cell;
    Cell *cells = calloc(cap, sizeof *cells);
    double (*pts)[3] = malloc((np ? np : 1) * sizeof *pts);
    uint32_t *next = malloc((np ? np : 1) * sizeof *next);
    double (*o)[9] = malloc((t.n ? t.n : 1) * sizeof *o);
    size_t npts = 0, no = 0;
    if (!cells || !pts || !next || !o || np > 0xfffffffeu) {
        free(cells), free(pts), free(next), free(o);
        return -1;
    }
    for (size_t ti = 0; ti < t.n; ti++) {
        double q[3][3];
        for (int c = 0; c < 3; c++) {
            const double *p = &t.t[ti][3 * c];
            int64_t k[3];
            for (int r = 0; r < 3; r++) k[r] = f_to_i64(f_floor(p[r] / tol));
            const double *found = NULL;
            for (int dx = -1; dx <= 1 && !found; dx++)
                for (int dy = -1; dy <= 1 && !found; dy++)
                    for (int dz = -1; dz <= 1 && !found; dz++) {
                        int64_t kk[3] = {(int64_t)((uint64_t)k[0] + (uint64_t)(int64_t)dx),
                                         (int64_t)((uint64_t)k[1] + (uint64_t)(int64_t)dy),
                                         (int64_t)((uint64_t)k[2] + (uint64_t)(int64_t)dz)};
                        size_t h = (size_t)mix((uint64_t)kk[0] ^ mix((uint64_t)kk[1] ^ mix((uint64_t)kk[2]))) & (cap - 1);
                        while (cells[h].head && memcmp(cells[h].k, kk, sizeof kk)) h = (h + 1) & (cap - 1);
                        for (uint32_t s = cells[h].head; s && !found; s = next[s - 1]) {
                            const double *v = pts[s - 1];
                            double d0 = v[0] - p[0], d1 = v[1] - p[1], d2 = v[2] - p[2];
                            if ((d0 < 0 ? -d0 : d0) <= tol && (d1 < 0 ? -d1 : d1) <= tol && (d2 < 0 ? -d2 : d2) <= tol)
                                found = v;
                        }
                    }
            if (found) {
                memcpy(q[c], found, sizeof q[c]);
                continue;
            }
            size_t h = (size_t)mix((uint64_t)k[0] ^ mix((uint64_t)k[1] ^ mix((uint64_t)k[2]))) & (cap - 1);
            while (cells[h].head && memcmp(cells[h].k, k, sizeof k)) h = (h + 1) & (cap - 1);
            memcpy(pts[npts], p, sizeof pts[npts]);
            next[npts] = 0;
            npts++;
            if (!cells[h].head) {
                memcpy(cells[h].k, k, sizeof k);
                cells[h].head = (uint32_t)npts;
            } else next[cells[h].tail - 1] = (uint32_t)npts;
            cells[h].tail = (uint32_t)npts;
            memcpy(q[c], p, sizeof q[c]);
        }
        int eq01 = q[0][0] == q[1][0] && q[0][1] == q[1][1] && q[0][2] == q[1][2];
        int eq12 = q[1][0] == q[2][0] && q[1][1] == q[2][1] && q[1][2] == q[2][2];
        int eq02 = q[0][0] == q[2][0] && q[0][1] == q[2][1] && q[0][2] == q[2][2];
        if (eq01 || eq12 || eq02) continue;
        memcpy(o[no++], q, sizeof o[0]);
    }
    free(cells), free(pts), free(next);
    *out = o, *nout = no;
    return 0;
}

/* ---- apply */

static int kernel(const cc_boolean *k, cc_bool_op op, cc_tri_set a, const cc_tri_set *b, size_t nb,
                  const double origin[3], double (**out)[9], size_t *nout, char *err) {
    cc_bmesh *m = calloc(nb + 1, sizeof *m), r;
    memset(&r, 0, sizeof r);
    int rc = m ? 0 : -1;
    for (size_t i = 0; rc == 0 && i <= nb; i++) rc = weld(i ? b[i - 1] : a, origin, &m[i]);
    if (rc == 0) {
        rc = (op == CC_BOOL_DIFFERENCE ? k->difference : op == CC_BOOL_UNION ? k->unite : k->intersection)(
            &m[0], &m[1], nb, &r, err);
    }
    for (size_t i = 0; m && i <= nb; i++) bmesh_free(&m[i]);
    free(m);
    if (rc) {
        bmesh_free(&r);
        return rc;
    }
    double (*o)[9] = malloc((r.nt ? r.nt : 1) * sizeof *o);
    if (!o) {
        bmesh_free(&r);
        return -1;
    }
    for (size_t t = 0; t < r.nt; t++)
        for (int c = 0; c < 3; c++) {
            uint64_t v = r.idx[3 * t + c];
            if (v >= r.nv) { /* a bad mesh from the kernel */
                free(o);
                bmesh_free(&r);
                put_err(err, "bad mesh from the kernel", NULL);
                return CC_BOOL_FAILED;
            }
            for (int q = 0; q < 3; q++) o[t][3 * c + q] = r.pos[3 * v + q] + origin[q];
        }
    *out = o, *nout = r.nt;
    bmesh_free(&r);
    return 0;
}

int cc_boolean_apply(cc_bool_op op, cc_tri_set a, const cc_tri_set *b, size_t nb, double (**out)[9], size_t *nout,
                     char *err) {
    *out = NULL, *nout = 0;
    if (err) err[0] = 0;
    cc_tri_set *bb = malloc((nb ? nb : 1) * sizeof *bb);
    if (!bb) return -1;
    size_t n = 0;
    for (size_t i = 0; i < nb; i++)
        if (b[i].n) bb[n++] = b[i];
    if (!n) { /* nothing to cut: the input (an intersection: nothing) */
        free(bb);
        if (op == CC_BOOL_INTERSECTION || !a.n) return 0;
        double (*o)[9] = malloc(a.n * sizeof *o);
        if (!o) return -1;
        memcpy(o, a.t, a.n * sizeof *o);
        *out = o, *nout = a.n;
        return 0;
    }
    const cc_boolean *k = g_kernel;
    if (!k) {
        free(bb);
        put_err(err, "no boolean kernel", NULL);
        return CC_BOOL_NO_KERNEL;
    }
    /* bounds over a then b: f64::min / max from the first point. */
    double lo[3], hi[3];
    int any = 0;
    for (size_t s = 0; s <= n; s++) {
        cc_tri_set t = s ? bb[s - 1] : a;
        for (size_t i = 0; i < t.n; i++)
            for (int c = 0; c < 3; c++) {
                const double *p = &t.t[i][3 * c];
                for (int q = 0; q < 3; q++) {
                    if (!any) lo[q] = hi[q] = p[q];
                    else lo[q] = f_min(lo[q], p[q]), hi[q] = f_max(hi[q], p[q]);
                }
                any = 1;
            }
    }
    if (!any) {
        free(bb);
        return 0;
    }
    /* Coordinates relative to the middle keep full precision with site coordinates. */
    double origin[3];
    for (int q = 0; q < 3; q++) origin[q] = f_round((lo[q] + hi[q]) / 2.0);
    int rc = kernel(k, op, a, bb, n, origin, out, nout, err);
    if (rc == CC_BOOL_NOT_CLOSED) {
        /* Solids exported with corners a hair apart (Revit Breps: 0.03-0.07 mm) are not closed for
         * the kernel: once more with points that close welded together, as IfcOpenShell sews them. */
        double (*sa)[9] = NULL, (**sb)[9] = calloc(n, sizeof *sb);
        size_t nsa = 0, *nsb = calloc(n, sizeof *nsb);
        rc = sb && nsb ? cc_boolean_snap(a, CC_SEW_MM, &sa, &nsa) : -1;
        for (size_t i = 0; rc == 0 && i < n; i++) rc = cc_boolean_snap(bb[i], CC_SEW_MM, &sb[i], &nsb[i]);
        if (rc == 0) {
            cc_tri_set as = {(const double(*)[9])sa, nsa};
            for (size_t i = 0; i < n; i++) bb[i].t = (const double(*)[9])sb[i], bb[i].n = nsb[i];
            if (err) err[0] = 0;
            rc = kernel(k, op, as, bb, n, origin, out, nout, err);
        }
        free(sa);
        for (size_t i = 0; sb && i < n; i++) free(sb[i]);
        free(sb), free(nsb);
    }
    free(bb);
    return rc;
}

/* ---- manifold through its C binding: the calls manifold-csg makes */

#ifdef CC_HAVE_MANIFOLD
#include <manifold/manifoldc.h>

static const char *status_name(ManifoldError e) { /* manifold_csg_sys::ManifoldError, Debug */
    static const char *N[] = {"NoError", "NonFiniteVertex", "NotManifold", "VertexOutOfBounds",
                              "PropertiesWrongLength", "MissingPositionProperties", "MergeVectorsDifferentLengths",
                              "MergeIndexOutOfBounds", "TransformWrongLength", "RunIndexWrongLength",
                              "FaceIdWrongLength", "InvalidConstruction", "ResultTooLarge", "InvalidTangents",
                              "Cancelled"};
    unsigned i = (unsigned)e;
    return i < sizeof N / sizeof *N ? N[i] : "Unknown";
}

/* Manifold::from_mesh_f64 then status() (to_manifold). NULL with err set when not closed. */
static ManifoldManifold *to_manifold(const cc_bmesh *m, char *err) {
    ManifoldMeshGL64 *gl = manifold_meshgl64(manifold_alloc_meshgl64(), m->pos, m->nv, 3, m->idx, m->nt);
    ManifoldManifold *r = manifold_of_meshgl64(manifold_alloc_manifold(), gl);
    manifold_delete_meshgl64(gl);
    ManifoldError s = manifold_status(r);
    if (s != MANIFOLD_NO_ERROR) {
        put_err(err, "not a closed solid: manifold3d status: ", status_name(s));
        manifold_delete_manifold(r);
        return NULL;
    }
    return r;
}

static int mf_op(int op, const cc_bmesh *a, const cc_bmesh *b, size_t nb, cc_bmesh *out, char *err) {
    memset(out, 0, sizeof *out);
    ManifoldManifold *ma = to_manifold(a, err), **cut = calloc(nb ? nb : 1, sizeof *cut);
    int rc = ma && cut ? 0 : cut ? CC_BOOL_NOT_CLOSED : -1;
    for (size_t i = 0; rc == 0 && i < nb; i++)
        if (!(cut[i] = to_manifold(&b[i], err))) rc = CC_BOOL_NOT_CLOSED;
    ManifoldManifold *u = NULL, *r = NULL;
    if (rc == 0) {
        u = manifold_copy(manifold_alloc_manifold(), cut[0]);
        for (size_t i = 1; i < nb; i++) {
            ManifoldManifold *v = manifold_union(manifold_alloc_manifold(), u, cut[i]);
            manifold_delete_manifold(u);
            u = v;
        }
        r = op == 0   ? manifold_difference(manifold_alloc_manifold(), ma, u)
            : op == 1 ? manifold_union(manifold_alloc_manifold(), ma, u)
                      : manifold_intersection(manifold_alloc_manifold(), ma, u);
        ManifoldError s = manifold_status(r);
        if (s != MANIFOLD_NO_ERROR) {
            put_err(err, "boolean failed: manifold3d status: ", status_name(s));
            rc = CC_BOOL_FAILED;
        }
    }
    if (rc == 0) {
        ManifoldMeshGL64 *gl = manifold_get_meshgl64(manifold_alloc_meshgl64(), r);
        size_t np = manifold_meshgl64_num_prop(gl), nv = manifold_meshgl64_num_vert(gl);
        size_t nt = manifold_meshgl64_num_tri(gl);
        double *props = malloc((manifold_meshgl64_vert_properties_length(gl) + 1) * sizeof *props);
        out->pos = malloc((nv * 3 + 1) * sizeof *out->pos);
        out->idx = malloc((manifold_meshgl64_tri_length(gl) + 1) * sizeof *out->idx);
        if (!props || !out->pos || !out->idx || np < 3) {
            free(out->pos), free(out->idx);
            memset(out, 0, sizeof *out);
            rc = np < 3 && props ? CC_BOOL_FAILED : -1;
            if (rc == CC_BOOL_FAILED) put_err(err, "bad mesh from the kernel", NULL);
        } else {
            manifold_meshgl64_vert_properties(props, gl);
            manifold_meshgl64_tri_verts(out->idx, gl);
            for (size_t i = 0; i < nv; i++)
                for (int q = 0; q < 3; q++) out->pos[3 * i + q] = props[i * np + q];
            out->nv = nv, out->nt = nt;
        }
        free(props);
        manifold_delete_meshgl64(gl);
    }
    if (r) manifold_delete_manifold(r);
    if (u) manifold_delete_manifold(u);
    for (size_t i = 0; cut && i < nb; i++)
        if (cut[i]) manifold_delete_manifold(cut[i]);
    free(cut);
    if (ma) manifold_delete_manifold(ma);
    return rc;
}
static int mf_difference(const cc_bmesh *a, const cc_bmesh *b, size_t nb, cc_bmesh *out, char *err) {
    return mf_op(0, a, b, nb, out, err);
}
static int mf_union(const cc_bmesh *a, const cc_bmesh *b, size_t nb, cc_bmesh *out, char *err) {
    return mf_op(1, a, b, nb, out, err);
}
static int mf_intersection(const cc_bmesh *a, const cc_bmesh *b, size_t nb, cc_bmesh *out, char *err) {
    return mf_op(2, a, b, nb, out, err);
}
static int mf_is_closed(const cc_bmesh *m) {
    ManifoldManifold *r = to_manifold(m, NULL);
    if (r) manifold_delete_manifold(r);
    return r != NULL;
}
static const cc_boolean MANIFOLD_TABLE = {mf_difference, mf_intersection, mf_union, mf_is_closed};
#endif
