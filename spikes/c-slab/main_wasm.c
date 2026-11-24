/* Freestanding wasm32 build of the slab mesh (no libc, no imports). The ABI is the one of
 * web/glue.js: slab_mesh(x0, y0, x1, y1, t) -> 1|0, then pos/idx/edge pointers and counts. */
#include <stddef.h>
#include <stdint.h>

/* ---- the libc subset slab.c uses ---- */
#define INFINITY __builtin_inff()
#define isfinite(x) __builtin_isfinite(x)
#define isnan(x) __builtin_isnan(x)
#define sqrt(x) __builtin_sqrt(x)   /* f64.sqrt */
#define sqrtf(x) __builtin_sqrtf(x) /* f32.sqrt */
#define fabs(x) __builtin_fabs(x)   /* f64.abs */

/* C round: half away from zero. x - trunc(x) is exact. */
static double round(double x) {
    double t = __builtin_trunc(x);
    if (__builtin_fabs(x - t) >= 0.5) t += __builtin_copysign(1.0, x);
    return t;
}
static float roundf(float x) {
    float t = __builtin_truncf(x);
    if (__builtin_fabsf(x - t) >= 0.5f) t += __builtin_copysignf(1.0f, x);
    return t;
}
/* C fmin/fmax: a NaN operand is ignored (wasm f64.min would propagate it). */
static double fmin(double a, double b) { return a != a ? b : b != b ? a : (a < b ? a : b); }
static double fmax(double a, double b) { return a != a ? b : b != b ? a : (a > b ? a : b); }

/* slab.c calls cosf once, for the 20 degree crease angle. Use the f32 glibc computes natively
 * (cosf(0x3eb2b8c2) = 0x3f708fb2) so the crease test is bit-identical; any other angle traps. */
static float cosf(float x) {
    union { float f; uint32_t u; } in = {x}, out;
    if (in.u != 0x3eb2b8c2u) __builtin_trap();
    out.u = 0x3f708fb2u;
    return out.f;
}

/* slab.c calls these, and clang emits calls to them for copies and zeroing; never let it turn them into calls to themselves. */
__attribute__((no_builtin)) void *memcpy(void *restrict d, const void *restrict s, size_t n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
__attribute__((no_builtin)) void *memset(void *d, int c, size_t n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
__attribute__((no_builtin)) void *memmove(void *d, const void *s, size_t n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else while (n--) p[n] = q[n];
    return d;
}

#include "slab.c"

/* ---- exported ABI ---- */
static SlabMesh mesh;

#define EXPORT(name) __attribute__((export_name(#name)))

EXPORT(slab_mesh) int wasm_slab_mesh(double x0, double y0, double x1, double y1, double t) {
    static const double at[3] = {1000.0, 2000.0, 3000.0}; /* placement of the golden case */
    double outline[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    return slab_mesh(outline, 4, t, at, &mesh, 1) == 0;
}
EXPORT(pos_ptr) uint32_t pos_ptr(void) { return (uint32_t)(uintptr_t)mesh.pos; }
EXPORT(pos_len) int pos_len(void) { return mesh.nv; }
EXPORT(idx_ptr) uint32_t idx_ptr(void) { return (uint32_t)(uintptr_t)mesh.idx; }
EXPORT(idx_len) int idx_len(void) { return 3 * mesh.nt; }
EXPORT(edge_ptr) uint32_t edge_ptr(void) { return (uint32_t)(uintptr_t)mesh.edges; }
EXPORT(edge_len) int edge_len(void) { return mesh.ne; }
