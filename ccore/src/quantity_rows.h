/* WAVE1B (B5, ADR 0077 decision 19): the foundations list the Quantities page shows apart from the
 * slabs: every IfcFooting and every IfcSlab whose Bimble/PredefinedType is BASESLAB (a raft), in
 * the scene's order, measured as the schedules are (quantity.h cc_measure: a sleeve's exact
 * cylinder too). A header of its own so the session can call it without quantity.h (whose
 * cc_measure clashes with session/measure.h's type). Layer L4 (pure). */
#ifndef CCORE_QUANTITY_ROWS_H
#define CCORE_QUANTITY_ROWS_H
#include "json_w.h"
#include "model.h"
#include "scene.h"

/* [{"id","kind":"pad"|"strip"|"raft"|"footing","level","name","section","volume_m3"}]. 0 ok, -1 out
 * of memory. */
int cc_foundation_rows_json(const cc_model *m, const cc_scene *sc, JsonW *w);
#endif
