/* The native app's macOS pieces (mac.h; ticket MACAPP, ADR 0088). */
#include "mac.h"
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t mac_rss(uint64_t *peak) {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    if (peak) *peak = info.resident_size_max;
    return info.resident_size;
}

int mac_exe_path(char *buf, size_t n) {
    char raw[PATH_MAX];
    uint32_t sz = sizeof raw;
    if (_NSGetExecutablePath(raw, &sz) != 0) return -1;
    char real[PATH_MAX];
    const char *p = realpath(raw, real) ? real : raw;
    return snprintf(buf, n, "%s", p) > 0 && strlen(p) < n ? 0 : -1;
}

void mac_trim(void) { malloc_zone_pressure_relief(NULL, 0); }

uintptr_t mac_exe_base(char *uuid, size_t n) {
    if (n) uuid[0] = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header *h = _dyld_get_image_header(i);
        if (!h || h->filetype != MH_EXECUTE) continue;
        if (h->magic == MH_MAGIC_64) {
            const uint8_t *p = (const uint8_t *)h + sizeof(struct mach_header_64);
            for (uint32_t k = 0; k < h->ncmds; k++) {
                const struct load_command *lc = (const struct load_command *)p;
                if (lc->cmd == LC_UUID && n >= 37) { /* as dwarfdump --uuid prints it: 8-4-4-4-12, upper case */
                    const struct uuid_command *u = (const struct uuid_command *)p;
                    char *o = uuid;
                    for (int b = 0; b < 16; b++) {
                        if (b == 4 || b == 6 || b == 8 || b == 10) *o++ = '-';
                        snprintf(o, 3, "%02X", u->uuid[b]), o += 2;
                    }
                }
                if (!lc->cmdsize) break;
                p += lc->cmdsize;
            }
        }
        return (uintptr_t)h;
    }
    return 0;
}

int mac_library(char *out, size_t n, const char *sub) {
    const char *h = getenv("HOME");
    if (!h || !*h) return 0;
    int w = snprintf(out, n, "%s/Library/%s", h, sub);
    return w > 0 && (size_t)w < n;
}
