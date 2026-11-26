/* Georeference (georef.h): crates/bimble-core/src/georef.rs, the status bar's text
 * (bimble-viewer coords.rs) and proj4rs's LV95 -> WGS84 path, without libm. */
#include "georef.h"
#pragma STDC FP_CONTRACT OFF /* a*b+c rounds twice, as in Rust */
#include <stdint.h>
#include <string.h>
#include "cbor.h"
#include "command.h"
#include "fmt.h"
#include "ifc_math.h"
#include "math.h"
#include "quantity.h"

static const double PI_ = 3.14159265358979323846;
static int finite_(double x) { return x - x == 0.0; }
static double fabs_(double x) { return x < 0 ? -x : x; }
static double to_radians(double x) { return x * (PI_ / 180.0); } /* f64::to_radians */
static double to_degrees(double x) { return x * (180.0 / PI_); } /* f64::to_degrees */
/* f64::round: half away from zero. */
static double round_rs(double x) {
    double ax = fabs_(x);
    if (!(ax < 4503599627370496.0)) return x;
    double t = (double)(int64_t)x, d = x - t;
    if (d >= 0.5) t += 1.0;
    else if (d <= -0.5) t -= 1.0;
    if (t == 0.0) return x < 0 || (x == 0 && 1.0 / x < 0) ? -0.0 : 0.0;
    return t;
}

int cc_georef_places(const cc_georef *g) {
    return !(g->rotation_deg == 0.0 && !g->has_crs && g->east_m == 0.0 && g->north_m == 0.0 && g->height_m == 0.0 &&
             g->map_rotation_deg == 0.0);
}

int cc_georef_is_default(const cc_georef *g) { return !cc_georef_places(g) && !g->has_survey_point; }

int cc_georef_eq(const cc_georef *a, const cc_georef *b) {
    return a->rotation_deg == b->rotation_deg && a->has_crs == b->has_crs &&
           (!a->has_crs || (a->crs.n == b->crs.n && (!a->crs.n || !memcmp(a->crs.p, b->crs.p, a->crs.n)))) &&
           a->east_m == b->east_m && a->north_m == b->north_m && a->height_m == b->height_m &&
           a->map_rotation_deg == b->map_rotation_deg && a->has_survey_point == b->has_survey_point &&
           (!a->has_survey_point || (a->survey_point_mm[0] == b->survey_point_mm[0] &&
                                     a->survey_point_mm[1] == b->survey_point_mm[1] &&
                                     a->survey_point_mm[2] == b->survey_point_mm[2]));
}

static int is_lv95(const cc_georef *g) {
    return g->has_crs && g->crs.n == sizeof CC_LV95 - 1 && !memcmp(g->crs.p, CC_LV95, sizeof CC_LV95 - 1);
}

void cc_georef_from_frame(cc_georef *g, const double x_axis[2], const double at_mm[3]) {
    memset(g, 0, sizeof *g);
    double r = to_degrees(cc_atan2(x_axis[1], x_axis[0]));
    g->rotation_deg = round_rs(r * 1e6) / 1e6 + 0.0;
    double e = at_mm[0] / 1000.0, n = at_mm[1] / 1000.0;
    if (e >= 2480000.0 && e <= 2840000.0 && n >= 1070000.0 && n <= 1300000.0)
        g->has_crs = 1, g->crs.p = CC_LV95, g->crs.n = sizeof CC_LV95 - 1;
}

void cc_georef_survey(const cc_georef *g, const double w[3], double out[3]) {
    double a = to_radians(g->map_rotation_deg), s = cc_sin(a), c = cc_cos(a);
    double x = w[0] / 1000.0, y = w[1] / 1000.0;
    out[0] = c * x - s * y + g->east_m;
    out[1] = s * x + c * y + g->north_m;
    out[2] = w[2] / 1000.0 + g->height_m;
}

int cc_georef_has_map(const cc_georef *g) { return g->east_m != 0.0 || g->north_m != 0.0 || g->map_rotation_deg != 0.0; }

void cc_georef_survey_plan_to_world(const cc_georef *g, double *deg, double off[2]) {
    double a = -to_radians(g->map_rotation_deg), s = cc_sin(a), c = cc_cos(a);
    double e = g->east_m * 1000.0, n = g->north_m * 1000.0;
    *deg = -g->map_rotation_deg;
    off[0] = -(c * e - s * n) + 0.0;
    off[1] = -(s * e + c * n) + 0.0;
}

double cc_georef_plan_turn(const cc_georef *g, int true_north) {
    return true_north ? -to_radians(g->map_rotation_deg) : to_radians(g->rotation_deg);
}

