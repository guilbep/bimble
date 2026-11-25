/* The IFC display reader, the C twin of bimble_ifc::read::read_str (crates/bimble-ifc/src/read.rs):
 * what the viewer draws when it opens an IFC read-only. Not a model: a render scene with, per
 * element, the file's GlobalId, IFC class, name, storey name, material and category, and its own
 * mesh (scene coordinates) or a copy of a shared shape (an IfcRepresentationMap meshed once,
 * ADR 0034, or a copy found by share_copies, ADR 0036). Plus storeys, grid axes, the
 * georeference (building frame, IfcMapConversion) and the schema.
 *
 * Same limits as Rust (Limits::for_text: 10 triangles per displayed byte, at least 1 M; item
 * visits per 10 displayed bytes, at least 5 M; per element at most that many triangles + 1), the
 * same batches of 1024 elements and the same cut, single-threaded. The geometry is ifc_geom.h,
 * shared with the import (ifc_in.c).
 *
 * Not done (reported in the warnings, never silent): no boolean kernel in the C core (Rust:
 * manifold3d), so openings (IfcRelVoidsElement) are not cut and boolean results are drawn as
 * their first operand. Like the import, trigonometry is correctly rounded (ifc_math.h). */
#ifndef CCORE_IFC_READ_H
#define CCORE_IFC_READ_H
#include <stddef.h>
#include "buf.h"
#include "model.h"
#include "scene.h"

typedef struct { /* bimble_geom::StoreyLevel */
    cc_str name;
    double elevation_mm;
    cc_str id; /* the file's GlobalId text, empty when none */
} cc_ifc_storey;

typedef struct { /* bimble_geom::GridAxis: plan, world mm */
    cc_str tag;
    double start_mm[2], end_mm[2];
} cc_ifc_grid;

typedef struct { /* bimble_core::georef::Georef */
    double rotation_deg, east_m, north_m, height_m, map_rotation_deg;
    int has_crs;
    cc_str crs;
} cc_ifc_georef;

typedef struct { /* ElementInfo of a scene element (SceneElement's text fields) */
    cc_str global_id; /* the file's text as is (not checked) */
    cc_str class_name;
    cc_str name;
    cc_str storey;    /* the storey's (or other container's) name */
    cc_str material;  /* display name, "A / B" for layer sets */
} cc_ifc_info;

typedef struct {
    /* The geometry, as for a model (cc_scene_local_mesh, cc_scene_world_mesh work on it):
     * elements in scene order (file order of the products, empty ones dropped); per element
     * class_name and category are set, `id` is the GlobalId when it is a valid one (else
     * zeros), and model_index is its index in `info`. scene.warnings: the reader's warnings,
     * sorted, "(×N)" when repeated. All memory (strings too) lives in scene.arena. */
    cc_scene scene;
    cc_ifc_info *info; /* one per scene element, same index */
    cc_ifc_storey *storeys; /* lowest first */
    size_t nstoreys;
    cc_ifc_grid *grids; /* file order */
    size_t ngrids;
    cc_ifc_georef georef;
    cc_str schema; /* FILE_SCHEMA names joined by ", " ("IFC4", "IFC2X3") */
    size_t triangles; /* Scene::triangle_count: local meshes' triangles */
} cc_ifc_scene;

/* Reads STEP text for display. Always initializes `out` (free it with cc_ifc_scene_free).
 * 0 ok; -1 with `err`: "parse: line N: …", "not an IFC file (schema \"…\")", "out of memory". */
int cc_ifc_read(const char *text, size_t n, cc_ifc_scene *out, cc_error *err);
void cc_ifc_scene_free(cc_ifc_scene *s);

/* serde_json of the Georef ("{}" when default), appended. */
void cc_ifc_georef_json(Buf *b, const cc_ifc_georef *g);

/* The golden text of ccore/golden/ifc/read/<name>/ (golden/README.md, "ifc/read"): scene.txt
 * and, per element k, <k>.txt (the mesh/ format). */
void cc_ifc_scene_txt(Buf *b, const cc_ifc_scene *s);
void cc_ifc_mesh_txt(Buf *b, const cc_ifc_scene *s, size_t k);
/* The text of a failed read: "error <text>\n". */
void cc_ifc_error_txt(Buf *b, const cc_error *err);

/* The scene as BMSH version 1 (the layout of host/mesh.c, what the lab's WebGL2 viewport
 * draws), appended to `out`. The GlobalId field holds the file's id text, cut or padded with
 * '0' to 22 bytes. 0 ok, -1 with `err` (out of memory, a count over u32). */
int cc_ifc_scene_bmsh(const cc_ifc_scene *s, uint32_t rev, Buf *out, cc_error *err);
#endif
