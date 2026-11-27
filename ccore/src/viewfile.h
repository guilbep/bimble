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
/* LINKF: the largest .bimbleview an open or a link reads: 8 GiB on a 64-bit build (Rust's
 * file.rs MAX_FILE_BYTES native), 1 GiB in a 32-bit one (its wasm cap). Each section stays at most
 * 1 GiB and a gzipped file inflates to at most 64 times its size (viewfile.c). */
#define CC_VIEWFILE_MAX_FILE_BYTES (sizeof(size_t) > 4 ? (size_t)((uint64_t)8 << 30) : (size_t)1 << 30)
/* Peak memory of a read per file byte: the bytes, the arena's copy (inflated when gzipped: up to
 * 64 times, bounded by the reader), the element and shape tables (ADR 0079). */
#define CC_VIEWFILE_READ_PEAK_PER_BYTE 4

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

typedef struct { /* SIZE3: where a mesh's codec bytes are in a mapped view file (cc_viewfile_read_mapped), its box */
    uint64_t at;
    uint32_t len;
    float lo[3], hi[3];
} cc_viewfile_ref;

typedef struct {
    cc_str global_id, name, class_name, storey, material, family;
    cc_category category; /* from the family label; Other when unknown */
    int32_t shape;        /* index in shapes, or -1: own mesh in `mesh` */
    cc_mesh mesh;         /* own mesh, scene coordinates (shape < 0) */
    cc_placement32 to_scene; /* shape >= 0 */
    cc_viewfile_ref ref;     /* SIZE3: a mapped read's own mesh (shape < 0) */
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
    int has_source;
    cc_str source;   /* LINKF: SRCE's JSON text (a link cache: the IFC it was read from), when has_source */
    /* M1LINKS: a section PROP (a link cache: the elements' property sets, ifc_psets.h), not kept in
     * memory: where its bytes are in the raw file (read lazily: pf->read_range), when has_psets. Its
     * head was checked (entries = elements, ends rising, within the section). */
    int has_psets;
    uint64_t psets_off, psets_len;
    int psets_packed; /* SIZE2: the section is PRPZ (ifc_psets.h cc_psets_packed_entry), not PROP; its head checked */
    /* LNK3D (ADR 0078 amendment 3): a section HLOD (a link cache's baked proxies, BPRX: hlod.h), kept on
     * disk as PROP (only HLOD and PROP sections after it): its bytes' place in the raw file, when has_hlod.
     * Not checked here: the session checks the bytes it reads back (cc_hlod_bprx_check). */
    int has_hlod;
    uint64_t hlod_off, hlod_len;
    cc_mesh *shapes; /* shared shapes, own coordinates (malloc) */
    size_t nshapes;
    cc_view_element *elements; /* malloc */
    size_t nelements;
    /* SIZE3: read in place (cc_viewfile_read_mapped): `base` is the caller's bytes (a mapped cache), which
     * must outlive this view file; strings point into them; the meshes hold their counts only (arrays
     * NULL) until cc_viewfile_materialize decodes them; shape_refs[k] is shape k's bytes. */
    int lazy;
    float box[6]; /* SIZE3: a lazy read's scene box (lo, hi; scene coordinates: placed shapes' boxes' corners placed) */
    const uint8_t *base;
    size_t base_len; /* SIZE3: the bytes at `base` (the whole mapped file: a later section's byte range is base + off) */
    cc_viewfile_ref *shape_refs; /* malloc, lazy only */
    Arena *lazy_scratch;         /* during the read only */
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
    /* QTY1 (ADR 0080): per scene element its model element (properties, PredefinedType, openings),
     * set by cc_viewfile_meta_from_model only; NULL for an IFC read as it is or a view file. */
    const cc_element *const *elements;
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

/* LINKF (ADR 0079, DEVIATIONS 37): appends a section "SRCE" holding `json` (the source of a link
 * cache: the IFC's size, time and a hash of its ends) to the raw view file in `out`. A reader
 * that does not know SRCE skips it (Rust's and this one skip a later addition); this reader keeps
 * it in `source` (checked to be UTF-8 JSON). 0 ok, -1 out of memory. */
int cc_viewfile_append_source(Buf *out, const char *json, size_t n);
/* M1LINKS: appends a section PROP holding the property sets table `t` (ifc_psets.h) to the raw
 * view file in `out`. 0 ok, -1 out of memory. */
int cc_viewfile_append_psets(Buf *out, const char *t, size_t n);
/* SIZE2: appends a section PRPZ holding the packed table `t` (ifc_psets.h cc_psets_pack), read lazily as PROP. */
int cc_viewfile_append_psets_packed(Buf *out, const char *t, size_t n);
/* LNK3D: appends a section HLOD holding BPRX bytes `t` (before PROP: both stay on disk when read). 0 ok, -1 out of memory.
 * SIZE2: the bytes may be one gzip member of BPRX (cache format 5); its reader inflates them, bounded. */
int cc_viewfile_append_hlod(Buf *out, const char *t, size_t n);

/* Reads a view file. Always initializes `out` (free it with cc_viewfile_free either way).
 * 0 ok, -1 with err->msg. */
int cc_viewfile_read(const uint8_t *p, size_t n, cc_viewfile *out, cc_error *err);
void cc_viewfile_free(cc_viewfile *v);

/* SIZE3 (ADR 0079 amendment): a raw view file read in place, nothing copied: `p` (a mapped cache) must
 * stay valid and unchanged until cc_viewfile_free. Every mesh is decoded once to check it (into a
 * scratch arena, freed) and only its counts and place are kept: the arrays are NULL ("lazy"). A gzipped
 * file is read as cc_viewfile_read does. 0 ok, -1 with err->msg. */
int cc_viewfile_read_mapped(const uint8_t *p, size_t n, cc_viewfile *out, cc_error *err);
/* A lazy view file's meshes decoded into `a` (the caller frees it after cc_viewfile_dematerialize);
 * `*bytes` their size. 0 ok (nothing to do when not lazy), -1 (out of memory, or the bytes no longer
 * decode to the counts read: the view file is left without arrays). */
int cc_viewfile_materialize(cc_viewfile *v, Arena *a, uint64_t *bytes);
/* The arrays forgotten (NULL again), the counts kept. */
void cc_viewfile_dematerialize(cc_viewfile *v);
/* A lazy view file's mesh bytes (BMG1): block b < nshapes is shape b, else element b - nshapes's own
 * mesh. 0 ok, -1 not lazy or not an own mesh. */
int cc_viewfile_block(const cc_viewfile *v, size_t b, const uint8_t **p, size_t *n);

/* The element's mesh as stored: its own, or its shared shape in the shape's coordinates. */
const cc_mesh *cc_viewfile_local_mesh(const cc_viewfile *v, size_t k);
#endif
