/* Views, sheets and schedules of a model (crates/bimble-core/src/{view,sheet,schedule}.rs):
 * the structs, their serde form (JSON text and CBOR, field order and omissions as Rust), their
 * validation (same texts) and deep copies. Included by model.h; the model holds them sorted by
 * id (views, sheets) or in their order (schedules). */
#ifndef CCORE_VIEW_H
#define CCORE_VIEW_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "buf.h"

/* ---- views (view.rs) ---- */

typedef enum { CC_VIEW_THREE_D, CC_VIEW_PLAN, CC_VIEW_SECTION, CC_VIEW_KIND_COUNT } cc_view_kind;
typedef enum { CC_LOOK_DOWN, CC_LOOK_UP } cc_look;
typedef enum { CC_FACING_NORTH, CC_FACING_SOUTH, CC_FACING_EAST, CC_FACING_WEST } cc_facing;
typedef enum { CC_LEVEL_ASSOCIATED, CC_LEVEL_ABOVE, CC_LEVEL_BELOW, CC_LEVEL_UNLIMITED } cc_level_ref;
typedef enum { CC_AXIS_ALIGNED, CC_AXIS_HORIZONTAL, CC_AXIS_VERTICAL } cc_dim_axis;
typedef enum { CC_ANN_DIMENSION, CC_ANN_TEXT } cc_ann_type;

typedef struct {
    cc_str *v;
    size_t n;
} cc_strs;

typedef struct { /* Visibility: what is hidden in a view */
    cc_strs hidden_families, hidden_classes, hidden_storeys, hidden_links, hidden_materials,
        hidden_elements, isolated_elements;
} cc_visibility;

typedef struct {
    double at[3];
    int has_element;
    cc_gid element;
} cc_anchor;

typedef struct { /* Annotation: a dimension or a text */
    cc_ann_type type;
    cc_gid id;
    cc_anchor a, b;        /* dimension */
    double offset_mm;      /* dimension */
    cc_dim_axis axis;      /* dimension */
    cc_str text;           /* text */
    double at[3];          /* text */
    double size_mm;        /* text */
    double angle_deg;      /* text */
    int has_kind;          /* both: kind: Option<String> */
    cc_str kind;
} cc_annotation;

typedef struct { /* Reference: a PDF or DXF plan placed in a 2D view */
    cc_gid id;
    cc_str name, format;
    int has_source;
    cc_str source;
    double offset_mm[2];
    double rotation_deg;
    int visible;
    cc_loop *polylines; /* polyline points as cc_loop (any count) */
    size_t npolylines;
} cc_reference;

typedef struct {
    cc_level_ref level;
    double offset_mm;
} cc_range_plane;

typedef struct {
    cc_gid id;
    cc_str name;
    cc_view_kind kind;
    /* plan */
    cc_str storey;
    cc_look look;
    cc_range_plane range[4]; /* top, cut, bottom, depth */
    /* section */
    double origin[2];
    cc_facing facing;
    double depth_mm;
    cc_visibility visibility;
    cc_reference *references;
    size_t nreferences;
    cc_annotation *annotations;
    size_t nannotations;
    int true_north;
    uint32_t scale; /* 100 when not said */
    int has_crop;
    double crop_min[2], crop_max[2];
} cc_view;

enum { CC_RANGE_TOP, CC_RANGE_CUT, CC_RANGE_BOTTOM, CC_RANGE_DEPTH };

/* ---- sheets (sheet.rs) ---- */

typedef struct {
    cc_gid id, view;
    uint32_t scale;
    double at_mm[2];
} cc_placed;

typedef struct {
    cc_gid id;
    cc_str text;
    double at_mm[2];
    double size_mm, angle_deg;
    int has_kind;
    cc_str kind;
} cc_sheet_text;

typedef struct {
    cc_gid id;
    cc_str number, name, paper;
    int portrait;
    cc_kv *fields; /* sorted by key bytes */
    size_t nfields;
    cc_placed *placed;
    size_t nplaced;
    cc_sheet_text *texts;
    size_t ntexts;
} cc_sheet;

/* ---- schedules (schedule.rs) ---- */

typedef enum {
    CC_FIELD_LEVEL,
    CC_FIELD_LOCATION,
    CC_FIELD_NAME,
    CC_FIELD_CLASS,
    CC_FIELD_MATERIAL,
    CC_FIELD_SECTION,
    CC_FIELD_LENGTH,
    CC_FIELD_WIDTH,
    CC_FIELD_HEIGHT,
    CC_FIELD_AREA,
    CC_FIELD_VOLUME,
    CC_FIELD_MASS,
    CC_FIELD_GLOBAL_ID,
    CC_FIELD_COUNT
} cc_sched_field;

typedef struct { /* ScheduleDef */
    cc_str name;
    cc_strs classes;
    int has_family;
    int family; /* cc_category */
    uint8_t *fields; /* cc_sched_field */
    size_t nfields;
    int group_by_level, totals;
} cc_schedule;

const char *cc_view_kind_name(int k);   /* "three_d", "plan", "section" */
const char *cc_look_name(int k);        /* "down", "up" */
const char *cc_facing_name(int k);      /* "north"… */
const char *cc_level_ref_name(int k);   /* "associated"… */
const char *cc_sched_field_name(int k); /* "level"… "global_id" */

#endif
