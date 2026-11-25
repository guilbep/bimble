/* L0 maths without libm: neither test/run.sh nor build.sh links libm, and freestanding wasm has
 * none. Each result has the bits Rust's f64/f32 functions give (math.c). Named math.h but always
 * included as "math.h" (with -Isrc it would also shadow <math.h>: the core never includes that). */
#ifndef CCORE_MATH_H
#define CCORE_MATH_H

double cc_sqrt(double x); /* correctly rounded, like f64::sqrt */
float cc_sqrtf(float x);  /* correctly rounded, like f32::sqrt */
/* f64::hypot (glibc's FMA kernel, the FMA emulated; see math.c). */
double cc_hypot(double x, double y);
#endif
