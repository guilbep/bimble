/* Where a model is on Earth and which way its project north points: crates/bimble-core/src/
 * georef.rs (ADR 0045), on cc_georef (model.h). World (model) coordinates are mm; survey ones
 * metres in the model's survey system: LV95 (CH1903+, EPSG:2056) for Swiss models.
 *
 * File form: serde of Georef (`#[serde(default, deny_unknown_fields)]`, zeros and None left out),
 * the model's key "georef", itself left out when the default. Rust checks nothing more; the C
 * core refuses non-finite values (a CBOR file can hold them; Rust's canonical form would panic).
 *
 * Coordinates as the viewer's status bar says them (bimble-viewer coords.rs `text`): the survey
 * system ("E 2'600'123.45 N 1'200'000.00 · 432.10 m (LV95)"), or WGS84 latitude and longitude.
 * WGS84 is worked out as Rust's proj4rs does for LV95 (somerc inverse on Bessel 1841, the
 * three-parameter shift to WGS84, geocentric to geodetic), without libm: the C numbers match
 * Rust's within 2e-9 degrees (golden/geo/<name>.coords.txt) and the 6-decimal text byte for byte.
 * One family at a time (ADR 0045), Swiss first: a model in another system Rust's table knows
 * (crs_table.rs) is refused for WGS84, UTM and other targets, with a clear error. */
#ifndef CCORE_GEOREF_H
#define CCORE_GEOREF_H
#include "buf.h"
#include "json_r.h"
#include "model.h"
#include "view_io.h"

#define CC_LV95 "EPSG:2056"

int cc_georef_is_default(const cc_georef *g); /* Georef::is_default (-0.0 counts as 0) */
int cc_georef_eq(const cc_georef *a, const cc_georef *b);
/* Georef::from_frame: the turn of the building's x axis (world XY, rounded to micro-degrees,
 * no -0.0) and LV95 when the building (world mm) lies in Switzerland's LV95 range. `crs` points
 * to a static string. */
void cc_georef_from_frame(cc_georef *g, const double x_axis[2], const double at_mm[3]);
/* Survey coordinates (m) of a world point (mm). */
void cc_georef_survey(const cc_georef *g, const double world_mm[3], double out[3]);
int cc_georef_has_map(const cc_georef *g);
/* How a plan drawn in survey coordinates (mm) lies in the world: turn by *deg, move by off (mm). */
void cc_georef_survey_plan_to_world(const cc_georef *g, double *deg, double off[2]);
/* How much a plan turns (radians, counter-clockwise): the building's turn in project north, the
 * map's opposite in true north. */
double cc_georef_plan_turn(const cc_georef *g, int true_north);
/* swisstopo's approximate formulas (about 1 m): lat, lon in degrees (georef.rs lv95_to_wgs84). */
void cc_lv95_to_wgs84_approx(double east, double north, double out[2]);
/* Georef::wgs84: the approximate formulas on a world point, when the survey system is LV95.
 * 0 ok, -1 when it is not. */
int cc_georef_wgs84_approx(const cc_georef *g, const double world_mm[3], double out[2]);
/* LV95 (m) -> WGS84 (degrees) as proj4rs transforms it. 0 ok, -1 outside the projection. */
int cc_lv95_to_wgs84(double east, double north, double *lat, double *lon);

typedef enum { CC_COORDS_SURVEY, CC_COORDS_WGS84, CC_COORDS_UTM, CC_COORDS_EPSG } cc_coords_target;
/* The status bar text of a world point (coords.rs `text`), appended. `epsg`: the code for
 * CC_COORDS_EPSG. 0 ok; -1 with the reason appended instead, when Rust would say it in a system
 * the C core does not convert to (a known system other than LV95, or a target other than the
 * survey system and WGS84). A model with no known system says its own coordinates, as Rust. */
int cc_georef_text(Buf *b, const cc_georef *g, const double world_mm[3], cc_coords_target t, int epsg);
/* Whether "EPSG:<code>" is in Rust's table of systems (crs_table.rs). */
int cc_crs_known(int code);

/* ---- file form ---- */
/* serde_json::to_string(&Georef), compact: "{}" when the default. */
void cc_georef_json(Buf *b, const cc_georef *g);
/* Decoding as serde does (texts in d->msg); strings go to d->a. `skip`: a member ignored. */
int cc_vd_georef(cc_vd *d, const JrVal *v, const char *skip, cc_georef *out);
/* Writing as serde does, JSON (pretty with the writer) or CBOR. */
void cc_em_georef(cc_em *e, const cc_georef *g);
/* The number of members cc_em_georef writes. */
size_t cc_georef_nfields(const cc_georef *g);
/* 0, or -1 with "georef: non-finite value" appended. */
int cc_georef_validate(const cc_georef *g, Buf *why);
void cc_georef_canonical(cc_georef *g); /* -0.0 to 0.0 */
int cc_georef_dup(Arena *a, cc_georef *g); /* crs copied into a */
#endif
