/* cc_log (platform.h). */
#include "platform.h"
#include <stdarg.h>
#include <stdio.h>

void cc_log(const cc_platform *pf, const char *fmt, ...) {
    if (!pf || !pf->log) return;
    char line[8192];
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    size_t n = (size_t)k < sizeof line ? (size_t)k : sizeof line - 1;
    pf->log(pf->ctx, line, n);
}
