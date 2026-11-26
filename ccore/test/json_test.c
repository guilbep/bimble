/* Suite json: string escapes and pretty layout against serde_json (golden/floats/strings.txt,
 * pretty.txt), plus writer, buf and arena unit checks. */
#include <stdint.h>
#include "../src/arena.h"
#include "../src/json_r.h"
#include "../src/json_w.h"
#include "check.h"

static int hexval(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

/* The value of pretty.txt, written key by key in serde_json's (sorted) order. */
static int write_sample(Buf *b, int pretty) {
    JsonW w;
    jw_init(&w, b, pretty);
    jw_obj_begin(&w);
    jw_key(&w, "a");
    jw_arr_begin(&w);
    jw_i64(&w, 1);
    jw_i64(&w, 2);
    jw_obj_begin(&w);
    jw_key(&w, "b");
    jw_null(&w);
    jw_obj_end(&w);
    jw_arr_end(&w);
    jw_key(&w, "c");
    jw_obj_begin(&w);
    jw_obj_end(&w);
    jw_key(&w, "d");
    jw_arr_begin(&w);
    jw_arr_end(&w);
    jw_key(&w, "e");
    jw_obj_begin(&w);
    jw_key(&w, "f");
    jw_f64(&w, 0.1);
    jw_key(&w, "g");
    jw_bool(&w, 1);
    jw_key(&w, "h");
    jw_str(&w, "x\ny");
    jw_key(&w, "i");
    jw_arr_begin(&w);
    jw_i64(&w, -3);
    jw_u64(&w, UINT64_MAX);
    jw_i64(&w, INT64_MIN);
    jw_arr_end(&w);
    jw_obj_end(&w);
    jw_obj_end(&w);
    return jw_finish(&w);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "ccore/golden/floats";
    size_t n;
    Buf b;
    buf_init(&b);

    /* strings.txt: <hex input or -> <serde_json text> */
    char *text = slurp(dir, "strings.txt", &n);
    int strings = 0;
    for (char *line = text; *line;) {
        char *eol = strchr(line, '\n');
        if (!eol) eol = line + strlen(line);
        char *sp = memchr(line, ' ', (size_t)(eol - line));
        if (!sp) return 2;
        char raw[512];
        size_t rn = 0;
        if (*line != '-')
            for (char *h = line; h + 1 < sp && rn < sizeof raw; h += 2)
                raw[rn++] = (char)(hexval(h[0]) * 16 + hexval(h[1]));
        buf_clear(&b);
        json_escape(&b, raw, rn);
        char what[32];
        snprintf(what, sizeof what, "string %d", ++strings);
        check_eq(what, b.p, b.len, sp + 1, (size_t)(eol - sp - 1));
        line = *eol ? eol + 1 : eol;
    }
    check_true("10 golden strings", strings == 10);
    free(text);

    /* pretty.txt: the pretty text, then the compact text on the last line. */
    text = slurp(dir, "pretty.txt", &n);
    char *last = text + n - 1; /* final '\n' */
    char *compact = last;
    while (compact > text && compact[-1] != '\n') compact--;
    buf_clear(&b);
    check_true("pretty sample finishes", write_sample(&b, 1) == JW_OK);
    check_eq("pretty", b.p, b.len, text, (size_t)(compact - 1 - text));
    buf_clear(&b);
    check_true("compact sample finishes", write_sample(&b, 0) == JW_OK);
    check_eq("compact", b.p, b.len, compact, (size_t)(last - compact));
    free(text);

    /* Writer unit checks. */
    JsonW w;
    buf_clear(&b);
    jw_init(&w, &b, 0);
    jw_arr_begin(&w);
    jw_f64(&w, 1e16);
    jw_f64(&w, -0.0);
    jw_str_n(&w, "a\0b", 3);
    jw_arr_end(&w);
    check_true("array finishes", jw_finish(&w) == JW_OK);
    check_eq("array", b.p, b.len, "[1e+16,-0.0,\"a\\u0000b\"]", 23);

    buf_clear(&b);
    jw_init(&w, &b, 1);
    jw_arr_begin(&w);
    jw_arr_end(&w);
    check_true("pretty empty top array", jw_finish(&w) == JW_OK);
    check_eq("pretty []", b.p, b.len, "[]", 2);

    double inf, nan;
    uint64_t ib = 0x7ff0000000000000ull, nb = 0x7ff8000000000000ull;
    memcpy(&inf, &ib, 8);
    memcpy(&nan, &nb, 8);
    buf_clear(&b);
    jw_init(&w, &b, 0);
    jw_arr_begin(&w);
    jw_f64(&w, nan);
    check_true("NaN is an error", w.err == JW_ERR_NONFINITE);
    check_true("NaN writes nothing", b.len == 1);
    buf_clear(&b);
    jw_init(&w, &b, 0);
    jw_f64(&w, inf);
    check_true("inf is an error", jw_finish(&w) == JW_ERR_NONFINITE);

    buf_clear(&b);
    jw_init(&w, &b, 0);
    jw_arr_begin(&w);
    jw_key(&w, "k");
    check_true("key in array", w.err == JW_ERR_MISUSE);
    buf_clear(&b);
    jw_init(&w, &b, 0);
    jw_obj_begin(&w);
    jw_i64(&w, 1);
    check_true("value without key", w.err == JW_ERR_MISUSE);
    buf_clear(&b);
    jw_init(&w, &b, 0);
    jw_obj_begin(&w);
    jw_key(&w, "k");
    jw_obj_end(&w);
    check_true("end after key", w.err == JW_ERR_MISUSE);
    buf_clear(&b);
    jw_init(&w, &b, 0);
    jw_arr_begin(&w);
    jw_obj_end(&w);
    check_true("wrong end", w.err == JW_ERR_MISUSE);
    buf_clear(&b);
    jw_init(&w, &b, 0);
    jw_arr_begin(&w);
    check_true("unclosed", jw_finish(&w) == JW_ERR_MISUSE);
    buf_clear(&b);
    jw_init(&w, &b, 0);
    jw_null(&w);
    jw_null(&w);
    check_true("two top values", w.err == JW_ERR_MISUSE);
    buf_clear(&b);
    jw_init(&w, &b, 0);
    for (int i = 0; i < JW_MAX_DEPTH + 5; i++) jw_arr_begin(&w);
    check_true("depth bounded", w.err == JW_ERR_DEPTH);

    /* buf */
    buf_clear(&b);
    buf_i64(&b, INT64_MIN);
    buf_putc(&b, ' ');
    buf_u64(&b, 0);
    buf_putc(&b, ' ');
    buf_i64(&b, -42);
    check_eq("buf ints", b.p, b.len, "-9223372036854775808 0 -42", 26);
    buf_clear(&b);
    for (int i = 0; i < 100000; i++) buf_putc(&b, (char)('a' + i % 26));
    check_true("buf grows", b.len == 100000 && b.p[99999] == 'a' + 99999 % 26 && !b.oom);
    check_true("buf cstr", strlen(buf_cstr(&b)) == 100000);

    /* arena */
    Arena a;
    arena_init(&a, 256);
    char *p1 = arena_alloc(&a, 10, 1);
    double *p2 = arena_alloc(&a, sizeof(double) * 4, 8);
    check_true("arena align", p1 && p2 && ((uintptr_t)p2 & 7) == 0 && p2[3] == 0.0);
    char *big = arena_alloc(&a, 10000, 16);
    check_true("arena big block", big && ((uintptr_t)big & 15) == 0 && big[9999] == 0);
    char *d = arena_dup(&a, "hello", 5);
    check_true("arena dup", d && memcmp(d, "hello", 5) == 0);
    check_true("arena bad align", arena_alloc(&a, 1, 3) == NULL);
    arena_reset(&a);
    check_true("arena reset keeps one block", a.head != NULL);
    check_true("arena after reset", arena_alloc(&a, 100, 8) != NULL);
    /* FIXLIB: n * size checked (a count from a file; on wasm32 the product wrapped to a small
     * block). SIZE_MAX / 8 + 1 items of 8 bytes wrap to 0 bytes without the check. */
    check_true("arena_alloc_n overflow -> NULL", arena_alloc_n(&a, (size_t)-1 / 8 + 1, 8, 8) == NULL);
    check_true("arena_alloc_n overflow, size 48", arena_alloc_n(&a, (size_t)-1 / 48 + 1, 48, 8) == NULL);
    double *pn = arena_alloc_n(&a, 3, sizeof(double), 8);
    check_true("arena_alloc_n 3 doubles", pn && pn[2] == 0.0);
    arena_free(&a);
    check_true("arena freed", a.head == NULL);

    /* FIXLIB: the DOM budget (json_r.h), one for the text and the CBOR form */
    {
        JrBudget bud = {0, 0};
        const char *why = NULL;
        check_true("budget: 16 M values of 64 B fit", jr_budget_take(&bud, JR_MAX_NODES, 64, 0, &why) == 0);
        check_true("budget: one more value refused", jr_budget_take(&bud, 1, 0, 0, &why) == -1 && why && !strcmp(why, JR_ERR_NODES));
        check_true("budget: nothing taken on refusal", bud.nodes == JR_MAX_NODES && bud.bytes == JR_MAX_DOM_BYTES);
        JrBudget b2 = {0, 0};
        why = NULL;
        check_true("budget: bytes over the limit refused",
                   jr_budget_take(&b2, 10, 64, JR_MAX_DOM_BYTES, &why) == -1 && why && !strcmp(why, JR_ERR_BYTES) && b2.bytes == 0);
        check_true("budget: a huge count refused before the multiply",
                   jr_budget_take(&b2, (size_t)-1, (size_t)-1, 0, &why) == -1 && why && !strcmp(why, JR_ERR_NODES));
        /* the text reader charges: an honest document parses, its values counted */
        Arena ja;
        arena_init(&ja, 0);
        JrError je;
        const char *doc = "{\"a\": [1, 2, \"xyz\"], \"b\": null}";
        check_true("budget: a small document parses", jr_parse(&ja, doc, strlen(doc), &je) != NULL);
        arena_free(&ja);
    }

    buf_free(&b);
    return report("json");
}
