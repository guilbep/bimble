/* The IFC display reader, the C twin of bimble_ifc::read::read_str (crates/bimble-ifc/src/read.rs):
 * what the viewer draws when it opens an IFC read-only. Not a model: a render scene with, per
 * element, the file's GlobalId, IFC class, name, storey name, material and category, and its own
 * mesh (scene coordinates) or a copy of a shared shape (an IfcRepresentationMap meshed once,
 * ADR 0034, or a copy found by share_copies, ADR 0036). Plus storeys, grid axes, the
 * georeference (building frame, IfcMapConversion) and the schema.
 *
 * Same limits as Rust (Limits::for_text: 10 triangles per displayed byte, at least 1 M; item
 * visits per 10 displayed bytes, at least 5 M; per element at most that many triangles + 1), the
 * same batches of 1024 elements and the same cut. Multi-threaded like Rust (cc_jobs, jobs.h):
 * the STEP chunks, the shared shapes, the elements of a batch and their meshes on all cores,
 * merged in file order, so the scene is the same on any thread count (a batch that runs out of
 * budget is meshed again on one thread, as read.rs). The property and quantity types Rust does
 * not parse for display are not parsed (read.rs NOT_DISPLAYED). One difference: a placement
 * whose chain warned is not cached, so its warning counts once per product (Rust: once per
 * worker that met it, which depends on the timing). The geometry is ifc_geom.h, shared with the
 * import (ifc_in.c).
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
#include "step.h"
#include "viewfile.h"

typedef struct { /* bimble_geom::StoreyLevel */
    cc_str name;
    double elevation_mm;
    cc_str id; /* the file's GlobalId text, empty when none */
} cc_ifc_storey;

typedef struct { /* bimble_geom::GridAxis: plan, world mm */
    cc_str tag;
    double start_mm[2], end_mm[2];
} cc_ifc_grid;

typedef cc_georef cc_ifc_georef; /* bimble_core::georef::Georef (model.h, georef.h) */

typedef struct { /* ElementInfo of a scene element (SceneElement's text fields) */
    cc_str global_id; /* the file's text as is (not checked) */
    cc_str class_name;
    cc_str name;
    cc_str storey;    /* the storey's (or other container's) name */
    cc_str material;  /* display name, "A / B" for layer sets */
    uint64_t step_id; /* M1LINKS: the product's STEP instance number (ifc_psets.h finds its sets by it) */
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
    double base_point_mm[3]; /* LINKF: the origin of the IfcBuilding's placement, world mm (a link's base point) */
    cc_str schema; /* FILE_SCHEMA names joined by ", " ("IFC4", "IFC2X3") */
    size_t triangles; /* Scene::triangle_count: local meshes' triangles */
    /* Counts for reports (not in the golden text, no Rust twin): the products with a body
     * (IfcProductDefinitionShape and a placement, the scene's candidates; those drawn empty are
     * dropped from the scene) and the STEP entities parsed. */
    size_t products, entities;
    cc_str empty_classes; /* the products with a body drawn empty: "<ENTITY> <count>\n" lines */
} cc_ifc_scene;

/* Reads STEP text for display. Always initializes `out` (free it with cc_ifc_scene_free).
 * 0 ok; -1 with `err`: "parse: line N: …", "not an IFC file (schema \"…\")", "out of memory". */
int cc_ifc_read(const char *text, size_t n, cc_ifc_scene *out, cc_error *err);

/* LINKF: the largest IFC an open or a link reads: the STEP reader's (step.h CC_STEP_MAX_BYTES,
 * read.rs 64 GiB) on a 64-bit build; 1 GiB in a 32-bit one (wasm32: Rust's file.rs wasm cap).
 * Not CC_MAX_FILE_BYTES: that is the .bimble's (its JSON DOM). What the machine can give is the
 * other bound (session.c: "too large for this machine"). */
