/* Exact decimal -> double, correctly rounded (round half to even), for builds without a libc
 * strtod (freestanding wasm32). Natively the callers keep glibc's strtod; test/strtod_test.c
 * checks this one against it bit for bit. */
#ifndef CCORE_STRTOD_H
#define CCORE_STRTOD_H
#include <stddef.h>

/* Parses s[0..n): [+-] digits* ['.' digits*] [(e|E) [+-] digits*] (the caller has checked the
 * grammar; parsing stops at the first byte outside it). Any number of digits. Returns the double
 * nearest to the exact decimal value, as glibc strtod: +-inf on overflow, +-0 or a subnormal on
 * underflow, -0.0 for a negative zero. Allocates nothing; stack use is about 1 KiB. */
double cc_strtod(const char *s, size_t n);
#endif
