/* The graphic style of drawings on paper (L2), byte for byte as bimble-core's style.rs, pens.rs and
 * hatches.rs, and the pens and looks of plan.rs (role_pen, hatches_for, seen_fill_for,
 * hatch_type_lines, cut_look, seen_look). Built-in defaults only: reading a style file is a later
 * ticket. Lengths are f32 as in Rust (mm on paper, or screen pixels for the family patterns);
 * pens and hatches on paper are f64 (paper.h), widened from them.
 *
 * Strings and arrays of a style live in the arena given to cc_style_default. */
#ifndef CCORE_DRAW_STYLE_H
#define CCORE_DRAW_STYLE_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "buf.h"
#include "model.h" /* cc_category, cc_str */
#include "paper.h" /* cc_rgb, cc_pen, cc_hatch, cc_sheet_font */

/* style::Lines: a family of parallel lines in screen pixels. */
typedef struct {
    float angle_deg, spacing_px, width;
    int has_dash;
    float dash_px;
    float dash_on; /* 1 when not given */
} cc_style_lines;

typedef struct {
    float cell_px, density, strength;
} cc_stipple;

/* style::Fill: a material family's colour and pattern. */
typedef struct {
    cc_rgb color;
    cc_style_lines lines[2];
    int nlines; /* 0..2 */
    int has_stipple;
    cc_stipple stipple;
    int has_ink;
    cc_rgb ink;
} cc_fill;

/* pens::PenTable. model[p][i]: pen p + 1 at scales[i]. */
#define CC_PENS 16
#define CC_PEN_MAX_SCALES 24
typedef struct {
    uint32_t scales[CC_PEN_MAX_SCALES];
    size_t nscales;
    float model[CC_PENS][CC_PEN_MAX_SCALES];
    float perspective[CC_PENS];
    float annotation[CC_PENS];
} cc_pen_table;

#define CC_MAX_DASH 16
/* pens::LineType. */
typedef struct {
    cc_str name;
    cc_rgb color;
    float dash_mm[CC_MAX_DASH];
    size_t ndash;
    uint8_t pen; /* 1..16 */
} cc_line_type;

/* pens::TextType. */
typedef struct {
    cc_str name, font;
    float size_mm;
    cc_rgb color;
} cc_text_type;

/* pens::DimUnit, in its declaration order. */
typedef enum { CC_DIM_M, CC_DIM_CM, CC_DIM_MM } cc_dim_unit;

/* pens::DimType. */
typedef struct {
    cc_str name;
    cc_dim_unit unit;
    uint8_t decimals;
    int show_unit;
    cc_rgb color;
    float text_mm;
    cc_str line_type;
} cc_dim_type;

/* hatches::HatchLine. */
typedef struct {
    float angle_deg, spacing_mm, offset_mm;
    float dash_mm[CC_MAX_DASH];
    size_t ndash;
} cc_hatch_line;

/* hatches::HatchType. */
typedef struct {
    cc_str name;
    int has_fill;
    cc_rgb fill;
    cc_hatch_line *lines;
    size_t nlines;
    cc_rgb color;
    uint8_t pen;
} cc_hatch_type;

/* hatches::MaterialHatch: n == 0 for an absent name. */
typedef struct {
    cc_category category;
    cc_str cut, seen;
} cc_material_hatch;

/* The kinds of line on a drawing (plan::Role), in pens::LineRoles order. */
typedef enum {
    CC_ROLE_CUT,
    CC_ROLE_SEEN,
    CC_ROLE_ABOVE,
    CC_ROLE_HATCH,
    CC_ROLE_AXIS,
    CC_ROLE_DIMENSION,
    CC_ROLE_FRAME,
    CC_ROLE_COUNT
} cc_role;
const char *cc_role_name(cc_role r); /* {:?}: "Cut", "Seen"… */

