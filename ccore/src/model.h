/* The building model, mirroring bimble-core's model.rs (docs/research/c-core-spec.md §3). All
 * memory of a model lives in its arena. Arrays are sorted by id (storeys, materials,
 * elements) or by key bytes (properties, project), like Rust's BTreeMaps. Units: mm. */
#ifndef CCORE_MODEL_H
#define CCORE_MODEL_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "buf.h"

/* IFC GlobalId text: 22 chars of 0-9A-Za-z_$, first in 0123. Order = memcmp (Rust's Ord). */
typedef struct {
    char c[22];
} cc_gid;

/* UTF-8 bytes (arena), not NUL-terminated in general. */
typedef struct {
    const char *p;
    size_t n;
} cc_str;

typedef struct {
    cc_str key, value;
} cc_kv;

typedef enum {
    CC_IFC_COLUMN,
    CC_IFC_BEAM,
    CC_IFC_SLAB,
    CC_IFC_WALL,
    CC_IFC_FOOTING,
    CC_IFC_PROXY, /* IfcBuildingElementProxy */
    CC_IFC_ELEMENT_ASSEMBLY, /* IfcElementAssembly: no generator, its shape is its parts (ADR 0070) */
    CC_IFC_VIRTUAL_ELEMENT,  /* IfcVirtualElement: a reference plane, no generator (WAVE1B, refplane.h) */
    CC_IFC_CLASS_COUNT
} cc_ifc_class;

typedef enum { /* MaterialCategory, in MaterialCategory::ALL order */
    CC_CAT_CONCRETE,
    CC_CAT_STEEL,
    CC_CAT_TIMBER,
    CC_CAT_MASONRY,
    CC_CAT_INSULATION,
    CC_CAT_EARTH,
    CC_CAT_GLASS,
    CC_CAT_OTHER,
    CC_CAT_COUNT
} cc_category;

/* A level (ADR 0056). `not_storey` 0 (the default, so every zeroed storey is one): a storey,
 * IfcBuildingStorey, elements belong to it. 1: a datum level (top of parapet, bottom of
 * foundation): views snap and dimension to it, no element belongs to it, the IFC export leaves it
 * out. File key "is_storey": false, left out when the level is a storey (existing files keep
 * their bytes). */
typedef struct {
    cc_gid id;
    cc_str name;
    double elevation;
    int not_storey;
} cc_storey;

typedef struct { /* library.rs MaterialProps: every value optional */
    int has_density, has_e, has_strength;
    double density_kg_m3, e_mpa, strength_mpa;
    cc_str source; /* n == 0: absent */
} cc_material_props;

typedef struct {
    cc_gid id;
    cc_str name;
    cc_category category;
    cc_material_props props;
} cc_material;

typedef struct {
    double origin[3], z_axis[3], x_axis[3];
} cc_placement;

typedef struct {
    double (*pts)[2];
    size_t n;
} cc_loop;

typedef enum {
    CC_GEN_RECT_COLUMN,
    CC_GEN_RECT_BEAM,
    CC_GEN_SLAB,
    CC_GEN_WALL,
    CC_GEN_PAD_FOOTING,
    CC_GEN_EXTRUDED_POLYGON,
    CC_GEN_MESH,
    CC_GEN_COUNT,
    /* No shape of its own: the generator of an IfcElementAssembly (ADR 0070), and only of one.
     * Past CC_GEN_COUNT so no table of kinds (file tags, verbs) knows it; the file leaves the
     * element's "generator" out. cc_gen_kind_name gives "none". */
    CC_GEN_NONE = CC_GEN_COUNT
} cc_gen_kind;

typedef struct {
    cc_gen_kind kind;
    union {
        struct { double width, depth, height; } col;       /* rect_column and pad_footing */
        struct { double width, height, length; } beam;     /* rect_beam */
        struct { cc_loop outline; double thickness; } slab;
        struct { double length, thickness, height; } wall;
        struct { cc_loop outline; cc_loop *voids; size_t nvoids; double height; } poly;
        struct { double (*positions)[3]; size_t npositions; uint32_t *indices; size_t nindices; } mesh;
    } u;
} cc_generator;

#define CC_MAX_OPENINGS 10000 /* CONC1: openings of one element in a command (set_openings, add_element) */
typedef struct { /* model.rs Opening: a solid cut out of its element (element-local mm) */
    int has_id; /* id: Option<GlobalId> */
    cc_gid id;
    cc_placement placement;
    cc_generator generator;
} cc_opening;

