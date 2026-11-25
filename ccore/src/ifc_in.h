/* IFC import of the subset Bimble exports, the C twin of bimble_ifc::import::import_str
 * (crates/bimble-ifc/src/import.rs). Covered: units (SI and conversion-based length units),
 * project id, storeys (placement z or Elevation), containment and aggregation, materials
 * (IfcMaterial and the layer/profile/constituent sets, by type too), property sets and
 * quantity sets as text, and elements of IfcColumn/Beam/Slab/Wall/Footing (and any other
 * product with a shape, as IfcBuildingElementProxy) whose body is an IfcExtrudedAreaSolid
 * (through IfcMappedItem too) of an IfcRectangleProfileDef, IfcArbitraryClosedProfileDef or
 * IfcArbitraryProfileDefWithVoids on IfcPolyline curves, placed by IfcLocalPlacement chains.
 * Like Rust: the project is named "Imported model", materials get derived ids
 * (GlobalId::derived("material/<name>")) and no values.
 *
 * Outside the subset (Rust makes a mesh, cuts openings or keeps a georeference): the C core
 * says so in the report ("unsupported: …" lines) and leaves it out; it never drops silently. */
#ifndef CCORE_IFC_IN_H
#define CCORE_IFC_IN_H
#include "buf.h"
#include "model.h"
#include "scene.h"
#include "step.h"

typedef struct {
    size_t elements, parametric, extruded, properties;
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
