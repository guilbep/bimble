/* Elementary functions for the IFC import, without libm (tcc, wasm32): sin, cos, atan2 and acos
 * as Rust's f64 methods give them (glibc on Linux), fmod as Rust's `%`.
 *
 * sin/cos/atan2/acos are evaluated in double-double (about 100 bits) and rounded once:
 * correctly rounded (checked against aarch64 quad-precision sinl/atan2l/acosl: 0 differences in
 * 300k inputs each). glibc's double versions are not always correctly rounded: they differ from
 * these in the last bit for about 0.1 % of random inputs (sin 0.14 %, acos 0.12 %, atan2 0.02 %
 * on 2M inputs), so a Rust import can differ from the C one by 1 ulp there. Arguments of sin/cos are reduced with a 3-part pi/2, accurate for
 * |x| < 2^20; past that the result is still bounded and finite, not correctly rounded.
 * cc_fmod is exact (integer long division, as musl's fmod). */
#ifndef CCORE_IFC_MATH_H
#define CCORE_IFC_MATH_H
double cc_sin(double x);
double cc_cos(double x);
/* Both at once, the same bits as cc_sin and cc_cos (each of those evaluates both series: half the
 * work for a caller that needs the two, ~2 us a call at any argument). */
void cc_sincos(double x, double *s, double *c);
double cc_atan2(double y, double x);
double cc_acos(double x);
double cc_fmod(double x, double y);
/* hypot: cc_hypot in verbs.h (a port of glibc's). */
#endif
