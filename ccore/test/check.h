/* Tiny test helpers shared by the suites. */
#ifndef CCORE_CHECK_H
#define CCORE_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed, failed;

static void check_eq(const char *what, const char *got, size_t got_n, const char *want, size_t want_n) {
    if (got_n == want_n && memcmp(got, want, got_n) == 0) {
        passed++;
        return;
    }
    if (failed++ < 20) {
        printf("FAIL %s\n  got:  %.*s\n  want: %.*s\n", what, (int)got_n, got, (int)want_n, want);
    }
}

static void check_true(const char *what, int ok) {
    if (ok) {
        passed++;
        return;
    }
    if (failed++ < 20) printf("FAIL %s\n", what);
}

static int report(const char *suite) {
    printf("%s: %d passed, %d failed\n", suite, passed, failed);
    return failed || !passed;
}

/* Whole file into a NUL-terminated heap string; exits on error. */
static char *slurp(const char *dir, const char *name, size_t *n) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = malloc((size_t)len + 1);
    if (!p || fread(p, 1, (size_t)len, f) != (size_t)len) exit(2);
    p[len] = 0;
    fclose(f);
    *n = (size_t)len;
    return p;
}
#endif