/* style::PaperStyle. */
typedef struct {
    float cut_pen_mm, projection_pen_mm, above_pen_mm, above_dash_mm[2], hatch_pen_mm, hatch_mm_per_px,
        frame_pen_mm, text_mm, annotation_pen_mm, axis_pen_mm, axis_dash_mm[4], seen_lighten;
    cc_pen_table pens;
    cc_line_type *line_types;
    size_t nline_types;
    cc_str lines[CC_ROLE_COUNT]; /* the line type of each role, by name */
    cc_text_type *text_types;
    size_t ntext_types;
    cc_dim_type *dim_types;
    size_t ndim_types;
    cc_hatch_type *hatch_types;
    size_t nhatch_types;
    cc_material_hatch *material_hatches;
    size_t nmaterial_hatches;
    cc_sheet_font font;
} cc_paper_style;

/* style::GraphicStyle. material_families (material name -> family) is sorted by name bytes. */
typedef struct {
    cc_rgb background, edges, selection, hover, dimensions, reference, reference_edges, only_here,
        only_in_reference;
    float hatch_strength, hatch_darken, ambient, cut_shade;
    cc_fill families[CC_CAT_COUNT];
    cc_str *material_family_names;
    cc_category *material_family_cats;
    size_t nmaterial_families;
    cc_paper_style paper;
} cc_style;

/* GraphicStyle::default(). 0 ok, -1 out of memory. */
int cc_style_default(cc_style *s, Arena *a);

/* GraphicStyle::to_json(): pretty JSON, every field, a newline at the end. 0 ok, -1 a value
 * that cannot be written (out of memory, a non-finite or out-of-range number). */
int cc_style_json(const cc_style *s, Buf *b);

/* PenTable::model_mm: pen 1..16 at 1:scale, the column of the nearest listed scale in ratio.
 * 1 and *mm set, or 0 (no such pen). */
int cc_pen_model_mm(const cc_pen_table *t, int pen, uint32_t scale, float *mm);
int cc_pen_annotation_mm(const cc_pen_table *t, int pen, float *mm);

/* PaperStyle::line_type, hatch_type, material_hatch: NULL when there is none. */
const cc_line_type *cc_style_line_type(const cc_paper_style *p, const char *name, size_t n);
const cc_hatch_type *cc_style_hatch_type(const cc_paper_style *p, const char *name, size_t n);
const cc_hatch_type *cc_style_material_hatch(const cc_paper_style *p, cc_category c, int cut);

/* plan::role_pen: the pen of a kind of line at 1:scale. Its dashes go to `a`. 0, or -1 out of
 * memory. */
int cc_role_pen(const cc_style *s, cc_role r, uint32_t scale, Arena *a, cc_pen *out);

/* Fill and hatches of an area (plan::cut_look, seen_look). */
typedef struct {
    int has_fill;
    cc_rgb fill;
    cc_hatch *hatches;
    size_t nhatches;
} cc_area_look;
/* plan::cut_look: a family where it is cut; seen_look: seen from above. Hatches go to `a`.
 * 0, or -1 out of memory. */
int cc_cut_look(const cc_style *s, cc_category c, uint32_t scale, Arena *a, cc_area_look *out);
int cc_seen_look(const cc_style *s, cc_category c, uint32_t scale, Arena *a, cc_area_look *out);

/* plan::darken and lighten (f32, rounded half away from zero). */
cc_rgb cc_rgb_darken(cc_rgb c, float k);
cc_rgb cc_rgb_lighten(cc_rgb c, float k);
/* Rgb::to_hex: "#rrggbb". */
void cc_rgb_hex(Buf *b, cc_rgb c);

/* An f32 as serde_json writes it (its shortest digits), returned as the f64 whose own shortest
 * digits are the same (so fmt_f64_json and fmt_f64_fixed(-1) print it as Rust prints the f32).
 * 0 ok, -1 for a value it does not handle: not finite, or beyond 1e±22 (styles never are). */
int cc_f32_shortest(float v, double *out);

/* MaterialCategory::label: "Concrete"… */
const char *cc_category_label(cc_category c);
#endif