void cc_lv95_to_wgs84_approx(double east, double north, double out[2]) {
    double y = (east - 2600000.0) / 1000000.0, x = (north - 1200000.0) / 1000000.0;
    double lon = 2.6779094 + 4.728982 * y + 0.791484 * y * x + 0.1306 * y * x * x - 0.0436 * (y * y * y);
    double lat = 16.9023892 + 3.238272 * x - 0.270978 * y * y - 0.002528 * x * x - 0.0447 * y * y * x -
                 0.0140 * (x * x * x);
    out[0] = lat * 100.0 / 36.0;
    out[1] = lon * 100.0 / 36.0;
}

int cc_georef_wgs84_approx(const cc_georef *g, const double w[3], double out[2]) {
    if (!is_lv95(g)) return -1;
    double s[3];
    cc_georef_survey(g, w, s);
    cc_lv95_to_wgs84_approx(s[0], s[1], out);
    return 0;
}

/* ---- elementary functions (no libm): exp, log, tan, atan, asin, about 1 ulp ---- */

static double ldexp_(double x, int e) { /* x * 2^e, e within the normal range */
    while (e > 1000) x *= 0x1p1000, e -= 1000;
    while (e < -1000) x *= 0x1p-1000, e += 1000;
    uint64_t u = (uint64_t)(e + 1023) << 52;
    double p;
    memcpy(&p, &u, 8);
    return x * p;
}

static const double LN2_HI = 6.93147180369123816490e-01, LN2_LO = 1.90821492927058770002e-10;

static double exp_(double x) {
    if (x != x) return x;
    if (x > 709.78) return 1.0 / 0.0;
    if (x < -745.2) return 0.0;
    double kf = round_rs(x / (LN2_HI + LN2_LO));
    int k = (int)kf;
    double r = (x - kf * LN2_HI) - kf * LN2_LO; /* |r| <= ln2/2 */
    /* Taylor to r^16 / 16!: below 1e-17 relative for |r| <= 0.35. */
    double t = 1.0;
    for (int i = 16; i >= 1; i--) t = 1.0 + t * r / i;
    return ldexp_(t, k);
}

static double log_(double x) {
    if (!(x > 0.0)) return x == 0.0 ? -1.0 / 0.0 : 0.0 / 0.0;
    if (!finite_(x)) return x;
    uint64_t u;
    memcpy(&u, &x, 8);
    int e = (int)((u >> 52) & 0x7ff);
    if (e == 0) { /* subnormal */
        x *= 0x1p54;
        memcpy(&u, &x, 8);
        e = (int)((u >> 52) & 0x7ff) - 54;
    }
    e -= 1023;
    u = (u & 0x000fffffffffffffull) | 0x3ff0000000000000ull; /* m in [1, 2) */
    double m;
    memcpy(&m, &u, 8);
    if (m > 1.41421356237309504880) m *= 0.5, e++;
    double f = m - 1.0, s = f / (2.0 + f), s2 = s * s;
    /* log(m) = 2 atanh(s) = 2 (s + s^3/3 + s^5/5 + ...), |s| < 0.172 */
    double t = 0.0;
    for (int i = 29; i >= 3; i -= 2) t = (1.0 / i + t) * s2; /* sum s^2k / (2k+1), k >= 1 */
    double r = 2.0 * s + 2.0 * s * t;
    return e * LN2_HI + (r + e * LN2_LO);
}

static double tan_(double x) { return cc_sin(x) / cc_cos(x); }
static double atan_(double x) { return cc_atan2(x, 1.0); }
static double asin_(double x) { return cc_atan2(x, cc_sqrt((1.0 - x) * (1.0 + x))); }

/* proj4rs aasin: |v| a little past 1 clamps, further is an error. */
static int aasin(double v, double *out) {
    double av = fabs_(v);
    if (av >= 1.0) {
        if (av > 1.00000000000001) return -1;
        *out = v < 0 ? -PI_ / 2 : PI_ / 2;
        return 0;
    }
    *out = asin_(v);
    return 0;
}

/* ---- LV95 -> WGS84, as proj4rs 0.3 transforms "+proj=somerc +lat_0=46.95240555555556
 * +lon_0=7.439583333333333 +k_0=1 +x_0=2600000 +y_0=1200000 +ellps=bessel
 * +towgs84=674.374,15.056,405.346,0,0,0,0 +units=m" to "+proj=longlat +datum=WGS84" ---- */

static const double FRAC_PI_4 = 0.78539816339744830962, FRAC_PI_2 = 1.57079632679489661923;

