/* IFC import, the C twin of bimble_ifc::import::import_str (crates/bimble-ifc/src/import.rs and
 * the parts of read.rs it uses). Covered as in Rust: units (length and plane angle), project id,
 * storeys, containment and aggregation, materials, property and quantity sets as text; elements
 * of IfcColumn/Beam/Slab/Wall/Footing (and any other product with a shape, as
 * IfcBuildingElementProxy) whose body is an IfcExtrudedAreaSolid become rectangle generators,
 * slabs or extruded polygons (profiles: rectangle, circle, hollow circle and rectangle, I-shape
 * with root fillets, derived, arbitrary with voids; curves: polyline, indexed polycurve with
 * arcs, composite, trimmed line/circle/ellipse, circle, ellipse); other bodies are tessellated
 * into meshes (face sets, faceted breps, shells, swept disks, mapped items, a single mapped
 * item as a shared placed mesh); openings (IfcRelVoidsElement) are kept as mesh openings. The
 * geometry budget is Limits::for_text. Like Rust: the project is named "Imported model",
 * materials get derived ids (GlobalId::derived("material/<name>")) and no values.
 *
 * Not done: boolean results (IfcBooleanResult, clipping) are drawn uncut, the C core has no
 * boolean kernel (Rust: manifold3d); the georeference (building frame, IfcMapConversion) is
 * not kept (the C model has none). Both are reported in "unsupported: …" lines, never silent.
 * Trigonometry is correctly rounded (ifc_math.h); glibc's, which Rust uses, is not always. */
#ifndef CCORE_IFC_IN_H
#define CCORE_IFC_IN_H
#include "buf.h"
#include "model.h"
#include "scene.h"
#include "step.h"

typedef struct {
    size_t elements, parametric, extruded, meshes, openings, properties;
    size_t unsupported; /* things Rust imports that the C core left out (elements, openings…) */
    size_t entities, malformed; /* STEP instances parsed, and skipped as malformed */
    Buf lines;          /* ImportReport::lines(), then the "unsupported: …" lines; '\n' each */
} cc_ifc_report;

/* Imports STEP text. Always initializes `m` and `rep` (free both). 0 ok; -1 with err:
 * "parse: line N: …", "not an IFC file (schema \"…\")", "no element with geometry found",
 * "imported model invalid: …". */
int cc_ifc_import(cc_model *m, cc_ifc_report *rep, const char *text, size_t n, cc_error *err);
void cc_ifc_report_free(cc_ifc_report *rep);

/* MaterialCategory::classify (ASCII and Latin-1 case folding). */
cc_category cc_material_classify(const char *name, size_t n);
/* Correctly rounded square root (no libm: tcc and wasm32). */
/* cc_sqrt: the one in scene.h (both are correctly rounded; one definition). */
double cc_sqrt_soft(double x); /* the integer version cc_sqrt uses under tcc, for tests */
#endif
