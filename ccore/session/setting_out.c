/* The Setting out tab's markers (ticket SETOUT): see setting_out.h. */
#include "setting_out.h"
#include <string.h>
#include "georef.h"
#include "ifc_math.h" /* cc_sin, cc_cos */

static const char *const NAMES[] = {"show_base_point", "show_survey_point"};

int cc_setout_index(const char *name) {
    for (int i = 0; i < 2; i++)
        if (!strcmp(name, NAMES[i])) return i;
    return -1;
}

int cc_setout_on(const cc_setout *so, int i) { return i == 0 ? so->base_point : i == 1 ? so->survey_point : 0; }

void cc_setout_flip(cc_setout *so, int i) {
    if (i == 0) so->base_point = !so->base_point;
    else if (i == 1) so->survey_point = !so->survey_point;
}

static void v3(JsonW *w, const double v[3]) {
    jw_arr_begin(w);
    for (int k = 0; k < 3; k++) jw_f64(w, v[k] + 0.0);
    jw_arr_end(w);
}

/* The world plan direction a plan turned by `turn` (radians, counter-clockwise: georef.h
 * cc_georef_plan_turn) shows upwards: rot(turn) d = (0, 1). */
static void up_dir(JsonW *w, double turn) {
    jw_arr_begin(w);
    jw_f64(w, cc_sin(turn) + 0.0), jw_f64(w, cc_cos(turn) + 0.0);
    jw_arr_end(w);
}

static void point(JsonW *w, const cc_georef *g, const double at[3], int shown) {
    double s[3];
    cc_georef_survey(g, at, s);
    jw_key(w, "at_mm"), v3(w, at);
    jw_key(w, "shown"), jw_bool(w, shown);
    jw_key(w, "survey_m"), v3(w, s);
}

void cc_setout_json(const cc_setout *so, const cc_model *m, JsonW *w) {
    const cc_georef *g = &m->georef;
    static const double ORIGIN[3] = {0, 0, 0};
    jw_obj_begin(w);
    jw_key(w, "base_point"), jw_obj_begin(w), point(w, g, ORIGIN, so->base_point), jw_obj_end(w);
    jw_key(w, "crs");
    if (g->has_crs) jw_str_n(w, g->crs.n ? g->crs.p : "", g->crs.n);
    else jw_null(w);
    jw_key(w, "north"), jw_obj_begin(w);
    jw_key(w, "project"), up_dir(w, cc_georef_plan_turn(g, 0));
    jw_key(w, "rotation_deg"), jw_f64(w, g->rotation_deg + 0.0);
    jw_key(w, "true"), up_dir(w, cc_georef_plan_turn(g, 1));
    jw_obj_end(w);
    jw_key(w, "project_zero_m"), jw_f64(w, g->height_m + 0.0);
    jw_key(w, "survey_point"), jw_obj_begin(w);
    point(w, g, g->has_survey_point ? g->survey_point_mm : ORIGIN, so->survey_point);
    jw_key(w, "set"), jw_bool(w, g->has_survey_point);
    jw_obj_end(w);
    jw_obj_end(w);
}
