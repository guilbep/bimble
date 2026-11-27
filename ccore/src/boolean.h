/* Boolean operations on closed triangle meshes: bimble-geom boolean.rs (ADR 0010, 0068, 0069).
 *
 * Input and output are triangle soups in f64 (mm, world): 9 doubles per triangle, three points.
 * cc_boolean_apply is boolean.rs `apply`, in portable C: shared vertices welded exactly (same
 * bits) after a shift to a local origin (the middle of the box, rounded to mm), the kernel
 * called, and once more with corners within SEW_MM sewn when an operand is not closed.
 *
 * The kernel is a function table over welded meshes, set at start. Built with CC_HAVE_MANIFOLD
 * (build.sh, when the manifold C binding is found), the table defaults to manifold through its C
 * binding (manifoldc, the same calls as the manifold-csg crate Rust uses, so the same bits).
 * Without it (wasm, a build without the library) there is no kernel: cc_boolean_apply returns
 * CC_BOOL_NO_KERNEL and callers draw uncut with a warning, as before. */
#ifndef CCORE_BOOLEAN_H
#define CCORE_BOOLEAN_H
#include <stddef.h>
#include <stdint.h>

typedef enum { CC_BOOL_DIFFERENCE, CC_BOOL_UNION, CC_BOOL_INTERSECTION } cc_bool_op;

/* Results of the kernel and of cc_boolean_apply. -1: out of memory. */
enum {
    CC_BOOL_OK = 0,
    CC_BOOL_NOT_CLOSED = 1, /* an operand is not a closed 2-manifold ("not a closed solid: ...") */
    CC_BOOL_FAILED = 2,     /* the operation itself failed ("boolean failed: ...") */
    CC_BOOL_NO_KERNEL = 3   /* no kernel set */
};

/* A welded mesh: nv points (x, y, z f64), nt triangles (3 indices each). */
typedef struct {
    double *pos;
    size_t nv;
    uint64_t *idx;
    size_t nt;
} cc_bmesh;

#define CC_BOOL_ERR 96 /* room for an error text, with its 0 */

/* The kernel. Each operation: a op (union of b[0..nb)), nb >= 1, b[0] unioned with b[1], then
 * b[2]... (boolean.rs `kernel`). out is malloc'd (free out->pos and out->idx). err gets Rust's
 * text ("not a closed solid: manifold3d status: NotManifold"...). is_closed: 1 when the mesh is a
 * closed 2-manifold for the kernel. */
typedef struct {
    int (*difference)(const cc_bmesh *a, const cc_bmesh *b, size_t nb, cc_bmesh *out, char *err);
    int (*intersection)(const cc_bmesh *a, const cc_bmesh *b, size_t nb, cc_bmesh *out, char *err);
    int (*unite)(const cc_bmesh *a, const cc_bmesh *b, size_t nb, cc_bmesh *out, char *err);
    int (*is_closed)(const cc_bmesh *m);
} cc_boolean;

/* The manifold adapter, NULL when not built in. */
const cc_boolean *cc_boolean_manifold(void);
/* The kernel in use (default: cc_boolean_manifold()), NULL: none. Set at start; NULL turns
 * cutting off (the tests use it to check the uncut path in a build with the kernel). */
const cc_boolean *cc_boolean_kernel(void);
void cc_boolean_set(const cc_boolean *k);

/* A triangle set (borrowed). */
typedef struct {
    const double (*t)[9];
    size_t n;
} cc_tri_set;

/* boolean.rs apply: a op (union of the non-empty b). On CC_BOOL_OK, *out (malloc, *nout
 * triangles; NULL when none) holds the result; the caller frees it. err (CC_BOOL_ERR bytes, may
 * be NULL) gets the error text otherwise. Returns CC_BOOL_*, or -1 out of memory. */
int cc_boolean_apply(cc_bool_op op, cc_tri_set a, const cc_tri_set *b, size_t nb, double (**out)[9], size_t *nout,
                     char *err);

/* boolean.rs snap: points within tol (each coordinate) of an earlier one moved onto it,
 * collapsed triangles dropped. *out malloc'd. 0 ok, -1 out of memory. */
int cc_boolean_snap(cc_tri_set t, double tol, double (**out)[9], size_t *nout);

#define CC_SEW_MM 0.1

/* TESS1 (ADR 0085): a kernel result made of fewer triangles. Coplanar neighbours (every point of a
 * triangle within tol mm of the plane of the region's largest triangle, facing its way) merged into
 * one polygon with its holes and triangulated again by earcut, keeping every boundary point (so a
 * closed solid stays closed and no point moves). A region is kept as given when its boundary
 * pinches, earcut would drop a point, the area would change or nothing is saved. Only for a closed
 * 2-manifold soup (every edge once each way, as the kernel gives): else 1 and nothing. 0 with *out
 * (malloc, *nout triangles, the caller frees it); 1 nothing saved (*out NULL); -1 out of memory. */
int cc_boolean_merge_coplanar(const double (*t)[9], size_t n, double tol, double (**out)[9], size_t *nout);
#endif