#define CC_IFC_MAX_FILE_BYTES (sizeof(size_t) > 4 ? (size_t)CC_STEP_MAX_BYTES : (size_t)1 << 30)
/* Peak memory of a link's read (the text, the STEP entities, the meshes, the cache written) as
 * PER_BYTE x the file + BASE + FLOOR, BASE at most SMALL x the file, from a local folder of 17 IFC
 * files (ADR 0079): the files over 100 MB peaked at 2.2 to 4.4 times their size; small files with
 * heavy meshes up to 10 times (60 MB: 0.58 GB). SIZE2 refit (ADR 0079 amendment): 3 x + min(640 MiB,
 * 7 x) + 32 MiB is above every row of that table, by 5.7 % (60 MB) to 13 % (0.87 GB: 3.3 GB against
 * 2.91 GB measured; the old 4 x + min(512 MiB, 6 x) asked 4.0 GB); session.c cc_session_fits. */
#define CC_IFC_READ_PEAK_PER_BYTE 3
#define CC_IFC_READ_PEAK_BASE ((uint64_t)640 << 20)
#define CC_IFC_READ_PEAK_SMALL 7
#define CC_READ_PEAK_FLOOR ((uint64_t)32 << 20)

/* LINKF: cc_ifc_read with progress and cancel (both optional): `done` goes 0..1000 as it reads
 * (the STEP parse to 300, the elements to 950, the rest to 1000); a nonzero `cancel` stops it
 * between batches with "cancelled". Written and read with relaxed atomics (another thread polls). */
typedef struct {
    size_t done;
    int cancel;
} cc_ifc_progress;
int cc_ifc_read_progress(const char *text, size_t n, cc_ifc_scene *out, cc_error *err, cc_ifc_progress *pg);
/* TESS1 (ADR 0085): how finely curves become triangles. EXACT: Rust's reader (arcs within 0.5 mm,
 * 15 degrees per chord): the goldens of Rust. STANDARD (the default of cc_ifc_read,
 * cc_ifc_read_progress and opts NULL): arcs within 2 mm, 18 degrees per chord (20 per circle: no
 * crease line along a cylinder). LIGHT: arcs within 10 mm, 30 degrees per chord. Given meshes
 * (faceted breps, face sets) are kept as the file gives them at every level. */
typedef enum { CC_IFC_DETAIL_EXACT = 0, CC_IFC_DETAIL_STANDARD = 1, CC_IFC_DETAIL_LIGHT = 2 } cc_ifc_detail;
typedef struct {
    cc_ifc_detail detail;
} cc_ifc_read_opts;
/* cc_ifc_read_progress with options (NULL: the defaults); an unknown detail reads as STANDARD. */
int cc_ifc_read_with(const char *text, size_t n, const cc_ifc_read_opts *opts, cc_ifc_scene *out, cc_error *err,
                     cc_ifc_progress *pg);
void cc_ifc_scene_free(cc_ifc_scene *s);

/* serde_json of the Georef ("{}" when default), appended. */
void cc_ifc_georef_json(Buf *b, const cc_ifc_georef *g);

/* The META of the read scene for the .bimbleview writer (cc_viewfile_write_meta): Rust shares
 * the Scene read_str gives (ribbon.rs:575), so per element the file's GlobalId text, name,
 * storey and material; the storeys and grids as read; no densities (read_str sets none); GEOR
 * when the georeference is not the default (its serde_json text). `mt` is always initialized
 * (free it with cc_viewfile_meta_free); it borrows from `s`. 0 ok, -1 out of memory. */
int cc_ifc_scene_meta(const cc_ifc_scene *s, cc_viewfile_meta *mt);

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
/* 3D1: the same as BMSH version 3 (scene_bmsh.h: each shape once, a placement per element). */
int cc_ifc_scene_bmsh3(const cc_ifc_scene *s, uint32_t rev, Buf *out, cc_error *err);
#endif
