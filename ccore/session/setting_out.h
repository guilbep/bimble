/* The Setting out tab (ticket SETOUT, ADR 0075): what the markers of the project base point, the
 * survey point and north show, as the page draws them in 3D and in plans.
 *
 * The data is the model's georeference (georef.h, ADR 0045): the project base point is the world
 * origin, its survey coordinates the georeference's east, north and height; project zero is the
 * height (ADR 0056 decision 1); true north is `rotation_deg` (project north from true north); the
 * survey point is `survey_point_mm` (a C addition), at the base point while it is not set (as a
 * new Revit project has them). Whether each marker is shown is session state (the actions
 * show_base_point and show_survey_point: no history step, rev unchanged), as the plan annotation
 * toggles (toggles.h).
 *
 * query setting_out (null without an editable model):
 *   {"base_point": {"at_mm": [0,0,0], "shown": b, "survey_m": [e, n, h]},
 *    "crs": "EPSG:2056" | null,
 *    "north": {"project": [x, y], "rotation_deg": r, "true": [x, y]},   unit vectors, world plan
 *    "project_zero_m": h,
 *    "survey_point": {"at_mm": [x, y, z], "set": b, "shown": b, "survey_m": [e, n, h]}} */
#ifndef CCORE_SESSION_SETTING_OUT_H
#define CCORE_SESSION_SETTING_OUT_H
#include "json_w.h"
#include "model.h"

typedef struct {
    int base_point, survey_point; /* the markers shown (0 at start, as Revit hides them) */
} cc_setout;

/* 0 show_base_point, 1 show_survey_point, -1 another name. */
int cc_setout_index(const char *name);
int cc_setout_on(const cc_setout *so, int i);
void cc_setout_flip(cc_setout *so, int i);
/* The query's answer for the model `m`. */
void cc_setout_json(const cc_setout *so, const cc_model *m, JsonW *w);
#endif