int cc_lv95_to_wgs84(double east, double north, double *lat, double *lon) {
    /* Ellipsoid Bessel 1841 (ellps.rs calc_ellipsoid_params, SP_rf). */
    const double a = 6377397.155, rf = 299.1528128;
    double f = 1.0 / rf, es = 2.0 * f - f * f, e = cc_sqrt(es);
    double one_es = 1.0 - es, rone_es = 1.0 / one_es, ra = 1.0 / a;
    double phi0 = to_radians(46.95240555555556), lam0 = to_radians(7.439583333333333);
    /* somerc setup */
    double hlf_e = 0.5 * e, sinphi = cc_sin(phi0), cosphi = cc_cos(phi0);
    double cp = cosphi * cosphi, c = cc_sqrt(1.0 + es * cp * cp * rone_es);
    double sinp0 = sinphi / c, phip0;
    if (aasin(sinp0, &phip0)) return -1;
    double cosp0 = cc_cos(phip0), sp = sinphi * e;
    double k = log_(tan_(FRAC_PI_4 + 0.5 * phip0)) -
               c * (log_(tan_(FRAC_PI_4 + 0.5 * phi0)) - hlf_e * log_((1.0 + sp) / (1.0 - sp)));
    double k_r = 1.0 * cc_sqrt(one_es) / (1.0 - sp * sp);
    /* projected_to_geographic: inverse((x * to_meter - x0) * ra, (y * to_meter - y0) * ra) */
    double x = (east * 1.0 - 2600000.0) * ra, y = (north * 1.0 - 1200000.0) * ra;
    double phipp = 2.0 * (atan_(exp_(y / k_r)) - FRAC_PI_4), lampp = x / k_r;
    double cpp = cc_cos(phipp), phip, lamp;
    if (aasin(cosp0 * cc_sin(phipp) + sinp0 * cpp * cc_cos(lampp), &phip)) return -1;
    if (aasin(cpp * cc_sin(lampp) / cc_cos(phip), &lamp)) return -1;
    double con = (k - log_(tan_(FRAC_PI_4 + 0.5 * phip))) / c;
    int i = 6;
    while (i > 0) {
        double esp = e * cc_sin(phip);
        double delp = (con + log_(tan_(FRAC_PI_4 + 0.5 * phip)) - hlf_e * log_((1.0 + esp) / (1.0 - esp))) *
                      (1.0 - esp * esp) * cc_cos(phip) * rone_es;
        phip -= delp;
        if (fabs_(delp) < 1e-10) break;
        i--;
    }
    if (i <= 0) return -1;
    double lam = lamp / c + lam0, phi = phip;
    if (fabs_(lam) >= PI_ + 1e-12) { /* adjlon */
        lam += PI_;
        double t = lam / (2 * PI_), fl = (double)(int64_t)t;
        if (fl > t) fl -= 1.0;
        lam -= 2 * PI_ * fl;
        lam -= PI_;
    }
    /* datum: geodetic_to_geocentric on Bessel, + (dx, dy, dz), geocentric_to_geodetic on WGS84 */
    if (phi < -FRAC_PI_2 || phi > FRAC_PI_2) return -1;
    if (lam > PI_) lam -= 2 * PI_;
    double z = 0.0, sl = cc_sin(phi), cl = cc_cos(phi);
    double rn = a / cc_sqrt(1.0 - es * (sl * sl));
    double X = (rn + z) * cl * cc_cos(lam) + 674.374, Y = (rn + z) * cl * cc_sin(lam) + 15.056,
           Z = ((rn * (1.0 - es)) + z) * sl + 405.346;
    const double wa = 6378137.0, wb = 6356752.314, wes = 0.0066943799901413165, GENAU2 = 1.0e-12 * 1.0e-12;
    double d2 = X * X + Y * Y, p = cc_sqrt(d2), rr = cc_sqrt(d2 + Z * Z);
    double ln;
    if (p / wa < 1.0e-12) {
        if (rr / wa < 1.0e-12) {
            *lat = to_degrees(FRAC_PI_2), *lon = 0.0;
            (void)wb;
            return 0;
        }
        ln = 0.0;
    } else ln = cc_atan2(Y, X);
    double ct = Z / rr, st = p / rr;
    double rx = 1.0 / cc_sqrt(1.0 - wes * (2.0 - wes) * st * st);
    double cphi0 = st * (1.0 - wes) * rx, sphi0 = ct * rx, cphi = cphi0, sphi = sphi0;
    for (int iter = 1;; iter++) {
        double rn2 = wa / cc_sqrt(1.0 - wes * sphi0 * sphi0);
        double h = p * cphi0 + Z * sphi0 - rn2 * (1.0 - wes * sphi0 * sphi0);
        if (rn2 + h == 0.0) {
            *lat = 0.0, *lon = to_degrees(ln);
            return 0;
        }
        double rk = wes * rn2 / (rn2 + h);
        rx = 1.0 / cc_sqrt(1.0 - rk * (2.0 - rk) * st * st);
        cphi = st * (1.0 - rk) * rx;
        sphi = ct * rx;
        double sdphi = sphi * cphi0 - cphi * sphi0;
        cphi0 = cphi, sphi0 = sphi;
        if (sdphi * sdphi <= GENAU2 || iter >= 30) break;
    }
    double la = to_degrees(cc_atan2(sphi, fabs_(cphi))), lo = to_degrees(ln);
    if (!finite_(la) || !finite_(lo)) return -1;
    *lat = la, *lon = lo;
    return 0;
}

