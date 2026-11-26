/* Quantities and schedules (ADR 0029, ADR 0031): the C twin of bimble_drawing::schedule
 * (crates/bimble-drawing/src/schedule.rs: measure, density, schedule, to_csv, table_json) and
 * of the built-in definitions of bimble_core::schedule (default_schedules, effective).
 *
 * L3, pure: no files, clock, threads or globals. Works on a render scene and its META (what
 * Rust's `Scene` holds beside the meshes): per element its name, storey and material, the
 * storeys lowest first, the grid axes, the model's densities. `cc_viewfile_meta_from_model`
 * gives it for a `.bimble` model, `cc_viewfile_meta_from_file` for a `.bimbleview`; an opened
 * IFC has the same fields (cc_ifc_scene). Every measure comes from the element's world mesh, as
 * in Rust (no generator formulas: Rust has none; an opening is in the volume only when the mesh
 * is cut, which the C core does not do yet, see scene.h).
 *
 * The cell texts, the sums and their order are Rust's, so the CSV and JSON are byte-equal
 * (golden/schedule/, golden/README.md "schedule/"). Known gap: the natural sort lowers case
 * with a table of the Latin, Greek and Cyrillic letters (char::to_lowercase is all Unicode);
 * names with other upper-case letters may sort differently from Rust. */
#ifndef CCORE_QUANTITY_H
#define CCORE_QUANTITY_H
#include <stddef.h>
#include "arena.h"
#include "buf.h"
#include "model.h"
#include "scene.h"
#include "viewfile.h"

/* schedule.rs Measures: mm, m² and m³. */
typedef struct {
    double length, width, height, area_m2, volume_m3;
    double centre[2]; /* plan centre, world mm */
    cc_str section;   /* "30/30", "Ø30", "GL24h 24x46", or empty */
} cc_measures;

/* measure(scene, element k). 1 measured; 0 the mesh has no positions; -1 out of memory. The
 * section text and the placed copy of a shared mesh go in `a`. */
int cc_measure(const cc_scene *s, const cc_viewfile_meta *mt, size_t k, Arena *a, cc_measures *out);

/* density(scene, material): the model's own value, else its library grade's. 1 known, 0 not. */
int cc_density(const cc_viewfile_meta *mt, cc_str material, double *out);

/* ---- tables ---- */

typedef enum { CC_CELL_EMPTY, CC_CELL_TEXT, CC_CELL_NUM } cc_cell_kind;

typedef struct { /* Cell */
    cc_cell_kind kind;
    cc_str text; /* CC_CELL_TEXT */
    double v;    /* CC_CELL_NUM, written with `dec` decimals */
    int dec;
} cc_cell;

typedef struct { /* Row */
    size_t element; /* scene index */
    cc_cell *cells; /* one per field */
} cc_sched_row;

typedef struct { /* Group */
    cc_str level; /* empty when not grouped */
    cc_sched_row *rows;
    size_t nrows;
    cc_cell *subtotal; /* one per field, or NULL (not grouped) */
    /* QTY1 (ADR 0080), a list grouped by material: the group's material; `head` 1 for the
     * material's own line (no rows, its subtotal: the material's sums) before its level groups. */
    cc_str material;
    int head;
} cc_sched_group;

typedef struct { /* Table; all memory in `arena` */
    Arena arena;
    cc_str name;
    uint8_t *fields; /* cc_sched_field */
    size_t nfields;
    cc_sched_group *groups;
    size_t ngroups;
    cc_cell *total; /* one per field, or NULL */
    size_t count;
    /* QTY1 (ADR 0080): the property columns, after the fields (their `fields` entry is
     * CC_SCHED_PROP); grouped by material; only the subtotal and total lines written. */
    const cc_str *props;
    size_t nprops;
    int by_material, summary;
} cc_sched_table;
#define CC_SCHED_PROP 255 /* QTY1: a property column in cc_sched_table.fields */

/* schedule(scene, def). Always initializes `t` (free it with cc_sched_table_free either way).
 * 0 ok, -1 with `err` (out of memory). */
int cc_schedule_table(cc_sched_table *t, const cc_scene *s, const cc_viewfile_meta *mt, const cc_schedule *def,
                      cc_error *err);
void cc_sched_table_free(cc_sched_table *t);

/* Cell::text, appended. */
void cc_cell_text(Buf *b, const cc_cell *c);
/* to_csv(table), appended. */
void cc_schedule_csv(Buf *out, const cc_sched_table *t);
/* QTY1: the same with the header line given (`nh` == the table's columns; else the titles). */
void cc_schedule_csv_headers(Buf *out, const cc_sched_table *t, const cc_str *headers, size_t nh);
/* QTY1: the title of column `k` ("Length (m)", or a property column's key). */
cc_str cc_sched_column_title(const cc_sched_table *t, size_t k);
/* QTY1: table_json's object for the page, rows `offset` .. `offset + limit` (in the table's order)
 * and the groups they are in (a material's line with its first rows); `offset`, `row_count` (all
 * rows), `material` and `head` keys added, keys sorted. */
void cc_schedule_json_page(Buf *out, const cc_sched_table *t, const cc_viewfile_meta *mt, size_t offset, size_t limit);
/* table_json(table, scene, max) as serde_json::to_string writes it (keys sorted), appended. */
void cc_schedule_json(Buf *out, const cc_sched_table *t, const cc_viewfile_meta *mt, size_t max);

/* format!("{:.p$}", x): exact digits, rounded half to even, "-" when the sign bit is set
 * ("-0.000"), "NaN", "inf", "-inf". 0 <= p <= 40. */
void cc_fmt_fixed(Buf *b, double x, int p);

/* ---- definitions ---- */

/* default_schedules(), in their order. */
const cc_schedule *cc_schedule_builtins(size_t *n);
/* effective(stored): the built-in ones (each replaced by a stored one of the same name, any
 * ASCII case), then the stored ones that are not built-in, in their order. Writes at most
 * `cap` pointers into `out` (cap >= nstored + 10 holds all) and returns the full count. */
size_t cc_schedules_effective(const cc_schedule *stored, size_t nstored, const cc_schedule **out, size_t cap);
/* Field::title: "Length (m)", "Area (m²)"… */
const char *cc_sched_field_title(int f);
#endif
