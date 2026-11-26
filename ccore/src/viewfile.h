/* The viewer export, `.bimbleview` (bimble_geom::viewfile, ADR 0040 and 0069): a scene as bytes
 * for others to look at, without the model. Byte for byte with Rust's
 * `viewfile::write(Scene::from_model(m), gzip)`: the raw stream equals Rust's exactly; the gzip
 * wrapper is this core's own deflate (inflate.h), so compare gzipped files after inflating.
 * Layout in golden/README.md (view/): "BIMBLEVW", u32 version 1, sections HEAD, META, GEOR (when
 * the georeference is not the default, ADR 0045), ELEM chunks of about 1 MiB; meshes in the
 * "BMG1" codec (meshcodec.h).
 *
 * Rust writes from any Scene shown (ribbon.rs:575): a model's, an IFC read as it is, a view file
 * opened. Here the scene's text fields beside the geometry are a cc_viewfile_meta: per element
 * GlobalId, name, storey and material (class and family come from the cc_scene element), the
 * storeys, grids, densities and georeference. Fillers: cc_viewfile_meta_from_model (from_model:
 * storeys sorted by elevation, no grids, densities by material name, the model's georeference),
 * cc_viewfile_meta_from_file (what an opened view file holds, as is: sharing it again gives its
 * raw bytes) and cc_ifc_scene_meta (ifc_read.h). The META table is computed by the writer: the
 * sorted distinct class, storey, material and family names; the warnings are the scene's.
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
 * `gzip`: cc_viewfile_meta_from_model then cc_viewfile_write_meta. 0 ok, -1 out of memory (or a
 * scene that does not belong to the model). */
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

typedef struct { /* the text of one scene element (SceneElement's fields beside class, family) */
    cc_str global_id, name, storey, material;
    /* WAVE1B (B1): what the quantities add to the mesh's volume (m3): for each sleeve of the element,
     * its 32-gon's area minus the circle's, times the host's thickness (the exact cylinder pi r^2 t,
     * ADR 0077). From the model only (cc_viewfile_meta_from_model); 0 otherwise; never written. */
    double volume_fix_m3;
} cc_view_info;

/* A scene's META (and GEOR) for the writer. Every string and array is borrowed (from the model,
 * the scene, the view file or the IFC scene, which must outlive it) or lives in `arena`. */
typedef struct {
    Arena arena;
    const cc_view_info *info; /* one per scene element, same index */
    size_t ninfo;
    const cc_view_storey *storeys; /* written in this order */
    size_t nstoreys;
    const cc_view_grid *grids;
    size_t ngrids;
    const cc_view_density *densities; /* sorted by material, one per material */
    size_t ndensities;
    int has_georef;
    cc_str georef; /* GEOR's JSON text, when has_georef (Rust: only when not the default) */
} cc_viewfile_meta;

void cc_viewfile_meta_init(cc_viewfile_meta *mt);
void cc_viewfile_meta_free(cc_viewfile_meta *mt);
/* Fill `mt` (always initialized: free it either way). 0 ok, -1 out of memory or a scene that
 * does not belong to the model. */
int cc_viewfile_meta_from_model(cc_viewfile_meta *mt, const cc_model *m, const cc_scene *s);
int cc_viewfile_meta_from_file(cc_viewfile_meta *mt, const cc_viewfile *v);

/* Appends the view file of scene `s` with its META `mt` to `out`, gzipped when `gzip`
 * (viewfile.rs write). 0 ok, -1 out of memory or `mt` not of `s` (ninfo != nelements). */
int cc_viewfile_write_meta(const cc_viewfile_meta *mt, const cc_scene *s, int gzip, Buf *out);

/* Reads a view file. Always initializes `out` (free it with cc_viewfile_free either way).
 * 0 ok, -1 with err->msg. */
int cc_viewfile_read(const uint8_t *p, size_t n, cc_viewfile *out, cc_error *err);
void cc_viewfile_free(cc_viewfile *v);

/* The element's mesh as stored: its own, or its shared shape in the shape's coordinates. */
const cc_mesh *cc_viewfile_local_mesh(const cc_viewfile *v, size_t k);
#endif
