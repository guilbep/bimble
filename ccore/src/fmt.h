/* Exact f64 -> text, byte for byte as Rust prints it. Shortest round-trip digits come from Ryu.
 * Both return 0, or CCORE_ERR_NONFINITE (nothing appended) for NaN and infinities. */
#ifndef CCORE_FMT_H
#define CCORE_FMT_H
#include "buf.h"

#define CCORE_ERR_NONFINITE (-1)

/* serde_json::to_string(&x): "300.0", "1e+16", "1e-7", "0.0001", "-0.0". */
int fmt_f64_json(Buf *b, double x);
/* format!("{:?}", x): "300.0", "1e16", "1e-5", "0.0001", "-0.0". */
int fmt_f64_debug(Buf *b, double x);
/* prec 0..20: format!("{:.prec}", x), the exact value rounded half to even ("0.062" for 0.0625
 * at 3, "-0.000" for -0.0004). prec < 0: format!("{}", x), shortest digits, never an exponent
 * ("300", "0.0000001", "-0"). Any other prec: CCORE_ERR_NONFINITE, nothing written. */
int fmt_f64_fixed(Buf *b, double x, int prec);
#endif