typedef struct {
    cc_gid id;
    cc_ifc_class cls;
    cc_str name;
    cc_gid storey, material;
    cc_placement placement;
    cc_generator generator;
    cc_opening *openings; /* in file order; geometry is not cut by the C core (data only) */
    size_t nopenings;
    cc_kv *properties; /* sorted by key bytes */
    size_t nproperties;
    int has_ifc_entity;
    cc_str ifc_entity;
} cc_element;

#include "view.h"

/* An IFC relationship as a model object (ADR 0070, relation.h): its own GlobalId, the IFC class
 * name of the relationship as text (checked against relation.h's table), one relating element,
 * the related elements (sorted, no duplicates), the realizing elements of
 * IfcRelConnectsWithRealizingElements (sorted, no duplicates, else empty) and properties
 * ("Pset_Name/Property" -> text, sorted by key bytes; disassembly data lives here). */
typedef struct {
    cc_gid id;
    cc_str kind;
    cc_gid relating;
    cc_gid *related;
    size_t nrelated;
    cc_gid *realizing;
    size_t nrealizing;
    cc_kv *properties;
    size_t nproperties;
} cc_relation;

/* A grid (ADR 0056, grid.h): its own GlobalId, a name, straight axes in plan, world mm, each
 * with its own GlobalId and a tag ("A", "1"), in the order given. */
typedef struct {
    cc_gid id;
    cc_str tag;
    double start[2], end[2];
} cc_grid_axis;

typedef struct {
    cc_gid id;
    cc_str name;
    cc_grid_axis *axes;
    size_t naxes;
} cc_grid;

/* Where the model is on Earth and its project north (bimble_core::georef::Georef, ADR 0045;
 * georef.h). All zero, has_crs 0: unknown (the default), left out of the file. */
typedef struct {
    double rotation_deg;     /* project north: degrees counter-clockwise from true (grid) north */
    int has_crs;             /* crs: Option<String> */
    cc_str crs;              /* the survey system, e.g. "EPSG:2056" (LV95) */
    double east_m, north_m, height_m; /* what the survey system adds to world coordinates (m) */
    double map_rotation_deg; /* IfcMapConversion: the turn of the model's x axis in the survey system */
    /* SETOUT (ADR 0075, DEVIATIONS 32): the survey point, a C addition: a benchmark of the site in
     * world mm, its survey coordinates those of the georeference. File key "survey_point_mm"
     * [x, y, z] after Rust's six, left out when unset (has_survey_point 0). */
    int has_survey_point;
    double survey_point_mm[3];
} cc_georef;

/* ---- LNK1: a linked model the file names (GitHub issue #69, DEVIATIONS 29): file key "links",
 * the C core's own section, left out when empty (Rust ignores it: its links are not saved). Not
 * model data: the session keeps the live links (session/links.h) and writes this list when it
 * saves; no Command changes it. `path` as linked (absolute on the desktop), `relative` the same
 * file from the saved file's folder ("" when none), `hidden` hidden in the session. ---- */
#define CC_MODEL_LINKS_MAX 64
/* M1LINKS (ADR 0081): a note on an element of the link: "notes" [{"at": [x, y, z] (the anchor, the
 * link's own world mm), "element": its GlobalId, "text"}], left out when empty; at most
 * CC_MODEL_NOTES_MAX per link, a text at most CC_MODEL_NOTE_TEXT bytes (UTF-8, no NUL). */
#define CC_MODEL_NOTES_MAX 256
#define CC_MODEL_NOTE_TEXT 2000
typedef struct {
    double at[3];
    cc_gid element;
    cc_str text;
} cc_model_note;
typedef struct {
    cc_str name, path, relative;
    int hidden;
    /* LINKF (session/links.h, ADR 0079), each left out at its default: "origin" one of "origin",
     * "base_point", "survey_point" (default "file": 0 here), "move_mm" [x, y, z] (zeros),
     * "rotation_deg" (0; |x| < 1e6), "pinned": false (default true), "colour" 0..11 (none: left out). */
    int origin;           /* 0 file, 1 origin, 2 base_point, 3 survey_point */
    double move_mm[3];    /* each |x| < 1e9 */
    double rotation_deg;
    int unpinned;
    int has_colour, colour; /* 0..11 when has_colour (zero-initialized: none) */
    cc_model_note *notes;   /* M1LINKS: in the order made */
    size_t nnotes;
} cc_model_link;

