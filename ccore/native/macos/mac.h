/* The native app's macOS pieces (ticket MACAPP, ADR 0088): what Linux reads from /proc or glibc, the Mach way. Only
 * compiled on macOS (native/build.sh adds macos/mac.c there); the shared files call these under __APPLE__. */
#ifndef BIMBLE_NATIVE_MAC_H
#define BIMBLE_NATIVE_MAC_H
#include <stddef.h>
#include <stdint.h>

/* resident memory now (bytes) and, when peak, the highest so far (mach_task_basic_info) */
uint64_t mac_rss(uint64_t *peak);
/* this executable's path (_NSGetExecutablePath, resolved): 0 ok */
int mac_exe_path(char *buf, size_t n);
/* freed memory given back to the system (malloc_zone_pressure_relief: glibc's malloc_trim) */
void mac_trim(void);
/* this executable's load address (its Mach-O header: atos -l) and its LC_UUID as dwarfdump --uuid prints it (the .dSYM's
 * name; n >= 37) */
uintptr_t mac_exe_base(char *uuid, size_t n);
/* $HOME/Library/<sub> (Application Support/Bimble, Caches/bimble, Logs/bimble/sessions): 0 when no $HOME */
int mac_library(char *out, size_t n, const char *sub);
#endif
