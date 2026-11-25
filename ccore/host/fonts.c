/* Where the native host finds the Roboto files of the PDF export (ticket D7, session/sheet_svg.h
 * cc_paper_pdf_query). They are 350 KB each, so they stay out of the program and of the wasm:
 *   - `bimble-ccore open`: the lab's files (static.h), "/fonts/Roboto-Regular.ttf" and
 *     "/fonts/Roboto-Bold.ttf" (build.sh release puts them in the archive's web/fonts/), copied;
 *   - otherwise the first folder that holds Roboto-Regular.ttf of <exe dir>/web/fonts (an unpacked
 *     archive) and <exe dir>/../../../crates/bimble-drawing/fonts (the repository: the program is
 *     ccore/out/<mode>/bimble-ccore), read on the first export.
 * None found: the export sets its text in Helvetica (sheet_svg.h). */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <limits.h>
#include <stdio.h>
#include <string.h>
#if defined(__linux__) || defined(__APPLE__)
#include <stdlib.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "host.h"
#include "sheet_svg.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* The program's folder (links resolved), or 0. */
static int exe_dir(char *out, size_t n) {
    char exe[PATH_MAX];
    exe[0] = 0;
#if defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t pn = sizeof path;
    if (_NSGetExecutablePath(path, &pn) != 0 || !realpath(path, exe)) return 0;
#elif defined(__linux__)
    ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (k <= 0) return 0;
    exe[k] = 0;
#else
    return 0;
#endif
    char *slash = strrchr(exe, '/');
    if (!slash) return 0;
    int w = snprintf(out, n, "%.*s", (int)(slash - exe), exe);
    return w > 0 && (size_t)w < n;
}

static int has_regular(const char *dir) {
    char f[PATH_MAX + 32];
    if (snprintf(f, sizeof f, "%s/Roboto-Regular.ttf", dir) >= (int)sizeof f) return 0;
    FILE *fp = fopen(f, "rb");
    if (fp) fclose(fp);
    return fp != NULL;
}

void cc_host_fonts(cc_session *s, const cc_static *web) {
    if (web) {
        const cc_static_file *r = cc_static_find(web, "/fonts/Roboto-Regular.ttf", 25);
        const cc_static_file *b = cc_static_find(web, "/fonts/Roboto-Bold.ttf", 22);
        if (r && b) {
            cc_paper_set_fonts(s, (const unsigned char *)r->data, r->n, (const unsigned char *)b->data, b->n);
            return;
        }
    }
    char dir[PATH_MAX], cand[PATH_MAX + 64];
    if (!exe_dir(dir, sizeof dir)) return;
    static const char *const UNDER[] = {"/web/fonts", "/../../../crates/bimble-drawing/fonts"};
    for (size_t i = 0; i < sizeof UNDER / sizeof *UNDER; i++) {
        if (snprintf(cand, sizeof cand, "%s%s", dir, UNDER[i]) >= (int)sizeof cand) continue;
        if (has_regular(cand)) {
            cc_paper_set_font_dir(s, cand);
            return;
        }
    }
}