/* ---- the status bar ---- */

/* crs_table.rs SYSTEMS codes, sorted (generated from it; 403 systems). */
static const unsigned short CODES[] = {
    2056, 2205, 2222, 2223, 2224, 2225, 2226, 2227, 2228, 2229, 2230, 2231, 2232, 2233, 2234, 2235,
    2236, 2237, 2238, 2239, 2240, 2241, 2242, 2243, 2244, 2245, 2246, 2247, 2248, 2249, 2250, 2251,
    2252, 2253, 2254, 2255, 2256, 2257, 2258, 2259, 2260, 2261, 2262, 2263, 2264, 2265, 2266, 2267,
    2268, 2269, 2270, 2271, 2272, 2273, 2274, 2275, 2276, 2277, 2278, 2279, 2280, 2281, 2282, 2283,
    2284, 2285, 2286, 2287, 2288, 2289, 2965, 2966, 2991, 2992, 3070, 3072, 3073, 3074, 3078, 3081,
    3082, 3088, 3089, 3417, 3418, 3419, 3420, 3421, 3422, 3423, 3424, 3433, 3434, 3435, 3436, 3437,
    3438, 3451, 3452, 3453, 3454, 3455, 3463, 3560, 3566, 3567, 3734, 3735, 3736, 3737, 3738, 3739,
    3759, 3814, 3968, 4457, 5554, 5555, 5556, 5646, 5971, 5972, 5973, 5974, 5975, 5976, 6171, 6172,
    6173, 6174, 6175, 6176, 6922, 6923, 7416, 7417, 8700, 8701, 8702, 8703, 8704, 8705, 8706, 8707,
    8708, 8709, 8710, 8711, 8712, 8713, 8714, 8715, 8716, 8717, 8718, 8719, 8720, 8721, 8722, 8723,
    8724, 8725, 8726, 8727, 8728, 8729, 8730, 8731, 8732, 8733, 8734, 8735, 8736, 8737, 8738, 8739,
    8740, 8741, 8742, 8743, 8744, 8745, 8746, 8747, 8748, 8749, 8750, 8751, 8752, 8753, 8754, 8755,
    8756, 8757, 8758, 8759, 8760, 8761, 8762, 8763, 8764, 8765, 8766, 8767, 8768, 8769, 8770, 8771,
    8772, 8773, 8774, 8775, 8776, 8777, 8778, 8779, 8780, 8781, 8782, 8783, 8784, 8785, 8786, 8787,
    8788, 8789, 8790, 8791, 8792, 8793, 8794, 8795, 8796, 8797, 8798, 8799, 8800, 8801, 8802, 8803,
    8804, 8805, 8806, 8807, 8808, 8809, 8810, 8811, 8812, 8813, 8814, 8815, 8826, 21781, 25828, 25829,
    25830, 25831, 25832, 25833, 25834, 25835, 25836, 25837, 26814, 26815, 26819, 26820, 26821, 26822,
    26823, 26824, 26847, 26848, 26849, 26850, 26851, 26852, 26853, 26854, 26929, 26930, 26931, 26932,
    26933, 26934, 26935, 26936, 26937, 26938, 26939, 26940, 26941, 26942, 26943, 26944, 26945, 26946,
    26948, 26949, 26950, 26951, 26952, 26953, 26954, 26955, 26956, 26957, 26958, 26959, 26960, 26961,
    26962, 26963, 26964, 26965, 26966, 26967, 26968, 26969, 26970, 26971, 26972, 26973, 26974, 26975,
    26976, 26977, 26978, 26979, 26980, 26981, 26982, 26983, 26984, 26985, 26986, 26987, 26988, 26989,
    26990, 26991, 26992, 26993, 26994, 26995, 26996, 26997, 26998, 32100, 32104, 32107, 32108, 32109,
    32110, 32111, 32112, 32113, 32114, 32115, 32116, 32117, 32118, 32119, 32120, 32121, 32122, 32123,
    32124, 32125, 32126, 32127, 32128, 32129, 32130, 32133, 32134, 32135, 32136, 32137, 32138, 32139,
    32140, 32141, 32142, 32143, 32144, 32145, 32146, 32147, 32148, 32149, 32150, 32151, 32152, 32153,
    32154, 32155, 32156, 32157, 32158, 32161, 32199,
};