typedef struct {
    Arena arena;
    cc_gid project_id;
    cc_str project_name;
    cc_storey *storeys;
    size_t nstoreys;
    cc_material *materials;
    size_t nmaterials;
    cc_element *elements;
    size_t nelements;
    /* Room in `elements` (0: exactly nelements, as a loader leaves it). Only command.c's insert
     * sets it; code that replaces `elements` leaves it 0 (cc_model_init) or sets it back to 0. */
    size_t cap_elements;
    cc_kv *project; /* project information, sorted by key bytes */
    size_t nproject;
    cc_view *views; /* stored views (changed defaults and others), sorted by id */
    size_t nviews;
    cc_sheet *sheets; /* sorted by id */
    size_t nsheets;
    cc_schedule *schedules; /* the model's schedules, in their order */
    size_t nschedules;
    cc_relation *relations; /* sorted by id; key "relations", left out of the file when empty */
    size_t nrelations;
    cc_grid *grids; /* sorted by id; key "grids", left out of the file when empty (ADR 0056) */
    size_t ngrids;
    cc_georef georef; /* key "georef" after "schedules", left out when the default (ADR 0045) */
    cc_model_link *links; /* LNK1: key "links" after "grids", in link order, left out when empty */
    size_t nlinks;
} cc_model;

/* An error with a message (UTF-8, NUL-terminated, truncated to fit). `offset` is the input
 * byte offset for parse errors, CC_NO_OFFSET for validation errors. */
#define CC_NO_OFFSET ((size_t)-1)
typedef struct {
    size_t offset;
    char msg[512];
} cc_error;

extern const char cc_gid_alphabet[65];
int cc_gid_valid(const char *s, size_t n); /* shape only: length 22, alphabet, first in 0123 */
int cc_gid_cmp(const cc_gid *a, const cc_gid *b);
const char *cc_ifc_class_name(cc_ifc_class c);   /* "IfcColumn"… */
const char *cc_category_name(cc_category c);     /* serde name: "concrete"… */
const char *cc_gen_kind_name(cc_gen_kind k);     /* serde tag: "rect_column"…, "none" for CC_GEN_NONE */

void cc_model_init(cc_model *m); /* empty model, arena ready */
void cc_model_free(cc_model *m);

/* Binary search by id; NULL when absent. */
const cc_storey *cc_model_storey(const cc_model *m, const cc_gid *id);
const cc_material *cc_model_material(const cc_model *m, const cc_gid *id);
const cc_element *cc_model_element(const cc_model *m, const cc_gid *id);

/* Model::validate: same rules and, where feasible, the same texts. 0 ok, -1 with err. */
int cc_model_validate(const cc_model *m, cc_error *err);
/* The same, with the map keys the file gave (Rust checks key == id); NULL means the ids. */
int cc_model_validate_keys(const cc_model *m, const cc_gid *storey_keys, const cc_gid *material_keys,
                           const cc_gid *element_keys, const cc_gid *view_keys, const cc_gid *sheet_keys,
                           const cc_gid *relation_keys, const cc_gid *grid_keys, cc_error *err);
/* version::canonical_model: every -0.0 becomes 0.0. */
void cc_model_canonical(cc_model *m);

/* Text helpers for error messages, as Rust prints them. */
void cc_buf_display_f64(Buf *b, double x);              /* format!("{}", x): "300", "-0.5", "NaN" */
void cc_buf_debug_str(Buf *b, const char *s, size_t n); /* format!("{:?}", s), common escapes */
void cc_buf_gid(Buf *b, const cc_gid *g);
/* CONC1 (ADR 0077, verbs_concrete.h). */
#define CC_SLEEVE_SIDES 32     /* a sleeve's polygon (area error 0.64 % against the circle) */
#define CC_PREDEFINED_KEY "Bimble/PredefinedType" /* the element property the IFC export writes as PredefinedType */
/* An opening made by opening_add with shape "round": an extruded_polygon of CC_SLEEVE_SIDES corners
 * at one radius around the opening's origin. 1 with the radius in *r (may be NULL), else 0. */
int cc_opening_is_sleeve(const cc_opening *o, double *r);
/* WAVE1B (B1): the exact cylinder against the polygon drawn: for each sleeve of `e` (a wall or slab
 * generator host), (polygon area - pi r^2) x the host's thickness, in m3 (<= 0); 0 without one. The
 * radius is the opening's own parameter (its polygon's circumradius), not read from the mesh. */
double cc_sleeve_volume_fix_m3(const cc_element *e);
/* The element's PredefinedType property (CC_PREDEFINED_KEY), NULL when it has none. */
const cc_str *cc_element_predefined(const cc_element *e);
#endif
