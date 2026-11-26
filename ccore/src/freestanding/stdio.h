/* Freestanding wasm32 shim (build.sh wasm only); implemented in ../wasm_api.c. snprintf and
 * vsnprintf know %s %.*s %d %u %zu %llu %.<n>f and %%. No files: logging goes through the
 * session's platform (session/platform.h). */
#ifndef CCORE_FS_STDIO_H
#define CCORE_FS_STDIO_H
#include <stdarg.h>
#include <stddef.h>
int snprintf(char *out, size_t cap, const char *fmt, ...);
int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
#endif