int cc_crs_known(int code) {
    size_t lo = 0, hi = sizeof CODES / sizeof CODES[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (CODES[mid] == code) return 1;
        if (CODES[mid] < code) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

/* Georef::source: "EPSG:<n>" with n a u16 in the table; the code, or 0. */
static int source_code(const cc_georef *g) {
    if (!g->has_crs || g->crs.n < 6 || g->crs.n > 10 || memcmp(g->crs.p, "EPSG:", 5)) return 0;
    unsigned v = 0;
    size_t i = 5;
    if (g->crs.p[i] == '+') i++; /* u16::from_str takes a leading '+' */
    if (i == g->crs.n) return 0;
    for (; i < g->crs.n; i++) {
        char ch = g->crs.p[i];
        if (ch < '0' || ch > '9') return 0;
        v = v * 10 + (unsigned)(ch - '0');
        if (v > 65535) return 0;
    }
    return cc_crs_known((int)v) ? (int)v : 0;
}

/* coords.rs swiss: thousands with ', decimals after the point, "-" only when a digit is not 0. */
static void swiss(Buf *b, double x, int decimals) {
    Buf t;
    buf_init(&t);
    cc_fmt_fixed(&t, fabs_(x), decimals);
    size_t dot = 0;
    while (dot < t.len && t.p[dot] != '.') dot++;
    int nonzero = 0;
    for (size_t i = 0; i < t.len; i++) nonzero |= t.p[i] >= '1' && t.p[i] <= '9';
    if (x < 0.0 && nonzero) buf_putc(b, '-');
    for (size_t i = 0; i < dot; i++) {
        if (i > 0 && (dot - i) % 3 == 0) buf_putc(b, '\'');
        buf_putc(b, t.p[i]);
    }
    if (dot < t.len) buf_append(b, t.p + dot, t.len - dot);
    buf_free(&t);
}

int cc_georef_text(Buf *b, const cc_georef *g, const double w[3], cc_coords_target t, int epsg) {
    double s[3];
    cc_georef_survey(g, w, s);
    if (t != CC_COORDS_SURVEY) {
        int src = source_code(g);
        /* Rust says nothing in another system when the model's own is not known: its own below. */
        if (src && (src != 2056 || t != CC_COORDS_WGS84)) {
            buf_puts(b, "coordinates in ");
            if (src != 2056) {
                buf_puts(b, "EPSG:"), buf_u64(b, (uint64_t)src);
                buf_puts(b, " are not converted by the C core (Swiss LV95 only, ADR 0045)");
            } else {
                buf_puts(b, t == CC_COORDS_UTM ? "ETRS89 / UTM" : "EPSG:");
                if (t == CC_COORDS_EPSG) buf_u64(b, (uint64_t)(epsg < 0 ? 0 : epsg));
                buf_puts(b, " are not converted by the C core (LV95 and WGS84 only, ADR 0045)");
            }
            return -1;
        }
        if (src == 2056) {
            double lat, lon;
            if (!cc_lv95_to_wgs84(s[0], s[1], &lat, &lon)) {
                cc_fmt_fixed(b, fabs_(lat), 6);
                buf_puts(b, lat >= 0.0 ? "\xc2\xb0 N, " : "\xc2\xb0 S, ");
                cc_fmt_fixed(b, fabs_(lon), 6);
                buf_puts(b, lon >= 0.0 ? "\xc2\xb0 E \xc2\xb7 " : "\xc2\xb0 W \xc2\xb7 ");
                swiss(b, s[2], 2);
                buf_puts(b, " m (WGS84)");
                return 0;
            }
        }
    }
    if (g->has_crs) {
        buf_puts(b, "E "), swiss(b, s[0], 2);
        buf_puts(b, " N "), swiss(b, s[1], 2);
        buf_puts(b, " \xc2\xb7 "), swiss(b, s[2], 2);
        buf_puts(b, " m (");
        if (is_lv95(g)) buf_puts(b, "LV95");
        else buf_append(b, g->crs.p, g->crs.n);
        buf_putc(b, ')');
    } else {
        buf_puts(b, "x "), swiss(b, s[0], 3);
        buf_puts(b, " y "), swiss(b, s[1], 3);
        buf_puts(b, " \xc2\xb7 "), swiss(b, s[2], 2);
        buf_puts(b, " m");
    }
    return 0;
}

/* ---- file form ---- */

static const char *const F[] = {"rotation_deg", "crs", "east_m", "north_m", "height_m", "map_rotation_deg"};
#define SURVEY_POINT "survey_point_mm" /* SETOUT: the C core's seventh key (ADR 0075) */

static void json_escape_(Buf *b, cc_str s) {
    JsonW w;
    jw_init(&w, b, 0);
    jw_str_n(&w, s.n ? s.p : "", s.n);
    jw_finish(&w);
}

void cc_georef_json(Buf *b, const cc_georef *g) {
    int first = 1;
    double v[6] = {g->rotation_deg, 0, g->east_m, g->north_m, g->height_m, g->map_rotation_deg};
    buf_putc(b, '{');
    for (int i = 0; i < 6; i++) {
        if (i == 1 ? !g->has_crs : v[i] == 0.0) continue;
        if (!first) buf_putc(b, ',');
        first = 0;
        buf_putc(b, '"'), buf_puts(b, F[i]), buf_puts(b, "\":");
        if (i == 1) json_escape_(b, g->crs);
        else fmt_f64_json(b, v[i]);
    }
    if (g->has_survey_point) { /* SETOUT: after Rust's six */
        if (!first) buf_putc(b, ',');
        buf_puts(b, "\"" SURVEY_POINT "\":[");
        for (int k = 0; k < 3; k++) {
            if (k) buf_putc(b, ',');
            fmt_f64_json(b, g->survey_point_mm[k]);
        }
        buf_putc(b, ']');
    }
    buf_putc(b, '}');
}

size_t cc_georef_nfields(const cc_georef *g) {
    return (size_t)(g->rotation_deg != 0.0) + (size_t)(g->has_crs != 0) + (size_t)(g->east_m != 0.0) +
           (size_t)(g->north_m != 0.0) + (size_t)(g->height_m != 0.0) + (size_t)(g->map_rotation_deg != 0.0) +
           (size_t)(g->has_survey_point != 0);
}

void cc_em_georef(cc_em *e, const cc_georef *g) {
    double v[6] = {g->rotation_deg, 0, g->east_m, g->north_m, g->height_m, g->map_rotation_deg};
    if (e->j) jw_obj_begin(e->j);
    else cb_head(e->c, CB_MAP, cc_georef_nfields(g));
    for (int i = 0; i < 6; i++) {
        if (i == 1 ? !g->has_crs : v[i] == 0.0) continue;
        if (e->j) {
            jw_key(e->j, F[i]);
            if (i == 1) jw_str_n(e->j, g->crs.n ? g->crs.p : "", g->crs.n);
            else jw_f64(e->j, v[i]);
        } else {
            cb_text(e->c, F[i], strlen(F[i]));
            if (i == 1) cb_text(e->c, g->crs.n ? g->crs.p : "", g->crs.n);
            else cb_f64(e->c, v[i]);
        }
    }
    if (g->has_survey_point) { /* SETOUT (ADR 0075): after Rust's six */
        if (e->j) {
            jw_key(e->j, SURVEY_POINT), jw_arr_begin(e->j);
            for (int k = 0; k < 3; k++) jw_f64(e->j, g->survey_point_mm[k]);
            jw_arr_end(e->j);
        } else {
            cb_text(e->c, SURVEY_POINT, strlen(SURVEY_POINT)), cb_head(e->c, CB_ARRAY, 3);
            for (int k = 0; k < 3; k++) cb_f64(e->c, g->survey_point_mm[k]);
        }
    }
    if (e->j) jw_obj_end(e->j);
}

static int vfail(cc_vd *d, size_t off) {
    if (!d->failed) d->failed = 1, d->off = off;
    return -1;
}

/* serde's "invalid type: <unexpected>, expected <what>" (the number's token as file.c gives it). */
static int type_err(cc_vd *d, const JrVal *v, const char *expected) {
    if (d->failed) return -1;
    buf_clear(&d->msg);
    buf_puts(&d->msg, "invalid type: ");
    switch (v->type) {
    case JR_NULL: buf_puts(&d->msg, "null"); break;
    case JR_BOOL: buf_puts(&d->msg, v->b ? "boolean `true`" : "boolean `false`"); break;
    case JR_NUM:
        buf_puts(&d->msg, v->num_int ? "integer `" : "floating point `");
        if (v->s) buf_append(&d->msg, v->s, v->n < 64 ? v->n : 64);
        else if (v->num_int && v->num >= -9.2e18 && v->num <= 1.8e19) {
            if (v->num_neg) buf_i64(&d->msg, (int64_t)v->num);
            else buf_u64(&d->msg, (uint64_t)v->num);
        } else if (fmt_f64_json(&d->msg, v->num))
            buf_puts(&d->msg, v->num != v->num ? "NaN" : v->num > 0 ? "inf" : "-inf");
        buf_putc(&d->msg, '`');
        break;
    case JR_STR:
        buf_puts(&d->msg, "string ");
        cc_buf_debug_str(&d->msg, v->s, v->n < 200 ? v->n : 200);
        break;
    case JR_ARR: buf_puts(&d->msg, "sequence"); break;
    default: buf_puts(&d->msg, "map"); break;
    }
    buf_puts(&d->msg, ", expected ");
    buf_puts(&d->msg, expected);
    return vfail(d, v->off);
}

int cc_vd_georef(cc_vd *d, const JrVal *v, const char *skip, cc_georef *out) {
    const JrVal *f[6] = {0}, *sp = NULL;
    memset(out, 0, sizeof *out);
    if (v->type != JR_OBJ) return type_err(d, v, "struct Georef");
    for (size_t j = 0; j < v->n; j++) {
        const JrMember *mb = &v->members[j];
        if (skip && jr_key_is(mb, skip)) continue;
        int i = 0;
        while (i < 6 && !jr_key_is(mb, F[i])) i++;
        if (d->failed) return -1;
        if (i == 6 && jr_key_is(mb, SURVEY_POINT)) { /* SETOUT: the C core's seventh key */
            if (sp) {
                buf_clear(&d->msg);
                buf_puts(&d->msg, "duplicate field `" SURVEY_POINT "`");
                return vfail(d, mb->val.off);
            }
            sp = &mb->val;
            continue;
        }
        if (i == 6) { /* deny_unknown_fields (Rust's six named, as Rust says it) */
            buf_clear(&d->msg);
            buf_puts(&d->msg, "unknown field `");
            buf_append(&d->msg, mb->key, mb->key_n < 200 ? mb->key_n : 200);
            buf_puts(&d->msg, "`, expected one of ");
            for (int k = 0; k < 6; k++) {
                if (k) buf_puts(&d->msg, ", ");
                buf_putc(&d->msg, '`'), buf_puts(&d->msg, F[k]), buf_putc(&d->msg, '`');
            }
            return vfail(d, mb->val.off);
        }
        if (f[i]) {
            buf_clear(&d->msg);
            buf_puts(&d->msg, "duplicate field `"), buf_puts(&d->msg, F[i]), buf_putc(&d->msg, '`');
            return vfail(d, mb->val.off);
        }
        f[i] = &mb->val;
    }
    double *num[6] = {&out->rotation_deg, NULL, &out->east_m, &out->north_m, &out->height_m, &out->map_rotation_deg};
    for (int i = 0; i < 6; i++) {
        if (!f[i]) continue;
        if (i == 1) {
            if (f[i]->type == JR_NULL) continue;
            if (f[i]->type != JR_STR) return type_err(d, f[i], "a string");
            char *p = f[i]->n ? arena_dup(d->a, f[i]->s, f[i]->n) : NULL;
            if (f[i]->n && !p) {
                buf_clear(&d->msg), buf_puts(&d->msg, "out of memory");
                return vfail(d, f[i]->off);
            }
            out->has_crs = 1, out->crs.p = p, out->crs.n = f[i]->n;
            continue;
        }
        if (f[i]->type != JR_NUM) return type_err(d, f[i], "f64");
        if (!finite_(f[i]->num)) {
            buf_clear(&d->msg), buf_puts(&d->msg, "number out of range");
            return vfail(d, f[i]->off);
        }
        *num[i] = f[i]->num;
    }
    if (sp && sp->type != JR_NULL) { /* SETOUT: [x, y, z], finite, within 1e7 mm (cc_georef_validate) */
        if (sp->type != JR_ARR) return type_err(d, sp, "an array of 3 numbers");
        if (sp->n != 3) {
            buf_clear(&d->msg), buf_puts(&d->msg, "survey_point_mm: expected 3 numbers");
            return vfail(d, sp->off);
        }
        for (int k = 0; k < 3; k++) {
            const JrVal *x = &sp->items[k];
            if (x->type != JR_NUM) return type_err(d, x, "f64");
            if (!finite_(x->num)) {
                buf_clear(&d->msg), buf_puts(&d->msg, "number out of range");
                return vfail(d, x->off);
            }
            out->survey_point_mm[k] = x->num;
        }
        out->has_survey_point = 1;
    }
    return 0;
}

int cc_georef_validate(const cc_georef *g, Buf *why) {
    if (!(finite_(g->rotation_deg) && finite_(g->east_m) && finite_(g->north_m) && finite_(g->height_m) &&
          finite_(g->map_rotation_deg))) {
        if (why) buf_puts(why, "georef: non-finite value");
        return -1;
    }
    if (g->has_survey_point) /* SETOUT: finite and within CC_SURVEY_POINT_MAX_MM, as grid axes */
        for (int k = 0; k < 3; k++)
            if (!finite_(g->survey_point_mm[k]) || fabs_(g->survey_point_mm[k]) > CC_SURVEY_POINT_MAX_MM) {
                if (why) buf_puts(why, "georef: survey_point_mm out of range (|x| <= 1e7 mm)");
                return -1;
            }
    return 0;
}

void cc_georef_canonical(cc_georef *g) {
    g->rotation_deg += 0.0, g->east_m += 0.0, g->north_m += 0.0, g->height_m += 0.0, g->map_rotation_deg += 0.0;
    for (int k = 0; k < 3; k++) g->survey_point_mm[k] += 0.0;
}

int cc_georef_dup(Arena *a, cc_georef *g) {
    cc_georef_canonical(g);
    if (!g->has_crs || !g->crs.n) return 0;
    char *p = arena_dup(a, g->crs.p, g->crs.n);
    if (!p) return -1;
    g->crs.p = p;
    return 0;
}

/* ---- the verb georef_set (a C addition, verb_table.c) ---- */

double cc_angle_norm_deg(double deg) {
    if (!finite_(deg)) return deg;
    while (deg > 180.0) deg -= 360.0;
    while (deg <= -180.0) deg += 360.0;
    return deg + 0.0;
}

static int vbad(Buf *err, const char *a, const char *b, const char *c) {
    buf_clear(err);
    buf_puts(err, "bad arguments: ");
    if (a) buf_puts(err, a);
    if (b) buf_puts(err, b);
    if (c) buf_puts(err, c);
    return -1;
}

int cc_vh_georef_set(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err) {
    static const char *const A[] = {"clear",   "crs",          "east_m",         "height_m",
                                    "map_rotation_deg", "north_m", "rotation_deg", "survey_point_mm"};
    const JrVal *f[8] = {0};
    if (args->type != JR_OBJ) return vbad(err, "expected an object", 0, 0);
    for (size_t j = 0; j < args->n; j++) {
        const JrMember *mb = &args->members[j];
        int i = 0;
        while (i < 8 && !jr_key_is(mb, A[i])) i++;
        if (i == 8) {
            vbad(err, "unknown field `", 0, 0);
            buf_append(err, mb->key, mb->key_n < 200 ? mb->key_n : 200);
            buf_puts(err, "`, expected one of");
            for (int k = 0; k < 8; k++) buf_puts(err, k ? ", `" : " `"), buf_puts(err, A[k]), buf_putc(err, '`');
            return -1;
        }
        f[i] = mb->val.type == JR_NULL ? NULL : &mb->val; /* the last of equal keys wins; null: absent */
    }
    cc_georef g = m->georef;
    if (f[0]) {
        if (f[0]->type != JR_BOOL) return vbad(err, "`clear`: expected a boolean", 0, 0);
        if (f[0]->b) memset(&g, 0, sizeof g);
    }
    if (f[1]) {
        if (f[1]->type != JR_STR) return vbad(err, "`crs`: expected a string", 0, 0);
        if (f[1]->n > 64) return vbad(err, "`crs`: at most 64 bytes", 0, 0);
        for (size_t k = 0; k < f[1]->n; k++)
            if ((unsigned char)f[1]->s[k] < 0x20 || f[1]->s[k] == ' ')
                return vbad(err, "`crs`: one word, e.g. \"EPSG:2056\"", 0, 0);
        g.has_crs = f[1]->n > 0;
        g.crs.p = NULL, g.crs.n = 0;
        if (f[1]->n) {
            char *p = arena_dup(&l->arena, f[1]->s, f[1]->n);
            if (!p) return vbad(err, "out of memory", 0, 0);
            g.crs.p = p, g.crs.n = f[1]->n;
        }
    }
    /* east_m, height_m, map_rotation_deg, north_m, rotation_deg, with their bounds */
    double *to[5] = {&g.east_m, &g.height_m, &g.map_rotation_deg, &g.north_m, &g.rotation_deg};
    static const double LIM[5] = {1e8, 1e5, 360.0, 1e8, 360.0};
    static const char *const WHY[5] = {"at most 1e8 m", "at most 1e5 m", "at most 360 degrees", "at most 1e8 m",
                                       "at most 360 degrees"};
    for (int i = 0; i < 5; i++) {
        const JrVal *v = f[i + 2];
        if (!v) continue;
        if (v->type != JR_NUM) return vbad(err, "`", A[i + 2], "`: expected a number");
        if (!finite_(v->num) || fabs_(v->num) > LIM[i]) {
            vbad(err, "`", A[i + 2], "`: ");
            buf_puts(err, WHY[i]);
            return -1;
        }
        *to[i] = v->num + 0.0;
    }
    /* SETOUT (ADR 0075): the angles in (-180, 180], one value per direction */
    g.rotation_deg = cc_angle_norm_deg(g.rotation_deg), g.map_rotation_deg = cc_angle_norm_deg(g.map_rotation_deg);
    if (f[7]) { /* survey_point_mm: [x, y, z] world mm sets it, [] removes it */
        const JrVal *v = f[7];
        if (v->type != JR_ARR || (v->n != 0 && v->n != 3))
            return vbad(err, "`survey_point_mm`: expected [x, y, z] (mm) or [] to remove it", 0, 0);
        g.has_survey_point = v->n == 3;
        for (size_t k = 0; k < 3; k++) {
            g.survey_point_mm[k] = 0.0;
            if (v->n != 3) continue;
            if (v->items[k].type != JR_NUM) return vbad(err, "`survey_point_mm`: expected numbers", 0, 0);
            double x = v->items[k].num;
            if (!finite_(x) || fabs_(x) > CC_SURVEY_POINT_MAX_MM) return vbad(err, "`survey_point_mm`: at most 1e7 mm", 0, 0);
            g.survey_point_mm[k] = x + 0.0;
        }
    }
    if (cc_georef_eq(&g, &m->georef)) return 0; /* nothing changes: no command */
    cc_command *c = cc_cmds_push(l, CC_CMD_SET_GEOREF);
    if (!c) return vbad(err, "out of memory", 0, 0);
    c->georef = g;
    return 0;
}
