/* Suite fmt: every line of golden/floats/floats.txt, in both formats. */
#include <stdint.h>
#include "../src/fmt.h"
#include "check.h"

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "ccore/golden/floats";
    size_t n;
    char *text = slurp(dir, "floats.txt", &n);
    Buf b;
    buf_init(&b);
    int lines = 0;
    for (char *line = text; *line;) {
        char *eol = strchr(line, '\n');
        if (!eol) eol = line + strlen(line);
        char *sp1 = memchr(line, ' ', (size_t)(eol - line));
        char *sp2 = sp1 ? memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1)) : NULL;
        if (!sp2) {
            printf("bad line %d\n", lines + 1);
            return 2;
        }
        uint64_t bits = strtoull(line, NULL, 16);
        double x;
        memcpy(&x, &bits, 8);
        char what[64];
        snprintf(what, sizeof what, "json %016llx", (unsigned long long)bits);
        buf_clear(&b);
        check_true(what, fmt_f64_json(&b, x) == 0);
        check_eq(what, b.p, b.len, sp1 + 1, (size_t)(sp2 - sp1 - 1));
        snprintf(what, sizeof what, "debug %016llx", (unsigned long long)bits);
        buf_clear(&b);
        check_true(what, fmt_f64_debug(&b, x) == 0);
        check_eq(what, b.p, b.len, sp2 + 1, (size_t)(eol - sp2 - 1));
        lines++;
        line = *eol ? eol + 1 : eol;
    }
    check_true("at least 2000 golden values", lines >= 2000);
    /* NaN and infinities: an error, nothing written. */
    uint64_t bad[] = {0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0xfff0000000000001ull};
    for (int i = 0; i < 4; i++) {
        double x;
        memcpy(&x, &bad[i], 8);
        buf_clear(&b);
        check_true("non-finite json", fmt_f64_json(&b, x) == CCORE_ERR_NONFINITE && b.len == 0);
        check_true("non-finite debug", fmt_f64_debug(&b, x) == CCORE_ERR_NONFINITE && b.len == 0);
    }
    printf("fmt: %d golden values\n", lines);
    buf_free(&b);
    free(text);
    return report("fmt");
}
