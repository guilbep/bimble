/* The viewer export, `.bimbleview` (bimble_geom::viewfile, ADR 0040 and 0069): a scene as bytes
 * for others to look at, without the model. Byte for byte with Rust's
 * `viewfile::write(Scene::from_model(m), gzip)`: the raw stream equals Rust's exactly; the gzip
 * wrapper is this core's own deflate (inflate.h), so compare gzipped files after inflating.
 * Layout in golden/README.md (view/): "BIMBLEVW", u32 version 1, sections HEAD, META, GEOR (never
 * written here: the C model has no georeference), ELEM chunks of about 1 MiB; meshes in the
 * "BMG1" codec (meshcodec.h).
 *
 * The META section is computed from the model and the scene: the sorted table of class, storey,
 * material and family names, the storeys sorted by elevation, no grids (from_model has none), the
 * densities by material name, the scene's warnings.
 *
 * Reading takes untrusted bytes (raw or gzipped): lengths checked against what is there, indices
 * against what was read, floats finite, UTF-8 checked, inflated size bounded, never aborts. The
 * error texts are Rust's `viewfile::Error` display texts. */
#ifndef CCORE_VIEWFILE_H
#define CCORE_VIEWFILE_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "buf.h"
#include "model.h"
#include "scene.h"

#define CC_VIEWFILE_VERSION 1u
#define CC_VIEWFILE_CHUNK_BYTES ((size_t)1 << 20)

/* Appends the view file of scene `s` (built from `m` by cc_scene_build) to `out`, gzipped when
 * `gzip`. 0 ok, -1 out of memory (or a scene that does not belong to the model). */
int cc_viewfile_write(const cc_model *m, const cc_scene *s, int gzip, Buf *out);

/* Whether bytes start like a view file (raw magic or gzip). */
int cc_viewfile_sniff(const uint8_t *p, size_t n);

typedef struct {
    cc_str name;
    double elevation; /* mm */
    cc_str id;
} cc_view_storey;

typedef struct {
    cc_str tag;
    double start[2], end[2];
} cc_view_grid;

typedef struct {
    cc_str material;
    double density; /* kg/m3 */
} cc_view_density;

typedef struct {
    cc_str global_id, name, class_name, storey, material, family;
    cc_category category; /* from the family label; Other when unknown */
    int32_t shape;        /* index in shapes, or -1: own mesh in `mesh` */
    cc_mesh mesh;         /* own mesh, scene coordinates (shape < 0) */
    cc_placement32 to_scene; /* shape >= 0 */
} cc_view_element;

typedef struct {
    Arena arena; /* every string and mesh */
    double origin[3];
    uint64_t stored_triangles; /* as HEAD states it */
    cc_str *table;
    size_t ntable;
    cc_view_storey *storeys;
    size_t nstoreys;
    cc_view_grid *grids;
    size_t ngrids;
    cc_view_density *densities; /* file order (sorted by name) */
    size_t ndensities;
    cc_str *warnings;
    size_t nwarnings;
    int has_georef;
    cc_str georef; /* GEOR's JSON text (checked to parse), when has_georef */
    cc_mesh *shapes; /* shared shapes, own coordinates (malloc) */
    size_t nshapes;
    cc_view_element *elements; /* malloc */
    size_t nelements;
} cc_viewfile;

/* Reads a view file. Always initializes `out` (free it with cc_viewfile_free either way).
 * 0 ok, -1 with err->msg. */
int cc_viewfile_read(const uint8_t *p, size_t n, cc_viewfile *out, cc_error *err);
void cc_viewfile_free(cc_viewfile *v);

/* The element's mesh as stored: its own, or its shared shape in the shape's coordinates. */
const cc_mesh *cc_viewfile_local_mesh(const cc_viewfile *v, size_t k);
#endif
